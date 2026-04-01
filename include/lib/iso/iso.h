// ============================================================
//  O Language Compiler — o_iso.h
//  GRUB2 bootable ISO creator — Multiboot2 kernel support
//  Z-TEAM | C23
// ============================================================
#pragma once
#include "core/common.h"
#include "core/arena.h"
#include "ir/ir.h"

#define MB2_MAGIC    0xe85250d6
#define MB2_ARCH_X86 0
#define MB2_CHECKSUM(magic, arch, hdr_len) \
    ((u32)(-(i32)((magic) + (arch) + (hdr_len))))

typedef struct { u32 magic, arch, header_length, checksum; } Mb2Header;
typedef struct { u16 type, flags; u32 size; } Mb2Tag;

typedef struct {
    Arena      *arena;
    const char *grub_prefix;
    const char *grub_modules;
    u8         *kernel_elf;
    usize       kernel_elf_size;
    const char *module_paths[16];
    const char *module_names[16];
    u32         module_count;
    const char *boot_menu_title;
    u32         boot_timeout_sec;
    bool        serial_console;
    bool        vga_console;
    const char *cmdline;
} ISOContext;

ISOContext *iso_context_new(Arena *arena);
void iso_set_kernel(ISOContext *ctx, const u8 *elf_data, usize size);
void iso_add_module(ISOContext *ctx, const char *path, const char *name);

typedef struct { bool had_error; const char *msg; } ISOResult;
ISOResult iso_compile_and_write(ISOContext *ctx, IRModule *module,
                                const char *out_iso_path);
ISOResult iso_write(ISOContext *ctx, const char *kernel_path,
                    const char *out_iso_path);
usize iso_mb2_header(u8 *buf, usize buf_size, u64 load_addr, u64 entry_addr);
bool  iso_grub_available(void);
