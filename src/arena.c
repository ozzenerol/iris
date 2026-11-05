/* Arenas and growing strings. */
#define _DEFAULT_SOURCE           /* MAP_ANONYMOUS, madvise */
#include "iris.h"

#include <sys/mman.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ArenaBlock {
    ArenaBlock *next;
    size_t used, cap;
    size_t mapped;          /* the bytes of its mmap, 0 when malloc gave it; also keeps data 16-byte aligned */
    char data[];
};

/* The first block has 64 KB, each next one twice as much, up to 32 MB. A
 * block of 2 MB or more comes from mmap on a 2 MB boundary with huge
 * pages: a big program (a 20000-line file has 55 MB of nodes) then takes
 * some hundred page faults in place of 14000. */
#define BLOCK_SIZE (64 * 1024)
#define BLOCK_MAX (32 * 1024 * 1024)
#define HUGE_PAGE (2 * 1024 * 1024)

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) {
        fputs("iris: out of memory\n", stderr);
        exit(2);
    }
    return p;
}

void *arenaAlloc(Arena *a, size_t size) {
    size = (size + 15) & ~(size_t)15;
    ArenaBlock *b = a->head;
    if (!b || b->used + size > b->cap) {
        size_t grow = b ? b->cap * 2 : BLOCK_SIZE;
        if (grow > BLOCK_MAX) grow = BLOCK_MAX;
        size_t cap = size > grow ? size : grow;
        ArenaBlock *nb = NULL;
        size_t mapped = 0;
        if (cap >= HUGE_PAGE) {
            mapped = (sizeof(ArenaBlock) + cap + HUGE_PAGE - 1) & ~(size_t)(HUGE_PAGE - 1);
            char *m = mmap(NULL, mapped + HUGE_PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (m != MAP_FAILED) {
                char *start = (char *)(((uintptr_t)m + HUGE_PAGE - 1) & ~(uintptr_t)(HUGE_PAGE - 1));
                if (start > m) munmap(m, (size_t)(start - m));
                if (start + mapped < m + mapped + HUGE_PAGE) munmap(start + mapped, (size_t)(m + HUGE_PAGE - start));
#ifdef MADV_HUGEPAGE
                madvise(start, mapped, MADV_HUGEPAGE);
#endif
                nb = (ArenaBlock *)start;
                cap = mapped - sizeof(ArenaBlock);
            } else {
                mapped = 0;
            }
        }
        if (!nb) nb = xmalloc(sizeof(ArenaBlock) + cap);
        nb->next = b;
        nb->used = 0;
        nb->cap = cap;
        nb->mapped = mapped;
        a->head = b = nb;
    }
    void *p = b->data + b->used;
    b->used += size;
    memset(p, 0, size);
    return p;
}

char *arenaStrndup(Arena *a, const char *s, size_t n) {
    char *p = arenaAlloc(a, n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

void arenaFree(Arena *a) {
    ArenaBlock *b = a->head;
    while (b) {
        ArenaBlock *next = b->next;
        if (b->mapped) munmap(b, b->mapped);
        else free(b);
        b = next;
    }
    a->head = NULL;
}

static void sbGrow(StrBuf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) {
        return;
    }
    size_t cap = b->cap ? b->cap * 2 : 64;
    while (cap < b->len + extra + 1) {
        cap *= 2;
    }
    char *p = realloc(b->buf, cap);
    if (!p) {
        fputs("iris: out of memory\n", stderr);
        exit(2);
    }
    b->buf = p;
    b->cap = cap;
}

void sbPutn(StrBuf *b, const char *s, size_t n) {
    sbGrow(b, n);
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = 0;
}

void sbPutc(StrBuf *b, char c) { sbPutn(b, &c, 1); }

void sbPuts(StrBuf *b, const char *s) { sbPutn(b, s, strlen(s)); }

void sbPrintf(StrBuf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char tmp[256];
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if ((size_t)n < sizeof tmp) {
        sbPutn(b, tmp, (size_t)n);
        return;
    }
    sbGrow(b, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(b->buf + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}

char *sbFinish(StrBuf *b, Arena *a) {
    char *s = arenaStrndup(a, b->buf ? b->buf : "", b->len);
    sbFree(b);
    return s;
}

void sbFree(StrBuf *b) {
    free(b->buf);
    b->buf = NULL;
    b->len = b->cap = 0;
}
