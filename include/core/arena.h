// ============================================================
//  O Language Compiler — o_arena.h
//  High-performance arena allocator
//  Z-TEAM | C23
// ============================================================
#pragma once
#include "core/common.h"
#ifndef _WIN32
#include <sys/mman.h>
#endif

typedef struct ArenaChunk ArenaChunk;
struct ArenaChunk {
    ArenaChunk *prev;
    usize       cap;
    usize       used;
    u8          data[];
};

typedef struct {
    ArenaChunk *curr;
    usize       total_allocated;
    usize       chunk_size;
} Arena;

#define ARENA_DEFAULT_CHUNK MB(8)

static inline ArenaChunk *arena_chunk_new(usize min_size) {
    usize cap = MAX(min_size, ARENA_DEFAULT_CHUNK);
    usize total = sizeof(ArenaChunk) + cap;
#ifdef _WIN32
    void *mem = VirtualAlloc(NULL, total, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (UNLIKELY(!mem)) return NULL;
#else
    void *mem = mmap(NULL, total, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (UNLIKELY(mem == MAP_FAILED)) return NULL;
#endif
    ArenaChunk *c = (ArenaChunk *)mem;
    *c = (ArenaChunk){.prev = NULL, .cap = cap, .used = 0};
    return c;
}

static inline void arena_chunk_free(ArenaChunk *c) {
    while (c) {
        ArenaChunk *prev = c->prev;
#ifdef _WIN32
        VirtualFree(c, 0, MEM_RELEASE);
#else
        munmap(c, sizeof(ArenaChunk) + c->cap);
#endif
        c = prev;
    }
}

static inline bool arena_init(Arena *a, usize chunk_size) {
    *a = (Arena){.chunk_size = chunk_size ? chunk_size : ARENA_DEFAULT_CHUNK};
    a->curr = arena_chunk_new(a->chunk_size);
    return a->curr != NULL;
}

static inline void arena_destroy(Arena *a) {
    arena_chunk_free(a->curr);
    *a = (Arena){0};
}

HOT static inline void *arena_alloc_aligned(Arena * restrict a,
                                             usize size, usize align) {
    O_ASSERT(IS_POW2(align), "alignment must be power-of-2");
    ArenaChunk *c = a->curr;
    uptr base  = (uptr)(c->data + c->used);
    usize pad  = (usize)((-(i64)base) & (align - 1));
    usize need = pad + size;

    if (UNLIKELY(c->used + need > c->cap)) {
        ArenaChunk *nc = arena_chunk_new(MAX(size + align, a->chunk_size));
        if (UNLIKELY(!nc)) return NULL;
        nc->prev = c;
        a->curr  = nc;
        c        = nc;
        base = (uptr)(c->data);
        pad  = (usize)((-(i64)base) & (align - 1));
        need = pad + size;
    }

    void *ptr = c->data + c->used + pad;
    c->used  += need;
    a->total_allocated += need;
    return ptr;
}

#define ARENA_ALLOC(arena, T) \
    ((T *)arena_alloc_aligned((arena), sizeof(T), _Alignof(T)))

#define ARENA_ALLOC_N(arena, T, n) \
    ((T *)arena_alloc_aligned((arena), sizeof(T) * (n), _Alignof(T)))

#define ARENA_ALLOC_ZERO(arena, T) \
    ((T *)memset(ARENA_ALLOC(arena, T), 0, sizeof(T)))

#define ARENA_ALLOC_N_ZERO(arena, T, n) \
    ((T *)memset(ARENA_ALLOC_N(arena, T, n), 0, sizeof(T) * (n)))

static inline char *arena_strdup(Arena *a, const char *s) {
    usize len = strlen(s) + 1;
    char *dst = arena_alloc_aligned(a, len, 1);
    if (dst) memcpy(dst, s, len);
    return dst;
}

static inline char *arena_strndup(Arena *a, const char *s, usize len) {
    char *dst = arena_alloc_aligned(a, len + 1, 1);
    if (dst) { memcpy(dst, s, len); dst[len] = '\0'; }
    return dst;
}

static inline StrView arena_sv_intern(Arena *a, StrView sv) {
    char *p = arena_strndup(a, sv.ptr, sv.len);
    return (StrView){.ptr = p, .len = sv.len};
}

typedef struct { ArenaChunk *chunk; usize used; } ArenaMark;

static inline ArenaMark arena_mark(Arena *a) {
    return (ArenaMark){.chunk = a->curr, .used = a->curr->used};
}

static inline void arena_restore(Arena *a, ArenaMark m) {
    while (a->curr != m.chunk) {
        ArenaChunk *c = a->curr;
        a->curr = c->prev;
#ifdef _WIN32
        VirtualFree(c, 0, MEM_RELEASE);
#else
        munmap(c, sizeof(ArenaChunk) + c->cap);
#endif
    }
    a->curr->used = m.used;
}

#define DARRAY(T) struct { T *data; u32 len; u32 cap; Arena *_arena; }

#define DA_INIT(da, arena) do { \
    (da)->data   = NULL;        \
    (da)->len    = 0;           \
    (da)->cap    = 0;           \
    (da)->_arena = (arena);     \
} while (0)

#define DA_PUSH(da, val) do {                                            \
    if ((da)->len == (da)->cap) {                                        \
        u32 nc = (da)->cap ? (da)->cap * 2 : 8;                         \
        void *nb = arena_alloc_aligned((da)->_arena,                     \
                    nc * sizeof(*(da)->data),                            \
                    _Alignof(typeof(*(da)->data)));                      \
        if ((da)->data && (da)->len)                                     \
            memcpy(nb, (da)->data, (da)->len * sizeof(*(da)->data));     \
        (da)->data = nb;                                                 \
        (da)->cap  = nc;                                                 \
    }                                                                    \
    (da)->data[(da)->len++] = (val);                                     \
} while (0)

#define DA_LAST(da)  ((da)->data[(da)->len - 1])
#define DA_AT(da, i) ((da)->data[(i)])

static inline Arena *arena_new(usize chunk_size) {
    Arena *a = (Arena *)malloc(sizeof(Arena));
    if (!a) return NULL;
    arena_init(a, chunk_size);
    return a;
}

static inline void arena_free(Arena *a) {
    if (!a) return;
    arena_destroy(a);
    free(a);
}

static inline void *arena_alloc(Arena *a, usize size, usize align) {
    return arena_alloc_aligned(a, size, align);
}
