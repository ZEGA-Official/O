// ============================================================
//  O Language Compiler — o_common.h
//  Foundation types, macros, arena, result
//  Z-TEAM | C23 | x86-64 | JIT + AOT
// ============================================================
#pragma once

// Windows portability — must come before any POSIX includes
#ifdef _WIN32
#  include "core/win_compat.h"
#endif


#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>

// ── Primitive aliases ────────────────────────────────────────
typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;
typedef int8_t    i8;
typedef int16_t   i16;
typedef int32_t   i32;
typedef int64_t   i64;
typedef float     f32;
typedef double    f64;
typedef uintptr_t uptr;
typedef ptrdiff_t isize;
typedef size_t    usize;

// ── Compiler hints ───────────────────────────────────────────
#define HOT       __attribute__((hot))
#define COLD      __attribute__((cold))
#define NORETURN  __attribute__((noreturn))
#define PACKED    __attribute__((packed))
#define ALIGNED(n) __attribute__((aligned(n)))
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#define UNREACHABLE() __builtin_unreachable()
#define UNUSED(x)   ((void)(x))

// ── Compile-time utilities ───────────────────────────────────
#define ARRAY_LEN(a)  (sizeof(a) / sizeof((a)[0]))
#define MAX(a, b)     ((a) > (b) ? (a) : (b))
#define MIN(a, b)     ((a) < (b) ? (a) : (b))
#define CLAMP(v,lo,hi) MIN(MAX((v),(lo)),(hi))
#define ALIGN_UP(v,a) (((v) + (a) - 1) & ~((a) - 1))
#define ALIGN_DOWN(v,a) ((v) & ~((a) - 1))
#define IS_POW2(v)    ((v) && !((v) & ((v) - 1)))
#define KB(n)         ((usize)(n) * 1024ULL)
#define MB(n)         ((usize)(n) * 1024ULL * 1024ULL)

// ── Version & branding ───────────────────────────────────────
#define O_LANG_VERSION_MAJOR 1
#define O_LANG_VERSION_MINOR 0
#define O_LANG_VERSION_PATCH 0
#define O_LANG_VERSION_STR  "1.0.0"
#define O_LANG_BANNER \
    "\033[38;2;88;240;27m" \
    "  ██████╗     ██╗      █████╗ ███╗   ██╗ ██████╗\n" \
    " ██╔═══██╗    ██║     ██╔══██╗████╗  ██║██╔════╝\n" \
    " ██║   ██║    ██║     ███████║██╔██╗ ██║██║  ███╗\n" \
    " ██║   ██║    ██║     ██╔══██║██║╚██╗██║██║   ██║\n" \
    " ╚██████╔╝    ███████╗██║  ██║██║ ╚████║╚██████╔╝\n" \
    "  ╚═════╝     ╚══════╝╚═╝  ╚═╝╚═╝  ╚═══╝ ╚═════╝\n" \
    "\033[0m" \
    "  The O Programming Language v" O_LANG_VERSION_STR "\n" \
    "  JIT + AOT | x86-64 | C23 Backend\n" \
    "  Z-TEAM Systems\n\n"

// ── Source location ──────────────────────────────────────────
typedef struct {
    const char *file;
    u32         line;
    u32         col;
} SrcLoc;

static inline SrcLoc srcloc_invalid(void) {
    return (SrcLoc){.file = "<unknown>", .line = 0, .col = 0};
}

// ── String view (non-owning) ─────────────────────────────────
typedef struct {
    const char *ptr;
    usize       len;
} StrView;

static inline StrView sv_from_cstr(const char *s) {
    return (StrView){.ptr = s, .len = strlen(s)};
}

static inline bool sv_eq(StrView a, StrView b) {
    return a.len == b.len && memcmp(a.ptr, b.ptr, a.len) == 0;
}

static inline bool sv_eq_cstr(StrView a, const char *b) {
    return sv_eq(a, sv_from_cstr(b));
}

static inline void sv_print(StrView sv) {
    fwrite(sv.ptr, 1, sv.len, stdout);
}

// ── Result type ──────────────────────────────────────────────
typedef enum {
    O_OK    = 0,
    O_ERR   = 1,
    O_OOM   = 2,
    O_EPARSE= 3,
    O_ESEMA = 4,
    O_ECODEGEN = 5,
} OStatus;

typedef struct {
    OStatus     status;
    const char *msg;
    SrcLoc      loc;
} OResult;

static inline OResult o_ok(void) {
    return (OResult){.status = O_OK};
}

static inline OResult o_err(OStatus s, const char *msg, SrcLoc loc) {
    return (OResult){.status = s, .msg = msg, .loc = loc};
}

#define O_TRY(expr)                  \
    do {                             \
        OResult _r = (expr);         \
        if (_r.status != O_OK)       \
            return _r;               \
    } while (0)

// ── Diagnostic output ────────────────────────────────────────
#define O_COLOR_ERROR   "\033[1;31m"
#define O_COLOR_WARN    "\033[1;33m"
#define O_COLOR_NOTE    "\033[1;36m"
#define O_COLOR_BOLD    "\033[1m"
#define O_COLOR_GREEN   "\033[38;2;88;240;27m"
#define O_COLOR_RESET   "\033[0m"

#define o_diag_error(loc, ...) do { \
    fprintf(stderr, O_COLOR_ERROR "error" O_COLOR_RESET O_COLOR_BOLD \
            " [%s:%u:%u]: " O_COLOR_RESET, (loc).file, (loc).line, (loc).col); \
    fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr); \
} while(0)

#define o_diag_warn(loc, ...) do { \
    fprintf(stderr, O_COLOR_WARN "warning" O_COLOR_RESET O_COLOR_BOLD \
            " [%s:%u:%u]: " O_COLOR_RESET, (loc).file, (loc).line, (loc).col); \
    fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr); \
} while(0)

// ── Panic ────────────────────────────────────────────────────
NORETURN static inline void o_panic(const char *file, int line, const char *msg) {
    fprintf(stderr, O_COLOR_ERROR "PANIC" O_COLOR_RESET " %s:%d -- %s\n",
            file, line, msg);
    abort();
}

#define O_PANIC(msg)       o_panic(__FILE__, __LINE__, (msg))
#define O_ASSERT(cond,msg) do { if (UNLIKELY(!(cond))) O_PANIC(msg); } while(0)
