// ============================================================
//  O Language Compiler — o_aot.h
//  AOT compiler: IR -> x86-64 -> ELF64 relocatable object
//  Zero external dependencies — pure C23 ELF emitter
//  Z-TEAM | C23
// ============================================================
#pragma once
#include "core/common.h"
#include "ir/ir.h"
#include "backend/x64.h"
#include "core/arena.h"

// ELF64 constants
#define ELFCLASS64   2
#define ELFDATA2LSB  1
#define EV_CURRENT   1
#define ET_REL       1
#define EM_X86_64    62
#define SHT_NULL     0
#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_RELA     4
#define SHF_WRITE     0x1
#define SHF_ALLOC     0x2
#define SHF_EXECINSTR 0x4
#define STB_LOCAL  0
#define STB_GLOBAL 1
#define STT_FUNC   2
#define STV_DEFAULT 0
#define R_X86_64_PLT32 4
#define SHN_UNDEF  0
#define ELF64_ST_INFO(b,t) (((b)<<4)|((t)&0xF))
#define ELF64_R_INFO(s,t)  (((u64)(s)<<32)|(u64)(t))

typedef PACKED struct {
    u8  e_ident[16]; u16 e_type,e_machine; u32 e_version;
    u64 e_entry,e_phoff,e_shoff; u32 e_flags;
    u16 e_ehsize,e_phentsize,e_phnum,e_shentsize,e_shnum,e_shstrndx;
} Elf64_Ehdr;

typedef PACKED struct {
    u32 sh_name,sh_type; u64 sh_flags,sh_addr,sh_offset,sh_size;
    u32 sh_link,sh_info; u64 sh_addralign,sh_entsize;
} Elf64_Shdr;

typedef PACKED struct {
    u32 st_name; u8 st_info,st_other; u16 st_shndx;
    u64 st_value,st_size;
} Elf64_Sym;

typedef PACKED struct { u64 r_offset,r_info; i64 r_addend; } Elf64_Rela;

typedef struct { u8 *data; u32 len,cap; Arena *arena; } StrTab;
void strtab_init(StrTab *s, Arena *a);
u32  strtab_add(StrTab *s, const char *str);
u32  strtab_add_sv(StrTab *s, StrView sv);

#define AOT_MAX_RELOCS 4096
#define AOT_MAX_SYMS   1024

typedef struct {
    CodeBuf    text;
    u8        *data_buf;
    usize      data_len, data_cap, bss_size;
    Elf64_Sym  syms[AOT_MAX_SYMS];
    u32        sym_count;
    StrTab     symstrtab;
    Elf64_Rela relocs[AOT_MAX_RELOCS];
    u32        reloc_count;
    StrTab     shstrtab;
    StrView   *func_names;
    usize     *func_offsets;
    usize     *func_sizes;
    u32        func_count, func_cap;
    StrView   *extern_names;
    u32       *extern_sym_idx;
    u32        extern_count;
    Arena     *arena;
    FixupTable fixups;
    LabelTable labels;
    X64Asm     asm_;
} AOTContext;

typedef struct {
    const char *output_path;
    bool        debug_info, optimize, verbose;
} AOTOptions;

AOTContext *aot_context_new(Arena *arena);
void        aot_context_free(AOTContext *ctx);
OResult aot_compile_module(AOTContext *ctx, IRModule *module, const AOTOptions *opts);
OResult aot_write_elf64(AOTContext *ctx, const char *path);
OResult aot_link(const char *obj_path, const char *exe_path,
                 const char **extra_args, u32 extra_count);
void aot_peephole_optimize(AOTContext *ctx);
