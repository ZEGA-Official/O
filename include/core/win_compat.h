// ============================================================
//  O Language Compiler — include/core/win_compat.h
//  Windows (MinGW) portability shim
//  Replaces Linux-only APIs with Windows equivalents
//  Z-TEAM | C23
// ============================================================
#pragma once
#ifdef _WIN32

#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <direct.h>

// ── mmap / munmap ─────────────────────────────────────────────
#define PROT_READ    0x01
#define PROT_WRITE   0x02
#define PROT_EXEC    0x04
#define MAP_PRIVATE  0x02
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED   ((void*)-1)

static inline void *mmap(void *addr, size_t len,
                          int prot, int flags, int fd, long off) {
    (void)addr; (void)flags; (void)fd; (void)off;
    DWORD protect = PAGE_NOACCESS;
    DWORD access  = 0;
    if ((prot & PROT_READ)  && (prot & PROT_WRITE) && (prot & PROT_EXEC))
        { protect = PAGE_EXECUTE_READWRITE; access = FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE; }
    else if ((prot & PROT_READ) && (prot & PROT_EXEC))
        { protect = PAGE_EXECUTE_READ; access = FILE_MAP_READ | FILE_MAP_EXECUTE; }
    else if ((prot & PROT_READ) && (prot & PROT_WRITE))
        { protect = PAGE_READWRITE; access = FILE_MAP_ALL_ACCESS; }
    else if (prot & PROT_READ)
        { protect = PAGE_READONLY; access = FILE_MAP_READ; }

    void *p = VirtualAlloc(NULL, len, MEM_COMMIT | MEM_RESERVE, protect);
    return p ? p : MAP_FAILED;
}

static inline int munmap(void *addr, size_t len) {
    (void)len;
    return VirtualFree(addr, 0, MEM_RELEASE) ? 0 : -1;
}

static inline int mprotect(void *addr, size_t len, int prot) {
    DWORD old, np = PAGE_NOACCESS;
    if ((prot & PROT_READ) && (prot & PROT_WRITE) && (prot & PROT_EXEC))
        np = PAGE_EXECUTE_READWRITE;
    else if ((prot & PROT_READ) && (prot & PROT_EXEC))
        np = PAGE_EXECUTE_READ;
    else if ((prot & PROT_READ) && (prot & PROT_WRITE))
        np = PAGE_READWRITE;
    else if (prot & PROT_READ)
        np = PAGE_READONLY;
    return VirtualProtect(addr, len, np, &old) ? 0 : -1;
}

// ── dlsym / RTLD_DEFAULT ──────────────────────────────────────
#define RTLD_DEFAULT ((void*)0)
static inline void *dlsym(void *handle, const char *name) {
    (void)handle;
    static const char *crts[] = {
        "msvcrt.dll", "ucrtbase.dll", "kernel32.dll",
        "user32.dll", "ntdll.dll", NULL
    };
    for (int i = 0; crts[i]; i++) {
        HMODULE h = GetModuleHandleA(crts[i]);
        if (!h) h = LoadLibraryA(crts[i]);
        if (!h) continue;
        void *p = (void*)(uintptr_t)GetProcAddress(h, name);
        if (p) return p;
    }
    return NULL;
}

// ── POSIX file I/O compat ─────────────────────────────────────
#ifndef O_CREAT
#  define O_CREAT  _O_CREAT
#  define O_TRUNC  _O_TRUNC
#  define O_WRONLY _O_WRONLY
#  define O_RDONLY _O_RDONLY
#endif
#define open    _open
#define close   _close
#define write   _write
#define read    _read
#define lseek   _lseek
#define unlink  _unlink
#define chmod(p,m) (0)
#define fstat   _fstat
#define fileno  _fileno
#define stat    _stat
typedef struct _stat os_stat_t;
#define mkdir(p,m)  _mkdir(p)

// ── clock_gettime – only define if not already provided ──────
// On MinGW, the system provides clock_gettime in pthread_time.h
#if !defined(__MINGW32__)
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#ifndef _TIMESPEC_DEFINED
#define _TIMESPEC_DEFINED
struct timespec { long tv_sec; long tv_nsec; };
#endif
static inline int clock_gettime(int clk, struct timespec *tp) {
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    tp->tv_sec  = (long)(cnt.QuadPart / freq.QuadPart);
    tp->tv_nsec = (long)((cnt.QuadPart % freq.QuadPart) * 1000000000LL / freq.QuadPart);
    return 0;
}
#endif
#endif // !__MINGW32__

// ── Process stubs – avoid conflicting with MinGW built-in ────
#ifndef _PID_T_
#define _PID_T_
typedef int pid_t;
#endif
#if !defined(__MINGW32__) && !defined(fork)
static inline pid_t fork(void) { return -1; }
#endif

// ── sys/wait.h – minimal stubs ───────────────────────────────
#define WEXITSTATUS(s) (s)
static inline int waitpid(int pid, int *st, int opts) {
    (void)pid; (void)st; (void)opts; return -1;
}

// ── ANSI console ─────────────────────────────────────────────
static inline void win_enable_ansi(void) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode; GetConsoleMode(h, &mode);
    SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    h = GetStdHandle(STD_ERROR_HANDLE);
    GetConsoleMode(h, &mode);
    SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

#endif // _WIN32