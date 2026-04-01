// ============================================================
//  O Language Compiler — o_target.h
//  Cross-compilation target + output format system
//  Supports: Linux, Windows (x86-64)
//  Outputs:  .o .elf .so .exe .dll .iso
//  Z-TEAM | C23
// ============================================================
#pragma once
#include "core/common.h"

typedef enum {
    TARGET_OS_LINUX        = 0,
    TARGET_OS_WINDOWS      = 1,
    TARGET_OS_FREESTANDING = 2,
} TargetOS;

typedef enum { TARGET_ARCH_X86_64 = 0 } TargetArch;

typedef enum {
    OUTPUT_OBJ,   // .o  — ELF64 or COFF relocatable object
    OUTPUT_ELF,   // .elf — ELF64 executable
    OUTPUT_SO,    // .so  — ELF64 shared library
    OUTPUT_EXE,   // .exe — PE32+ executable
    OUTPUT_DLL,   // .dll — PE32+ DLL
    OUTPUT_ISO,   // .iso — GRUB2 bootable image
} OutputFormat;

typedef struct { TargetArch arch; TargetOS os; OutputFormat fmt; } Target;

static inline bool target_is_windows(const Target *t)    { return t->os  == TARGET_OS_WINDOWS; }
static inline bool target_is_linux(const Target *t)      { return t->os  == TARGET_OS_LINUX || t->os == TARGET_OS_FREESTANDING; }
static inline bool target_is_pe(const Target *t)         { return t->fmt == OUTPUT_EXE || t->fmt == OUTPUT_DLL; }
static inline bool target_is_elf(const Target *t)        { return t->fmt == OUTPUT_OBJ || t->fmt == OUTPUT_ELF || t->fmt == OUTPUT_SO; }
static inline bool target_is_shared(const Target *t)     { return t->fmt == OUTPUT_SO  || t->fmt == OUTPUT_DLL; }
static inline bool target_is_executable(const Target *t) { return t->fmt == OUTPUT_ELF || t->fmt == OUTPUT_EXE; }

static inline const char *target_default_ext(const Target *t) {
    switch (t->fmt) {
        case OUTPUT_OBJ: return target_is_windows(t) ? ".obj" : ".o";
        case OUTPUT_ELF: return ".elf";
        case OUTPUT_SO:  return ".so";
        case OUTPUT_EXE: return ".exe";
        case OUTPUT_DLL: return ".dll";
        case OUTPUT_ISO: return ".iso";
        default:         return ".out";
    }
}

static inline bool target_parse(const char *triple, Target *out) {
    out->arch = TARGET_ARCH_X86_64;
    out->os   = TARGET_OS_LINUX;
    out->fmt  = OUTPUT_ELF;
    if (!triple) return true;
    if (strstr(triple, "win") || strstr(triple, "Win"))
        out->os = TARGET_OS_WINDOWS;
    else if (strstr(triple, "freestanding") || strstr(triple, "bare"))
        out->os = TARGET_OS_FREESTANDING;
    return true;
}

#if defined(_WIN32) || defined(_WIN64)
#  define O_HOST_WINDOWS 1
#  define O_HOST_LINUX   0
#else
#  define O_HOST_WINDOWS 0
#  define O_HOST_LINUX   1
#endif

static inline TargetOS target_host_os(void) {
#if O_HOST_WINDOWS
    return TARGET_OS_WINDOWS;
#else
    return TARGET_OS_LINUX;
#endif
}

typedef enum { ABI_SYSV, ABI_WINDOWS } CallingConvention;
static inline CallingConvention target_abi(const Target *t) {
    return target_is_windows(t) ? ABI_WINDOWS : ABI_SYSV;
}
