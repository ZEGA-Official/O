// ============================================================
//  O Language Compiler — o_driver.c  (v2 — cross-compilation)
//  Full output format support:
//    .o / .obj  — relocatable object (ELF64 or COFF)
//    .elf       — ELF64 static/dynamic executable
//    .so        — ELF64 shared library
//    .exe       — PE32+ Windows executable
//    .dll       — PE32+ Windows DLL
//    .iso       — Bootable GRUB2 CD image
//    JIT        — in-process execution (Linux only)
//
//  Cross-compilation: build any target on any host
//    oc hello.o --target x86_64-windows --fmt exe -o hello.exe
//    oc hello.o --target x86_64-linux   --fmt elf -o hello
//    oc kernel.o --fmt iso -o boot.iso
//
//  Z-TEAM | C23
// ============================================================

#include "core/common.h"
#include "core/arena.h"
#include "frontend/lexer.h"
#include "frontend/ast.h"
#include "ir/ir.h"
#include "jit/jit.h"
#include "lib/elf/aot.h"
#include "lib/windows/pe.h"
#include "lib/elf/exec.h"
#include "lib/iso/iso.h"
#include "backend/target.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

// ── Forward declarations ──────────────────────────────────────
AstModule *parse_source(const char *source, const char *filename, Arena *arena);
typedef struct { bool had_error; int error_count; } SemaResult;
SemaResult sema_module(AstModule *m, Arena *arena);
IRModule  *codegen_module(AstModule *ast, Arena *arena);

// ── File I/O ──────────────────────────────────────────────────
static char *load_file(const char *path, usize *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "\033[31merror\033[0m: cannot open '%s': %s\n",
                path, strerror(errno));
        return NULL;
    }
    struct stat st; fstat(fileno(f), &st);
    usize len = (usize)st.st_size;
    char *buf = malloc(len + 1);
    if (fread(buf, 1, len, f) != len) { free(buf); fclose(f); return NULL; }
    buf[len] = '\0'; fclose(f);
    if (out_len) *out_len = len;
    return buf;
}

static double now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

// ── Compile mode ──────────────────────────────────────────────
typedef enum {
    MODE_JIT,      // JIT execute (Linux only)
    MODE_EMIT_IR,  // dump IR and exit
    MODE_AOT,      // compile to output file
    MODE_VERSION,  // print banner
    MODE_HELP,
} CompileMode;

// ── CLI options ───────────────────────────────────────────────
typedef struct {
    const char  *input_file;
    const char  *output_file;
    CompileMode  mode;
    Target       target;
    bool         verbose;
    bool         dump_ir;
    bool         grub_check;  // just check if grub is available
    // ISO options
    const char  *boot_title;
    const char  *cmdline;
    u32          boot_timeout;
    bool         serial_console;
} DriverOpts;

static void print_banner(void) { printf("%s", O_LANG_BANNER); }

static void print_help(const char *argv0) {
    print_banner();
    printf(
        "Usage:\n"
        "  %s <file.o>                     JIT compile and run\n"
        "  %s <file.o> -o <out> [options]  Compile to file\n"
        "  %s <file.o> --emit-ir           Dump IR and exit\n"
        "  %s --version\n\n"
        "Output Formats  (--fmt <fmt>):\n"
        "  obj     Relocatable object  (.o on Linux, .obj on Windows)\n"
        "  elf     ELF64 executable    (.elf / no ext)\n"
        "  so      ELF64 shared lib    (.so)\n"
        "  exe     PE32+ executable    (.exe)  [Windows target]\n"
        "  dll     PE32+ DLL           (.dll)  [Windows target]\n"
        "  iso     Bootable GRUB2 ISO  (.iso)  [bare-metal]\n\n"
        "Target (--target <triple>):\n"
        "  x86_64-linux       Linux ELF output   [default on Linux]\n"
        "  x86_64-windows     Windows PE output  [default on Windows]\n"
        "  x86_64-freestanding  Bare-metal (for .iso)\n\n"
        "Examples:\n"
        "  %s hello.o --jit\n"
        "  %s hello.o -o hello            -- Linux ELF executable\n"
        "  %s hello.o -o hello.exe --target x86_64-windows --fmt exe\n"
        "  %s hello.o -o hello.dll --target x86_64-windows --fmt dll\n"
        "  %s hello.o -o hello.so  --fmt so\n"
        "  %s kernel.o -o boot.iso --fmt iso --title \"My OS\"\n\n"
        "ISO Options:\n"
        "  --title <str>      Boot menu title\n"
        "  --cmdline <str>    Kernel command line\n"
        "  --timeout <sec>    Boot timeout (default: 3)\n"
        "  --serial           Enable serial console\n\n"
        "Flags:\n"
        "  -v / --verbose     Verbose output with timing\n"
        "  --dump-ir          Print IR alongside output\n"
        "  --grub-check       Check if grub-mkrescue is installed\n",
        argv0, argv0, argv0, argv0,
        argv0, argv0, argv0, argv0, argv0, argv0);
}

// ── Parse output format string ────────────────────────────────
static OutputFormat parse_fmt(const char *s) {
    if (!s) return OUTPUT_ELF;
    if (!strcmp(s, "obj") || !strcmp(s, "o"))  return OUTPUT_OBJ;
    if (!strcmp(s, "elf"))                      return OUTPUT_ELF;
    if (!strcmp(s, "so")  || !strcmp(s, "dylib")) return OUTPUT_SO;
    if (!strcmp(s, "exe"))                      return OUTPUT_EXE;
    if (!strcmp(s, "dll"))                      return OUTPUT_DLL;
    if (!strcmp(s, "iso"))                      return OUTPUT_ISO;
    fprintf(stderr, "\033[33mwarning\033[0m: unknown format '%s', using elf\n", s);
    return OUTPUT_ELF;
}

// ── Infer format from output filename ────────────────────────
static OutputFormat infer_fmt(const char *path) {
    if (!path) return OUTPUT_ELF;
    const char *ext = strrchr(path, '.');
    if (!ext) return OUTPUT_ELF;
    if (!strcmp(ext, ".o") || !strcmp(ext, ".obj")) return OUTPUT_OBJ;
    if (!strcmp(ext, ".elf"))  return OUTPUT_ELF;
    if (!strcmp(ext, ".so"))   return OUTPUT_SO;
    if (!strcmp(ext, ".exe"))  return OUTPUT_EXE;
    if (!strcmp(ext, ".dll"))  return OUTPUT_DLL;
    if (!strcmp(ext, ".iso"))  return OUTPUT_ISO;
    return OUTPUT_ELF;
}

// ── CLI parser ─────────────────────────────────────────────────
static DriverOpts parse_args(int argc, char **argv) {
    DriverOpts o = {
        .output_file  = NULL,
        .mode         = MODE_JIT,
        .boot_title   = "O Language Kernel",
        .boot_timeout = 3,
    };
    // Default target = host OS
    o.target.arch = TARGET_ARCH_X86_64;
    o.target.os   = target_host_os();
    o.target.fmt  = OUTPUT_ELF;

    const char *fmt_str    = NULL;
    const char *target_str = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if      (!strcmp(a, "--version") || !strcmp(a, "-V")) o.mode = MODE_VERSION;
        else if (!strcmp(a, "--help")    || !strcmp(a, "-h")) o.mode = MODE_HELP;
        else if (!strcmp(a, "--jit"))     o.mode = MODE_JIT;
        else if (!strcmp(a, "--emit-ir")) o.mode = MODE_EMIT_IR;
        else if (!strcmp(a, "--aot"))     o.mode = MODE_AOT;
        else if (!strcmp(a, "--dump-ir")) o.dump_ir = true;
        else if (!strcmp(a, "--grub-check")) o.grub_check = true;
        else if (!strcmp(a, "--serial"))  o.serial_console = true;
        else if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) o.verbose = true;
        else if (!strcmp(a, "-o") && i+1 < argc)      o.output_file  = argv[++i];
        else if (!strcmp(a, "--fmt") && i+1 < argc)   fmt_str         = argv[++i];
        else if (!strcmp(a, "--target") && i+1 < argc) target_str     = argv[++i];
        else if (!strcmp(a, "--title") && i+1 < argc)  o.boot_title   = argv[++i];
        else if (!strcmp(a, "--cmdline") && i+1 < argc) o.cmdline      = argv[++i];
        else if (!strcmp(a, "--timeout") && i+1 < argc)
            o.boot_timeout = (u32)atoi(argv[++i]);
        else if (a[0] != '-') {
            if (!o.input_file) o.input_file = a;
        } else {
            fprintf(stderr, "\033[33mwarning\033[0m: unknown option '%s'\n", a);
        }
    }

    // Parse target triple
    if (target_str) target_parse(target_str, &o.target);

    // Determine format: explicit > inferred from output name > default
    if (fmt_str) {
        o.target.fmt = parse_fmt(fmt_str);
    } else if (o.output_file) {
        OutputFormat inf = infer_fmt(o.output_file);
        if (inf != OUTPUT_ELF || strrchr(o.output_file, '.'))
            o.target.fmt = inf;
    }

    // Set OS from format if not explicitly set
    if (!target_str) {
        if (o.target.fmt == OUTPUT_EXE || o.target.fmt == OUTPUT_DLL)
            o.target.os = TARGET_OS_WINDOWS;
        else if (o.target.fmt == OUTPUT_ISO)
            o.target.os = TARGET_OS_FREESTANDING;
    }

    // If any output file given and not JIT, switch to AOT
    if (o.output_file && o.mode == MODE_JIT && o.target.fmt != OUTPUT_ISO)
        o.mode = MODE_AOT;
    if (o.target.fmt == OUTPUT_ISO)
        o.mode = MODE_AOT;

    return o;
}

// ── Format label for verbose output ──────────────────────────
static const char *fmt_label(OutputFormat f) {
    switch (f) {
        case OUTPUT_OBJ: return "relocatable object (.o)";
        case OUTPUT_ELF: return "ELF64 executable";
        case OUTPUT_SO:  return "ELF64 shared library (.so)";
        case OUTPUT_EXE: return "PE32+ executable (.exe)";
        case OUTPUT_DLL: return "PE32+ DLL (.dll)";
        case OUTPUT_ISO: return "Bootable ISO (.iso)";
        default:         return "?";
    }
}

static const char *os_label(TargetOS os) {
    switch (os) {
        case TARGET_OS_LINUX:        return "Linux";
        case TARGET_OS_WINDOWS:      return "Windows";
        case TARGET_OS_FREESTANDING: return "Freestanding";
        default:                     return "?";
    }
}

// ── Determine output path with correct extension ──────────────
static const char *resolve_output(const DriverOpts *opts, Arena *arena) {
    if (opts->output_file) return opts->output_file;
    // Auto-generate from input + target extension
    const char *ext = target_default_ext(&opts->target);
    if (!opts->input_file) return "a.out";
    const char *base = strrchr(opts->input_file, '/');
    base = base ? base + 1 : opts->input_file;
    const char *dot = strrchr(base, '.');
    usize stem_len = dot ? (usize)(dot - base) : strlen(base);
    usize total = stem_len + strlen(ext) + 1;
    char *out = arena_alloc(arena, total, 1);
    memcpy(out, base, stem_len);
    strcpy(out + stem_len, ext);
    return out;
}

// ── AOT dispatch ─────────────────────────────────────────────
static int run_aot(const DriverOpts *opts, IRModule *ir, Arena *arena) {
    const char *out = resolve_output(opts, arena);
    double ta = now_ms();

    switch (opts->target.fmt) {

    // ── ELF relocatable (.o) ──────────────────────────────────
    case OUTPUT_OBJ: {
        Arena *aot_arena = arena_new(MB(16));
        AOTContext *aot  = aot_context_new(aot_arena);
        AOTOptions ao    = { .optimize = true, .verbose = opts->verbose };
        OResult cr = aot_compile_module(aot, ir, &ao);
        if (cr.status != O_OK) {
            fprintf(stderr, "\033[31merror\033[0m: AOT: %s\n", cr.msg);
            return 1;
        }
        char obj[512]; snprintf(obj, sizeof(obj), "%s", out);
        OResult wr = aot_write_elf64(aot, obj);
        if (wr.status != O_OK) {
            fprintf(stderr, "\033[31merror\033[0m: write: %s\n", wr.msg);
            return 1;
        }
        if (opts->verbose)
            fprintf(stderr,
                "\033[38;2;88;240;27m[oc]\033[0m OBJ    %.2f ms → %s\n",
                now_ms()-ta, obj);
        break;
    }

    // ── ELF executable ────────────────────────────────────────
    case OUTPUT_ELF: {
        Arena *ea = arena_new(MB(16));
        ElfExecCtx *ec = elf_exec_new(ea, ELF_EXEC_MODE_DYNAMIC);
        ElfExecResult er = elf_exec_compile(ec, ir);
        if (er.had_error) {
            fprintf(stderr, "\033[31merror\033[0m: ELF compile: %s\n", er.msg);
            return 1;
        }
        er = elf_exec_write(ec, out);
        if (er.had_error) {
            fprintf(stderr, "\033[31merror\033[0m: ELF write: %s\n", er.msg);
            return 1;
        }
        if (opts->verbose)
            fprintf(stderr,
                "\033[38;2;88;240;27m[oc]\033[0m ELF    %.2f ms → %s\n",
                now_ms()-ta, out);
        // Link via system linker for full symbol resolution
        char obj_tmp[256]; snprintf(obj_tmp, sizeof(obj_tmp), "%s.tmp.o", out);
        Arena *laot = arena_new(MB(16));
        AOTContext *laot_ctx = aot_context_new(laot);
        AOTOptions lo = { .optimize = true };
        aot_compile_module(laot_ctx, ir, &lo);
        aot_write_elf64(laot_ctx, obj_tmp);
        aot_link(obj_tmp, out, NULL, 0);
        unlink(obj_tmp);
        break;
    }

    // ── ELF shared library (.so) ──────────────────────────────
    case OUTPUT_SO: {
        Arena *ea = arena_new(MB(16));
        ElfExecCtx *ec = elf_exec_new(ea, ELF_EXEC_MODE_SO);
        ElfExecResult er = elf_exec_compile(ec, ir);
        if (!er.had_error) er = elf_exec_write(ec, out);
        if (er.had_error) {
            // Fallback: use system linker
            char obj_tmp[256]; snprintf(obj_tmp, sizeof(obj_tmp), "%s.tmp.o", out);
            Arena *la = arena_new(MB(16));
            AOTContext *ac = aot_context_new(la);
            AOTOptions lo  = { .optimize = true };
            aot_compile_module(ac, ir, &lo);
            aot_write_elf64(ac, obj_tmp);
            char cmd[512];
            snprintf(cmd, sizeof(cmd),
                     "gcc -shared -fPIC -o %s %s -nostartfiles 2>&1", out, obj_tmp);
            fprintf(stderr, "\033[38;2;88;240;27m[oc]\033[0m %s\n", cmd);
            system(cmd);
            unlink(obj_tmp);
        }
        if (opts->verbose)
            fprintf(stderr,
                "\033[38;2;88;240;27m[oc]\033[0m SO     %.2f ms → %s\n",
                now_ms()-ta, out);
        break;
    }

    // ── PE32+ executable (.exe) ───────────────────────────────
    case OUTPUT_EXE: {
        Arena *pa = arena_new(MB(16));
        Target t  = opts->target;
        t.fmt     = OUTPUT_EXE;
        PE_Context *pe = pe_context_new(pa, t);
        // Declare common Windows imports
        pe_declare_import(pe, sv_from_cstr("msvcrt.dll"),   sv_from_cstr("printf"));
        pe_declare_import(pe, sv_from_cstr("msvcrt.dll"),   sv_from_cstr("puts"));
        pe_declare_import(pe, sv_from_cstr("msvcrt.dll"),   sv_from_cstr("fprintf"));
        pe_declare_import(pe, sv_from_cstr("msvcrt.dll"),   sv_from_cstr("malloc"));
        pe_declare_import(pe, sv_from_cstr("msvcrt.dll"),   sv_from_cstr("free"));
        pe_declare_import(pe, sv_from_cstr("kernel32.dll"), sv_from_cstr("ExitProcess"));
        pe_declare_import(pe, sv_from_cstr("kernel32.dll"), sv_from_cstr("GetStdHandle"));
        pe_declare_import(pe, sv_from_cstr("kernel32.dll"), sv_from_cstr("WriteConsoleA"));
        PEResult pr = pe_compile_module(pe, ir);
        if (!pr.had_error) pr = pe_write(pe, out);
        if (pr.had_error) {
            fprintf(stderr, "\033[31merror\033[0m: PE/EXE: %s\n",
                    pr.msg ? pr.msg : "unknown");
            return 1;
        }
        if (opts->verbose)
            fprintf(stderr,
                "\033[38;2;88;240;27m[oc]\033[0m EXE    %.2f ms → %s\n",
                now_ms()-ta, out);
        printf("  Cross-compiled for Windows x64\n"
               "  Run with Wine: wine %s\n"
               "  Or copy to a Windows machine\n", out);
        break;
    }

    // ── PE32+ DLL (.dll) ──────────────────────────────────────
    case OUTPUT_DLL: {
        Arena *pa = arena_new(MB(16));
        Target t  = opts->target;
        t.fmt     = OUTPUT_DLL;
        PE_Context *pe = pe_context_new(pa, t);
        pe_declare_import(pe, sv_from_cstr("msvcrt.dll"),   sv_from_cstr("printf"));
        pe_declare_import(pe, sv_from_cstr("msvcrt.dll"),   sv_from_cstr("malloc"));
        pe_declare_import(pe, sv_from_cstr("kernel32.dll"), sv_from_cstr("ExitProcess"));
        PEResult pr = pe_compile_module(pe, ir);
        if (!pr.had_error) pr = pe_write(pe, out);
        if (pr.had_error) {
            fprintf(stderr, "\033[31merror\033[0m: PE/DLL: %s\n",
                    pr.msg ? pr.msg : "unknown");
            return 1;
        }
        if (opts->verbose)
            fprintf(stderr,
                "\033[38;2;88;240;27m[oc]\033[0m DLL    %.2f ms → %s\n",
                now_ms()-ta, out);
        printf("  Cross-compiled Windows DLL\n"
               "  Exports: %u functions\n", pe->export_count);
        break;
    }

    // ── Bootable ISO ──────────────────────────────────────────
    case OUTPUT_ISO: {
        if (!iso_grub_available()) {
            fprintf(stderr,
                "\033[31merror\033[0m: grub-mkrescue not found\n"
                "  Install: apt install grub-pc-bin grub-efi-amd64-bin xorriso\n"
                "           yum install grub2-tools xorriso\n");
            return 1;
        }
        Arena *ia = arena_new(MB(16));
        ISOContext *ic = iso_context_new(ia);
        ic->boot_menu_title  = opts->boot_title;
        ic->boot_timeout_sec = opts->boot_timeout;
        ic->serial_console   = opts->serial_console;
        ic->cmdline          = opts->cmdline ? opts->cmdline : "";

        ISOResult ir_res = iso_compile_and_write(ic, ir, out);
        if (ir_res.had_error) {
            fprintf(stderr, "\033[31merror\033[0m: ISO: %s\n", ir_res.msg);
            return 1;
        }
        if (opts->verbose)
            fprintf(stderr,
                "\033[38;2;88;240;27m[oc]\033[0m ISO    %.2f ms → %s\n",
                now_ms()-ta, out);
        break;
    }

    default:
        fprintf(stderr, "\033[31merror\033[0m: unsupported format\n");
        return 1;
    }

    return 0;
}

// ── Main pipeline ─────────────────────────────────────────────
static int run_pipeline(const DriverOpts *opts) {
    double t0 = now_ms();

    if (opts->verbose) {
        fprintf(stderr,
            "\033[38;2;88;240;27m[oc]\033[0m Target: %s / %s\n",
            os_label(opts->target.os), fmt_label(opts->target.fmt));
    }

    usize src_len = 0;
    char *source  = load_file(opts->input_file, &src_len);
    if (!source) return 1;

    if (opts->verbose)
        fprintf(stderr, "\033[38;2;88;240;27m[oc]\033[0m Loaded '%s' (%zu bytes)\n",
                opts->input_file, src_len);

    Arena *parse_arena = arena_new(MB(4));
    Arena *ir_arena    = arena_new(MB(8));
    Arena *tmp_arena   = arena_new(MB(1));

    // Parse
    double tp = now_ms();
    AstModule *ast = parse_source(source, opts->input_file, parse_arena);
    if (opts->verbose)
        fprintf(stderr, "\033[38;2;88;240;27m[oc]\033[0m Parse   %.2f ms  (%u decls)\n",
                now_ms()-tp, ast->decl_count);

    // Sema
    double ts = now_ms();
    SemaResult sr = sema_module(ast, parse_arena);
    if (opts->verbose)
        fprintf(stderr, "\033[38;2;88;240;27m[oc]\033[0m Sema    %.2f ms\n", now_ms()-ts);
    if (sr.had_error) { free(source); return 1; }

    // Codegen
    double tc = now_ms();
    IRModule *ir = codegen_module(ast, ir_arena);
    if (opts->verbose)
        fprintf(stderr, "\033[38;2;88;240;27m[oc]\033[0m Codegen %.2f ms  (%u funcs)\n",
                now_ms()-tc, ir->func_count);

    if (opts->dump_ir || opts->mode == MODE_EMIT_IR) {
        ir_module_dump(ir, stdout);
        if (opts->mode == MODE_EMIT_IR) { free(source); return 0; }
    }

    int result = 0;

    if (opts->mode == MODE_JIT) {
        // ── JIT (Linux only) ──────────────────────────────────
        double tj = now_ms();
        JITEngine *engine = jit_engine_new(ir);
        OResult cr = jit_compile_module(engine);
        if (cr.status != O_OK) {
            fprintf(stderr, "\033[31merror\033[0m: JIT: %s\n", cr.msg ? cr.msg : "?");
            free(source); return 1;
        }
        if (opts->verbose)
            fprintf(stderr,
                "\033[38;2;88;240;27m[oc]\033[0m JIT     %.2f ms  (total %.2f ms)\n",
                now_ms()-tj, now_ms()-t0);
        JITCompiledFunc *fn = jit_find_func(engine, sv_from_cstr("main"));
        if (!fn) {
            fprintf(stderr, "\033[31merror\033[0m: no 'main' function found\n"
                    "  \033[33mhint\033[0m: fn main() -> i64 { ... }\n");
            free(source); return 1;
        }
        i64 ret = jit_call_i64(fn, 0, 0, 0, 0, 0, 0);
        if (opts->verbose)
            fprintf(stderr,
                "\033[38;2;88;240;27m[oc]\033[0m exit %lld\n", (long long)ret);
        result = (int)ret;
        jit_engine_free(engine);

    } else if (opts->mode == MODE_AOT) {
        result = run_aot(opts, ir, tmp_arena);
    }

    free(source);
    return result;
}

// ── Entry point ───────────────────────────────────────────────
int main(int argc, char **argv) {
    if (argc < 2) { print_help(argv[0]); return 1; }

    DriverOpts opts = parse_args(argc, argv);

    if (opts.grub_check) {
        bool ok = iso_grub_available();
        printf("grub-mkrescue: %s\n", ok ? "✓ available" : "✗ not found");
        if (!ok) printf("Install: apt install grub-pc-bin grub-efi-amd64-bin xorriso\n");
        return ok ? 0 : 1;
    }

    if (opts.mode == MODE_VERSION) { print_banner(); return 0; }
    if (opts.mode == MODE_HELP)    { print_help(argv[0]); return 0; }

    if (!opts.input_file) {
        fprintf(stderr, "\033[31merror\033[0m: no input file\n");
        print_help(argv[0]); return 1;
    }
    return run_pipeline(&opts);
}
