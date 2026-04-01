// ============================================================
//  O Language Compiler — o_ir.h
//  Three-address IR — bridges AST to x86-64 backend
//  Z-TEAM | C23
// ============================================================
#pragma once
#include "core/common.h"
#include "core/arena.h"
#include "frontend/ast.h"

typedef enum {
    IRVAL_VOID, IRVAL_TEMP, IRVAL_CONST_I, IRVAL_CONST_F,
    IRVAL_GLOBAL, IRVAL_FUNC, IRVAL_LABEL,
} IRValKind;

typedef struct {
    IRValKind kind;
    TypeKind  type;
    union {
        u32     vreg; i64 ival; f64 fval;
        StrView name; u32 label_id;
    };
} IRVal;

static inline IRVal irval_void(void)                          { return (IRVal){.kind=IRVAL_VOID}; }
static inline IRVal irval_temp(u32 vr, TypeKind ty)          { return (IRVal){.kind=IRVAL_TEMP,.type=ty,.vreg=vr}; }
static inline IRVal irval_imm(i64 v, TypeKind ty)            { return (IRVal){.kind=IRVAL_CONST_I,.type=ty,.ival=v}; }
static inline IRVal irval_fimm(f64 v, TypeKind ty)           { return (IRVal){.kind=IRVAL_CONST_F,.type=ty,.fval=v}; }
static inline IRVal irval_label(u32 id)                      { return (IRVal){.kind=IRVAL_LABEL,.label_id=id}; }
static inline IRVal irval_global(StrView name, TypeKind ty)  { return (IRVal){.kind=IRVAL_GLOBAL,.type=ty,.name=name}; }
static inline IRVal irval_func(StrView name)                 { return (IRVal){.kind=IRVAL_FUNC,.name=name}; }

typedef enum {
    IOP_ADD, IOP_SUB, IOP_MUL, IOP_DIV, IOP_MOD,
    IOP_AND, IOP_OR, IOP_XOR, IOP_SHL, IOP_SHR, IOP_SAR,
    IOP_NEG, IOP_NOT, IOP_BNOT,
    IOP_FADD, IOP_FSUB, IOP_FMUL, IOP_FDIV, IOP_FNEG,
    IOP_CMP_EQ, IOP_CMP_NE, IOP_CMP_LT, IOP_CMP_LE,
    IOP_CMP_GT, IOP_CMP_GE,
    IOP_CMP_FLT, IOP_CMP_FLE, IOP_CMP_FGT, IOP_CMP_FGE,
    IOP_CMP_FEQ, IOP_CMP_FNE,
    IOP_MOV, IOP_LOAD, IOP_STORE, IOP_LEA,
    IOP_SEXT, IOP_ZEXT, IOP_TRUNC, IOP_ITOF, IOP_FTOI, IOP_BITCAST,
    IOP_LABEL, IOP_JMP, IOP_JZ, IOP_JNZ,
    IOP_CALL, IOP_RET, IOP_RET_VOID,
    IOP_ALLOCA, IOP_MEMCPY, IOP_MEMSET,
    IOP_NOP, IOP_COUNT
} IROp;

typedef struct IRInstr IRInstr;
struct IRInstr {
    IROp     op;
    IRVal    dst, src1, src2;
    u32      arg_count;
    IRVal   *args;
    TypeKind result_type;
};

typedef struct IRBlock IRBlock;
struct IRBlock {
    u32       id;
    StrView   name;
    IRInstr  *instrs;
    u32       instr_count, instr_cap;
    IRBlock  *succ[2];
    u32       succ_count;
    IRBlock **preds;
    u32       pred_count;
    u64      *live_in, *live_out;
};

typedef struct {
    StrView    name;
    TypeNode  *ret_type;
    StrView   *param_names;
    TypeKind  *param_types;
    u32        param_count;
    bool       variadic;
    IRBlock  **blocks;
    u32        block_count, block_cap;
    u32        next_vreg, next_label;
    Arena     *arena;
} IRFunc;

typedef struct {
    IRFunc **funcs;
    u32      func_count, func_cap;
    Arena   *arena;
} IRModule;

IRModule *ir_module_new(Arena *arena);
IRFunc   *ir_func_new(IRModule *m, StrView name,
                      StrView *param_names, TypeKind *param_types,
                      u32 param_count, TypeNode *ret_type, bool variadic);
IRBlock  *ir_block_new(IRFunc *fn, StrView name);
void      ir_block_seal(IRFunc *fn, IRBlock *b);
IRVal ir_emit(IRFunc *fn, IRBlock *b, IROp op,
              IRVal src1, IRVal src2, TypeKind result_type);
IRVal ir_emit_call(IRFunc *fn, IRBlock *b, IRVal func_val,
                   IRVal *args, u32 arg_count, TypeKind ret_type);
IRVal ir_emit_alloca(IRFunc *fn, IRBlock *b, TypeKind ty, u32 size);
void  ir_emit_store(IRFunc *fn, IRBlock *b, IRVal ptr, IRVal offset, IRVal value);
IRVal ir_emit_load(IRFunc *fn, IRBlock *b, IRVal ptr, IRVal offset, TypeKind ty);
void  ir_emit_jmp(IRFunc *fn, IRBlock *b, u32 label_id);
void  ir_emit_branch(IRFunc *fn, IRBlock *b, IRVal cond,
                     u32 true_label, u32 false_label);
void  ir_emit_ret(IRFunc *fn, IRBlock *b, IRVal val);
static inline u32 ir_new_vreg(IRFunc *fn)  { return fn->next_vreg++; }
static inline u32 ir_new_label(IRFunc *fn) { return fn->next_label++; }
void ir_func_dump(const IRFunc *fn, FILE *out);
void ir_module_dump(const IRModule *m, FILE *out);
const char *irop_name(IROp op);
