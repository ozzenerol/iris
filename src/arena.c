/* Arenas and growing strings. */
#include "iris.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ArenaBlock {
    ArenaBlock *next;
    size_t used, cap;
    size_t pad;             /* keeps data 16-byte aligned */
    char data[];
};

#define BLOCK_SIZE (64 * 1024)

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
        size_t cap = size > BLOCK_SIZE ? size : BLOCK_SIZE;
        b = xmalloc(sizeof(ArenaBlock) + cap);
        b->next = a->head;
        b->used = 0;
        b->cap = cap;
        a->head = b;
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
        free(b);
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
