// ============================================================
//  O Language Compiler — o_elf_exec.h
//  ELF64 executable (.elf) and shared library (.so) emitter
//  No ld/gold needed — pure binary output
//  Z-TEAM | C23
// ============================================================
#pragma once
#include "core/common.h"
#include "core/arena.h"
#include "ir/ir.h"
#include "backend/target.h"

typedef enum {
    ELF_EXEC_MODE_STATIC,
    ELF_EXEC_MODE_DYNAMIC,
    ELF_EXEC_MODE_SO,
} ElfExecMode;

typedef struct {
    Arena       *arena;
    ElfExecMode  mode;
    const char  *interp;
    u8          *text_buf;
    usize        text_size, text_cap;
    u8          *data_buf;
    usize        data_size, data_cap;
    StrView      needed_libs[32];
    u32          needed_count;
    StrView      extern_syms[256];
    u32          extern_count;
    u32          extern_got_off[256];
    StrView      export_names[256];
    u32          export_text_off[256];
    u32          export_count;
    u64          load_vaddr;
    u32          entry_text_off;
} ElfExecCtx;

ElfExecCtx *elf_exec_new(Arena *arena, ElfExecMode mode);
void elf_exec_add_needed(ElfExecCtx *ctx, StrView libname);
u32  elf_exec_declare_extern(ElfExecCtx *ctx, StrView sym);
void elf_exec_add_export(ElfExecCtx *ctx, StrView name, u32 text_off);

typedef struct { bool had_error; const char *msg; } ElfExecResult;
ElfExecResult elf_exec_compile(ElfExecCtx *ctx, IRModule *module);
ElfExecResult elf_exec_write(ElfExecCtx *ctx, const char *path);
