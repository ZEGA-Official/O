// ============================================================
//  O Language Compiler — o_x64.c
//  x86-64 machine code encoder — REX / ModRM / SIB
//  System V AMD64 ABI
//  Z-TEAM | C23
// ============================================================
#include "backend/x64.h"
#ifndef _WIN32
#include <sys/mman.h>
#endif

// ABI register lists
const X64Reg    sysv_int_arg_regs[SYSV_ARG_REGS_INT] =
    { X64_RDI, X64_RSI, X64_RDX, X64_RCX, X64_R8, X64_R9 };
const X64XmmReg sysv_flt_arg_regs[SYSV_ARG_REGS_FLT] =
    { X64_XMM0, X64_XMM1, X64_XMM2, X64_XMM3,
      X64_XMM4, X64_XMM5, X64_XMM6, X64_XMM7 };

// ── Code buffer ──────────────────────────────────────────────

void codebuf_init_arena(CodeBuf *cb, Arena *arena, usize initial_cap) {
    *cb = (CodeBuf){
        .buf   = arena_alloc_aligned(arena, initial_cap, 16),
        .len   = 0,
        .cap   = initial_cap,
        .is_exec = false,
        .arena = arena,
    };
}

bool codebuf_init_exec(CodeBuf *cb, usize initial_cap) {
    usize cap = ALIGN_UP(initial_cap, 4096);
    void *mem = mmap(NULL, cap,
                     PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (UNLIKELY(mem == MAP_FAILED)) return false;
    *cb = (CodeBuf){
        .buf     = (u8 *)mem,
        .len     = 0,
        .cap     = cap,
        .is_exec = true,
        .arena   = NULL,
    };
    return true;
}

void codebuf_destroy(CodeBuf *cb) {
    if (cb->is_exec && cb->buf)
        munmap(cb->buf, cb->cap);
    *cb = (CodeBuf){0};
}

void codebuf_ensure(CodeBuf *cb, usize extra) {
    if (LIKELY(cb->len + extra <= cb->cap)) return;
    usize new_cap = MAX(cb->cap * 2, cb->len + extra + 64);
    if (cb->is_exec) {
        void *new_mem = mmap(NULL, new_cap,
                             PROT_READ | PROT_WRITE | PROT_EXEC,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (UNLIKELY(new_mem == MAP_FAILED)) O_PANIC("codebuf exec remap failed");
        memcpy(new_mem, cb->buf, cb->len);
        munmap(cb->buf, cb->cap);
        cb->buf = new_mem;
        cb->cap = new_cap;
    } else {
        O_ASSERT(cb->arena, "arena-backed codebuf has no arena");
        u8 *new_buf = arena_alloc_aligned(cb->arena, new_cap, 16);
        memcpy(new_buf, cb->buf, cb->len);
        cb->buf = new_buf;
        cb->cap = new_cap;
    }
}

HOT void codebuf_emit_byte(CodeBuf *cb, u8 b) {
    codebuf_ensure(cb, 1);
    cb->buf[cb->len++] = b;
}
HOT void codebuf_emit_u16(CodeBuf *cb, u16 v) {
    codebuf_ensure(cb, 2);
    cb->buf[cb->len+0] = (u8)(v);
    cb->buf[cb->len+1] = (u8)(v >> 8);
    cb->len += 2;
}
HOT void codebuf_emit_u32(CodeBuf *cb, u32 v) {
    codebuf_ensure(cb, 4);
    cb->buf[cb->len+0] = (u8)(v);
    cb->buf[cb->len+1] = (u8)(v >> 8);
    cb->buf[cb->len+2] = (u8)(v >> 16);
    cb->buf[cb->len+3] = (u8)(v >> 24);
    cb->len += 4;
}
HOT void codebuf_emit_u64(CodeBuf *cb, u64 v) {
    codebuf_ensure(cb, 8);
    for (int i = 0; i < 8; i++)
        cb->buf[cb->len+i] = (u8)(v >> (i*8));
    cb->len += 8;
}
HOT void codebuf_emit_i32(CodeBuf *cb, i32 v) { codebuf_emit_u32(cb, (u32)v); }
HOT void codebuf_emit_i64(CodeBuf *cb, i64 v) { codebuf_emit_u64(cb, (u64)v); }
void codebuf_emit_bytes(CodeBuf *cb, const u8 *data, usize len) {
    codebuf_ensure(cb, len);
    memcpy(cb->buf + cb->len, data, len);
    cb->len += len;
}
void codebuf_patch_i32(CodeBuf *cb, usize off, i32 v) {
    cb->buf[off+0] = (u8)v;
    cb->buf[off+1] = (u8)(v >> 8);
    cb->buf[off+2] = (u8)(v >> 16);
    cb->buf[off+3] = (u8)(v >> 24);
}
void codebuf_patch_u32(CodeBuf *cb, usize off, u32 v) {
    codebuf_patch_i32(cb, off, (i32)v);
}

// ── Fixup / Label tables ──────────────────────────────────────

void fixup_table_init(FixupTable *ft, Arena *a) {
    ft->table = NULL; ft->count = ft->cap = 0; ft->arena = a;
}
void fixup_add(FixupTable *ft, usize off, u32 label_id) {
    if (ft->count == ft->cap) {
        u32 nc = ft->cap ? ft->cap * 2 : 64;
        Fixup *nb = arena_alloc_aligned(ft->arena, nc * sizeof(Fixup), _Alignof(Fixup));
        if (ft->table) memcpy(nb, ft->table, ft->count * sizeof(Fixup));
        ft->table = nb; ft->cap = nc;
    }
    ft->table[ft->count++] = (Fixup){.offset=off, .label_id=label_id, .is_rel32=true};
}
void fixup_resolve_all(FixupTable *ft, LabelTable *lt, CodeBuf *cb) {
    for (u32 i = 0; i < ft->count; i++) {
        Fixup *f = &ft->table[i];
        O_ASSERT(f->label_id < lt->count, "label out of range");
        usize target = lt->offsets[f->label_id];
        i32 rel = (i32)((isize)target - (isize)(f->offset + 4));
        codebuf_patch_i32(cb, f->offset, rel);
    }
}

void label_table_init(LabelTable *lt, Arena *a) {
    u32 init_cap = 64;
    lt->offsets = arena_alloc_aligned(a, init_cap * sizeof(usize), _Alignof(usize));
    lt->count = 0; lt->cap = init_cap;
    (void)a;
}
void label_define(LabelTable *lt, u32 label_id, usize offset) {
    if (label_id >= lt->cap) {
        O_PANIC("label table overflow — increase initial cap");
    }
    while (lt->count <= label_id) {
        lt->offsets[lt->count++] = (usize)-1; // undefined marker
    }
    lt->offsets[label_id] = offset;
}

// ── X64Asm init ───────────────────────────────────────────────

void x64asm_init(X64Asm *a, CodeBuf *cb, FixupTable *ft, LabelTable *lt) {
    a->cb = cb; a->ft = ft; a->lt = lt;
}

// ── Encoding helpers ──────────────────────────────────────────
// REX prefix: 0100 WRXB
// W=1 for 64-bit operand
// R extends ModRM.reg field
// X extends SIB.index
// B extends ModRM.r/m or SIB.base or opcode reg
// All REX macros are defined in x64.h; do not redefine them here.

// Emit REX if needed (never for 32/8-bit unless extended regs involved)
static void emit_rex(CodeBuf *cb, bool W, bool R, bool X, bool B) {
    u8 rex = 0x40 | (W ? 8 : 0) | (R ? 4 : 0) | (X ? 2 : 0) | (B ? 1 : 0);
    if (rex != 0x40 || W)  // only emit if needed
        codebuf_emit_byte(cb, rex);
}

// Force REX for 8-bit regs to use SIL/DIL etc. instead of AH/BH
static void emit_rex_forced(CodeBuf *cb, bool W, bool R, bool X, bool B) {
    u8 rex = 0x40 | (W ? 8 : 0) | (R ? 4 : 0) | (X ? 2 : 0) | (B ? 1 : 0);
    codebuf_emit_byte(cb, rex);
}

// Encode ModRM: mod(2) | reg(3) | r/m(3)
static inline u8 modrm(u8 mod, u8 reg, u8 rm) {
    return (u8)((mod << 6) | ((reg & 7) << 3) | (rm & 7));
}
// SIB: scale(2) | index(3) | base(3)
static inline u8 sib_byte(u8 scale, u8 idx, u8 base) {
    return (u8)((scale << 6) | ((idx & 7) << 3) | (base & 7));
}

// Emit [base + disp] addressing
static void emit_mem_opnd(CodeBuf *cb, u8 reg_field, X64Reg base, i32 disp) {
    u8 base_low = base & 7;
    if (disp == 0 && base_low != 5 /*rbp/r13*/) {
        // mod=00
        codebuf_emit_byte(cb, modrm(0, reg_field, base_low));
        if (base_low == 4 /*rsp/r12*/)
            codebuf_emit_byte(cb, sib_byte(0, 4, 4)); // SIB: rsp base
    } else if (disp >= -128 && disp <= 127) {
        // mod=01: 8-bit displacement
        codebuf_emit_byte(cb, modrm(1, reg_field, base_low));
        if (base_low == 4) codebuf_emit_byte(cb, sib_byte(0, 4, 4));
        codebuf_emit_byte(cb, (u8)(i8)disp);
    } else {
        // mod=10: 32-bit displacement
        codebuf_emit_byte(cb, modrm(2, reg_field, base_low));
        if (base_low == 4) codebuf_emit_byte(cb, sib_byte(0, 4, 4));
        codebuf_emit_i32(cb, disp);
    }
}

// sz_prefix: emit operand-size prefix for 16-bit ops
static inline void maybe_size16(CodeBuf *cb, u8 sz) {
    if (sz == 2) codebuf_emit_byte(cb, 0x66);
}

// ── MOV ──────────────────────────────────────────────────────

void x64_mov_rr(X64Asm *a, X64Reg dst, X64Reg src, u8 sz) {
    CodeBuf *cb = a->cb;
    maybe_size16(cb, sz);
    if (sz == 8) emit_rex(cb, true, src >= 8, false, dst >= 8);
    else if (sz == 4) { if (src>=8||dst>=8) emit_rex(cb,false,src>=8,false,dst>=8); }
    else if (sz == 1) emit_rex_forced(cb, false, src>=8, false, dst>=8);
    u8 opc = (sz == 1) ? 0x88 : 0x89; // MOV r/m, r
    codebuf_emit_byte(cb, opc);
    codebuf_emit_byte(cb, modrm(3, src & 7, dst & 7));
}

void x64_mov_ri(X64Asm *a, X64Reg dst, i64 imm, u8 sz) {
    CodeBuf *cb = a->cb;
    maybe_size16(cb, sz);
    if (sz == 8) {
        if (imm >= 0 && imm <= 0xFFFFFFFF) {
            // Use 32-bit MOV (zero-extends to 64-bit) — shorter encoding
            if (dst >= 8) emit_rex(cb, false, false, false, true);
            codebuf_emit_byte(cb, 0xB8 | (dst & 7));
            codebuf_emit_u32(cb, (u32)imm);
        } else if (imm >= -0x80000000LL && imm <= 0x7FFFFFFF) {
            // MOV r/m64, sign-extended imm32
            emit_rex(cb, true, false, false, dst >= 8);
            codebuf_emit_byte(cb, 0xC7);
            codebuf_emit_byte(cb, modrm(3, 0, dst & 7));
            codebuf_emit_i32(cb, (i32)imm);
        } else {
            // Full 64-bit immediate: REX.W 0xB8+rd imm64
            emit_rex(cb, true, false, false, dst >= 8);
            codebuf_emit_byte(cb, 0xB8 | (dst & 7));
            codebuf_emit_i64(cb, imm);
        }
    } else if (sz == 4) {
        if (dst >= 8) emit_rex(cb, false, false, false, true);
        codebuf_emit_byte(cb, 0xB8 | (dst & 7));
        codebuf_emit_u32(cb, (u32)(i32)imm);
    } else if (sz == 2) {
        codebuf_emit_byte(cb, 0xB8 | (dst & 7));
        codebuf_emit_u16(cb, (u16)(i16)imm);
    } else { // sz == 1
        emit_rex_forced(cb, false, false, false, dst >= 8);
        codebuf_emit_byte(cb, 0xB0 | (dst & 7));
        codebuf_emit_byte(cb, (u8)(i8)imm);
    }
}

void x64_mov_rm(X64Asm *a, X64Reg dst, X64Reg base, i32 disp, u8 sz) {
    CodeBuf *cb = a->cb;
    maybe_size16(cb, sz);
    if (sz == 8) emit_rex(cb, true, dst >= 8, false, base >= 8);
    else if (sz == 4 && (dst>=8||base>=8)) emit_rex(cb,false,dst>=8,false,base>=8);
    else if (sz == 1) emit_rex_forced(cb, false, dst>=8, false, base>=8);
    u8 opc = (sz == 1) ? 0x8A : 0x8B;
    codebuf_emit_byte(cb, opc);
    emit_mem_opnd(cb, dst & 7, base, disp);
}

void x64_mov_mr(X64Asm *a, X64Reg base, i32 disp, X64Reg src, u8 sz) {
    CodeBuf *cb = a->cb;
    maybe_size16(cb, sz);
    if (sz == 8) emit_rex(cb, true, src >= 8, false, base >= 8);
    else if (sz == 4 && (src>=8||base>=8)) emit_rex(cb,false,src>=8,false,base>=8);
    else if (sz == 1) emit_rex_forced(cb, false, src>=8, false, base>=8);
    u8 opc = (sz == 1) ? 0x88 : 0x89;
    codebuf_emit_byte(cb, opc);
    emit_mem_opnd(cb, src & 7, base, disp);
}

void x64_mov_mi(X64Asm *a, X64Reg base, i32 disp, i32 imm, u8 sz) {
    CodeBuf *cb = a->cb;
    maybe_size16(cb, sz);
    if (sz == 8) emit_rex(cb, true, false, false, base >= 8);
    else if (base >= 8) emit_rex(cb, false, false, false, true);
    u8 opc = (sz == 1) ? 0xC6 : 0xC7;
    codebuf_emit_byte(cb, opc);
    emit_mem_opnd(cb, 0, base, disp);
    if (sz == 1) codebuf_emit_byte(cb, (u8)imm);
    else if (sz == 2) codebuf_emit_u16(cb, (u16)imm);
    else codebuf_emit_i32(cb, imm);
}

void x64_lea(X64Asm *a, X64Reg dst, X64Reg base, i32 disp) {
    CodeBuf *cb = a->cb;
    emit_rex(cb, true, dst >= 8, false, base >= 8);
    codebuf_emit_byte(cb, 0x8D);
    emit_mem_opnd(cb, dst & 7, base, disp);
}

void x64_movsx(X64Asm *a, X64Reg dst, X64Reg src, u8 src_sz, u8 dst_sz) {
    CodeBuf *cb = a->cb;
    bool W = (dst_sz == 8);
    emit_rex(cb, W, dst >= 8, false, src >= 8);
    if (src_sz == 1) {
        codebuf_emit_byte(cb, 0x0F); codebuf_emit_byte(cb, 0xBE);
    } else if (src_sz == 2) {
        codebuf_emit_byte(cb, 0x0F); codebuf_emit_byte(cb, 0xBF);
    } else { // 32->64
        codebuf_emit_byte(cb, 0x63); // MOVSXD
    }
    if (src_sz < 4 || dst_sz == 8)
        codebuf_emit_byte(cb, modrm(3, dst & 7, src & 7));
    else
        codebuf_emit_byte(cb, modrm(3, dst & 7, src & 7));
}

void x64_movzx(X64Asm *a, X64Reg dst, X64Reg src, u8 src_sz, u8 dst_sz) {
    CodeBuf *cb = a->cb;
    bool W = (dst_sz == 8);
    emit_rex(cb, W, dst >= 8, false, src >= 8);
    codebuf_emit_byte(cb, 0x0F);
    codebuf_emit_byte(cb, src_sz == 1 ? 0xB6 : 0xB7);
    codebuf_emit_byte(cb, modrm(3, dst & 7, src & 7));
}

void x64_movsx_mem(X64Asm *a, X64Reg dst, X64Reg base, i32 disp, u8 src_sz, u8 dst_sz) {
    CodeBuf *cb = a->cb;
    bool W = (dst_sz == 8);
    emit_rex(cb, W, dst >= 8, false, base >= 8);
    if (src_sz == 1)      { codebuf_emit_byte(cb, 0x0F); codebuf_emit_byte(cb, 0xBE); }
    else if (src_sz == 2) { codebuf_emit_byte(cb, 0x0F); codebuf_emit_byte(cb, 0xBF); }
    else                  { codebuf_emit_byte(cb, 0x63); }
    emit_mem_opnd(cb, dst & 7, base, disp);
}

void x64_movzx_mem(X64Asm *a, X64Reg dst, X64Reg base, i32 disp, u8 src_sz, u8 dst_sz) {
    CodeBuf *cb = a->cb;
    bool W = (dst_sz == 8);
    emit_rex(cb, W, dst >= 8, false, base >= 8);
    codebuf_emit_byte(cb, 0x0F);
    codebuf_emit_byte(cb, src_sz == 1 ? 0xB6 : 0xB7);
    emit_mem_opnd(cb, dst & 7, base, disp);
}

// ── Stack ────────────────────────────────────────────────────
void x64_push(X64Asm *a, X64Reg r) {
    if (r >= 8) codebuf_emit_byte(a->cb, REX_B);
    codebuf_emit_byte(a->cb, 0x50 | (r & 7));
}
void x64_pop(X64Asm *a, X64Reg r) {
    if (r >= 8) codebuf_emit_byte(a->cb, REX_B);
    codebuf_emit_byte(a->cb, 0x58 | (r & 7));
}
void x64_push_imm(X64Asm *a, i32 imm) {
    if (imm >= -128 && imm <= 127) {
        codebuf_emit_byte(a->cb, 0x6A);
        codebuf_emit_byte(a->cb, (u8)(i8)imm);
    } else {
        codebuf_emit_byte(a->cb, 0x68);
        codebuf_emit_i32(a->cb, imm);
    }
}

// ── ALU helper macros ─────────────────────────────────────────
// Standard 2-op ALU: opcode for r/m,r and r,r/m

static void alu_rr(CodeBuf *cb, u8 opc8, u8 opc, X64Reg dst, X64Reg src, u8 sz) {
    maybe_size16(cb, sz);
    if (sz == 8) emit_rex(cb, true, src >= 8, false, dst >= 8);
    else if (sz == 4 && (src>=8||dst>=8)) emit_rex(cb,false,src>=8,false,dst>=8);
    else if (sz == 1) emit_rex_forced(cb, false, src>=8, false, dst>=8);
    codebuf_emit_byte(cb, sz == 1 ? opc8 : opc);
    codebuf_emit_byte(cb, modrm(3, src & 7, dst & 7));
}

static void alu_ri(CodeBuf *cb, u8 ext, X64Reg dst, i32 imm, u8 sz) {
    maybe_size16(cb, sz);
    if (sz == 8) emit_rex(cb, true, false, false, dst >= 8);
    else if (sz == 4 && dst >= 8) emit_rex(cb, false, false, false, true);
    bool s8 = (imm >= -128 && imm <= 127 && sz > 1);
    codebuf_emit_byte(cb, sz == 1 ? 0x80 : (s8 ? 0x83 : 0x81));
    codebuf_emit_byte(cb, modrm(3, ext, dst & 7));
    if (sz == 1) codebuf_emit_byte(cb, (u8)imm);
    else if (s8) codebuf_emit_byte(cb, (u8)(i8)imm);
    else if (sz == 2) codebuf_emit_u16(cb, (u16)imm);
    else codebuf_emit_i32(cb, imm);
}

void x64_add_rr(X64Asm *a, X64Reg d, X64Reg s, u8 sz) { alu_rr(a->cb, 0x00, 0x01, d, s, sz); }
void x64_add_ri(X64Asm *a, X64Reg d, i32 imm, u8 sz)  { alu_ri(a->cb, 0, d, imm, sz); }
void x64_add_rm(X64Asm *a, X64Reg d, X64Reg b, i32 dp, u8 sz) {
    CodeBuf *cb = a->cb;
    maybe_size16(cb, sz);
    if (sz == 8) emit_rex(cb, true, d>=8, false, b>=8);
    else if (sz==4&&(d>=8||b>=8)) emit_rex(cb,false,d>=8,false,b>=8);
    codebuf_emit_byte(cb, sz==1 ? 0x02 : 0x03);
    emit_mem_opnd(cb, d&7, b, dp);
}
void x64_sub_rr(X64Asm *a, X64Reg d, X64Reg s, u8 sz) { alu_rr(a->cb, 0x28, 0x29, d, s, sz); }
void x64_sub_ri(X64Asm *a, X64Reg d, i32 imm, u8 sz)  { alu_ri(a->cb, 5, d, imm, sz); }

void x64_imul_rr(X64Asm *a, X64Reg d, X64Reg s, u8 sz) {
    CodeBuf *cb = a->cb;
    maybe_size16(cb, sz);
    if (sz == 8) emit_rex(cb, true, d>=8, false, s>=8);
    else if (d>=8||s>=8) emit_rex(cb, false, d>=8, false, s>=8);
    codebuf_emit_byte(cb, 0x0F); codebuf_emit_byte(cb, 0xAF);
    codebuf_emit_byte(cb, modrm(3, d&7, s&7));
}
void x64_imul_rri(X64Asm *a, X64Reg d, X64Reg s, i32 imm, u8 sz) {
    CodeBuf *cb = a->cb;
    maybe_size16(cb, sz);
    if (sz == 8) emit_rex(cb, true, d>=8, false, s>=8);
    bool s8 = (imm >= -128 && imm <= 127);
    codebuf_emit_byte(cb, s8 ? 0x6B : 0x69);
    codebuf_emit_byte(cb, modrm(3, d&7, s&7));
    if (s8) codebuf_emit_byte(cb, (u8)(i8)imm);
    else    codebuf_emit_i32(cb, imm);
}
void x64_idiv(X64Asm *a, X64Reg divisor, u8 sz) {
    CodeBuf *cb = a->cb;
    if (sz == 8) emit_rex(cb, true, false, false, divisor>=8);
    else if (divisor >= 8) emit_rex(cb, false, false, false, true);
    codebuf_emit_byte(cb, sz==1 ? 0xF6 : 0xF7);
    codebuf_emit_byte(cb, modrm(3, 7, divisor&7));
}
void x64_div(X64Asm *a, X64Reg divisor, u8 sz) {
    CodeBuf *cb = a->cb;
    if (sz == 8) emit_rex(cb, true, false, false, divisor>=8);
    codebuf_emit_byte(cb, sz==1 ? 0xF6 : 0xF7);
    codebuf_emit_byte(cb, modrm(3, 6, divisor&7));
}
void x64_neg(X64Asm *a, X64Reg r, u8 sz) {
    CodeBuf *cb = a->cb;
    if (sz == 8) emit_rex(cb, true, false, false, r>=8);
    codebuf_emit_byte(cb, sz==1 ? 0xF6 : 0xF7);
    codebuf_emit_byte(cb, modrm(3, 3, r&7));
}
void x64_inc(X64Asm *a, X64Reg r, u8 sz) {
    CodeBuf *cb = a->cb;
    if (sz == 8) emit_rex(cb, true, false, false, r>=8);
    codebuf_emit_byte(cb, sz==1 ? 0xFE : 0xFF);
    codebuf_emit_byte(cb, modrm(3, 0, r&7));
}
void x64_dec(X64Asm *a, X64Reg r, u8 sz) {
    CodeBuf *cb = a->cb;
    if (sz == 8) emit_rex(cb, true, false, false, r>=8);
    codebuf_emit_byte(cb, sz==1 ? 0xFE : 0xFF);
    codebuf_emit_byte(cb, modrm(3, 1, r&7));
}
void x64_cdq(X64Asm *a) { codebuf_emit_byte(a->cb, 0x99); }
void x64_cqo(X64Asm *a) { codebuf_emit_byte(a->cb, REX_W); codebuf_emit_byte(a->cb, 0x99); }

// ── Bitwise ───────────────────────────────────────────────────
void x64_and_rr(X64Asm *a, X64Reg d, X64Reg s, u8 sz) { alu_rr(a->cb, 0x20, 0x21, d, s, sz); }
void x64_and_ri(X64Asm *a, X64Reg d, i32 imm, u8 sz)  { alu_ri(a->cb, 4, d, imm, sz); }
void x64_or_rr(X64Asm *a, X64Reg d, X64Reg s, u8 sz)  { alu_rr(a->cb, 0x08, 0x09, d, s, sz); }
void x64_or_ri(X64Asm *a, X64Reg d, i32 imm, u8 sz)   { alu_ri(a->cb, 1, d, imm, sz); }
void x64_xor_rr(X64Asm *a, X64Reg d, X64Reg s, u8 sz) { alu_rr(a->cb, 0x30, 0x31, d, s, sz); }
void x64_xor_ri(X64Asm *a, X64Reg d, i32 imm, u8 sz)  { alu_ri(a->cb, 6, d, imm, sz); }
void x64_not(X64Asm *a, X64Reg r, u8 sz) {
    CodeBuf *cb = a->cb;
    if (sz == 8) emit_rex(cb, true, false, false, r>=8);
    codebuf_emit_byte(cb, sz==1 ? 0xF6 : 0xF7);
    codebuf_emit_byte(cb, modrm(3, 2, r&7));
}

static void shift_ri(CodeBuf *cb, u8 ext, X64Reg r, u8 amt, u8 sz) {
    if (sz == 8) emit_rex(cb, true, false, false, r>=8);
    else if (sz==4&&r>=8) emit_rex(cb, false, false, false, true);
    if (amt == 1) {
        codebuf_emit_byte(cb, sz==1 ? 0xD0 : 0xD1);
        codebuf_emit_byte(cb, modrm(3, ext, r&7));
    } else {
        codebuf_emit_byte(cb, sz==1 ? 0xC0 : 0xC1);
        codebuf_emit_byte(cb, modrm(3, ext, r&7));
        codebuf_emit_byte(cb, amt & 63);
    }
}
void x64_shl_ri(X64Asm *a, X64Reg r, u8 amt, u8 sz) { shift_ri(a->cb, 4, r, amt, sz); }
void x64_shr_ri(X64Asm *a, X64Reg r, u8 amt, u8 sz) { shift_ri(a->cb, 5, r, amt, sz); }
void x64_sar_ri(X64Asm *a, X64Reg r, u8 amt, u8 sz) { shift_ri(a->cb, 7, r, amt, sz); }

static void shift_cl(CodeBuf *cb, u8 ext, X64Reg r, u8 sz) {
    if (sz == 8) emit_rex(cb, true, false, false, r>=8);
    codebuf_emit_byte(cb, sz==1 ? 0xD2 : 0xD3);
    codebuf_emit_byte(cb, modrm(3, ext, r&7));
}
void x64_shl_cl(X64Asm *a, X64Reg r, u8 sz) { shift_cl(a->cb, 4, r, sz); }
void x64_shr_cl(X64Asm *a, X64Reg r, u8 sz) { shift_cl(a->cb, 5, r, sz); }
void x64_sar_cl(X64Asm *a, X64Reg r, u8 sz) { shift_cl(a->cb, 7, r, sz); }

// ── Compare & TEST ────────────────────────────────────────────
void x64_cmp_rr(X64Asm *a, X64Reg l, X64Reg r, u8 sz) { alu_rr(a->cb, 0x38, 0x39, l, r, sz); }
void x64_cmp_ri(X64Asm *a, X64Reg l, i32 imm, u8 sz)  { alu_ri(a->cb, 7, l, imm, sz); }
void x64_test_rr(X64Asm *a, X64Reg a_, X64Reg b, u8 sz) { alu_rr(a->cb, 0x84, 0x85, a_, b, sz); }
void x64_test_ri(X64Asm *a, X64Reg r, i32 imm, u8 sz)   { alu_ri(a->cb, 0, r, imm, sz); /* TEST /0 */ }

// SET* — write 0/1 based on flags
static void setcc(CodeBuf *cb, u8 cc, X64Reg r) {
    if (r >= 8) emit_rex(cb, false, false, false, true);
    else        emit_rex_forced(cb, false, false, false, false);
    codebuf_emit_byte(cb, 0x0F);
    codebuf_emit_byte(cb, cc);
    codebuf_emit_byte(cb, modrm(3, 0, r & 7));
}
void x64_sete(X64Asm *a, X64Reg r)  { setcc(a->cb, 0x94, r); }
void x64_setne(X64Asm *a, X64Reg r) { setcc(a->cb, 0x95, r); }
void x64_setl(X64Asm *a, X64Reg r)  { setcc(a->cb, 0x9C, r); }
void x64_setle(X64Asm *a, X64Reg r) { setcc(a->cb, 0x9E, r); }
void x64_setg(X64Asm *a, X64Reg r)  { setcc(a->cb, 0x9F, r); }
void x64_setge(X64Asm *a, X64Reg r) { setcc(a->cb, 0x9D, r); }
void x64_setb(X64Asm *a, X64Reg r)  { setcc(a->cb, 0x92, r); }
void x64_setbe(X64Asm *a, X64Reg r) { setcc(a->cb, 0x96, r); }
void x64_seta(X64Asm *a, X64Reg r)  { setcc(a->cb, 0x97, r); }
void x64_setae(X64Asm *a, X64Reg r) { setcc(a->cb, 0x93, r); }

// ── Jumps ─────────────────────────────────────────────────────
static void jcc_rel32(CodeBuf *cb, FixupTable *ft, u8 cc, u32 label_id) {
    codebuf_emit_byte(cb, 0x0F);
    codebuf_emit_byte(cb, cc);
    fixup_add(ft, cb->len, label_id);
    codebuf_emit_i32(cb, 0);  // placeholder, patched later
}
void x64_jmp(X64Asm *a, u32 id) {
    codebuf_emit_byte(a->cb, 0xE9);
    fixup_add(a->ft, a->cb->len, id);
    codebuf_emit_i32(a->cb, 0);
}
void x64_je(X64Asm *a, u32 id)  { jcc_rel32(a->cb, a->ft, 0x84, id); }
void x64_jne(X64Asm *a, u32 id) { jcc_rel32(a->cb, a->ft, 0x85, id); }
void x64_jl(X64Asm *a, u32 id)  { jcc_rel32(a->cb, a->ft, 0x8C, id); }
void x64_jle(X64Asm *a, u32 id) { jcc_rel32(a->cb, a->ft, 0x8E, id); }
void x64_jg(X64Asm *a, u32 id)  { jcc_rel32(a->cb, a->ft, 0x8F, id); }
void x64_jge(X64Asm *a, u32 id) { jcc_rel32(a->cb, a->ft, 0x8D, id); }
void x64_jb(X64Asm *a, u32 id)  { jcc_rel32(a->cb, a->ft, 0x82, id); }
void x64_jbe(X64Asm *a, u32 id) { jcc_rel32(a->cb, a->ft, 0x86, id); }
void x64_ja(X64Asm *a, u32 id)  { jcc_rel32(a->cb, a->ft, 0x87, id); }
void x64_jae(X64Asm *a, u32 id) { jcc_rel32(a->cb, a->ft, 0x83, id); }

void x64_call_reg(X64Asm *a, X64Reg r) {
    if (r >= 8) codebuf_emit_byte(a->cb, REX_B);
    codebuf_emit_byte(a->cb, 0xFF);
    codebuf_emit_byte(a->cb, modrm(3, 2, r&7));
}
void x64_call_label(X64Asm *a, u32 label_id) {
    codebuf_emit_byte(a->cb, 0xE8);
    fixup_add(a->ft, a->cb->len, label_id);
    codebuf_emit_i32(a->cb, 0);
}
void x64_ret(X64Asm *a) { codebuf_emit_byte(a->cb, 0xC3); }
void x64_nop(X64Asm *a) { codebuf_emit_byte(a->cb, 0x90); }

void x64_def_label(X64Asm *a, u32 label_id) {
    label_define(a->lt, label_id, a->cb->len);
}

// ── SSE2 ──────────────────────────────────────────────────────
static void sse2_rr(CodeBuf *cb, u8 pfx, u8 op1, u8 op2,
                    u8 dst, u8 src) {
    codebuf_emit_byte(cb, pfx);
    if (dst >= 8 || src >= 8)
        codebuf_emit_byte(cb, 0x40 | ((dst>=8)?4:0) | ((src>=8)?1:0));
    codebuf_emit_byte(cb, 0x0F);
    codebuf_emit_byte(cb, op1);
    if (op2) codebuf_emit_byte(cb, op2);
    codebuf_emit_byte(cb, modrm(3, dst&7, src&7));
}
static void sse2_mem(CodeBuf *cb, u8 pfx, u8 op1, u8 op2,
                     u8 dst, X64Reg base, i32 disp) {
    codebuf_emit_byte(cb, pfx);
    if (dst >= 8 || base >= 8)
        codebuf_emit_byte(cb, 0x40 | ((dst>=8)?4:0) | ((base>=8)?1:0));
    codebuf_emit_byte(cb, 0x0F);
    codebuf_emit_byte(cb, op1);
    if (op2) codebuf_emit_byte(cb, op2);
    emit_mem_opnd(cb, dst&7, base, disp);
}

void x64_movsd_rr(X64Asm *a, X64XmmReg d, X64XmmReg s) { sse2_rr(a->cb, 0xF2, 0x10, 0, d, s); }
void x64_movsd_load(X64Asm *a, X64XmmReg d, X64Reg b, i32 dp) { sse2_mem(a->cb, 0xF2, 0x10, 0, d, b, dp); }
void x64_movsd_store(X64Asm *a, X64Reg b, i32 dp, X64XmmReg s) { sse2_mem(a->cb, 0xF2, 0x11, 0, s, b, dp); }
void x64_movss_rr(X64Asm *a, X64XmmReg d, X64XmmReg s) { sse2_rr(a->cb, 0xF3, 0x10, 0, d, s); }
void x64_movss_load(X64Asm *a, X64XmmReg d, X64Reg b, i32 dp) { sse2_mem(a->cb, 0xF3, 0x10, 0, d, b, dp); }
void x64_movss_store(X64Asm *a, X64Reg b, i32 dp, X64XmmReg s) { sse2_mem(a->cb, 0xF3, 0x11, 0, s, b, dp); }

void x64_addsd(X64Asm *a, X64XmmReg d, X64XmmReg s) { sse2_rr(a->cb, 0xF2, 0x58, 0, d, s); }
void x64_subsd(X64Asm *a, X64XmmReg d, X64XmmReg s) { sse2_rr(a->cb, 0xF2, 0x5C, 0, d, s); }
void x64_mulsd(X64Asm *a, X64XmmReg d, X64XmmReg s) { sse2_rr(a->cb, 0xF2, 0x59, 0, d, s); }
void x64_divsd(X64Asm *a, X64XmmReg d, X64XmmReg s) { sse2_rr(a->cb, 0xF2, 0x5E, 0, d, s); }
void x64_addss(X64Asm *a, X64XmmReg d, X64XmmReg s) { sse2_rr(a->cb, 0xF3, 0x58, 0, d, s); }
void x64_subss(X64Asm *a, X64XmmReg d, X64XmmReg s) { sse2_rr(a->cb, 0xF3, 0x5C, 0, d, s); }
void x64_mulss(X64Asm *a, X64XmmReg d, X64XmmReg s) { sse2_rr(a->cb, 0xF3, 0x59, 0, d, s); }
void x64_divss(X64Asm *a, X64XmmReg d, X64XmmReg s) { sse2_rr(a->cb, 0xF3, 0x5E, 0, d, s); }

void x64_ucomisd(X64Asm *a, X64XmmReg l, X64XmmReg r) { sse2_rr(a->cb, 0x66, 0x2E, 0, l, r); }
void x64_ucomiss(X64Asm *a, X64XmmReg l, X64XmmReg r) {
    if ((u8)l>=8||(u8)r>=8)
        codebuf_emit_byte(a->cb, 0x40|((l>=8)?4:0)|((r>=8)?1:0));
    codebuf_emit_byte(a->cb, 0x0F);
    codebuf_emit_byte(a->cb, 0x2E);
    codebuf_emit_byte(a->cb, modrm(3, l&7, r&7));
}

void x64_cvtsi2sd(X64Asm *a, X64XmmReg d, X64Reg s, u8 src_sz) {
    codebuf_emit_byte(a->cb, 0xF2);
    emit_rex(a->cb, src_sz==8, d>=8, false, s>=8);
    codebuf_emit_byte(a->cb, 0x0F); codebuf_emit_byte(a->cb, 0x2A);
    codebuf_emit_byte(a->cb, modrm(3, d&7, s&7));
}
void x64_cvtsi2ss(X64Asm *a, X64XmmReg d, X64Reg s, u8 src_sz) {
    codebuf_emit_byte(a->cb, 0xF3);
    emit_rex(a->cb, src_sz==8, d>=8, false, s>=8);
    codebuf_emit_byte(a->cb, 0x0F); codebuf_emit_byte(a->cb, 0x2A);
    codebuf_emit_byte(a->cb, modrm(3, d&7, s&7));
}
void x64_cvttsd2si(X64Asm *a, X64Reg d, X64XmmReg s, u8 dst_sz) {
    codebuf_emit_byte(a->cb, 0xF2);
    emit_rex(a->cb, dst_sz==8, d>=8, false, s>=8);
    codebuf_emit_byte(a->cb, 0x0F); codebuf_emit_byte(a->cb, 0x2C);
    codebuf_emit_byte(a->cb, modrm(3, d&7, s&7));
}
void x64_cvttss2si(X64Asm *a, X64Reg d, X64XmmReg s, u8 dst_sz) {
    codebuf_emit_byte(a->cb, 0xF3);
    emit_rex(a->cb, dst_sz==8, d>=8, false, s>=8);
    codebuf_emit_byte(a->cb, 0x0F); codebuf_emit_byte(a->cb, 0x2C);
    codebuf_emit_byte(a->cb, modrm(3, d&7, s&7));
}
void x64_cvtsd2ss(X64Asm *a, X64XmmReg d, X64XmmReg s) { sse2_rr(a->cb, 0xF2, 0x5A, 0, d, s); }
void x64_cvtss2sd(X64Asm *a, X64XmmReg d, X64XmmReg s) { sse2_rr(a->cb, 0xF3, 0x5A, 0, d, s); }

// ── Prologue / Epilogue ───────────────────────────────────────
void x64_emit_prologue(X64Asm *a, u32 frame_size) {
    x64_push(a, X64_RBP);
    x64_mov_rr(a, X64_RBP, X64_RSP, 8);
    if (frame_size > 0) {
        frame_size = (u32)ALIGN_UP(frame_size, 16);
        x64_sub_ri(a, X64_RSP, (i32)frame_size, 8);
    }
}
void x64_emit_epilogue(X64Asm *a) {
    x64_mov_rr(a, X64_RSP, X64_RBP, 8);
    x64_pop(a, X64_RBP);
    x64_ret(a);
}

void x64_align(X64Asm *a, usize align) {
    usize pos = a->cb->len;
    usize pad = ALIGN_UP(pos, align) - pos;
    // Emit multi-byte NOPs where possible
    static const u8 nop1[]  = {0x90};
    static const u8 nop2[]  = {0x66, 0x90};
    static const u8 nop3[]  = {0x0F, 0x1F, 0x00};
    static const u8 nop4[]  = {0x0F, 0x1F, 0x40, 0x00};
    static const u8 nop8[]  = {0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00};
    while (pad >= 8) { codebuf_emit_bytes(a->cb, nop8, 8); pad -= 8; }
    if (pad >= 4) { codebuf_emit_bytes(a->cb, nop4, 4); pad -= 4; }
    if (pad >= 3) { codebuf_emit_bytes(a->cb, nop3, 3); pad -= 3; }
    if (pad >= 2) { codebuf_emit_bytes(a->cb, nop2, 2); pad -= 2; }
    if (pad >= 1) { codebuf_emit_bytes(a->cb, nop1, 1); pad -= 1; }
}