// ============================================================
//  O Language Compiler — o_jit.h
//  JIT engine: compile O IR to x86-64, execute immediately
//  Z-TEAM | C23
// ============================================================
#pragma once
#include "core/common.h"
#include "ir/ir.h"
#include "backend/x64.h"
#include "core/arena.h"

#define JIT_MAX_VREGS 1024
#define JIT_MAX_XMMS  8

typedef enum { VREG_UNALLOCATED, VREG_IN_REGISTER, VREG_SPILLED } VRegState;

typedef struct {
    VRegState state;
    bool      is_float;
    union {
        X64Reg    phys_reg;
        X64XmmReg xmm_reg;
        i32       stack_off;
    };
    u32 first_use, last_use;
} VRegInfo;

typedef struct {
    IRFunc           *func;
    struct JITEngine *engine;
    Arena             code_arena;
    CodeBuf           code_buf;
    FixupTable        fixups;
    LabelTable        labels;
    X64Asm            asm_;
    VRegInfo          vregs[JIT_MAX_VREGS];
    u32               vreg_count;
    u32               int_reg_free, xmm_reg_free;
    i32               frame_size, next_stack_slot;
    u32               callee_saved_used;
    u32               errors;
} JITContext;

typedef struct JITEngine JITEngine;

typedef struct {
    void  *entry;
    usize  code_size;
    void  *exec_mem;
    usize  exec_mem_sz;
} JITCompiledFunc;

struct JITEngine {
    Arena            arena;
    IRModule        *module;
    StrView         *cached_names;
    JITCompiledFunc **cached_funcs;
    u32              cache_count, cache_cap;
    void           **fn_table;
};

JITEngine       *jit_engine_new(IRModule *module);
void             jit_engine_free(JITEngine *e);
JITCompiledFunc *jit_compile_func(JITEngine *e, IRFunc *fn);
JITCompiledFunc *jit_find_func(JITEngine *e, StrView name);
OResult          jit_compile_module(JITEngine *e);
i64              jit_call_i64(JITCompiledFunc *fn,
                              i64 a0, i64 a1, i64 a2,
                              i64 a3, i64 a4, i64 a5);
void jit_dump_hex(const JITCompiledFunc *fn, FILE *out);
void jit_liveness_analysis(JITContext *ctx);
void jit_linear_scan_alloc(JITContext *ctx);
void jit_assign_stack_slots(JITContext *ctx);
