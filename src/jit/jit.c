// ============================================================
//  O Language Compiler — o_jit.c
//  JIT engine: IR → x86-64 machine code → execute now
//  Linear scan register allocator | mmap RWX pages
//  Z-TEAM | C23
// ============================================================
#include "jit/jit.h"
#ifndef _WIN32
#include <sys/mman.h>
#endif
#ifndef _WIN32
#include <dlfcn.h>
#endif  // dlsym for external symbol resolution

// ── JIT engine lifecycle ──────────────────────────────────────

JITEngine *jit_engine_new(IRModule *module) {
    JITEngine *e = calloc(1, sizeof(JITEngine));
    if (!e) return NULL;
    arena_init(&e->arena, MB(16));
    e->module = module;
    return e;
}

void jit_engine_free(JITEngine *e) {
    if (!e) return;
    // Free compiled function exec memories
    for (u32 i = 0; i < e->cache_count; i++) {
        JITCompiledFunc *f = e->cached_funcs[i];
        if (f && f->exec_mem)
            munmap(f->exec_mem, f->exec_mem_sz);
    }
    arena_destroy(&e->arena);
    free(e);
}

// ── Stack slot allocation ─────────────────────────────────────

static i32 jit_alloc_stack_slot(JITContext *ctx, u8 sz) {
    sz = (u8)ALIGN_UP(sz, sz); // ensure natural alignment
    ctx->next_stack_slot -= (i32)sz;
    ctx->next_stack_slot = (i32)ALIGN_DOWN(ctx->next_stack_slot, sz);
    return ctx->next_stack_slot;
}

// ── Linear scan register allocator ───────────────────────────
// Simple 2-pass:
//   Pass 1: compute live intervals per vreg
//   Pass 2: linear scan assignment

// Callee-saved first: survive calls. Caller-saved last: may be clobbered.
// R10/R11 reserved as scratch for arithmetic ops — never allocated to vregs.
#define INT_ALLOC_ORDER_COUNT 10
static const X64Reg INT_ALLOC_ORDER[INT_ALLOC_ORDER_COUNT] = {
    X64_RBX, X64_R12, X64_R13, X64_R14, X64_R15,  // callee-saved (survive calls)
    X64_RAX, X64_RCX, X64_RDX, X64_RSI, X64_RDI,  // caller-saved (short-lived only)
    // R10, R11 reserved as arithmetic scratch — not allocated
};

// Callee-saved GP regs that must be preserved across calls (SysV ABI)
static const X64Reg CALLEE_SAVED_REGS[] = { X64_RBX, X64_R12, X64_R13, X64_R14, X64_R15 };
#define CALLEE_SAVED_COUNT 5


// ── Liveness mark helper (C99/C23 compatible) ────────────────
static void mark_vreg_use(JITContext *ctx, IRVal v, u32 seq) {
    if (v.kind == IRVAL_TEMP && v.vreg < JIT_MAX_VREGS) {
        if (ctx->vregs[v.vreg].last_use < seq)
            ctx->vregs[v.vreg].last_use = seq;
        if (ctx->vregs[v.vreg].first_use == 0)
            ctx->vregs[v.vreg].first_use = seq;
    }
}

void jit_liveness_analysis(JITContext *ctx) {
    IRFunc *fn = ctx->func;
    u32 instr_seq = 0;
    for (u32 bi = 0; bi < fn->block_count; bi++) {
        IRBlock *b = fn->blocks[bi];
        for (u32 ii = 0; ii < b->instr_count; ii++) {
            IRInstr *instr = &b->instrs[ii];
            // Mark uses
            mark_vreg_use(ctx, instr->src1, instr_seq);
            mark_vreg_use(ctx, instr->src2, instr_seq);
            for (u32 a = 0; a < instr->arg_count; a++)
                mark_vreg_use(ctx, instr->args[a], instr_seq);
            // Mark def
            if (instr->dst.kind == IRVAL_TEMP) {
                u32 vr = instr->dst.vreg;
                if (vr < JIT_MAX_VREGS) {
                    ctx->vregs[vr].first_use = instr_seq;
                    ctx->vregs[vr].state = VREG_UNALLOCATED;
                    ctx->vregs[vr].is_float =
                        (instr->result_type == TY_F32 || instr->result_type == TY_F64);
                }
            }
            instr_seq++;
        }
    }
}

void jit_linear_scan_alloc(JITContext *ctx) {
    // Available physical registers (free = 1 in bitmask)
    u32 int_free = 0;
    for (int i = 0; i < INT_ALLOC_ORDER_COUNT; i++)
        int_free |= (1u << INT_ALLOC_ORDER[i]);
    // Reserve rsp, rbp
    int_free &= ~((1u << X64_RSP) | (1u << X64_RBP));
    ctx->int_reg_free = int_free;
    ctx->xmm_reg_free = (1u << JIT_MAX_XMMS) - 1; // xmm0..7 free

    // Active intervals (simple array — real compiler uses priority queue)
    #define ACTIVE_MAX 32
    u32 active[ACTIVE_MAX];
    u32 active_count = 0;

    for (u32 vr = 0; vr < ctx->vreg_count; vr++) {
        VRegInfo *info = &ctx->vregs[vr];
        if (info->state == VREG_UNALLOCATED && info->first_use == 0)
            continue; // never defined

        // Expire old intervals
        for (u32 ai = 0; ai < active_count; ) {
            VRegInfo *av = &ctx->vregs[active[ai]];
            if (av->last_use < info->first_use) {
                // Expire — free its register
                if (!av->is_float)
                    ctx->int_reg_free |= (1u << av->phys_reg);
                else
                    ctx->xmm_reg_free |= (1u << av->xmm_reg);
                active[ai] = active[--active_count];
            } else {
                ai++;
            }
        }

        if (!info->is_float) {
            if (ctx->int_reg_free != 0) {
                // Pick lowest free GP reg (prefer caller-saved for shorter funcs)
                u32 mask = ctx->int_reg_free;
                int bit = __builtin_ctz(mask);
                info->phys_reg = (X64Reg)bit;
                info->state = VREG_IN_REGISTER;
                ctx->int_reg_free &= ~(1u << bit);
                if (active_count < ACTIVE_MAX)
                    active[active_count++] = vr;
            } else {
                // Spill — pick longest live interval active
                info->state = VREG_SPILLED;
                info->stack_off = jit_alloc_stack_slot(ctx, 8);
            }
        } else {
            if (ctx->xmm_reg_free != 0) {
                int bit = __builtin_ctz(ctx->xmm_reg_free);
                info->xmm_reg = (X64XmmReg)bit;
                info->state = VREG_IN_REGISTER;
                ctx->xmm_reg_free &= ~(1u << bit);
                if (active_count < ACTIVE_MAX)
                    active[active_count++] = vr;
            } else {
                info->state = VREG_SPILLED;
                info->stack_off = jit_alloc_stack_slot(ctx, 8);
            }
        }
    }
}

// ── Code generation ──────────────────────────────────────────

static u8 ty_size(TypeKind ty) {
    return (u8)ty_primitive_size(ty);
}

// Get the physical register for a vreg (assert not spilled)
static X64Reg vreg_to_reg(JITContext *ctx, u32 vr) {
    O_ASSERT(vr < JIT_MAX_VREGS, "vreg out of range");
    VRegInfo *info = &ctx->vregs[vr];
    O_ASSERT(info->state == VREG_IN_REGISTER, "vreg is spilled — load needed");
    O_ASSERT(!info->is_float, "expected int reg");
    return info->phys_reg;
}

static X64XmmReg vreg_to_xmm(JITContext *ctx, u32 vr) {
    O_ASSERT(vr < JIT_MAX_VREGS, "vreg out of range");
    VRegInfo *info = &ctx->vregs[vr];
    O_ASSERT(info->state == VREG_IN_REGISTER, "vreg is spilled");
    O_ASSERT(info->is_float, "expected xmm reg");
    return info->xmm_reg;
}

// Materialize an IRVal into a GP register (may emit a load from stack)
static X64Reg jit_val_to_reg(JITContext *ctx, IRVal v, X64Reg scratch) {
    X64Asm *a = &ctx->asm_;
    switch (v.kind) {
        case IRVAL_TEMP: {
            VRegInfo *info = &ctx->vregs[v.vreg];
            if (info->state == VREG_IN_REGISTER) return info->phys_reg;
            // Spilled: load into scratch
            u8 sz = ty_size(v.type) ? ty_size(v.type) : 8;
            x64_mov_rm(a, scratch, X64_RBP, info->stack_off, sz);
            return scratch;
        }
        case IRVAL_CONST_I:
            x64_mov_ri(a, scratch, v.ival, 8);
            return scratch;
        case IRVAL_GLOBAL:
            // String literal: v.name.ptr IS the actual runtime address (arena-allocated)
            x64_mov_ri(a, scratch, (i64)(uintptr_t)v.name.ptr, 8);
            return scratch;
        default:
            O_PANIC("unsupported irval kind in jit_val_to_reg");
    }
}

// Write back a result in scratch to vreg's home location
static void jit_store_vreg(JITContext *ctx, u32 vr, X64Reg src) {
    VRegInfo *info = &ctx->vregs[vr];
    if (info->state == VREG_SPILLED) {
        x64_mov_mr(&ctx->asm_, X64_RBP, info->stack_off, src, 8);
    }
    // If in-register it's already there (src == info->phys_reg)
}

// Generate code for a single IR instruction
static void jit_emit_instr(JITContext *ctx, IRInstr *instr, u32 seq) {
    UNUSED(seq);
    X64Asm *a = &ctx->asm_;

    switch (instr->op) {
        case IOP_NOP:
            x64_nop(a);
            break;

        case IOP_MOV: {
            if (instr->dst.kind != IRVAL_TEMP) break;
            u8 sz = ty_size(instr->result_type); if (!sz) sz = 8;
            if (instr->src1.kind == IRVAL_CONST_I) {
                VRegInfo *di = &ctx->vregs[instr->dst.vreg];
                if (di->state == VREG_IN_REGISTER) {
                    x64_mov_ri(a, di->phys_reg, instr->src1.ival, sz);
                } else {
                    x64_mov_ri(a, X64_RAX, instr->src1.ival, sz);
                    x64_mov_mr(a, X64_RBP, di->stack_off, X64_RAX, sz);
                }
            } else if (instr->src1.kind == IRVAL_TEMP) {
                X64Reg src = jit_val_to_reg(ctx, instr->src1, X64_RAX);
                VRegInfo *di = &ctx->vregs[instr->dst.vreg];
                if (di->state == VREG_IN_REGISTER) {
                    if (di->phys_reg != src)
                        x64_mov_rr(a, di->phys_reg, src, sz);
                } else {
                    x64_mov_mr(a, X64_RBP, di->stack_off, src, sz);
                }
            }
            break;
        }

        case IOP_ADD: case IOP_SUB: {
            if (instr->dst.kind != IRVAL_TEMP) break;
            u8 sz = ty_size(instr->result_type); if (!sz) sz = 8;
            X64Reg lhs = jit_val_to_reg(ctx, instr->src1, X64_R10);
            if (lhs != X64_R10) { x64_mov_rr(a, X64_R10, lhs, sz); lhs = X64_R10; }
            X64Reg rhs = jit_val_to_reg(ctx, instr->src2, X64_R11);
            VRegInfo *di = &ctx->vregs[instr->dst.vreg];
            X64Reg dst = (di->state == VREG_IN_REGISTER) ? di->phys_reg : X64_RAX;
            if (dst != lhs) x64_mov_rr(a, dst, lhs, sz);
            if (instr->op == IOP_ADD) x64_add_rr(a, dst, rhs, sz);
            else                      x64_sub_rr(a, dst, rhs, sz);
            jit_store_vreg(ctx, instr->dst.vreg, dst);
            break;
        }

        case IOP_MUL: {
            // Use R10/R11 as safe scratch regs to avoid lhs/rhs/dst collisions
            u8 sz = ty_size(instr->result_type); if (!sz) sz = 8;
            X64Reg lhs = jit_val_to_reg(ctx, instr->src1, X64_R10);
            // Save lhs before loading rhs (rhs scratch might overwrite lhs)
            if (lhs != X64_R10) { x64_mov_rr(a, X64_R10, lhs, sz); lhs = X64_R10; }
            X64Reg rhs = jit_val_to_reg(ctx, instr->src2, X64_R11);
            VRegInfo *di = &ctx->vregs[instr->dst.vreg];
            X64Reg dst = (di->state == VREG_IN_REGISTER) ? di->phys_reg : X64_RAX;
            if (dst != lhs) x64_mov_rr(a, dst, lhs, sz);
            x64_imul_rr(a, dst, rhs, sz);
            jit_store_vreg(ctx, instr->dst.vreg, dst);
            break;
        }

        case IOP_DIV: {
            u8 sz = ty_size(instr->result_type); if (!sz) sz = 8;
            X64Reg rhs = jit_val_to_reg(ctx, instr->src2, X64_RCX);
            X64Reg lhs = jit_val_to_reg(ctx, instr->src1, X64_RAX);
            if (lhs != X64_RAX) x64_mov_rr(a, X64_RAX, lhs, sz);
            if (sz == 8) x64_cqo(a); else x64_cdq(a);
            x64_idiv(a, rhs, sz);
            VRegInfo *di = &ctx->vregs[instr->dst.vreg];
            if (di->state == VREG_IN_REGISTER && di->phys_reg != X64_RAX)
                x64_mov_rr(a, di->phys_reg, X64_RAX, sz);
            else if (di->state == VREG_SPILLED)
                x64_mov_mr(a, X64_RBP, di->stack_off, X64_RAX, sz);
            break;
        }

        case IOP_MOD: {
            u8 sz = ty_size(instr->result_type); if (!sz) sz = 8;
            X64Reg rhs = jit_val_to_reg(ctx, instr->src2, X64_RCX);
            X64Reg lhs = jit_val_to_reg(ctx, instr->src1, X64_RAX);
            if (lhs != X64_RAX) x64_mov_rr(a, X64_RAX, lhs, sz);
            if (sz == 8) x64_cqo(a); else x64_cdq(a);
            x64_idiv(a, rhs, sz);
            // Remainder is in RDX
            VRegInfo *di = &ctx->vregs[instr->dst.vreg];
            if (di->state == VREG_IN_REGISTER)
                x64_mov_rr(a, di->phys_reg, X64_RDX, sz);
            else
                x64_mov_mr(a, X64_RBP, di->stack_off, X64_RDX, sz);
            break;
        }

        case IOP_AND: case IOP_OR: case IOP_XOR: {
            u8 sz = ty_size(instr->result_type); if (!sz) sz = 8;
            X64Reg lhs = jit_val_to_reg(ctx, instr->src1, X64_R10);
            if (lhs != X64_R10) { x64_mov_rr(a, X64_R10, lhs, sz); lhs = X64_R10; }
            X64Reg rhs = jit_val_to_reg(ctx, instr->src2, X64_R11);
            VRegInfo *di = &ctx->vregs[instr->dst.vreg];
            X64Reg dst = (di->state == VREG_IN_REGISTER) ? di->phys_reg : X64_RAX;
            if (dst != lhs) x64_mov_rr(a, dst, lhs, sz);
            if (instr->op == IOP_AND)      x64_and_rr(a, dst, rhs, sz);
            else if (instr->op == IOP_OR)  x64_or_rr(a, dst, rhs, sz);
            else                           x64_xor_rr(a, dst, rhs, sz);
            jit_store_vreg(ctx, instr->dst.vreg, dst);
            break;
        }

        case IOP_NEG: {
            u8 sz = ty_size(instr->result_type); if (!sz) sz = 8;
            X64Reg src = jit_val_to_reg(ctx, instr->src1, X64_RAX);
            VRegInfo *di = &ctx->vregs[instr->dst.vreg];
            X64Reg dst = (di->state == VREG_IN_REGISTER) ? di->phys_reg : X64_RAX;
            if (dst != src) x64_mov_rr(a, dst, src, sz);
            x64_neg(a, dst, sz);
            jit_store_vreg(ctx, instr->dst.vreg, dst);
            break;
        }

        case IOP_CMP_EQ: case IOP_CMP_NE:
        case IOP_CMP_LT: case IOP_CMP_LE:
        case IOP_CMP_GT: case IOP_CMP_GE: {
            u8 sz = ty_size(instr->src1.type); if (!sz) sz = 8;
            // Pin lhs into R10 BEFORE loading rhs to avoid register collision
            X64Reg lhs = jit_val_to_reg(ctx, instr->src1, X64_R10);
            if (lhs != X64_R10) { x64_mov_rr(a, X64_R10, lhs, sz); lhs = X64_R10; }
            X64Reg rhs = jit_val_to_reg(ctx, instr->src2, X64_R11);
            x64_cmp_rr(a, lhs, rhs, sz);
            // Use RAX for setcc result (avoids clobbering R10/R11)
            x64_xor_rr(a, X64_RAX, X64_RAX, 4);
            switch (instr->op) {
                case IOP_CMP_EQ: x64_sete(a, X64_RAX);  break;
                case IOP_CMP_NE: x64_setne(a, X64_RAX); break;
                case IOP_CMP_LT: x64_setl(a, X64_RAX);  break;
                case IOP_CMP_LE: x64_setle(a, X64_RAX); break;
                case IOP_CMP_GT: x64_setg(a, X64_RAX);  break;
                case IOP_CMP_GE: x64_setge(a, X64_RAX); break;
                default: break;
            }
            VRegInfo *di = &ctx->vregs[instr->dst.vreg];
            if (di->state == VREG_IN_REGISTER && di->phys_reg != X64_RAX)
                x64_mov_rr(a, di->phys_reg, X64_RAX, 1);
            else if (di->state == VREG_SPILLED)
                x64_mov_mr(a, X64_RBP, di->stack_off, X64_RAX, 1);
            break;
        }

        case IOP_LABEL:
            x64_def_label(a, instr->src1.label_id);
            break;

        case IOP_JMP:
            x64_jmp(a, instr->src1.label_id);
            break;

        case IOP_JZ: {
            X64Reg cond = jit_val_to_reg(ctx, instr->src1, X64_RAX);
            x64_test_rr(a, cond, cond, 4);
            x64_je(a, instr->src2.label_id);
            break;
        }

        case IOP_JNZ: {
            X64Reg cond = jit_val_to_reg(ctx, instr->src1, X64_RAX);
            x64_test_rr(a, cond, cond, 4);
            x64_jne(a, instr->src2.label_id);
            break;
        }

        case IOP_CALL: {
            // System V AMD64: args in rdi, rsi, rdx, rcx, r8, r9
            // Align RSP before call (already done in prologue, but
            // if odd number of pushes occurred we need +8)
            u32 n = instr->arg_count;
            // Load each arg into R10 first, then move to target arg reg.
            // This avoids clobbering an already-placed arg when loading the next.
            for (u32 i = 0; i < n && i < SYSV_ARG_REGS_INT; i++) {
                X64Reg arg_reg = sysv_int_arg_regs[i];
                X64Reg src = jit_val_to_reg(ctx, instr->args[i], X64_R10);
                if (src != arg_reg) x64_mov_rr(a, arg_reg, src, 8);
            }
            // Stack args if n > 6 — push in reverse order
            for (i32 i = (i32)n - 1; i >= SYSV_ARG_REGS_INT; i--) {
                X64Reg src = jit_val_to_reg(ctx, instr->args[i], X64_R10);
                if (src != X64_RAX) x64_mov_rr(a, X64_RAX, src, 8);
                x64_push(a, X64_RAX);
            }

            // Emit CALL
            if (instr->src1.kind == IRVAL_FUNC) {
                // Resolve function address via fn_table (handles forward/recursive refs)
                // or dlsym for external libc symbols
                StrView fname = instr->src1.name;
                i32 fn_idx = -1;
                for (u32 ci = 0; ci < ctx->engine->cache_count; ci++) {
                    StrView cn = ctx->engine->cached_names[ci];
                    if (cn.len == fname.len && memcmp(cn.ptr, fname.ptr, fname.len) == 0) {
                        fn_idx = (i32)ci;
                        break;
                    }
                }
                if (fn_idx >= 0) {
                    // Load from fn_table[fn_idx] — indirect call through pointer table
                    // This works even for forward/recursive calls since fn_table is filled
                    // after all functions are compiled.
                    void **slot = &ctx->engine->fn_table[fn_idx];
                    x64_mov_ri(a, X64_RAX, (i64)(uintptr_t)slot, 8);
                    x64_mov_rm(a, X64_RAX, X64_RAX, 0, 8); // deref: rax = *slot
                    x64_call_reg(a, X64_RAX);
                } else {
                    // External symbol: resolve via dlsym at compile time
                    char name_buf[256];
                    usize nlen = fname.len < 255 ? fname.len : 255;
                    memcpy(name_buf, fname.ptr, nlen);
                    name_buf[nlen] = '\0';
                    void *fn_ptr = dlsym(RTLD_DEFAULT, name_buf);
                    x64_mov_ri(a, X64_RAX, (i64)(uintptr_t)fn_ptr, 8);
                    x64_call_reg(a, X64_RAX);
                }
            } else {
                X64Reg fn_reg = jit_val_to_reg(ctx, instr->src1, X64_RAX);
                x64_call_reg(a, fn_reg);
            }

            // Clean up stack args if any
            if (n > SYSV_ARG_REGS_INT) {
                u32 stack_args = n - SYSV_ARG_REGS_INT;
                x64_add_ri(a, X64_RSP, (i32)(stack_args * 8), 8);
            }

            // Move return value into dst
            if (instr->dst.kind == IRVAL_TEMP) {
                VRegInfo *di = &ctx->vregs[instr->dst.vreg];
                if (di->state == VREG_IN_REGISTER && di->phys_reg != X64_RAX)
                    x64_mov_rr(a, di->phys_reg, X64_RAX, 8);
                else if (di->state == VREG_SPILLED)
                    x64_mov_mr(a, X64_RBP, di->stack_off, X64_RAX, 8);
            }
            break;
        }

        case IOP_RET: {
            u8 sz = ty_size(instr->src1.type); if (!sz) sz = 8;
            X64Reg src = jit_val_to_reg(ctx, instr->src1, X64_RAX);
            if (src != X64_RAX) x64_mov_rr(a, X64_RAX, src, sz);
            for (i32 ci = (i32)ctx->callee_saved_used - 1; ci >= 0; ci--)
                x64_pop(&ctx->asm_, CALLEE_SAVED_REGS[ci]);
            x64_emit_epilogue(a);
            break;
        }

        case IOP_RET_VOID:
            // Restore callee-saved regs (in reverse order) before standard epilogue
            for (i32 ci = (i32)ctx->callee_saved_used - 1; ci >= 0; ci--)
                x64_pop(&ctx->asm_, CALLEE_SAVED_REGS[ci]);
            x64_emit_epilogue(a);
            break;

        case IOP_ALLOCA: {
            // Allocate a new stack data slot and store its address into the vreg.
            // Use R11 as scratch — NEVER RAX/RCX which may hold live param values.
            i32 data_slot = jit_alloc_stack_slot(ctx, (u8)instr->src1.ival);
            VRegInfo *di = &ctx->vregs[instr->dst.vreg];
            // Always spill alloca pointers — they are stack addresses, not real values
            di->state     = VREG_SPILLED;
            di->stack_off = jit_alloc_stack_slot(ctx, 8);
            // LEA into R11 (safe scratch), then store to spill slot
            x64_lea(a, X64_R11, X64_RBP, data_slot);
            x64_mov_mr(a, X64_RBP, di->stack_off, X64_R11, 8);
            break;
        }

        case IOP_LOAD: {
            u8 sz = ty_size(instr->result_type); if (!sz) sz = 8;
            X64Reg base = jit_val_to_reg(ctx, instr->src1, X64_RCX);
            i32 off = (instr->src2.kind == IRVAL_CONST_I) ? (i32)instr->src2.ival : 0;
            VRegInfo *di = &ctx->vregs[instr->dst.vreg];
            X64Reg dst = (di->state == VREG_IN_REGISTER) ? di->phys_reg : X64_RAX;
            x64_mov_rm(a, dst, base, off, sz);
            jit_store_vreg(ctx, instr->dst.vreg, dst);
            break;
        }

        case IOP_STORE: {
            u8 sz = ty_size(instr->src2.type); if (!sz) sz = 8;
            X64Reg base = jit_val_to_reg(ctx, instr->dst, X64_RCX);
            i32 off = (instr->src1.kind == IRVAL_CONST_I) ? (i32)instr->src1.ival : 0;
            X64Reg val  = jit_val_to_reg(ctx, instr->src2, X64_RAX);
            x64_mov_mr(a, base, off, val, sz);
            break;
        }

        case IOP_SEXT: {
            u8 src_sz = ty_size(instr->src1.type); if (!src_sz) src_sz = 4;
            u8 dst_sz = ty_size(instr->result_type); if (!dst_sz) dst_sz = 8;
            X64Reg src = jit_val_to_reg(ctx, instr->src1, X64_RAX);
            VRegInfo *di = &ctx->vregs[instr->dst.vreg];
            X64Reg dst = (di->state == VREG_IN_REGISTER) ? di->phys_reg : X64_RAX;
            x64_movsx(a, dst, src, src_sz, dst_sz);
            jit_store_vreg(ctx, instr->dst.vreg, dst);
            break;
        }

        case IOP_ZEXT: {
            u8 src_sz = ty_size(instr->src1.type); if (!src_sz) src_sz = 4;
            u8 dst_sz = ty_size(instr->result_type); if (!dst_sz) dst_sz = 8;
            X64Reg src = jit_val_to_reg(ctx, instr->src1, X64_RAX);
            VRegInfo *di = &ctx->vregs[instr->dst.vreg];
            X64Reg dst = (di->state == VREG_IN_REGISTER) ? di->phys_reg : X64_RAX;
            x64_movzx(a, dst, src, src_sz, dst_sz);
            jit_store_vreg(ctx, instr->dst.vreg, dst);
            break;
        }

        default:
            // Unimplemented — emit NOP + warning in debug builds
            x64_nop(a);
            break;
    }
}

// ── Compile a single IR function to machine code ──────────────

JITCompiledFunc *jit_compile_func(JITEngine *e, IRFunc *fn) {
    // ── Allocate JIT context ─────────────────────────────────
    JITContext *ctx = ARENA_ALLOC_ZERO(&e->arena, JITContext);
    ctx->func   = fn;
    ctx->engine = e;
    ctx->vreg_count = fn->next_vreg;
    ctx->next_stack_slot = 0;

    // ── Liveness + register allocation ──────────────────────
    jit_liveness_analysis(ctx);
    jit_linear_scan_alloc(ctx);

    // ── Allocate executable memory ───────────────────────────
    Arena scratch;
    arena_init(&scratch, MB(1));
    FixupTable ft; fixup_table_init(&ft, &scratch);
    LabelTable lt; label_table_init(&lt, &scratch);

    if (!codebuf_init_exec(&ctx->code_buf, KB(64)))
        return NULL;

    x64asm_init(&ctx->asm_, &ctx->code_buf, &ft, &lt);

    // ── Determine which callee-saved regs are used ──────────
    usize prologue_start = ctx->code_buf.len;
    // Scan allocated vregs to find which callee-saved regs are needed
    u32 callee_used = 0; // bitmask into CALLEE_SAVED_REGS
    for (u32 vr = 0; vr < ctx->vreg_count; vr++) {
        VRegInfo *vi = &ctx->vregs[vr];
        if (vi->state != VREG_IN_REGISTER) continue;
        for (u32 ci = 0; ci < CALLEE_SAVED_COUNT; ci++) {
            if (vi->phys_reg == CALLEE_SAVED_REGS[ci]) {
                callee_used |= (1u << ci);
            }
        }
    }

    // ── Emit prologue ─────────────────────────────────────────
    x64_push(&ctx->asm_, X64_RBP);
    x64_mov_rr(&ctx->asm_, X64_RBP, X64_RSP, 8);
    // Push callee-saved regs we use (in order)
    for (u32 ci = 0; ci < CALLEE_SAVED_COUNT; ci++) {
        if (callee_used & (1u << ci))
            x64_push(&ctx->asm_, CALLEE_SAVED_REGS[ci]);
    }
    // sub rsp, imm32  (patched after alloca analysis)
    usize sub_rsp_off = ctx->code_buf.len;
    codebuf_emit_byte(&ctx->code_buf, REX_W);
    codebuf_emit_byte(&ctx->code_buf, 0x81);
    codebuf_emit_byte(&ctx->code_buf, x64_modrm(3, 5, X64_RSP & 7));
    codebuf_emit_i32(&ctx->code_buf, 0); // placeholder

    // ── Emit function parameters (ABI: first N in int arg regs) ──
    for (u32 i = 0; i < fn->param_count && i < SYSV_ARG_REGS_INT; i++) {
        // Find the vreg corresponding to param i (it's vreg i)
        VRegInfo *info = &ctx->vregs[i];
        X64Reg arg_reg = sysv_int_arg_regs[i];
        if (info->state == VREG_IN_REGISTER && info->phys_reg != arg_reg) {
            x64_mov_rr(&ctx->asm_, info->phys_reg, arg_reg, 8);
        } else if (info->state == VREG_SPILLED) {
            x64_mov_mr(&ctx->asm_, X64_RBP, info->stack_off, arg_reg, 8);
        }
    }

    // Store callee-saved count so RET/RET_VOID can pop in reverse
    ctx->callee_saved_used = 0;
    for (u32 ci = 0; ci < CALLEE_SAVED_COUNT; ci++)
        if (callee_used & (1u << ci)) ctx->callee_saved_used++;

    // ── Emit all blocks ───────────────────────────────────────
    u32 instr_seq = 0;
    for (u32 bi = 0; bi < fn->block_count; bi++) {
        IRBlock *b = fn->blocks[bi];
        for (u32 ii = 0; ii < b->instr_count; ii++) {
            jit_emit_instr(ctx, &b->instrs[ii], instr_seq++);
        }
    }

    // ── Patch frame size ──────────────────────────────────────
    i32 frame_size = -ctx->next_stack_slot;
    frame_size = (i32)ALIGN_UP((u32)frame_size, 16);
    // Ensure stack is 16-byte aligned after prologue (ABI)
    if ((frame_size & 8) == 0) frame_size += 8;
    codebuf_patch_i32(&ctx->code_buf, sub_rsp_off + 3, frame_size);

    // ── Resolve fixups ────────────────────────────────────────
    fixup_resolve_all(&ft, &lt, &ctx->code_buf);

    arena_destroy(&scratch);

    // ── Wrap result ───────────────────────────────────────────
    JITCompiledFunc *result = ARENA_ALLOC(&e->arena, JITCompiledFunc);
    result->entry       = ctx->code_buf.buf + prologue_start;
    result->code_size   = ctx->code_buf.len;
    result->exec_mem    = ctx->code_buf.buf;
    result->exec_mem_sz = ctx->code_buf.cap;

    UNUSED(prologue_start);
    return result;
}

JITCompiledFunc *jit_find_func(JITEngine *e, StrView name) {
    for (u32 i = 0; i < e->cache_count; i++) {
        if (sv_eq(e->cached_names[i], name))
            return e->cached_funcs[i];
    }
    return NULL;
}

OResult jit_compile_module(JITEngine *e) {
    IRModule *m = e->module;
    e->cached_names = arena_alloc_aligned(&e->arena,
                        m->func_count * sizeof(StrView), _Alignof(StrView));
    e->cached_funcs = arena_alloc_aligned(&e->arena,
                        m->func_count * sizeof(JITCompiledFunc*), _Alignof(JITCompiledFunc*));

    // Pre-register all function names so recursive calls can find their index
    for (u32 i = 0; i < m->func_count; i++) {
        e->cached_names[i] = m->funcs[i]->name;
        e->cached_funcs[i] = NULL; // filled after compilation
    }
    e->cache_count = m->func_count;

    // Allocate an RW pointer table for indirect calls (resolves forward/recursive refs)
    e->fn_table = mmap(NULL, m->func_count * sizeof(void*),
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memset(e->fn_table, 0, m->func_count * sizeof(void*));

    // Compile all functions
    for (u32 i = 0; i < m->func_count; i++) {
        IRFunc *fn = m->funcs[i];
        JITCompiledFunc *cf = jit_compile_func(e, fn);
        if (!cf) return o_err(O_ECODEGEN, "JIT compile failed", srcloc_invalid());
        e->cached_funcs[i] = cf;
        e->fn_table[i]     = cf->entry;  // fill pointer table slot
    }
    return o_ok();
}

i64 jit_call_i64(JITCompiledFunc *fn,
                 i64 a0, i64 a1, i64 a2, i64 a3, i64 a4, i64 a5) {
    typedef i64 (*Fn6)(i64,i64,i64,i64,i64,i64);
    return ((Fn6)fn->entry)(a0, a1, a2, a3, a4, a5);
}

void jit_dump_hex(const JITCompiledFunc *fn, FILE *out) {
    fprintf(out, O_COLOR_GREEN "JIT code dump" O_COLOR_RESET
            " (%zu bytes @ %p):\n", fn->code_size, fn->entry);
    const u8 *p = (const u8 *)fn->entry;
    for (usize i = 0; i < fn->code_size; i++) {
        if (i % 16 == 0) fprintf(out, "  %04zx: ", i);
        fprintf(out, "%02x ", p[i]);
        if (i % 16 == 15 || i == fn->code_size - 1) fputc('\n', out);
    }
}
