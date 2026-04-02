// ============================================================
//  O Language Compiler — o_pe.c
//  PE32+ writer: generates .exe and .dll for Windows x64
//  Zero external tools — pure C23, hand-rolled binary output
//  Z-TEAM | C23
// ============================================================
#include "lib/windows/pe.h"
#include "jit/jit.h"   // VRegInfo, VRegState, JIT_MAX_VREGS
#include "backend/x64.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <fcntl.h>
#endif
#ifndef _WIN32
#include <unistd.h>
#endif

// ── Minimal DOS stub (prints "This program cannot be run in DOS mode") ──
static const u8 DOS_STUB[] = {
    0x0E,0x1F,0xBA,0x0E,0x00,0xB4,0x09,0xCD,0x21,0xB8,0x01,0x4C,0xCD,0x21,
    'T','h','i','s',' ','p','r','o','g','r','a','m',' ','c','a','n','n','o','t',
    ' ','b','e',' ','r','u','n',' ','i','n',' ','D','O','S',' ','m','o','d','e',
    '.',0x0D,0x0D,0x0A,'$',0x00,0x00,0x00
};

// ── Write helpers ─────────────────────────────────────────────
static void w8(int fd, u8 v)   { write(fd, &v, 1); }
static void w16(int fd, u16 v) { write(fd, &v, 2); }
static void w32(int fd, u32 v) { write(fd, &v, 4); }
static void w64(int fd, u64 v) { write(fd, &v, 8); }
static void wbuf(int fd, const void *p, usize n) { write(fd, p, n); }
static void wpad(int fd, usize n) {
    u8 z = 0;
    for (usize i = 0; i < n; i++) write(fd, &z, 1);
}
static usize wpos(int fd) { return (usize)lseek(fd, 0, SEEK_CUR); }
static void walign(int fd, usize align) {
    usize cur = wpos(fd);
    usize rem = cur % align;
    if (rem) wpad(fd, align - rem);
}

// ── Context lifecycle ─────────────────────────────────────────
PE_Context *pe_context_new(Arena *arena, Target target) {
    PE_Context *ctx = ARENA_ALLOC_ZERO(arena, PE_Context);
    ctx->arena  = arena;
    ctx->target = target;
    ctx->section_alignment = 0x1000;
    ctx->file_alignment    = 0x200;
    ctx->image_base = (target.fmt == OUTPUT_DLL)
                    ? 0x180000000ULL
                    : 0x140000000ULL;
    // Pre-allocate buffers
    ctx->text_cap  = 1 << 20; // 1 MB
    ctx->rdata_cap = 1 << 18; // 256 KB
    ctx->text_buf  = arena_alloc(arena, ctx->text_cap, 16);
    ctx->rdata_buf = arena_alloc(arena, ctx->rdata_cap, 16);
    return ctx;
}

// ── Import management ─────────────────────────────────────────
u32 pe_declare_import(PE_Context *ctx, StrView dll_name, StrView func_name) {
    // Check if already declared
    for (u32 i = 0; i < ctx->import_count; i++) {
        if (sv_eq(ctx->imports[i].dll_name, dll_name) &&
            sv_eq(ctx->imports[i].func_name, func_name))
            return i;
    }
    if (ctx->import_count >= PE_MAX_IMPORTS) return 0;
    u32 idx = ctx->import_count++;
    ctx->imports[idx].dll_name  = dll_name;
    ctx->imports[idx].func_name = func_name;
    ctx->imports[idx].iat_offset = idx * 8; // 8 bytes per IAT slot
    return idx;
}

// ── Code emission ─────────────────────────────────────────────
u32 pe_emit_code(PE_Context *ctx, const u8 *bytes, usize len) {
    u32 rva_offset = (u32)ctx->text_size;
    if (ctx->text_size + len > ctx->text_cap) {
        usize new_cap = ctx->text_cap * 2;
        u8 *nb = arena_alloc(ctx->arena, new_cap, 16);
        memcpy(nb, ctx->text_buf, ctx->text_size);
        ctx->text_buf = nb;
        ctx->text_cap = new_cap;
    }
    memcpy(ctx->text_buf + ctx->text_size, bytes, len);
    ctx->text_size += len;
    return rva_offset; // caller adds .text RVA base
}

u32 pe_emit_rdata(PE_Context *ctx, const void *data, usize len, usize align) {
    usize pad = (align - (ctx->rdata_size % align)) % align;
    if (ctx->rdata_size + pad + len > ctx->rdata_cap) {
        usize new_cap = ctx->rdata_cap * 2;
        u8 *nb = arena_alloc(ctx->arena, new_cap, 16);
        memcpy(nb, ctx->rdata_buf, ctx->rdata_size);
        ctx->rdata_buf = nb;
        ctx->rdata_cap = new_cap;
    }
    u32 off = (u32)(ctx->rdata_size + pad);
    memset(ctx->rdata_buf + ctx->rdata_size, 0, pad);
    memcpy(ctx->rdata_buf + off, data, len);
    ctx->rdata_size = off + len;
    return off;
}

void pe_add_export(PE_Context *ctx, StrView name, u32 code_rva) {
    if (ctx->export_count >= PE_MAX_EXPORTS) return;
    u32 idx = ctx->export_count++;
    ctx->exports[idx]        = name;
    ctx->export_offsets[idx] = code_rva;
}

u32 pe_iat_rva(PE_Context *ctx, u32 idx) {
    return ctx->imports[idx].iat_offset;
}

// ── Windows ABI arg regs (rcx, rdx, r8, r9) ─────────────────
static const X64Reg WIN_INT_ARG_REGS[4] = {
    X64_RCX, X64_RDX, X64_R8, X64_R9
};
#define WIN_ARG_REG_COUNT 4
#define WIN_SHADOW_SPACE  32  // 4*8 bytes mandatory shadow space

// ── IR → PE .text codegen ─────────────────────────────────────
// We reuse the same simple regalloc from the JIT, but emit into ctx->text_buf.

typedef struct {
    VRegState state;
    X64Reg    phys_reg;
    i32       stack_off;
    bool      is_float;
} PEVReg;

#define PE_MAX_VREGS 1024

typedef struct {
    PE_Context *pe;
    IRFunc     *fn;
    PEVReg      vregs[PE_MAX_VREGS];
    u32         vreg_count;
    i32         next_slot;
    // Code buffer
    CodeBuf     code_buf;
    FixupTable  fixups;
    LabelTable  labels;
    X64Asm      asm_;
    Arena       scratch;
} PECodeCtx;

static i32 pe_alloc_slot(PECodeCtx *ctx, u8 sz) {
    ctx->next_slot -= (i32)sz;
    ctx->next_slot = (i32)ALIGN_DOWN(ctx->next_slot, (usize)sz);
    return ctx->next_slot;
}

static u8 pe_ty_size(TypeKind k) {
    u8 s = (u8)ty_primitive_size(k);
    return s ? s : 8;
}

static X64Reg pe_val_to_reg(PECodeCtx *ctx, IRVal v, X64Reg scratch) {
    X64Asm *a = &ctx->asm_;
    switch (v.kind) {
        case IRVAL_TEMP: {
            PEVReg *info = &ctx->vregs[v.vreg];
            if (info->state == VREG_IN_REGISTER) return info->phys_reg;
            u8 sz = pe_ty_size(v.type);
            x64_mov_rm(a, scratch, X64_RBP, info->stack_off, sz);
            return scratch;
        }
        case IRVAL_CONST_I:
            x64_mov_ri(a, scratch, v.ival, 8);
            return scratch;
        case IRVAL_GLOBAL:
            // String literal address
            x64_mov_ri(a, scratch, (i64)(uintptr_t)v.name.ptr, 8);
            return scratch;
        default:
            return scratch;
    }
}

static void pe_store_vreg(PECodeCtx *ctx, u32 vr, X64Reg src) {
    PEVReg *info = &ctx->vregs[vr];
    if (info->state == VREG_SPILLED)
        x64_mov_mr(&ctx->asm_, X64_RBP, info->stack_off, src, 8);
}

// Allocate regs: callee-saved first, then caller-saved, excluding R10/R11 scratch
static const X64Reg PE_ALLOC_ORDER[] = {
    X64_RBX, X64_R12, X64_R13, X64_R14, X64_R15,
    X64_RAX, X64_RCX, X64_RDX, X64_RSI, X64_RDI,
    X64_R8,  X64_R9,
};
#define PE_ALLOC_COUNT 12

static const X64Reg PE_CALLEE_SAVED[] = { X64_RBX, X64_R12, X64_R13, X64_R14, X64_R15 };
#define PE_CALLEE_COUNT 5

static void pe_regalloc(PECodeCtx *ctx) {
    u32 int_free = 0;
    for (int i = 0; i < PE_ALLOC_COUNT; i++)
        int_free |= (1u << PE_ALLOC_ORDER[i]);
    int_free &= ~((1u << X64_RSP) | (1u << X64_RBP));

    for (u32 vr = 0; vr < ctx->vreg_count; vr++) {
        PEVReg *info = &ctx->vregs[vr];
        if (info->state != VREG_UNALLOCATED) continue;
        if (!info->is_float && int_free) {
            int bit = __builtin_ctz(int_free);
            info->phys_reg = (X64Reg)bit;
            info->state    = VREG_IN_REGISTER;
            int_free      &= ~(1u << bit);
        } else {
            info->state     = VREG_SPILLED;
            info->stack_off = pe_alloc_slot(ctx, 8);
        }
    }
}

static void pe_liveness(PECodeCtx *ctx) {
    IRFunc *fn = ctx->fn;
    for (u32 vr = 0; vr < ctx->vreg_count; vr++)
        ctx->vregs[vr].state = VREG_UNALLOCATED;
    for (u32 bi = 0; bi < fn->block_count; bi++) {
        IRBlock *b = fn->blocks[bi];
        for (u32 ii = 0; ii < b->instr_count; ii++) {
            IRInstr *ins = &b->instrs[ii];
            if (ins->dst.kind == IRVAL_TEMP && ins->dst.vreg < PE_MAX_VREGS) {
                u32 vr = ins->dst.vreg;
                ctx->vregs[vr].state    = VREG_UNALLOCATED;
                ctx->vregs[vr].is_float = (ins->result_type == TY_F32 || ins->result_type == TY_F64);
            }
        }
    }
}

static void pe_emit_func(PECodeCtx *ctx, IRFunc *fn, PE_Context *pe);

static void pe_emit_instr(PECodeCtx *ctx, IRInstr *ins) {
    X64Asm *a = &ctx->asm_;
    PE_Context *pe = ctx->pe;

    switch (ins->op) {
        case IOP_NOP: x64_nop(a); break;

        case IOP_LABEL:
            x64_def_label(a, ins->src1.label_id);
            break;
        case IOP_JMP:
            x64_jmp(a, ins->src1.label_id);
            break;
        case IOP_JZ: {
            X64Reg cr = pe_val_to_reg(ctx, ins->src1, X64_R10);
            x64_test_rr(a, cr, cr, 4);
            x64_je(a, ins->src2.label_id);
            break;
        }
        case IOP_JNZ: {
            X64Reg cr = pe_val_to_reg(ctx, ins->src1, X64_R10);
            x64_test_rr(a, cr, cr, 4);
            x64_jne(a, ins->src2.label_id);
            break;
        }

        case IOP_MOV: {
            if (ins->dst.kind != IRVAL_TEMP) break;
            u8 sz = pe_ty_size(ins->result_type);
            X64Reg src = pe_val_to_reg(ctx, ins->src1, X64_R10);
            PEVReg *di = &ctx->vregs[ins->dst.vreg];
            X64Reg dst = (di->state == VREG_IN_REGISTER) ? di->phys_reg : X64_R10;
            if (dst != src) x64_mov_rr(a, dst, src, sz);
            pe_store_vreg(ctx, ins->dst.vreg, dst);
            break;
        }

        case IOP_ADD: case IOP_SUB: {
            if (ins->dst.kind != IRVAL_TEMP) break;
            u8 sz = pe_ty_size(ins->result_type);
            X64Reg lhs = pe_val_to_reg(ctx, ins->src1, X64_R10);
            if (lhs != X64_R10) { x64_mov_rr(a, X64_R10, lhs, sz); lhs = X64_R10; }
            X64Reg rhs = pe_val_to_reg(ctx, ins->src2, X64_R11);
            PEVReg *di = &ctx->vregs[ins->dst.vreg];
            X64Reg dst = (di->state == VREG_IN_REGISTER) ? di->phys_reg : X64_RAX;
            if (dst != lhs) x64_mov_rr(a, dst, lhs, sz);
            if (ins->op == IOP_ADD) x64_add_rr(a, dst, rhs, sz);
            else                    x64_sub_rr(a, dst, rhs, sz);
            pe_store_vreg(ctx, ins->dst.vreg, dst);
            break;
        }

        case IOP_MUL: {
            u8 sz = pe_ty_size(ins->result_type);
            X64Reg lhs = pe_val_to_reg(ctx, ins->src1, X64_R10);
            if (lhs != X64_R10) { x64_mov_rr(a, X64_R10, lhs, sz); lhs = X64_R10; }
            X64Reg rhs = pe_val_to_reg(ctx, ins->src2, X64_R11);
            PEVReg *di = &ctx->vregs[ins->dst.vreg];
            X64Reg dst = (di->state == VREG_IN_REGISTER) ? di->phys_reg : X64_RAX;
            if (dst != lhs) x64_mov_rr(a, dst, lhs, sz);
            x64_imul_rr(a, dst, rhs, sz);
            pe_store_vreg(ctx, ins->dst.vreg, dst);
            break;
        }

        case IOP_DIV: {
            u8 sz = pe_ty_size(ins->result_type);
            X64Reg rhs = pe_val_to_reg(ctx, ins->src2, X64_RCX);
            X64Reg lhs = pe_val_to_reg(ctx, ins->src1, X64_RAX);
            if (lhs != X64_RAX) x64_mov_rr(a, X64_RAX, lhs, sz);
            if (sz == 8) x64_cqo(a); else x64_cdq(a);
            x64_idiv(a, rhs, sz);
            PEVReg *di = &ctx->vregs[ins->dst.vreg];
            if (di->state == VREG_IN_REGISTER && di->phys_reg != X64_RAX)
                x64_mov_rr(a, di->phys_reg, X64_RAX, sz);
            else if (di->state == VREG_SPILLED)
                x64_mov_mr(a, X64_RBP, di->stack_off, X64_RAX, sz);
            break;
        }

        case IOP_MOD: {
            u8 sz = pe_ty_size(ins->result_type);
            X64Reg rhs = pe_val_to_reg(ctx, ins->src2, X64_RCX);
            X64Reg lhs = pe_val_to_reg(ctx, ins->src1, X64_RAX);
            if (lhs != X64_RAX) x64_mov_rr(a, X64_RAX, lhs, sz);
            if (sz == 8) x64_cqo(a); else x64_cdq(a);
            x64_idiv(a, rhs, sz);
            PEVReg *di = &ctx->vregs[ins->dst.vreg];
            if (di->state == VREG_IN_REGISTER)
                x64_mov_rr(a, di->phys_reg, X64_RDX, sz);
            else
                x64_mov_mr(a, X64_RBP, di->stack_off, X64_RDX, sz);
            break;
        }

        case IOP_AND: case IOP_OR: case IOP_XOR: {
            u8 sz = pe_ty_size(ins->result_type);
            X64Reg lhs = pe_val_to_reg(ctx, ins->src1, X64_R10);
            if (lhs != X64_R10) { x64_mov_rr(a, X64_R10, lhs, sz); lhs = X64_R10; }
            X64Reg rhs = pe_val_to_reg(ctx, ins->src2, X64_R11);
            PEVReg *di = &ctx->vregs[ins->dst.vreg];
            X64Reg dst = (di->state == VREG_IN_REGISTER) ? di->phys_reg : X64_RAX;
            if (dst != lhs) x64_mov_rr(a, dst, lhs, sz);
            if (ins->op == IOP_AND) x64_and_rr(a, dst, rhs, sz);
            else if (ins->op == IOP_OR)  x64_or_rr(a, dst, rhs, sz);
            else                         x64_xor_rr(a, dst, rhs, sz);
            pe_store_vreg(ctx, ins->dst.vreg, dst);
            break;
        }

        case IOP_NEG: {
            u8 sz = pe_ty_size(ins->result_type);
            X64Reg src = pe_val_to_reg(ctx, ins->src1, X64_R10);
            PEVReg *di = &ctx->vregs[ins->dst.vreg];
            X64Reg dst = (di->state == VREG_IN_REGISTER) ? di->phys_reg : X64_RAX;
            if (dst != src) x64_mov_rr(a, dst, src, sz);
            x64_neg(a, dst, sz);
            pe_store_vreg(ctx, ins->dst.vreg, dst);
            break;
        }

        case IOP_CMP_EQ: case IOP_CMP_NE:
        case IOP_CMP_LT: case IOP_CMP_LE:
        case IOP_CMP_GT: case IOP_CMP_GE: {
            u8 sz = pe_ty_size(ins->src1.type);
            X64Reg lhs = pe_val_to_reg(ctx, ins->src1, X64_R10);
            if (lhs != X64_R10) { x64_mov_rr(a, X64_R10, lhs, sz); lhs = X64_R10; }
            X64Reg rhs = pe_val_to_reg(ctx, ins->src2, X64_R11);
            x64_cmp_rr(a, lhs, rhs, sz);
            x64_xor_rr(a, X64_RAX, X64_RAX, 4);
            switch (ins->op) {
                case IOP_CMP_EQ: x64_sete(a, X64_RAX);  break;
                case IOP_CMP_NE: x64_setne(a, X64_RAX); break;
                case IOP_CMP_LT: x64_setl(a, X64_RAX);  break;
                case IOP_CMP_LE: x64_setle(a, X64_RAX); break;
                case IOP_CMP_GT: x64_setg(a, X64_RAX);  break;
                case IOP_CMP_GE: x64_setge(a, X64_RAX); break;
                default: break;
            }
            PEVReg *di = &ctx->vregs[ins->dst.vreg];
            if (di->state == VREG_IN_REGISTER && di->phys_reg != X64_RAX)
                x64_mov_rr(a, di->phys_reg, X64_RAX, 1);
            else if (di->state == VREG_SPILLED)
                x64_mov_mr(a, X64_RBP, di->stack_off, X64_RAX, 1);
            break;
        }

        case IOP_ALLOCA: {
            i32 data_slot = pe_alloc_slot(ctx, (u8)ins->src1.ival);
            PEVReg *di = &ctx->vregs[ins->dst.vreg];
            di->state     = VREG_SPILLED;
            di->stack_off = pe_alloc_slot(ctx, 8);
            x64_lea(a, X64_R11, X64_RBP, data_slot);
            x64_mov_mr(a, X64_RBP, di->stack_off, X64_R11, 8);
            break;
        }

        case IOP_LOAD: {
            u8 sz = pe_ty_size(ins->result_type);
            X64Reg base = pe_val_to_reg(ctx, ins->src1, X64_RCX);
            i32 off = (ins->src2.kind == IRVAL_CONST_I) ? (i32)ins->src2.ival : 0;
            PEVReg *di = &ctx->vregs[ins->dst.vreg];
            X64Reg dst = (di->state == VREG_IN_REGISTER) ? di->phys_reg : X64_RAX;
            x64_mov_rm(a, dst, base, off, sz);
            pe_store_vreg(ctx, ins->dst.vreg, dst);
            break;
        }

        case IOP_STORE: {
            u8 sz = pe_ty_size(ins->src2.type);
            X64Reg base = pe_val_to_reg(ctx, ins->dst, X64_RCX);
            i32 off = (ins->src1.kind == IRVAL_CONST_I) ? (i32)ins->src1.ival : 0;
            X64Reg val = pe_val_to_reg(ctx, ins->src2, X64_RAX);
            x64_mov_mr(a, base, off, val, sz);
            break;
        }

        case IOP_CALL: {
            // Windows ABI: args in rcx, rdx, r8, r9; 32-byte shadow space
            u32 n = ins->arg_count;
            // Load args through R10 to avoid clobbering
            for (u32 i = 0; i < n && i < WIN_ARG_REG_COUNT; i++) {
                X64Reg arg_reg = WIN_INT_ARG_REGS[i];
                X64Reg src = pe_val_to_reg(ctx, ins->args[i], X64_R10);
                if (src != arg_reg) x64_mov_rr(a, arg_reg, src, 8);
            }
            // Stack args (beyond 4)
            for (i32 i = (i32)n - 1; i >= WIN_ARG_REG_COUNT; i--) {
                X64Reg src = pe_val_to_reg(ctx, ins->args[i], X64_R10);
                if (src != X64_RAX) x64_mov_rr(a, X64_RAX, src, 8);
                x64_push(a, X64_RAX);
            }

            if (ins->src1.kind == IRVAL_FUNC) {
                // Find import entry → call through IAT
                StrView fname = ins->src1.name;
                i32 imp_idx = -1;
                for (u32 i = 0; i < pe->import_count; i++) {
                    if (sv_eq(pe->imports[i].func_name, fname)) {
                        imp_idx = (i32)i; break;
                    }
                }
                if (imp_idx >= 0) {
                    // call [IAT_slot]  — indirect call through import
                    // We'll encode as: mov rax, IAT_addr; call [rax]
                    // At link time the IAT is filled by the loader
                    // For now emit as CALL rel32 placeholder (patched at write time)
                    codebuf_emit_byte(a->cb, 0xFF);  // CALL r/m64
                    codebuf_emit_byte(a->cb, 0x15);  // [rip+rel32]
                    codebuf_emit_i32(a->cb, 0);       // placeholder for IAT RVA
                    // Record fixup: (offset, imp_idx)
                    // TODO: add reloc fixup table
                } else {
                    // Internal O function: direct call
                    codebuf_emit_byte(a->cb, 0xE8);  // CALL rel32
                    codebuf_emit_i32(a->cb, 0);       // placeholder
                }
            } else {
                X64Reg fn_reg = pe_val_to_reg(ctx, ins->src1, X64_RAX);
                x64_call_reg(a, fn_reg);
            }
            // Cleanup stack args
            if (n > WIN_ARG_REG_COUNT) {
                u32 stack_args = n - WIN_ARG_REG_COUNT;
                x64_add_ri(a, X64_RSP, (i32)(stack_args * 8), 8);
            }
            // Capture return value
            if (ins->dst.kind == IRVAL_TEMP) {
                PEVReg *di = &ctx->vregs[ins->dst.vreg];
                if (di->state == VREG_IN_REGISTER && di->phys_reg != X64_RAX)
                    x64_mov_rr(a, di->phys_reg, X64_RAX, 8);
                else if (di->state == VREG_SPILLED)
                    x64_mov_mr(a, X64_RBP, di->stack_off, X64_RAX, 8);
            }
            break;
        }

        case IOP_RET_VOID:
        case IOP_RET: {
            if (ins->op == IOP_RET) {
                u8 sz = pe_ty_size(ins->src1.type);
                X64Reg src = pe_val_to_reg(ctx, ins->src1, X64_RAX);
                if (src != X64_RAX) x64_mov_rr(a, X64_RAX, src, sz);
            }
            // Restore callee-saved in reverse
            for (i32 ci = PE_CALLEE_COUNT - 1; ci >= 0; ci--) {
                bool used = false;
                for (u32 vr = 0; vr < ctx->vreg_count; vr++) {
                    if (ctx->vregs[vr].state == VREG_IN_REGISTER &&
                        ctx->vregs[vr].phys_reg == PE_CALLEE_SAVED[ci]) {
                        used = true; break;
                    }
                }
                if (used) x64_pop(a, PE_CALLEE_SAVED[ci]);
            }
            x64_emit_epilogue(a);
            break;
        }

        default:
            x64_nop(a);
            break;
    }
}

PEResult pe_compile_module(PE_Context *ctx, IRModule *module) {
    for (u32 fi = 0; fi < module->func_count; fi++) {
        IRFunc *fn = module->funcs[fi];

        // Setup per-function context
        PECodeCtx fctx = {0};
        fctx.pe         = ctx;
        fctx.fn         = fn;
        fctx.vreg_count = fn->next_vreg;
        fctx.next_slot  = 0;
        arena_init(&fctx.scratch, MB(2));

        // Liveness + regalloc
        pe_liveness(&fctx);
        pe_regalloc(&fctx);

        // Init code buffer (arena-backed, NOT executable)
        codebuf_init_arena(&fctx.code_buf, &fctx.scratch, MB(1));
        FixupTable ft; fixup_table_init(&ft, &fctx.scratch);
        LabelTable lt; label_table_init(&lt, &fctx.scratch);
        x64asm_init(&fctx.asm_, &fctx.code_buf, &ft, &lt);

        // Callee-saved regs used
        u32 callee_mask = 0;
        for (u32 vr = 0; vr < fctx.vreg_count; vr++) {
            PEVReg *vi = &fctx.vregs[vr];
            if (vi->state != VREG_IN_REGISTER) continue;
            for (u32 ci = 0; ci < PE_CALLEE_COUNT; ci++)
                if (vi->phys_reg == PE_CALLEE_SAVED[ci]) callee_mask |= (1u << ci);
        }

        // Prologue
        x64_push(&fctx.asm_, X64_RBP);
        x64_mov_rr(&fctx.asm_, X64_RBP, X64_RSP, 8);
        // Push callee-saved
        for (u32 ci = 0; ci < PE_CALLEE_COUNT; ci++)
            if (callee_mask & (1u << ci)) x64_push(&fctx.asm_, PE_CALLEE_SAVED[ci]);
        // Shadow space + frame
        i32 frame_size = -fctx.next_slot;
        frame_size = (i32)ALIGN_UP((u32)frame_size + WIN_SHADOW_SPACE, 16);
        if ((frame_size & 8) == 0) frame_size += 8;

        // sub rsp, frame
        codebuf_emit_byte(&fctx.code_buf, REX_W);
        codebuf_emit_byte(&fctx.code_buf, 0x81);
        codebuf_emit_byte(&fctx.code_buf, x64_modrm(3, 5, X64_RSP & 7));
        codebuf_emit_i32(&fctx.code_buf, frame_size);

        // Move params from Windows ABI regs to allocated vregs
        for (u32 i = 0; i < fn->param_count && i < WIN_ARG_REG_COUNT; i++) {
            PEVReg *info = &fctx.vregs[i];
            X64Reg arg_reg = WIN_INT_ARG_REGS[i];
            if (info->state == VREG_IN_REGISTER && info->phys_reg != arg_reg)
                x64_mov_rr(&fctx.asm_, info->phys_reg, arg_reg, 8);
            else if (info->state == VREG_SPILLED)
                x64_mov_mr(&fctx.asm_, X64_RBP, info->stack_off, arg_reg, 8);
        }

        // Emit function body
        for (u32 bi = 0; bi < fn->block_count; bi++) {
            IRBlock *b = fn->blocks[bi];
            for (u32 ii = 0; ii < b->instr_count; ii++)
                pe_emit_instr(&fctx, &b->instrs[ii]);
        }

        // Resolve label fixups
        fixup_resolve_all(&ft, &lt, &fctx.code_buf);

        // Append to PE .text section
        u32 func_rva = pe_emit_code(ctx, fctx.code_buf.buf, fctx.code_buf.len);
        UNUSED(func_rva);

        // Register export if DLL
        if (ctx->target.fmt == OUTPUT_DLL) {
            // Add all functions as exports
            u32 text_section_rva = ctx->section_alignment; // .text starts at VA 0x1000
            pe_add_export(ctx, fn->name, text_section_rva + func_rva);
        }

        arena_destroy(&fctx.scratch);
    }
    return (PEResult){0};
}

// ── PE file layout and write ──────────────────────────────────
PEResult pe_write(PE_Context *ctx, const char *path) {
    const u32 FA  = ctx->file_alignment;
    const u32 SA  = ctx->section_alignment;

    // Section layout (file offsets and virtual addresses)
    u32 hdr_size = (u32)ALIGN_UP(
        sizeof(PE_DosHeader) + sizeof(DOS_STUB) + 4 +
        sizeof(PE_FileHeader) + sizeof(PE_OptionalHeader64) +
        6 * sizeof(PE_SectionHeader),
        FA);

    // .text
    u32 text_vaddr = SA;
    u32 text_raw   = hdr_size;
    u32 text_fsz   = (u32)ALIGN_UP(ctx->text_size, FA);
    u32 text_vsz   = (u32)ALIGN_UP(ctx->text_size, SA);

    // .rdata (imports, strings)
    u32 rdata_vaddr = text_vaddr + text_vsz;
    u32 rdata_raw   = text_raw + text_fsz;

    // Build import tables into a temporary buffer
    // Layout: IAT | ILT | ImportDir | DLL names | function names
    u8  *imp_buf  = arena_alloc(ctx->arena, 65536, 16);
    usize imp_pos = 0;

    // Count unique DLLs
    char dll_names[16][64]; u32 dll_count = 0;
    for (u32 i = 0; i < ctx->import_count; i++) {
        bool found = false;
        for (u32 d = 0; d < dll_count; d++) {
            if (strcmp(dll_names[d], ctx->imports[i].dll_name.ptr) == 0)
                { found = true; break; }
        }
        if (!found && dll_count < 16) {
            usize nl = MIN(ctx->imports[i].dll_name.len, 63);
            memcpy(dll_names[dll_count], ctx->imports[i].dll_name.ptr, nl);
            dll_names[dll_count][nl] = '\0';
            dll_count++;
        }
    }

    // IAT: 8 bytes per import + 8 null terminator per DLL
    u32 iat_rva = rdata_vaddr; // IAT is at start of .rdata
    for (u32 i = 0; i < ctx->import_count; i++) {
        // Slot filled by Windows loader; we write the hint+name RVA initially
        *(u64*)(imp_buf + imp_pos) = 0; // filled by loader
        imp_pos += 8;
    }
    *(u64*)(imp_buf + imp_pos) = 0; imp_pos += 8; // null terminator

    u32 imp_data_size = (u32)imp_pos;
    u32 rdata_fsz  = (u32)ALIGN_UP(ctx->rdata_size + imp_data_size + 4096, FA);
    u32 rdata_vsz  = (u32)ALIGN_UP(ctx->rdata_size + imp_data_size + 4096, SA);

    // .bss / .data (empty for now)
    u32 data_vaddr = rdata_vaddr + rdata_vsz;
    u32 data_raw   = rdata_raw + rdata_fsz;
    u32 data_fsz   = FA;
    u32 data_vsz   = SA;

    u32 total_image = data_vaddr + data_vsz;

    // ── Open output file ──────────────────────────────────────
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) return (PEResult){ .had_error = true, .msg = "cannot open output file" };

    // ── DOS header ────────────────────────────────────────────
    PE_DosHeader dos = {0};
    dos.e_magic   = PE_MZ_MAGIC;
    dos.e_cblp    = 0x90;
    dos.e_cp      = 0x03;
    dos.e_cparhdr = 0x04;
    dos.e_maxalloc = 0xFFFF;
    dos.e_sp      = 0xB8;
    dos.e_lfarlc  = 0x40;
    dos.e_lfanew  = (u32)(sizeof(PE_DosHeader) + sizeof(DOS_STUB));
    wbuf(fd, &dos, sizeof(dos));
    wbuf(fd, DOS_STUB, sizeof(DOS_STUB));

    // ── PE signature ──────────────────────────────────────────
    u32 pe_sig = PE_SIGNATURE;
    wbuf(fd, &pe_sig, 4);

    // ── COFF header ───────────────────────────────────────────
    PE_FileHeader coff = {0};
    coff.Machine              = PE_MACHINE_AMD64;
    coff.NumberOfSections     = 3; // .text, .rdata, .data
    coff.TimeDateStamp        = (u32)time(NULL);
    coff.SizeOfOptionalHeader = sizeof(PE_OptionalHeader64);
    coff.Characteristics      = PE_FILE_EXECUTABLE | PE_FILE_LARGE_ADDRESS;
    if (ctx->target.fmt == OUTPUT_DLL) coff.Characteristics |= PE_FILE_DLL;
    wbuf(fd, &coff, sizeof(coff));

    // ── Optional header (PE32+) ───────────────────────────────
    PE_OptionalHeader64 opt = {0};
    opt.Magic                    = 0x020B;  // PE32+
    opt.MajorLinkerVersion       = 1;
    opt.SizeOfCode               = text_fsz;
    opt.SizeOfInitializedData    = rdata_fsz + data_fsz;
    opt.AddressOfEntryPoint      = text_vaddr; // first function in .text
    opt.BaseOfCode               = text_vaddr;
    opt.ImageBase                = ctx->image_base;
    opt.SectionAlignment         = SA;
    opt.FileAlignment            = FA;
    opt.MajorOSVersion           = 6;
    opt.MinorOSVersion           = 0;
    opt.MajorSubsystemVersion    = 6;
    opt.MinorSubsystemVersion    = 0;
    opt.SizeOfImage              = total_image;
    opt.SizeOfHeaders            = hdr_size;
    opt.Subsystem                = PE_SUBSYSTEM_CONSOLE;
    opt.DllCharacteristics       = PE_DLLCHAR_DYNAMIC_BASE | PE_DLLCHAR_NX_COMPAT;
    opt.SizeOfStackReserve       = 0x100000;
    opt.SizeOfStackCommit        = 0x1000;
    opt.SizeOfHeapReserve        = 0x100000;
    opt.SizeOfHeapCommit         = 0x1000;
    opt.NumberOfRvaAndSizes      = PE_NUM_DIRS;
    // IAT directory
    if (ctx->import_count > 0) {
        opt.DataDirectory[PE_DIR_IAT].VirtualAddress = iat_rva;
        opt.DataDirectory[PE_DIR_IAT].Size           = ctx->import_count * 8 + 8;
    }
    wbuf(fd, &opt, sizeof(opt));

    // ── Section headers ───────────────────────────────────────
    PE_SectionHeader sh_text = {0};
    memcpy(sh_text.Name, ".text\0\0\0", 8);
    sh_text.VirtualSize         = (u32)ctx->text_size;
    sh_text.VirtualAddress      = text_vaddr;
    sh_text.SizeOfRawData       = text_fsz;
    sh_text.PointerToRawData    = text_raw;
    sh_text.Characteristics     = PE_SCN_CNT_CODE | PE_SCN_MEM_EXECUTE | PE_SCN_MEM_READ;
    wbuf(fd, &sh_text, sizeof(sh_text));

    PE_SectionHeader sh_rdata = {0};
    memcpy(sh_rdata.Name, ".rdata\0\0", 8);
    sh_rdata.VirtualSize        = (u32)(ctx->rdata_size + imp_data_size);
    sh_rdata.VirtualAddress     = rdata_vaddr;
    sh_rdata.SizeOfRawData      = rdata_fsz;
    sh_rdata.PointerToRawData   = rdata_raw;
    sh_rdata.Characteristics    = PE_SCN_CNT_INIT_DATA | PE_SCN_MEM_READ;
    wbuf(fd, &sh_rdata, sizeof(sh_rdata));

    PE_SectionHeader sh_data = {0};
    memcpy(sh_data.Name, ".data\0\0\0", 8);
    sh_data.VirtualSize         = SA;
    sh_data.VirtualAddress      = data_vaddr;
    sh_data.SizeOfRawData       = data_fsz;
    sh_data.PointerToRawData    = data_raw;
    sh_data.Characteristics     = PE_SCN_CNT_INIT_DATA | PE_SCN_MEM_READ | PE_SCN_MEM_WRITE;
    wbuf(fd, &sh_data, sizeof(sh_data));

    // Pad to file_alignment
    walign(fd, FA);

    // ── .text section data ────────────────────────────────────
    wbuf(fd, ctx->text_buf, ctx->text_size);
    walign(fd, FA);

    // ── .rdata section data ───────────────────────────────────
    if (ctx->rdata_size) wbuf(fd, ctx->rdata_buf, ctx->rdata_size);
    if (imp_data_size)   wbuf(fd, imp_buf, imp_data_size);
    walign(fd, FA);

    // ── .data section (zeroed) ────────────────────────────────
    wpad(fd, FA);

    close(fd);
    return (PEResult){0};
}