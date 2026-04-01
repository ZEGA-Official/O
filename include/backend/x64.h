// ============================================================
//  O Language Compiler — o_x64.h
//  x86-64 machine code encoder
//  System V AMD64 ABI | REX prefix | ModRM | SIB
//  Z-TEAM | C23
// ============================================================
#pragma once
#include "core/common.h"
#include "core/arena.h"

typedef enum {
    X64_RAX=0,X64_RCX=1,X64_RDX=2,X64_RBX=3,
    X64_RSP=4,X64_RBP=5,X64_RSI=6,X64_RDI=7,
    X64_R8=8,X64_R9=9,X64_R10=10,X64_R11=11,
    X64_R12=12,X64_R13=13,X64_R14=14,X64_R15=15,
    X64_REG_NONE=255,
} X64Reg;

typedef enum {
    X64_XMM0=0,X64_XMM1=1,X64_XMM2=2,X64_XMM3=3,
    X64_XMM4=4,X64_XMM5=5,X64_XMM6=6,X64_XMM7=7,
    X64_XMM8=8,X64_XMM9=9,X64_XMM10=10,X64_XMM11=11,
    X64_XMM12=12,X64_XMM13=13,X64_XMM14=14,X64_XMM15=15,
} X64XmmReg;

#define SYSV_ARG_REGS_INT  6
#define SYSV_ARG_REGS_FLT  8
extern const X64Reg    sysv_int_arg_regs[SYSV_ARG_REGS_INT];
extern const X64XmmReg sysv_flt_arg_regs[SYSV_ARG_REGS_FLT];

typedef enum { OPD_REG, OPD_MEM, OPD_MEM_SIB, OPD_IMM, OPD_LABEL } OpdKind;

typedef struct {
    OpdKind kind; u8 size;
    union {
        X64Reg reg;
        struct { X64Reg base; i32 disp; }                        mem;
        struct { X64Reg base; X64Reg idx; u8 scale; i32 disp; }  sib;
        i64    imm;
        u32    label_id;
    };
} Opd;

static inline Opd opd_reg(X64Reg r, u8 sz)   { return (Opd){.kind=OPD_REG,.size=sz,.reg=r}; }
static inline Opd opd_mem(X64Reg b, i32 d, u8 sz) { return (Opd){.kind=OPD_MEM,.size=sz,.mem={b,d}}; }
static inline Opd opd_imm(i64 v, u8 sz)      { return (Opd){.kind=OPD_IMM,.size=sz,.imm=v}; }

typedef struct {
    u8    *buf;
    usize  len, cap;
    bool   is_exec;
    Arena *arena;
} CodeBuf;

void  codebuf_init_arena(CodeBuf *cb, Arena *arena, usize initial_cap);
bool  codebuf_init_exec(CodeBuf *cb, usize initial_cap);
void  codebuf_destroy(CodeBuf *cb);
void  codebuf_ensure(CodeBuf *cb, usize extra);
void  codebuf_emit_byte(CodeBuf *cb, u8 b);
void  codebuf_emit_u16(CodeBuf *cb, u16 v);
void  codebuf_emit_u32(CodeBuf *cb, u32 v);
void  codebuf_emit_u64(CodeBuf *cb, u64 v);
void  codebuf_emit_i32(CodeBuf *cb, i32 v);
void  codebuf_emit_i64(CodeBuf *cb, i64 v);
void  codebuf_emit_bytes(CodeBuf *cb, const u8 *data, usize len);
void  codebuf_patch_i32(CodeBuf *cb, usize off, i32 v);
void  codebuf_patch_u32(CodeBuf *cb, usize off, u32 v);

typedef struct { usize offset; u32 label_id; bool is_rel32; } Fixup;
typedef struct { Fixup *table; u32 count, cap; Arena *arena; } FixupTable;
typedef struct { usize *offsets; u32 count, cap; } LabelTable;

void fixup_table_init(FixupTable *ft, Arena *arena);
void fixup_add(FixupTable *ft, usize off, u32 label_id);
void fixup_resolve_all(FixupTable *ft, LabelTable *lt, CodeBuf *cb);
void label_table_init(LabelTable *lt, Arena *arena);
void label_define(LabelTable *lt, u32 label_id, usize offset);

typedef struct { CodeBuf *cb; FixupTable *ft; LabelTable *lt; } X64Asm;
void x64asm_init(X64Asm *a, CodeBuf *cb, FixupTable *ft, LabelTable *lt);

void x64_mov_rr(X64Asm *a, X64Reg dst, X64Reg src, u8 sz);
void x64_mov_ri(X64Asm *a, X64Reg dst, i64 imm, u8 sz);
void x64_mov_rm(X64Asm *a, X64Reg dst, X64Reg base, i32 disp, u8 sz);
void x64_mov_mr(X64Asm *a, X64Reg base, i32 disp, X64Reg src, u8 sz);
void x64_mov_mi(X64Asm *a, X64Reg base, i32 disp, i32 imm, u8 sz);
void x64_lea(X64Asm *a, X64Reg dst, X64Reg base, i32 disp);
void x64_movsx(X64Asm *a, X64Reg dst, X64Reg src, u8 src_sz, u8 dst_sz);
void x64_movzx(X64Asm *a, X64Reg dst, X64Reg src, u8 src_sz, u8 dst_sz);

void x64_push(X64Asm *a, X64Reg r);
void x64_pop(X64Asm *a, X64Reg r);
void x64_push_imm(X64Asm *a, i32 imm);

void x64_add_rr(X64Asm *a, X64Reg dst, X64Reg src, u8 sz);
void x64_add_ri(X64Asm *a, X64Reg dst, i32 imm, u8 sz);
void x64_sub_rr(X64Asm *a, X64Reg dst, X64Reg src, u8 sz);
void x64_sub_ri(X64Asm *a, X64Reg dst, i32 imm, u8 sz);
void x64_imul_rr(X64Asm *a, X64Reg dst, X64Reg src, u8 sz);
void x64_idiv(X64Asm *a, X64Reg divisor, u8 sz);
void x64_neg(X64Asm *a, X64Reg r, u8 sz);
void x64_inc(X64Asm *a, X64Reg r, u8 sz);
void x64_dec(X64Asm *a, X64Reg r, u8 sz);
void x64_cdq(X64Asm *a);
void x64_cqo(X64Asm *a);

void x64_and_rr(X64Asm *a, X64Reg dst, X64Reg src, u8 sz);
void x64_and_ri(X64Asm *a, X64Reg dst, i32 imm, u8 sz);
void x64_or_rr(X64Asm *a, X64Reg dst, X64Reg src, u8 sz);
void x64_xor_rr(X64Asm *a, X64Reg dst, X64Reg src, u8 sz);
void x64_not(X64Asm *a, X64Reg r, u8 sz);
void x64_shl_ri(X64Asm *a, X64Reg dst, u8 amt, u8 sz);
void x64_shr_ri(X64Asm *a, X64Reg dst, u8 amt, u8 sz);
void x64_sar_ri(X64Asm *a, X64Reg dst, u8 amt, u8 sz);

void x64_cmp_rr(X64Asm *a, X64Reg lhs, X64Reg rhs, u8 sz);
void x64_cmp_ri(X64Asm *a, X64Reg lhs, i32 imm, u8 sz);
void x64_test_rr(X64Asm *a, X64Reg a_, X64Reg b, u8 sz);

void x64_sete(X64Asm *a, X64Reg dst);
void x64_setne(X64Asm *a, X64Reg dst);
void x64_setl(X64Asm *a, X64Reg dst);
void x64_setle(X64Asm *a, X64Reg dst);
void x64_setg(X64Asm *a, X64Reg dst);
void x64_setge(X64Asm *a, X64Reg dst);

void x64_jmp(X64Asm *a, u32 label_id);
void x64_je(X64Asm *a, u32 label_id);
void x64_jne(X64Asm *a, u32 label_id);
void x64_jl(X64Asm *a, u32 label_id);
void x64_jle(X64Asm *a, u32 label_id);
void x64_jg(X64Asm *a, u32 label_id);
void x64_jge(X64Asm *a, u32 label_id);

void x64_call_reg(X64Asm *a, X64Reg r);
void x64_ret(X64Asm *a);
void x64_nop(X64Asm *a);
void x64_def_label(X64Asm *a, u32 label_id);

void x64_movsd_rr(X64Asm *a, X64XmmReg dst, X64XmmReg src);
void x64_addsd(X64Asm *a, X64XmmReg dst, X64XmmReg src);
void x64_subsd(X64Asm *a, X64XmmReg dst, X64XmmReg src);
void x64_mulsd(X64Asm *a, X64XmmReg dst, X64XmmReg src);
void x64_divsd(X64Asm *a, X64XmmReg dst, X64XmmReg src);
void x64_ucomisd(X64Asm *a, X64XmmReg lhs, X64XmmReg rhs);
void x64_cvtsi2sd(X64Asm *a, X64XmmReg dst, X64Reg src, u8 src_sz);
void x64_cvttsd2si(X64Asm *a, X64Reg dst, X64XmmReg src, u8 dst_sz);

void x64_emit_prologue(X64Asm *a, u32 frame_size);
void x64_emit_epilogue(X64Asm *a);
void x64_patch_frame_size(CodeBuf *cb, usize sub_rsp_offset, u32 frame_size);
void x64_align(X64Asm *a, usize align);

#define REX_W  0x48u
#define REX_R  0x44u
#define REX_X  0x42u
#define REX_B  0x41u
#define REX    0x40u

static inline u8 x64_modrm(u8 mod, u8 reg, u8 rm) {
    return (u8)((mod << 6) | ((reg & 7) << 3) | (rm & 7));
}
