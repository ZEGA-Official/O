// ============================================================
//  O Language Compiler -- o_iso.c
//  GRUB2 bootable ISO builder
//  Z-TEAM | C23
// ============================================================
#include "lib/iso/iso.h"
#include "lib/elf/aot.h"
#include "lib/elf/exec.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifndef _WIN32
#include <unistd.h>
#include <sys/wait.h>
#endif
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

usize iso_mb2_header(u8 *buf, usize buf_size, u64 load_addr, u64 entry_addr) {
    UNUSED(load_addr); UNUSED(entry_addr);
    if (buf_size < 64) return 0;
    u32 hdr_len = 16 + 8;
    u32 checksum = MB2_CHECKSUM(MB2_MAGIC, MB2_ARCH_X86, hdr_len);
    usize pos = 0;
    *(u32*)(buf+pos)=MB2_MAGIC;    pos+=4;
    *(u32*)(buf+pos)=MB2_ARCH_X86; pos+=4;
    *(u32*)(buf+pos)=hdr_len;      pos+=4;
    *(u32*)(buf+pos)=checksum;     pos+=4;
    *(u16*)(buf+pos)=0; pos+=2;
    *(u16*)(buf+pos)=0; pos+=2;
    *(u32*)(buf+pos)=8; pos+=4;
    return pos;
}

ISOContext *iso_context_new(Arena *arena) {
    ISOContext *ctx = ARENA_ALLOC_ZERO(arena, ISOContext);
    ctx->arena            = arena;
    ctx->boot_menu_title  = "O Language Kernel";
    ctx->boot_timeout_sec = 3;
    ctx->vga_console      = true;
    ctx->cmdline          = "";
    return ctx;
}

void iso_set_kernel(ISOContext *ctx, const u8 *data, usize size) {
    ctx->kernel_elf = (u8*)data; ctx->kernel_elf_size = size;
}

void iso_add_module(ISOContext *ctx, const char *path, const char *name) {
    if (ctx->module_count < 16) {
        ctx->module_paths[ctx->module_count] = path;
        ctx->module_names[ctx->module_count] = name;
        ctx->module_count++;
    }
}

bool iso_grub_available(void) {
    return system("which grub-mkrescue > /dev/null 2>&1") == 0
        || system("which grub2-mkrescue > /dev/null 2>&1") == 0;
}

static int run_cmd(const char *cmd) {
    fprintf(stderr, "\033[38;2;88;240;27m[iso]\033[0m %s\n", cmd);
    return system(cmd);
}

static void write_grub_cfg(FILE *f, const ISOContext *ctx, const char *kpath) {
    fprintf(f, "set timeout=%u\nset default=0\n\n", ctx->boot_timeout_sec);
    if (ctx->serial_console) {
        fprintf(f, "serial --speed=115200 --unit=0\nterminal_input serial\nterminal_output serial\n\n");
    }
    fprintf(f, "menuentry \"%s\" {\n", ctx->boot_menu_title);
    fprintf(f, "    multiboot2 %s %s\n", kpath, ctx->cmdline ? ctx->cmdline : "");
    for (u32 i = 0; i < ctx->module_count; i++)
        fprintf(f, "    module2 /boot/modules/%s %s\n", ctx->module_names[i], ctx->module_names[i]);
    fprintf(f, "    boot\n}\n");
}

static ISOResult build_kernel_elf(ISOContext *ctx, IRModule *module, const char *out_path) {
    Arena *aa = arena_new(MB(16));
    AOTContext *aot = aot_context_new(aa);
    AOTOptions opts = { .optimize = true };
    OResult r = aot_compile_module(aot, module, &opts);
    if (r.status != O_OK) return (ISOResult){ .had_error=true, .msg=r.msg };

    u8 mb2[64];
    usize mb2_len = iso_mb2_header(mb2, sizeof(mb2), 0x100000, 0x100000+64);
    const u64 LOAD = 0x100000;
    usize hdr_sz  = 0x40 + 0x38;
    usize total   = hdr_sz + mb2_len + aot->text.len;
    u8 *ef = arena_alloc(aa, total, 16);
    memset(ef, 0, total);

    ef[0]=0x7f; ef[1]='E'; ef[2]='L'; ef[3]='F';
    ef[4]=2; ef[5]=1; ef[6]=1;
    *(u16*)(ef+0x10)=2; *(u16*)(ef+0x12)=62; *(u32*)(ef+0x14)=1;
    *(u64*)(ef+0x18)=LOAD+hdr_sz; *(u64*)(ef+0x20)=0x40;
    *(u16*)(ef+0x34)=0x40; *(u16*)(ef+0x36)=0x38;
    *(u16*)(ef+0x38)=1;   *(u16*)(ef+0x3a)=0x40;
    u8 *ph=ef+0x40;
    *(u32*)(ph+0x00)=1; *(u32*)(ph+0x04)=0x5; *(u64*)(ph+0x08)=0;
    *(u64*)(ph+0x10)=LOAD; *(u64*)(ph+0x18)=LOAD;
    *(u64*)(ph+0x20)=(u64)total; *(u64*)(ph+0x28)=(u64)total;
    *(u64*)(ph+0x30)=0x1000;
    memcpy(ef+hdr_sz, mb2, mb2_len);
    memcpy(ef+hdr_sz+mb2_len, aot->text.buf, aot->text.len);

    FILE *f = fopen(out_path, "wb");
    if (!f) return (ISOResult){ .had_error=true, .msg="cannot write kernel ELF" };
    fwrite(ef, 1, total, f);
    fclose(f);
    chmod(out_path, 0755);
    ctx->kernel_elf = ef; ctx->kernel_elf_size = total;
    return (ISOResult){0};
}

ISOResult iso_write(ISOContext *ctx, const char *kernel_path, const char *out_iso_path) {
    char tmpdir[256], cmd[512];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/o_iso_%d", (int)getpid());
    snprintf(cmd, sizeof(cmd), "mkdir -p %s/boot/grub", tmpdir);
    if (run_cmd(cmd) != 0) return (ISOResult){.had_error=true,.msg="mkdir failed"};
    if (ctx->module_count) {
        snprintf(cmd, sizeof(cmd), "mkdir -p %s/boot/modules", tmpdir);
        run_cmd(cmd);
    }
    snprintf(cmd, sizeof(cmd), "cp %s %s/boot/kernel.elf", kernel_path, tmpdir);
    if (run_cmd(cmd) != 0) return (ISOResult){.had_error=true,.msg="failed to copy kernel"};
    for (u32 i=0; i<ctx->module_count; i++) {
        snprintf(cmd, sizeof(cmd), "cp %s %s/boot/modules/%s", ctx->module_paths[i], tmpdir, ctx->module_names[i]);
        run_cmd(cmd);
    }
    char cfg_path[256];
    snprintf(cfg_path, sizeof(cfg_path), "%s/boot/grub/grub.cfg", tmpdir);
    FILE *cfg = fopen(cfg_path, "w");
    if (!cfg) return (ISOResult){.had_error=true,.msg="cannot write grub.cfg"};
    write_grub_cfg(cfg, ctx, "/boot/kernel.elf");
    fclose(cfg);

    const char *mkrescue = "grub-mkrescue";
    if (system("which grub-mkrescue > /dev/null 2>&1") != 0) {
        if (system("which grub2-mkrescue > /dev/null 2>&1") == 0)
            mkrescue = "grub2-mkrescue";
        else
            return (ISOResult){.had_error=true,.msg="grub-mkrescue not found. apt install grub-pc-bin grub-efi-amd64-bin xorriso"};
    }
    snprintf(cmd, sizeof(cmd), "%s -output %s %s -- -volid \"O_LANG\" 2>&1", mkrescue, out_iso_path, tmpdir);
    int rc = run_cmd(cmd);
    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
    run_cmd(cmd);
    if (rc != 0) return (ISOResult){.had_error=true,.msg="grub-mkrescue failed"};
    fprintf(stderr, "\033[38;2;88;240;27m[iso]\033[0m Built %s\n"
            "\033[38;2;88;240;27m[iso]\033[0m Test: qemu-system-x86_64 -cdrom %s\n",
            out_iso_path, out_iso_path);
    return (ISOResult){0};
}

ISOResult iso_compile_and_write(ISOContext *ctx, IRModule *module, const char *out_iso_path) {
    char kp[256];
    snprintf(kp, sizeof(kp), "/tmp/o_kernel_%d.elf", (int)getpid());
    ISOResult r = build_kernel_elf(ctx, module, kp);
    if (r.had_error) return r;
    r = iso_write(ctx, kp, out_iso_path);
    unlink(kp);
    return r;
}
