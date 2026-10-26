/* The Iris runtime: what every program links with. Printing goes through
 * stdio's stdout buffer, which C flushes when the program ends. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE        /* MAP_ANON and SO_NOSIGPIPE on macOS */
#define _GNU_SOURCE             /* sched_getaffinity on Linux */
#define __APPLE_USE_RFC_3542    /* IPV6_RECVPKTINFO on macOS */
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <pthread.h>
#include <stdatomic.h>

/* ---------------------------------------------------------------- memory

   The program's malloc: each thread takes small blocks from pages of its
   own, so most allocations and frees touch no lock and no other thread's
   memory. Memory is taken from the system in 4 MiB segments of 64 KiB
   pages; each page holds blocks of one size class, up to 32 KiB. Larger
   blocks come from the C library's malloc, and so does everything under
   AddressSanitizer, which needs to see each allocation.

   free knows a block is one of ours from a bitmap of the segments; any
   other pointer (from the C library, from C code the program calls) goes
   to the C library's free. A block freed by another thread goes on its
   page's list of remote frees, which the owner takes back when it needs
   room. A thread that ends leaves its heap to the next thread made. */

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define IRIS_HEAP_OFF 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define IRIS_HEAP_OFF 1
#endif

void *iris_malloc(size_t n);
void *iris_calloc(size_t count, size_t size);
void *iris_realloc(void *p, size_t n);
void iris_free(void *p);

#ifdef IRIS_HEAP_OFF
void *iris_malloc(size_t n) { return malloc(n); }
void *iris_calloc(size_t count, size_t size) { return calloc(count, size); }
void *iris_realloc(void *p, size_t n) { return realloc(p, n); }
void iris_free(void *p) { free(p); }
#else

#define SEG_SHIFT 22                    /* 4 MiB */
#define SEG_SIZE ((size_t)1 << SEG_SHIFT)
#define PAGE_SHIFT 16                   /* 64 KiB */
#define PAGE_SIZE ((size_t)1 << PAGE_SHIFT)
#define SEG_PAGES (SEG_SIZE / PAGE_SIZE)
#define SMALL_MAX 32768
#define NCLASSES 85
#define ADDR_BITS 48

typedef struct Heap Heap;
typedef struct HPage HPage;

struct HPage {
    void *free;                         /* blocks freed by the owner, ready */
    _Atomic(void *) remote;             /* blocks freed by other threads */
    int64_t used;                       /* blocks given out and not taken back */
    uint32_t blockSize, capacity, bumped;
    uint16_t cls;
    uint8_t where;                      /* 0 nowhere, 1 its class's list, 2 the full list, 3 free pages */
    uint8_t zeroed;                     /* the blocks not given out yet are zero: fresh from the system */
    _Atomic int full;
    _Atomic int delayed;                /* on the heap's delayed stack */
    _Atomic(Heap *) heap;
    char *start;
    HPage *next, *prev;
    HPage *delayedNext;
};

typedef struct {
    HPage pages[SEG_PAGES];             /* page 0 holds these and no blocks */
} Segment;

struct Heap {
    HPage *cur[NCLASSES];
    HPage *lists[NCLASSES];             /* pages with room, or that may have it */
    HPage *fullList;
    HPage *freePages;
    int64_t nFree;
    _Atomic(HPage *) delayed;           /* full pages another thread freed into */
    Heap *nextAbandoned;
};

/* one bit per 4 MiB of address space: whether it is one of our segments */
static _Atomic uint64_t segBits[((size_t)1 << (ADDR_BITS - SEG_SHIFT)) / 64];
static pthread_mutex_t heapLock = PTHREAD_MUTEX_INITIALIZER;
static Heap *abandoned;
static pthread_key_t heapKey;
static pthread_once_t heapOnce = PTHREAD_ONCE_INIT;
static _Thread_local Heap *myHeap;

static inline bool ours(const void *p) {
    uintptr_t a = (uintptr_t)p;
    if (a >> ADDR_BITS) return false;
    size_t i = a >> SEG_SHIFT;
    return (atomic_load_explicit(&segBits[i / 64], memory_order_relaxed) >> (i % 64)) & 1;
}

static inline HPage *pageOf(void *p) {
    Segment *seg = (Segment *)((uintptr_t)p & ~(SEG_SIZE - 1));
    return &seg->pages[((uintptr_t)p - (uintptr_t)seg) >> PAGE_SHIFT];
}

/* 16-byte steps up to 1 KiB, then four steps for each doubling */
static inline int classOf(size_t n) {
    if (n <= 1024) return n == 0 ? 1 : (int)((n + 15) >> 4);
    int b = 63 - __builtin_clzll((unsigned long long)(n - 1));
    int sub = (int)(((n - 1) >> (b - 2)) & 3);
    return 64 + (b - 10) * 4 + sub + 1;
}

static inline size_t classSize(int cls) {
    if (cls <= 64) return (size_t)cls << 4;
    int k = cls - 65;
    int b = 10 + k / 4, sub = k % 4;
    return (size_t)(4 + sub + 1) << (b - 2);
}

static void heapExit(void *h) {
    pthread_mutex_lock(&heapLock);
    ((Heap *)h)->nextAbandoned = abandoned;
    abandoned = h;
    pthread_mutex_unlock(&heapLock);
}

static void heapKeyMake(void) { pthread_key_create(&heapKey, heapExit); }

static Heap *heapNew(void) {
    pthread_once(&heapOnce, heapKeyMake);
    pthread_mutex_lock(&heapLock);
    Heap *h = abandoned;
    if (h) {
        abandoned = h->nextAbandoned;
    }
    pthread_mutex_unlock(&heapLock);
    if (!h) {
        h = calloc(1, sizeof(Heap));
        if (!h) return NULL;
    }
    myHeap = h;
    pthread_setspecific(heapKey, h);
    return h;
}

static void listPushPage(HPage **list, HPage *pg, uint8_t where) {
    pg->prev = NULL;
    pg->next = *list;
    if (*list) (*list)->prev = pg;
    *list = pg;
    pg->where = where;
}

static void listUnlinkPage(Heap *h, HPage *pg) {
    HPage **list = pg->where == 1 ? &h->lists[pg->cls] : pg->where == 2 ? &h->fullList : &h->freePages;
    if (pg->prev) pg->prev->next = pg->next; else *list = pg->next;
    if (pg->next) pg->next->prev = pg->prev;
    pg->next = pg->prev = NULL;
    pg->where = 0;
}

/* A new segment: its pages go to the heap's free pages. */
static bool heapGrow(Heap *h) {
    char *raw = mmap(NULL, SEG_SIZE * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (raw == MAP_FAILED) return false;
    char *seg = (char *)(((uintptr_t)raw + SEG_SIZE - 1) & ~(SEG_SIZE - 1));
    if (seg > raw) munmap(raw, (size_t)(seg - raw));
    if (seg + SEG_SIZE < raw + SEG_SIZE * 2) munmap(seg + SEG_SIZE, (size_t)(raw + SEG_SIZE * 2 - (seg + SEG_SIZE)));
    if ((uintptr_t)seg >> ADDR_BITS) {
        munmap(seg, SEG_SIZE);
        return false;
    }
    Segment *s = (Segment *)seg;
    for (size_t i = SEG_PAGES - 1; i >= 1; i--) {
        HPage *pg = &s->pages[i];
        pg->start = seg + i * PAGE_SIZE;
        pg->zeroed = 1;
        atomic_store(&pg->heap, h);
        listPushPage(&h->freePages, pg, 3);
        h->nFree++;
    }
    size_t i = (uintptr_t)seg >> SEG_SHIFT;
    atomic_fetch_or(&segBits[i / 64], (uint64_t)1 << (i % 64));
    return true;
}

/* Takes back the blocks other threads freed. */
static void collectRemote(HPage *pg) {
    void *r = atomic_exchange(&pg->remote, NULL);
    while (r) {
        void *next = *(void **)r;
        *(void **)r = pg->free;
        pg->free = r;
        pg->used--;
        r = next;
    }
}

/* A page with no blocks in use goes back to the free pages; past 64 of
   them, its memory goes back to the system too. */
static void retirePage(Heap *h, HPage *pg) {
    if (h->cur[pg->cls] == pg || atomic_load(&pg->delayed)) return;
    if (pg->where) listUnlinkPage(h, pg);
    atomic_store(&pg->full, 0);
    pg->free = NULL;
    pg->bumped = 0;
    pg->zeroed = 0;
    if (h->nFree >= 64) {
        madvise(pg->start, PAGE_SIZE, MADV_DONTNEED);
        pg->zeroed = 1;
    }
    listPushPage(&h->freePages, pg, 3);
    h->nFree++;
}

static bool pageHasRoom(HPage *pg) {
    if (pg->free || pg->bumped < pg->capacity) return true;
    collectRemote(pg);
    return pg->free != NULL;
}

/* The page is out of room: off to the full list, unless another thread
   freed into it meanwhile. */
static void markFull(Heap *h, HPage *pg) {
    if (pg->where == 1) listUnlinkPage(h, pg);
    listPushPage(&h->fullList, pg, 2);
    atomic_store(&pg->full, 1);
    if (atomic_load(&pg->remote)) {
        atomic_store(&pg->full, 0);
        listUnlinkPage(h, pg);
        listPushPage(&h->lists[pg->cls], pg, 1);
    }
}

static void drainDelayed(Heap *h) {
    HPage *pg = atomic_exchange(&h->delayed, NULL);
    while (pg) {
        HPage *next = pg->delayedNext;
        atomic_store(&pg->delayed, 0);
        if (atomic_load(&pg->full) && pg->where == 2) {
            atomic_store(&pg->full, 0);
            listUnlinkPage(h, pg);
            listPushPage(&h->lists[pg->cls], pg, 1);
        }
        pg = next;
    }
}

/* whether the last block taken was zero already: never given out, on a
   page fresh from the system */
static _Thread_local bool tookZeroed;

static void *takeBlock(HPage *pg) {
    void *b = pg->free;
    if (b) {
        pg->free = *(void **)b;
        tookZeroed = false;
    } else {
        b = pg->start + (size_t)pg->bumped * pg->blockSize;
        pg->bumped++;
        tookZeroed = pg->zeroed;
    }
    pg->used++;
    return b;
}

static void *mallocSlow(Heap *h, int cls) {
    drainDelayed(h);
    HPage *pg = h->cur[cls];
    if (pg && pageHasRoom(pg)) return takeBlock(pg);
    if (pg) {
        h->cur[cls] = NULL;
        markFull(h, pg);
    }
    /* another page of the class with room */
    for (HPage *p = h->lists[cls]; p;) {
        HPage *next = p->next;
        if (pageHasRoom(p)) {
            h->cur[cls] = p;
            return takeBlock(p);
        }
        markFull(h, p);
        p = next;
    }
    if (!h->freePages && !heapGrow(h)) return NULL;
    pg = h->freePages;
    listUnlinkPage(h, pg);
    h->nFree--;
    pg->cls = (uint16_t)cls;
    pg->blockSize = (uint32_t)classSize(cls);
    pg->capacity = (uint32_t)(PAGE_SIZE / pg->blockSize);
    pg->bumped = 0;
    pg->free = NULL;
    pg->used = 0;
    atomic_store(&pg->full, 0);
    listPushPage(&h->lists[cls], pg, 1);
    h->cur[cls] = pg;
    return takeBlock(pg);
}

void *iris_malloc(size_t n) {
    if (n > SMALL_MAX) return malloc(n);
    Heap *h = myHeap;
    if (!h && !(h = heapNew())) return malloc(n);
    int cls = classOf(n);
    HPage *pg = h->cur[cls];
    if (pg && pg->free) {
        void *b = pg->free;
        pg->free = *(void **)b;
        pg->used++;
        return b;
    }
    void *b = mallocSlow(h, cls);
    return b ? b : malloc(n);
}

void iris_free(void *p) {
    if (!p) return;
    if (!ours(p)) {
        free(p);
        return;
    }
    HPage *pg = pageOf(p);
    Heap *owner = atomic_load_explicit(&pg->heap, memory_order_relaxed);
    if (owner == myHeap) {
        *(void **)p = pg->free;
        pg->free = p;
        pg->used--;
        if (pg->where == 2) {
            /* room again: back to its class's list */
            atomic_store(&pg->full, 0);
            listUnlinkPage(owner, pg);
            listPushPage(&owner->lists[pg->cls], pg, 1);
        }
        if (pg->used == 0) retirePage(owner, pg);
        return;
    }
    void *head = atomic_load(&pg->remote);
    do {
        *(void **)p = head;
    } while (!atomic_compare_exchange_weak(&pg->remote, &head, p));
    if (atomic_load(&pg->full)) {
        int no = 0;
        if (atomic_compare_exchange_strong(&pg->delayed, &no, 1)) {
            HPage *top = atomic_load(&owner->delayed);
            do {
                pg->delayedNext = top;
            } while (!atomic_compare_exchange_weak(&owner->delayed, &top, pg));
        }
    }
}

void *iris_calloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) return NULL;
    size_t n = count * size;
    if (n > SMALL_MAX) return calloc(count, size);
    Heap *h = myHeap;
    if (!h && !(h = heapNew())) return calloc(count, size);
    int cls = classOf(n);
    HPage *pg = h->cur[cls];
    void *p;
    if (pg && pg->free) {
        p = pg->free;
        pg->free = *(void **)p;
        pg->used++;
    } else {
        p = mallocSlow(h, cls);
        if (!p) return calloc(count, size);
        if (tookZeroed) {
            return p;   /* untouched memory: zeroing it would only fault its pages in */
        }
    }
    memset(p, 0, n);
    return p;
}

void *iris_realloc(void *p, size_t n) {
    if (!p) return iris_malloc(n);
    if (!ours(p)) return realloc(p, n);
    size_t have = pageOf(p)->blockSize;
    if (n <= have && n > have / 2) return p;
    void *q = iris_malloc(n);
    if (!q) return NULL;
    memcpy(q, p, n < have ? n : have);
    iris_free(p);
    return q;
}
#endif

/* everything below allocates with the program's malloc */
#define malloc(n) iris_malloc(n)
#define calloc(c, n) iris_calloc(c, n)
#define realloc(p, n) iris_realloc(p, n)
#define free(p) iris_free(p)

typedef struct {
    char *ptr;
    int64_t len, cap;
} IrisString;

/* Debug builds use AddressSanitizer. Leaks are not reported (yet). */
const char *__asan_default_options(void) {
    return "detect_leaks=0:exitcode=2:abort_on_error=0:detect_stack_use_after_return=0";
}

static _Thread_local char panicBuf[4096];
static _Thread_local size_t panicLen;

/* The names of every enum's values, written by the compiler. */
typedef struct {
    const char *name;
    int64_t count;
    const int64_t *values;
    const char **names;
} IrisEnum;

extern const IrisEnum iris_enum_table[];
extern const int64_t iris_enum_count;

/* The current error's message and trace, per thread. A new error's
 * message is written to `next` first, so it can quote the current one. */
#define MSG_MAX 1024
#define TRACE_MAX 64
typedef struct {
    const char *file, *fn;
    int line, col;
} Frame;
static _Thread_local char msgCur[MSG_MAX], msgNext[MSG_MAX];
static _Thread_local size_t msgCurLen, msgNextLen;
static _Thread_local Frame trace[TRACE_MAX];
static _Thread_local int traceLen, traceDropped;

static int gArgc;
static char **gArgv;

void iris_init(int argc, char **argv) {
    gArgc = argc;
    gArgv = argv;
}

static void stop(const char *msg, size_t n, const char *file, int line, int col, const char *fn) {
    fflush(stdout);
    fprintf(stderr, "panic: %.*s\n  at %s:%d:%d in %s\n", (int)n, msg, file, line, col, fn);
    exit(2);
}

static void outOfMemory(void) {
    fflush(stdout);
    fputs("panic: out of memory\n", stderr);
    exit(2);
}

/* the program's own calloc calls (ir.c, emitCalloc) end here when it fails */
void iris_out_of_memory(void) {
    outOfMemory();
}

/* Makes room for n more bytes. p may point into s itself (s.append(s[2:]))
   and moves with the buffer. */
__attribute__((noinline)) static void stringGrow(IrisString *s, const char **p, int64_t n) {
    /* doubles while small; past 1 MiB grows by half, so a big String
       keeps less unused room (realloc moves big blocks without copying) */
    int64_t cap = s->cap ? s->cap : 16;
    while (cap < s->len + n) {
        cap = cap < (1 << 20) ? cap * 2 : cap + cap / 2;
    }
    bool inside = s->ptr && *p >= s->ptr && *p < s->ptr + s->len;
    int64_t at = inside ? *p - s->ptr : 0;
    char *q;
    if (s->cap == 0 && s->ptr) {
        /* borrowed text, such as a field's default: copied, not moved */
        q = malloc((size_t)cap);
        if (q) {
            memcpy(q, s->ptr, (size_t)s->len);
        }
    } else {
        q = realloc(s->ptr, (size_t)cap);
    }
    if (!q) {
        outOfMemory();
    }
    s->ptr = q;
    s->cap = cap;
    if (inside) {
        *p = q + at;
    }
}

/* Short pieces, the usual ones in text being built, are copied with a
   few moves instead of a call to memcpy. */
void iris_string_append(IrisString *s, const char *p, int64_t n) {
    if (n <= 0) {
        return;
    }
    if (__builtin_expect(s->len + n > s->cap, 0)) {
        stringGrow(s, &p, n);
    }
    char *d = s->ptr + s->len;
    if (n >= 8 && n <= 16) {
        uint64_t a, b;
        __builtin_memcpy(&a, p, 8);
        __builtin_memcpy(&b, p + n - 8, 8);
        __builtin_memcpy(d, &a, 8);
        __builtin_memcpy(d + n - 8, &b, 8);
    } else if (n >= 4 && n < 8) {
        uint32_t a, b;
        __builtin_memcpy(&a, p, 4);
        __builtin_memcpy(&b, p + n - 4, 4);
        __builtin_memcpy(d, &a, 4);
        __builtin_memcpy(d + n - 4, &b, 4);
    } else if (n < 4) {
        d[0] = p[0];
        if (n > 1) {
            d[1] = p[1];
            if (n > 2) d[2] = p[2];
        }
    } else {
        memcpy(d, p, (size_t)n);
    }
    s->len += n;
}

/* s.reserve(n): room for n more bytes, so appends up to that don't grow it. */
void iris_string_reserve(IrisString *s, int64_t n) {
    if (n > 0 && s->len + n > s->cap) {
        char *q;
        if (s->cap == 0 && s->ptr) {
            q = malloc((size_t)(s->len + n));
            if (q) {
                memcpy(q, s->ptr, (size_t)s->len);
            }
        } else {
            q = realloc(s->ptr, (size_t)(s->len + n));
        }
        if (!q) {
            outOfMemory();
        }
        s->ptr = q;
        s->cap = s->len + n;
    }
}

/* s.truncate(n): the first n bytes are kept. */
void iris_string_truncate(IrisString *s, int64_t n, const char *file, int line, int col, const char *fn) {
    if (n < 0 || n > s->len) {
        char msg[96];
        int k = snprintf(msg, sizeof msg, "truncate to %lld bytes of a String of %lld", (long long)n, (long long)s->len);
        stop(msg, (size_t)k, file, line, col, fn);
    }
    s->len = n;
}

/* s.insert(at, text): the text put in before byte at; the text may be part
   of s itself. */
void iris_string_insert(IrisString *s, int64_t at, const char *p, int64_t n, const char *file, int line, int col,
                        const char *fn) {
    if (at < 0 || at > s->len) {
        char msg[96];
        int k = snprintf(msg, sizeof msg, "insert at byte %lld of a String of %lld", (long long)at, (long long)s->len);
        stop(msg, (size_t)k, file, line, col, fn);
    }
    if (n <= 0) {
        return;
    }
    char small[256];
    char *copy = NULL;
    if (s->ptr && p >= s->ptr && p < s->ptr + s->len) {
        copy = n <= (int64_t)sizeof small ? small : malloc((size_t)n);
        if (!copy) outOfMemory();
        memcpy(copy, p, (size_t)n);
        p = copy;
    }
    if (s->len + n > s->cap) {
        const char *none = NULL;
        stringGrow(s, &none, n);
    }
    memmove(s->ptr + at + n, s->ptr + at, (size_t)(s->len - at));
    memcpy(s->ptr + at, p, (size_t)n);
    s->len += n;
    if (copy && copy != small) free(copy);
}

/* A String with no room of its own (cap 0) borrows its text, as a field's
 * default does, and has nothing to free. */
void iris_string_free(IrisString *s) {
    if (s->cap) {
        free(s->ptr);
    }
    s->ptr = NULL;
    s->len = s->cap = 0;
}

/* A String that borrows n bytes at p (cap 0): freeing it frees nothing and
 * the first change copies them. For views of a buffer that outlives it. */
void iris_string_borrow(IrisString *s, const char *p, int64_t n) {
    s->ptr = (char *)p;
    s->len = n;
    s->cap = 0;
}

void iris_mem_move(void *dst, const void *src, int64_t n) {
    if (n > 0) memmove(dst, src, (size_t)n);
}

/* sink 1: stdout; 2: the text of a panic; 3: the String at dst; 4: the
 * message of a new error */
static void put(int sink, IrisString *dst, const char *s, size_t n) {
    if (sink == 1) {
        fwrite(s, 1, n, stdout);
    } else if (sink == 3) {
        iris_string_append(dst, s, (int64_t)n);
    } else if (sink == 4) {
        if (n > MSG_MAX - msgNextLen) {
            n = MSG_MAX - msgNextLen;
        }
        memcpy(msgNext + msgNextLen, s, n);
        msgNextLen += n;
    } else {
        if (n > sizeof panicBuf - panicLen) {
            n = sizeof panicBuf - panicLen;
        }
        memcpy(panicBuf + panicLen, s, n);
        panicLen += n;
    }
}

void iris_write_str(int sink, IrisString *dst, const char *s, int64_t n) { put(sink, dst, s, (size_t)n); }

/* The digits of u, written backwards from end; returns where they start.
 * Two digits at a time, from a table. */
static char *u64Digits(uint64_t u, char *end) {
    static const char pairs[] =
        "00010203040506070809101112131415161718192021222324252627282930313233343536373839"
        "40414243444546474849505152535455565758596061626364656667686970717273747576777879"
        "8081828384858687888990919293949596979899";
    char *p = end;
    while (u >= 100) {
        unsigned k = (unsigned)(u % 100) * 2;
        u /= 100;
        *--p = pairs[k + 1];
        *--p = pairs[k];
    }
    if (u >= 10) {
        *--p = pairs[u * 2 + 1];
        *--p = pairs[u * 2];
    } else {
        *--p = (char)('0' + u);
    }
    return p;
}

void iris_write_i64(int sink, IrisString *dst, int64_t v) {
    char buf[32];
    char *end = buf + sizeof buf;
    char *p = u64Digits(v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v, end);
    if (v < 0) {
        *--p = '-';
    }
    put(sink, dst, p, (size_t)(end - p));
}

void iris_write_u64(int sink, IrisString *dst, int64_t v) {
    char buf[32];
    char *end = buf + sizeof buf;
    char *p = u64Digits((uint64_t)v, end);
    put(sink, dst, p, (size_t)(end - p));
}

/* A Duration as people write it: 1.5s, 2h3m0s, 250ms, 12us, 7ns. */
void iris_write_duration(int sink, IrisString *dst, int64_t d) {
    char buf[64];
    int n = 0;
    uint64_t u = d < 0 ? (uint64_t)0 - (uint64_t)d : (uint64_t)d;
    if (d < 0) buf[n++] = '-';
    if (u == 0) {
        put(sink, dst, "0s", 2);
        return;
    }
    /* a number of units with up to 9 decimals, without zeros at the end */
    #define FRAC(whole, part, digits, unit)                                                     \
        do {                                                                                    \
            n += snprintf(buf + n, sizeof buf - (size_t)n, "%llu", (unsigned long long)(whole)); \
            if (part) {                                                                         \
                char f[16];                                                                     \
                snprintf(f, sizeof f, "%0*llu", digits, (unsigned long long)(part));            \
                int fl = digits;                                                                \
                while (fl > 0 && f[fl - 1] == '0') fl--;                                        \
                n += snprintf(buf + n, sizeof buf - (size_t)n, ".%.*s", fl, f);                 \
            }                                                                                   \
            n += snprintf(buf + n, sizeof buf - (size_t)n, "%s", unit);                         \
        } while (0)
    if (u < 1000ULL) {
        FRAC(u, 0, 1, "ns");
    } else if (u < 1000000ULL) {
        FRAC(u / 1000, u % 1000, 3, "us");
    } else if (u < 1000000000ULL) {
        FRAC(u / 1000000, u % 1000000, 6, "ms");
    } else {
        uint64_t s = u / 1000000000ULL, frac = u % 1000000000ULL;
        if (s >= 3600) {
            n += snprintf(buf + n, sizeof buf - (size_t)n, "%lluh", (unsigned long long)(s / 3600));
        }
        if (s >= 60) {
            n += snprintf(buf + n, sizeof buf - (size_t)n, "%llum", (unsigned long long)(s % 3600 / 60));
        }
        FRAC(s % 60, frac, 9, "s");
    }
    #undef FRAC
    put(sink, dst, buf, (size_t)n);
}

/* math.addChecked and the others: op 0 +, 1 -, 2 *. */
int64_t iris_math_checked(int64_t a, int64_t b, int op, bool *ok) {
    long long r;
    bool over = op == 0 ? __builtin_add_overflow(a, b, &r) : op == 1 ? __builtin_sub_overflow(a, b, &r)
                                                                     : __builtin_mul_overflow(a, b, &r);
    *ok = !over;
    return over ? 0 : r;
}

int64_t iris_math_saturate(int64_t a, int64_t b, int op) {
    long long r;
    bool over = op == 0 ? __builtin_add_overflow(a, b, &r) : op == 1 ? __builtin_sub_overflow(a, b, &r)
                                                                     : __builtin_mul_overflow(a, b, &r);
    if (!over) {
        return r;
    }
    /* the true result's sign says which end */
    bool negative = op == 0 ? a < 0 : op == 1 ? a < b : (a < 0) != (b < 0);
    return negative ? INT64_MIN : INT64_MAX;
}

int64_t iris_math_wrap(int64_t a, int64_t b, int op) {
    uint64_t x = (uint64_t)a, y = (uint64_t)b;
    return (int64_t)(op == 0 ? x + y : op == 1 ? x - y : x * y);
}

/* math.mulHigh and math.mulFull: the 128-bit product of two u64s. */
uint64_t iris_math_mul_full(uint64_t a, uint64_t b, uint64_t *lo) {
    __uint128_t p = (__uint128_t)a * b;
    if (lo) *lo = (uint64_t)p;
    return (uint64_t)(p >> 64);
}

/* str.validUtf8: ASCII 8 bytes at a time, then each sequence checked as
   decodeRune does (no overlong forms, surrogates or values past U+10FFFF).
   With count, the characters of valid text are counted on the way: -1 when
   it isn't valid. */
static int64_t utf8ScanScalar(const unsigned char *p, int64_t n, bool count) {
    int64_t i = 0, chars = 0;
    while (i < n) {
        if (i + 8 <= n) {
            uint64_t w;
            memcpy(&w, p + i, 8);
            if (!(w & 0x8080808080808080ULL)) {
                i += 8;
                chars += 8;
                continue;
            }
        }
        unsigned char b = p[i];
        chars++;
        if (b < 0x80) {
            i++;
            continue;
        }
        if (b >= 0xc2 && b <= 0xdf) {
            if (i + 1 >= n || (p[i + 1] & 0xc0) != 0x80) return -1;
            i += 2;
        } else if (b >= 0xe0 && b <= 0xef) {
            if (i + 2 >= n) return -1;
            unsigned char c1 = p[i + 1];
            if ((c1 & 0xc0) != 0x80 || (p[i + 2] & 0xc0) != 0x80) return -1;
            if (b == 0xe0 && c1 < 0xa0) return -1;       /* overlong */
            if (b == 0xed && c1 >= 0xa0) return -1;      /* a surrogate */
            i += 3;
        } else if (b >= 0xf0 && b <= 0xf4) {
            if (i + 3 >= n) return -1;
            unsigned char c1 = p[i + 1];
            if ((c1 & 0xc0) != 0x80 || (p[i + 2] & 0xc0) != 0x80 || (p[i + 3] & 0xc0) != 0x80) return -1;
            if (b == 0xf0 && c1 < 0x90) return -1;       /* overlong */
            if (b == 0xf4 && c1 >= 0x90) return -1;      /* past U+10FFFF */
            i += 4;
        } else {
            return -1;
        }
    }
    (void)count;
    return chars;
}

/* UTF-8 checked 16 bytes at a time: Keiser and Lemire's lookup algorithm
   ("Validating UTF-8 in less than one instruction per byte", 2021), as
   simdjson has it. Three table lookups on nibbles flag every error a byte
   pair can have; a fourth check catches 3- and 4-byte sequences whose
   continuation bytes are missing or extra. */
#if defined(__x86_64__)
#include <immintrin.h>

#define TOO_SHORT (1 << 0)
#define TOO_LONG (1 << 1)
#define OVERLONG_3 (1 << 2)
#define TOO_LARGE (1 << 3)
#define SURROGATE (1 << 4)
#define OVERLONG_2 (1 << 5)
#define TOO_LARGE_1000 (1 << 6)
#define OVERLONG_4 (1 << 6)
#define TWO_CONTS (1 << 7)
#define CARRY (TOO_SHORT | TOO_LONG | TWO_CONTS)

__attribute__((target("ssse3"))) static inline __m128i prevBytes(__m128i cur, __m128i prev, int n) {
    switch (n) {
    case 1: return _mm_alignr_epi8(cur, prev, 15);
    case 2: return _mm_alignr_epi8(cur, prev, 14);
    default: return _mm_alignr_epi8(cur, prev, 13);
    }
}

__attribute__((target("ssse3"))) static int utf8ValidSsse3(const unsigned char *p, int64_t n, int64_t *done) {
    const __m128i t1 = _mm_setr_epi8(TOO_LONG, TOO_LONG, TOO_LONG, TOO_LONG, TOO_LONG, TOO_LONG, TOO_LONG, TOO_LONG,
                                     TWO_CONTS, TWO_CONTS, TWO_CONTS, TWO_CONTS, TOO_SHORT | OVERLONG_2, TOO_SHORT,
                                     TOO_SHORT | OVERLONG_3 | SURROGATE,
                                     TOO_SHORT | TOO_LARGE | TOO_LARGE_1000 | OVERLONG_4);
    const __m128i t2 = _mm_setr_epi8(CARRY | OVERLONG_3 | OVERLONG_2 | OVERLONG_4, CARRY | OVERLONG_2, CARRY, CARRY,
                                     CARRY | TOO_LARGE, CARRY | TOO_LARGE | TOO_LARGE_1000,
                                     CARRY | TOO_LARGE | TOO_LARGE_1000, CARRY | TOO_LARGE | TOO_LARGE_1000,
                                     CARRY | TOO_LARGE | TOO_LARGE_1000, CARRY | TOO_LARGE | TOO_LARGE_1000,
                                     CARRY | TOO_LARGE | TOO_LARGE_1000, CARRY | TOO_LARGE | TOO_LARGE_1000,
                                     CARRY | TOO_LARGE | TOO_LARGE_1000,
                                     CARRY | TOO_LARGE | TOO_LARGE_1000 | SURROGATE,
                                     CARRY | TOO_LARGE | TOO_LARGE_1000, CARRY | TOO_LARGE | TOO_LARGE_1000);
    const __m128i t3 = _mm_setr_epi8(TOO_SHORT, TOO_SHORT, TOO_SHORT, TOO_SHORT, TOO_SHORT, TOO_SHORT, TOO_SHORT,
                                     TOO_SHORT,
                                     TOO_LONG | OVERLONG_2 | TWO_CONTS | OVERLONG_3 | TOO_LARGE_1000 | OVERLONG_4,
                                     TOO_LONG | OVERLONG_2 | TWO_CONTS | OVERLONG_3 | TOO_LARGE,
                                     TOO_LONG | OVERLONG_2 | TWO_CONTS | SURROGATE | TOO_LARGE,
                                     TOO_LONG | OVERLONG_2 | TWO_CONTS | SURROGATE | TOO_LARGE, TOO_SHORT, TOO_SHORT,
                                     TOO_SHORT, TOO_SHORT);
    const __m128i nib = _mm_set1_epi8(0x0f);
    /* bytes that start a sequence the block's end cuts short */
    const __m128i maxEnd = _mm_setr_epi8(-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, (char)(0xf0 - 1),
                                         (char)(0xe0 - 1), (char)(0xc0 - 1));
    __m128i prev = _mm_setzero_si128(), err = _mm_setzero_si128(), prevIncomplete = _mm_setzero_si128();
    int64_t i = 0;
    for (; i + 16 <= n; i += 16) {
        __m128i in = _mm_loadu_si128((const __m128i *)(p + i));
        if (_mm_movemask_epi8(in) == 0) {
            /* ASCII: only a sequence left open by the last block is wrong */
            err = _mm_or_si128(err, prevIncomplete);
            prevIncomplete = _mm_setzero_si128();
            prev = in;
            continue;
        }
        __m128i p1 = prevBytes(in, prev, 1);
        __m128i b1h = _mm_shuffle_epi8(t1, _mm_and_si128(_mm_srli_epi16(p1, 4), nib));
        __m128i b1l = _mm_shuffle_epi8(t2, _mm_and_si128(p1, nib));
        __m128i b2h = _mm_shuffle_epi8(t3, _mm_and_si128(_mm_srli_epi16(in, 4), nib));
        __m128i sc = _mm_and_si128(_mm_and_si128(b1h, b1l), b2h);
        __m128i p2 = prevBytes(in, prev, 2), p3 = prevBytes(in, prev, 3);
        __m128i third = _mm_subs_epu8(p2, _mm_set1_epi8((char)(0xe0 - 0x80)));
        __m128i fourth = _mm_subs_epu8(p3, _mm_set1_epi8((char)(0xf0 - 0x80)));
        __m128i must23 = _mm_and_si128(_mm_or_si128(third, fourth), _mm_set1_epi8((char)0x80));
        err = _mm_or_si128(err, _mm_xor_si128(must23, sc));
        prevIncomplete = _mm_subs_epu8(in, maxEnd);
        prev = in;
    }
    *done = i;
    /* what is left, and a sequence the last block left open, go to the
       scalar check from the start of that sequence */
    return _mm_movemask_epi8(_mm_cmpeq_epi8(err, _mm_setzero_si128())) == 0xffff;
}

/* the characters of valid UTF-8: the bytes that don't continue one */
static int64_t utf8CountSse2(const unsigned char *p, int64_t n) {
    int64_t count = 0, i = 0;
    const __m128i limit = _mm_set1_epi8((char)0xbf);       /* continuation bytes are 0x80 to 0xbf */
    for (; i + 16 <= n; i += 16) {
        __m128i in = _mm_loadu_si128((const __m128i *)(p + i));
        /* signed: continuation bytes are -128 to -65, all others above */
        __m128i notCont = _mm_cmpgt_epi8(in, limit);
        count += __builtin_popcount((unsigned)_mm_movemask_epi8(notCont));
    }
    for (; i < n; i++) count += (p[i] & 0xc0) != 0x80;
    return count;
}
#endif

static int64_t utf8Scan(const unsigned char *p, int64_t n, bool count) {
#if defined(__x86_64__)
    if (n >= 32 && __builtin_cpu_supports("ssse3")) {
        int64_t done;
        if (!utf8ValidSsse3(p, n, &done)) return -1;
        /* back to the start of the character the last block may have cut,
           and the rest checked one character at a time */
        int64_t j = done;
        for (int k = 0; k < 3 && j > 0 && (p[j - 1] & 0xc0) == 0x80; k++) j--;
        if (j > 0 && p[j - 1] >= 0xc0) j--;
        if (utf8ScanScalar(p + j, n - j, false) < 0) return -1;
        return count ? utf8CountSse2(p, n) : 1;
    }
#endif
    return utf8ScanScalar(p, n, count);
}

int iris_utf8_valid(const unsigned char *p, int64_t n) { return utf8Scan(p, n, false) >= 0; }

/* str.runeCount: the characters of valid text in the same pass, or -1. */
int64_t iris_utf8_count(const unsigned char *p, int64_t n) { return utf8Scan(p, n, true); }

/* str.decodeRune past ASCII: the character at p[at] and its length in
   *len; 0xFFFD and 1 for bytes that aren't UTF-8. */
__attribute__((always_inline)) int32_t iris_utf8_decode(const unsigned char *p, int64_t n, int64_t at,
                                                               int64_t *len) {
    unsigned char b = p[at];
    int64_t left = n - at;
    if (b >= 0xc2 && b <= 0xdf) {
        if (left >= 2 && (p[at + 1] & 0xc0) == 0x80) {
            *len = 2;
            return (int32_t)(b & 0x1f) << 6 | (p[at + 1] & 0x3f);
        }
    } else if (b >= 0xe0 && b <= 0xef) {
        if (left >= 3) {
            unsigned char c1 = p[at + 1], c2 = p[at + 2];
            if ((c1 & 0xc0) == 0x80 && (c2 & 0xc0) == 0x80 && !(b == 0xe0 && c1 < 0xa0) && !(b == 0xed && c1 >= 0xa0)) {
                *len = 3;
                return (int32_t)(b & 0x0f) << 12 | (int32_t)(c1 & 0x3f) << 6 | (c2 & 0x3f);
            }
        }
    } else if (b >= 0xf0 && b <= 0xf4) {
        if (left >= 4) {
            unsigned char c1 = p[at + 1], c2 = p[at + 2], c3 = p[at + 3];
            if ((c1 & 0xc0) == 0x80 && (c2 & 0xc0) == 0x80 && (c3 & 0xc0) == 0x80 && !(b == 0xf0 && c1 < 0x90) &&
                !(b == 0xf4 && c1 >= 0x90)) {
                *len = 4;
                return (int32_t)(b & 0x07) << 18 | (int32_t)(c1 & 0x3f) << 12 | (int32_t)(c2 & 0x3f) << 6 | (c3 & 0x3f);
            }
        }
    } else if (b < 0x80) {
        *len = 1;
        return b;
    }
    *len = 1;
    return 0xfffd;
}


/* ---------------------------------------------------------------- big

   The arithmetic of big.Int on magnitudes: 64-bit limbs, the lowest first.
   Each kernel writes into room its caller made and gives the length of
   the result without zero limbs at the top. */

static inline uint64_t divWord(uint64_t hi, uint64_t lo, uint64_t d, uint64_t *rem) {
#if defined(__x86_64__)
    uint64_t q, r;
    __asm__("divq %4" : "=a"(q), "=d"(r) : "a"(lo), "d"(hi), "rm"(d));
    *rem = r;
    return q;
#else
    unsigned __int128 x = (unsigned __int128)hi << 64 | lo;
    *rem = (uint64_t)(x % d);
    return (uint64_t)(x / d);
#endif
}

static int64_t bnTrim(const uint64_t *a, int64_t n) {
    while (n > 0 && a[n - 1] == 0) n--;
    return n;
}

int64_t iris_bn_cmp(const uint64_t *a, int64_t an, const uint64_t *b, int64_t bn) {
    if (an != bn) return an < bn ? -1 : 1;
    for (int64_t i = an - 1; i >= 0; i--) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

/* out has room for max(an, bn) + 1 */
int64_t iris_bn_add(const uint64_t *a, int64_t an, const uint64_t *b, int64_t bn, uint64_t *out) {
    if (an < bn) {
        const uint64_t *t = a; a = b; b = t;
        int64_t k = an; an = bn; bn = k;
    }
    unsigned char carry = 0;
    int64_t i = 0;
    for (; i < bn; i++) {
        uint64_t s;
        unsigned char c1 = __builtin_add_overflow(a[i], b[i], &s);
        unsigned char c2 = __builtin_add_overflow(s, (uint64_t)carry, &out[i]);
        carry = c1 | c2;
    }
    for (; i < an; i++) {
        carry = __builtin_add_overflow(a[i], (uint64_t)carry, &out[i]);
    }
    out[an] = carry;
    return bnTrim(out, an + 1);
}

/* a - b for a at least b; out has room for an */
int64_t iris_bn_sub(const uint64_t *a, int64_t an, const uint64_t *b, int64_t bn, uint64_t *out) {
    unsigned char borrow = 0;
    int64_t i = 0;
    for (; i < bn; i++) {
        uint64_t d;
        unsigned char b1 = __builtin_sub_overflow(a[i], b[i], &d);
        unsigned char b2 = __builtin_sub_overflow(d, (uint64_t)borrow, &out[i]);
        borrow = b1 | b2;
    }
    for (; i < an; i++) {
        borrow = __builtin_sub_overflow(a[i], (uint64_t)borrow, &out[i]);
    }
    return bnTrim(out, an);
}

/* out has room for an + 1 */
int64_t iris_bn_mul_small(const uint64_t *a, int64_t an, uint64_t m, uint64_t *out) {
    uint64_t carry = 0;
    for (int64_t i = 0; i < an; i++) {
        unsigned __int128 t = (unsigned __int128)a[i] * m + carry;
        out[i] = (uint64_t)t;
        carry = (uint64_t)(t >> 64);
    }
    out[an] = carry;
    return bnTrim(out, an + 1);
}

/* out has room for an + bn and doesn't overlap a or b */
int64_t iris_bn_mul(const uint64_t *a, int64_t an, const uint64_t *b, int64_t bn, uint64_t *out) {
    memset(out, 0, (size_t)(an + bn) * sizeof(uint64_t));
    for (int64_t i = 0; i < an; i++) {
        uint64_t ai = a[i], carry = 0;
        if (!ai) continue;
        uint64_t *r = out + i;
        for (int64_t j = 0; j < bn; j++) {
            unsigned __int128 t = (unsigned __int128)ai * b[j] + r[j] + carry;
            r[j] = (uint64_t)t;
            carry = (uint64_t)(t >> 64);
        }
        r[bn] = carry;
    }
    return bnTrim(out, an + bn);
}

/* a divided by one word d into q (room for an); gives the remainder */
uint64_t iris_bn_div_small(const uint64_t *a, int64_t an, uint64_t d, uint64_t *q, int64_t *qn) {
    uint64_t rem = 0;
    for (int64_t i = an - 1; i >= 0; i--) {
        q[i] = divWord(rem, a[i], d, &rem);
    }
    *qn = bnTrim(q, an);
    return rem;
}

/* a divided by b (bn of 2 or more, a at least b), Knuth's algorithm D:
   q has room for an - bn + 1, r for bn. */
void iris_bn_divmod(const uint64_t *a, int64_t an, const uint64_t *b, int64_t bn, uint64_t *q, int64_t *qn,
                    uint64_t *r, int64_t *rn) {
    int s = __builtin_clzll(b[bn - 1]);
    uint64_t *v = malloc((size_t)bn * sizeof(uint64_t));
    uint64_t *u = malloc((size_t)(an + 1) * sizeof(uint64_t));
    if (!v || !u) outOfMemory();
    for (int64_t i = bn - 1; i > 0; i--) v[i] = s ? b[i] << s | b[i - 1] >> (64 - s) : b[i];
    v[0] = b[0] << s;
    u[an] = s ? a[an - 1] >> (64 - s) : 0;
    for (int64_t i = an - 1; i > 0; i--) u[i] = s ? a[i] << s | a[i - 1] >> (64 - s) : a[i];
    u[0] = a[0] << s;
    int64_t m = an - bn;
    for (int64_t j = m; j >= 0; j--) {
        uint64_t rhat, qhat;
        if (u[j + bn] == v[bn - 1]) {
            /* the top words are equal: the digit is the largest one, and
               when rhat overflows the test below can't lower it */
            qhat = ~(uint64_t)0;
            rhat = u[j + bn - 1] + v[bn - 1];
            if (rhat < v[bn - 1]) goto mulsub;
        } else {
            qhat = divWord(u[j + bn], u[j + bn - 1], v[bn - 1], &rhat);
        }
        for (;;) {
            unsigned __int128 p = (unsigned __int128)qhat * v[bn - 2];
            if ((uint64_t)(p >> 64) > rhat || ((uint64_t)(p >> 64) == rhat && (uint64_t)p > u[j + bn - 2])) {
                qhat--;
                uint64_t old = rhat;
                rhat += v[bn - 1];
                if (rhat < old) break;      /* rhat overflowed: qhat is right now */
            } else {
                break;
            }
        }
    mulsub:;
        /* u[j..j+bn] -= qhat * v */
        uint64_t borrow = 0, carry = 0;
        for (int64_t i = 0; i < bn; i++) {
            unsigned __int128 p = (unsigned __int128)qhat * v[i] + carry;
            carry = (uint64_t)(p >> 64);
            uint64_t lo = (uint64_t)p;
            uint64_t t = u[i + j] - lo;
            uint64_t b1 = u[i + j] < lo;
            uint64_t t2 = t - borrow;
            uint64_t b2 = t < borrow;
            u[i + j] = t2;
            borrow = b1 + b2;
        }
        uint64_t top = u[j + bn];
        uint64_t t = top - carry;
        uint64_t b1 = top < carry;
        uint64_t t2 = t - borrow;
        uint64_t b2 = t < borrow;
        u[j + bn] = t2;
        if (b1 | b2) {
            /* qhat was one too many: add v back */
            qhat--;
            unsigned char c = 0;
            for (int64_t i = 0; i < bn; i++) {
                uint64_t s1;
                unsigned char c1 = __builtin_add_overflow(u[i + j], v[i], &s1);
                unsigned char c2 = __builtin_add_overflow(s1, (uint64_t)c, &u[i + j]);
                c = c1 | c2;
            }
            u[j + bn] += c;
        }
        q[j] = qhat;
    }
    *qn = bnTrim(q, m + 1);
    for (int64_t i = 0; i < bn; i++) r[i] = s ? u[i] >> s | u[i + 1] << (64 - s) : u[i];
    *rn = bnTrim(r, bn);
    free(u);
    free(v);
}

/* a shifted left by bits into out (room for an + bits/64 + 1) */
int64_t iris_bn_shl(const uint64_t *a, int64_t an, int64_t bits, uint64_t *out) {
    int64_t words = bits / 64;
    int s = (int)(bits % 64);
    memset(out, 0, (size_t)words * sizeof(uint64_t));
    uint64_t carry = 0;
    for (int64_t i = 0; i < an; i++) {
        out[i + words] = s ? a[i] << s | carry : a[i];
        carry = s ? a[i] >> (64 - s) : 0;
    }
    out[an + words] = carry;
    return bnTrim(out, an + words + 1);
}

/* a shifted right by bits into out (room for an) */
int64_t iris_bn_shr(const uint64_t *a, int64_t an, int64_t bits, uint64_t *out) {
    int64_t words = bits / 64;
    int s = (int)(bits % 64);
    if (words >= an) return 0;
    for (int64_t i = words; i < an; i++) {
        uint64_t lo = a[i] >> s;
        uint64_t hi = s && i + 1 < an ? a[i + 1] << (64 - s) : 0;
        out[i - words] = lo | hi;
    }
    return bnTrim(out, an - words);
}

/* a = a * m + add in place; a has room for one more word; gives the length */
int64_t iris_bn_mul_add_small(uint64_t *a, int64_t an, uint64_t m, uint64_t add) {
    uint64_t carry = add;
    for (int64_t i = 0; i < an; i++) {
        unsigned __int128 t = (unsigned __int128)a[i] * m + carry;
        a[i] = (uint64_t)t;
        carry = (uint64_t)(t >> 64);
    }
    a[an] = carry;
    return bnTrim(a, an + 1);
}

/* (hi, lo) divided by d, for d with its top bit set and hi below d, by
   multiplying with v, d's reciprocal (Moeller and Granlund, "Improved
   division by invariant integers", algorithm 4): two multiplications in
   place of a division. */
static inline uint64_t divPreinv(uint64_t hi, uint64_t lo, uint64_t d, uint64_t v, uint64_t *rem) {
    unsigned __int128 q = (unsigned __int128)v * hi;
    q += ((unsigned __int128)(hi + 1) << 64) | lo;
    uint64_t q1 = (uint64_t)(q >> 64), q0 = (uint64_t)q;
    uint64_t r = lo - q1 * d;
    if (r > q0) {
        q1--;
        r += d;
    }
    if (r >= d) {
        q1++;
        r -= d;
    }
    *rem = r;
    return q1;
}

/* Montgomery multiplication (CIOS) for an odd modulus m of n words:
   out = a * b / 2^(64n) mod m, for a and b below m; t has room for n + 2
   words, and out may be a or b. minv is -1/m[0] mod 2^64. */
static void bnMontMul(const uint64_t *a, const uint64_t *b, const uint64_t *m, int64_t n, uint64_t minv,
                      uint64_t *t, uint64_t *out) {
    memset(t, 0, (size_t)(n + 2) * sizeof(uint64_t));
    for (int64_t i = 0; i < n; i++) {
        /* t = (t + a[i] * b + f * m) / 2^64 in one pass, f chosen so the
           low word is 0: the two carry chains run side by side */
        uint64_t ai = a[i];
        unsigned __int128 p = (unsigned __int128)ai * b[0] + t[0];
        uint64_t c1 = (uint64_t)(p >> 64);
        uint64_t f = (uint64_t)p * minv;
        unsigned __int128 q = (unsigned __int128)f * m[0] + (uint64_t)p;
        uint64_t c2 = (uint64_t)(q >> 64);
        for (int64_t j = 1; j < n; j++) {
            p = (unsigned __int128)ai * b[j] + t[j] + c1;
            c1 = (uint64_t)(p >> 64);
            q = (unsigned __int128)f * m[j] + (uint64_t)p + c2;
            c2 = (uint64_t)(q >> 64);
            t[j - 1] = (uint64_t)q;
        }
        unsigned __int128 s = (unsigned __int128)t[n] + c1 + c2;
        t[n - 1] = (uint64_t)s;
        t[n] = t[n + 1] + (uint64_t)(s >> 64);
        t[n + 1] = 0;
    }
    /* t is below 2m: once more m off when it is at least m */
    bool ge = t[n] != 0;
    if (!ge) {
        ge = true;
        for (int64_t i = n - 1; i >= 0; i--) {
            if (t[i] != m[i]) {
                ge = t[i] > m[i];
                break;
            }
        }
    }
    if (ge) {
        unsigned char borrow = 0;
        for (int64_t i = 0; i < n; i++) {
            uint64_t d;
            unsigned char b1 = __builtin_sub_overflow(t[i], m[i], &d);
            unsigned char b2 = __builtin_sub_overflow(d, (uint64_t)borrow, &out[i]);
            borrow = b1 | b2;
        }
    } else {
        memcpy(out, t, (size_t)n * sizeof(uint64_t));
    }
}

/* x * 2^(64n) mod m into out (n words), for x below m */
static void bnToMont(const uint64_t *x, int64_t xn, const uint64_t *m, int64_t n, uint64_t *out) {
    int64_t un = xn + n;
    uint64_t *u = calloc((size_t)un + 1, sizeof(uint64_t));
    uint64_t *q = malloc((size_t)un * sizeof(uint64_t));
    if (!u || !q) outOfMemory();
    memcpy(u + n, x, (size_t)xn * sizeof(uint64_t));
    un = bnTrim(u, un);
    memset(out, 0, (size_t)n * sizeof(uint64_t));
    int64_t qn, rn;
    if (un == 0) {
        rn = 0;
    } else if (n == 1) {
        out[0] = iris_bn_div_small(u, un, m[0], q, &qn);
    } else if (iris_bn_cmp(u, un, m, n) < 0) {
        memcpy(out, u, (size_t)un * sizeof(uint64_t));
    } else {
        iris_bn_divmod(u, un, m, n, q, &qn, out, &rn);
    }
    free(u);
    free(q);
}

/* b^e mod m for an odd m (n words, the top one not zero) and b below m,
   into out (room for n words): Montgomery multiplication, so no step
   divides, and a window of 4 bits for long exponents. Gives the length. */
int64_t iris_bn_modpow_odd(const uint64_t *b, int64_t bn, const uint64_t *e, int64_t en, const uint64_t *m, int64_t n,
                           uint64_t *out) {
    uint64_t inv = m[0];                /* 1/m[0] mod 2^64 by Newton's method */
    for (int i = 0; i < 6; i++) inv *= 2 - m[0] * inv;
    uint64_t minv = (uint64_t)0 - inv;
    uint64_t *t = malloc((size_t)(n + 2) * sizeof(uint64_t));
    uint64_t *acc = malloc((size_t)n * sizeof(uint64_t));
    uint64_t *one = calloc((size_t)n, sizeof(uint64_t));
    int64_t bits = en ? (en - 1) * 64 + (64 - __builtin_clzll(e[en - 1])) : 0;
    int w = bits > 64 ? 4 : 1;
    uint64_t *tab = malloc((size_t)((1 << w) * n) * sizeof(uint64_t));
    if (!t || !acc || !one || !tab) outOfMemory();
    uint64_t unit = 1;
    bnToMont(&unit, 1, m, n, acc);              /* 1, in Montgomery form */
    bnToMont(b, bn, m, n, tab + n);             /* b */
    memcpy(tab, acc, (size_t)n * sizeof(uint64_t));
    for (int i = 2; i < 1 << w; i++) bnMontMul(tab + (i - 1) * n, tab + n, m, n, minv, t, tab + i * n);
    for (int64_t i = bits - 1; i >= 0;) {
        if (w == 1 || i < w - 1) {
            bnMontMul(acc, acc, m, n, minv, t, acc);
            if ((e[i / 64] >> (i % 64)) & 1) bnMontMul(acc, tab + n, m, n, minv, t, acc);
            i--;
            continue;
        }
        /* the next w bits, from bit i down */
        unsigned d = 0;
        for (int k = 0; k < w; k++) {
            int64_t bit = i - k;
            d = d << 1 | (unsigned)((e[bit / 64] >> (bit % 64)) & 1);
            bnMontMul(acc, acc, m, n, minv, t, acc);
        }
        if (d) bnMontMul(acc, tab + d * n, m, n, minv, t, acc);
        i -= w;
    }
    one[0] = 1;
    bnMontMul(acc, one, m, n, minv, t, out);    /* out of Montgomery form */
    free(t);
    free(acc);
    free(one);
    free(tab);
    return bnTrim(out, n);
}

/* x (xn words, overwritten) as exactly chunks groups of 19 decimal digits,
   zero-padded, ending at end: 19 digits a pass, each a division by 10^19. */
static void bnTextChunks(uint64_t *x, int64_t xn, int64_t chunks, char *end) {
    const uint64_t ten19 = 10000000000000000000ULL;     /* above 2^63: normalized as divPreinv needs */
    const uint64_t inv = (uint64_t)((((unsigned __int128)~ten19) << 64 | ~(uint64_t)0) / ten19);
    xn = bnTrim(x, xn);
    char tmp[24];
    char *tend = tmp + sizeof tmp;
    for (int64_t c = 0; c < chunks; c++) {
        uint64_t rem = 0;
        for (int64_t i = xn - 1; i >= 0; i--) {
            x[i] = divPreinv(rem, x[i], ten19, inv, &rem);
        }
        while (xn > 0 && x[xn - 1] == 0) xn--;
        char *p = u64Digits(rem, tend);
        int64_t k = tend - p;
        end -= 19;
        memset(end, '0', (size_t)(19 - k));
        memcpy(end + 19 - k, p, (size_t)k);
    }
}

/* x below pw[k]^2 as exactly 19 * 2^(k+1) digits, zero-padded, ending at
   end: split by pw[k] = 10^(19 * 2^k) into halves written the same way,
   so a long number takes a few long divisions in place of a pass of
   divisions per 19 digits. */
static void bnTextSplit(const uint64_t *x, int64_t xn, int k, uint64_t **pw, int64_t *pwn, char *end) {
    xn = bnTrim(x, xn);
    if (k < 4 || xn <= pwn[k]) {
        if (xn <= pwn[k] && k >= 4 && iris_bn_cmp(x, xn, pw[k], pwn[k]) < 0) {
            /* the top half is zero */
            int64_t half = (int64_t)19 << k;
            memset(end - 2 * half, '0', (size_t)half);
            bnTextSplit(x, xn, k - 1, pw, pwn, end);
            return;
        }
        if (k < 4) {
            uint64_t *w = malloc((size_t)(xn ? xn : 1) * sizeof(uint64_t));
            if (!w) outOfMemory();
            memcpy(w, x, (size_t)xn * sizeof(uint64_t));
            bnTextChunks(w, xn, (int64_t)2 << k, end);
            free(w);
            return;
        }
    }
    uint64_t *q = malloc((size_t)(xn - pwn[k] + 1) * sizeof(uint64_t));
    uint64_t *r = malloc((size_t)pwn[k] * sizeof(uint64_t));
    if (!q || !r) outOfMemory();
    int64_t qn, rn;
    iris_bn_divmod(x, xn, pw[k], pwn[k], q, &qn, r, &rn);
    int64_t half = (int64_t)19 << k;
    bnTextSplit(r, rn, k - 1, pw, pwn, end);
    bnTextSplit(q, qn, k - 1, pw, pwn, end - half);
    free(q);
    free(r);
}

/* the magnitude in decimal into out (room for an * 20 + 1); gives the
   length. */
int64_t iris_bn_text10(const uint64_t *limbs, int64_t n, char *out) {
    int64_t words = bnTrim(limbs, n);
    if (words == 0) {
        out[0] = '0';
        return 1;
    }
    /* pw[k] = 10^(19 * 2^k), up to the first above the number's square root */
    uint64_t *pw[40];
    int64_t pwn[40];
    int k = 0;
    pw[0] = malloc(sizeof(uint64_t));
    if (!pw[0]) outOfMemory();
    pw[0][0] = 10000000000000000000ULL;
    pwn[0] = 1;
    while (2 * pwn[k] - 1 <= words && k < 38) {
        pw[k + 1] = malloc((size_t)(2 * pwn[k]) * sizeof(uint64_t));
        if (!pw[k + 1]) outOfMemory();
        pwn[k + 1] = iris_bn_mul(pw[k], pwn[k], pw[k], pwn[k], pw[k + 1]);
        k++;
    }
    /* now the number is below pw[k]^2 */
    int64_t digits = (int64_t)19 << (k + 1);
    char *buf = malloc((size_t)digits);
    if (!buf) outOfMemory();
    if (k < 4) {
        uint64_t *w = malloc((size_t)words * sizeof(uint64_t));
        if (!w) outOfMemory();
        memcpy(w, limbs, (size_t)words * sizeof(uint64_t));
        bnTextChunks(w, words, (int64_t)2 << k, buf + digits);
        free(w);
    } else {
        bnTextSplit(limbs, words, k, pw, pwn, buf + digits);
    }
    int64_t skip = 0;
    while (skip < digits - 1 && buf[skip] == '0') skip++;
    memcpy(out, buf + skip, (size_t)(digits - skip));
    free(buf);
    for (int i = 0; i <= k; i++) free(pw[i]);
    return digits - skip;
}

/* {n:x}, {n:+}: an integer in a base, with its sign; into s. */
void iris_fmt_int(IrisString *s, int64_t v, int isSigned, int base, int upper, int plus) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    bool neg = isSigned && v < 0;
    uint64_t u = neg ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    char buf[72];
    char *end = buf + sizeof buf, *p = end;
    do {
        *--p = digits[u % (uint64_t)base];
        u /= (uint64_t)base;
    } while (u);
    if (neg) {
        *--p = '-';
    } else if (plus) {
        *--p = '+';
    }
    iris_string_append(s, p, end - p);
}

/* {x:.2}, {x:e}, {x:+}: a float with that many digits after the point, or
   in exponent form; into s. Without a precision, the fewest digits that
   read back the same, as plain text does. */
void iris_write_float(int sink, IrisString *dst, double v, int bits);

static int fmtFixedBuf(char *buf, double v, int prec, int plus);

void iris_fmt_float(IrisString *s, double v, int bits, int prec, int kind, int plus) {
    char buf[512];
    int n;
    if (kind == 'e') {
        n = snprintf(buf, sizeof buf, plus ? "%+.*e" : "%.*e", prec < 0 ? 6 : prec, v);
    } else if (prec >= 0 || kind == 'f') {
        n = fmtFixedBuf(buf, v, prec < 0 ? 6 : prec, plus);
        if (n < 0) n = snprintf(buf, sizeof buf, plus ? "%+.*f" : "%.*f", prec < 0 ? 6 : prec, v);
    } else {
        if (plus && !(v < 0) && !(v != v)) {
            iris_string_append(s, "+", 1);
        }
        iris_write_float(3, s, v, bits);
        return;
    }
    if (n < 0) n = 0;
    if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
    iris_string_append(s, buf, n);
}

/* An integer as {n:spec} writes it, into buf (at least 72 bytes); gives
   where the text starts, which ends at buf + 72. */
static char *fmtIntBuf(char *buf, int64_t v, int isSigned, int base, int upper, int plus) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    bool neg = isSigned && v < 0;
    uint64_t u = neg ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    char *p = buf + 72;
    if (base == 10) {
        p = u64Digits(u, p);
    } else {
        do {
            *--p = digits[u % (uint64_t)base];
            u /= (uint64_t)base;
        } while (u);
    }
    if (neg) {
        *--p = '-';
    } else if (plus) {
        *--p = '+';
    }
    return p;
}

/* v with prec digits after the point, exactly as printf's %.*f writes it,
   into buf (at least 64 bytes): v is m * 2^e, so v * 10^prec is an integer
   division that 128-bit numbers do exactly, rounded half to even. Gives
   the length, or -1 when v is out of the range this covers. */
static int fmtFixedBuf(char *buf, double v, int prec, int plus) {
    if (prec < 0 || prec > 19 || v != v || v - v != 0) return -1;
    bool neg = signbit(v);
    double a = fabs(v);
    int e2;
    double frac = frexp(a, &e2);                    /* a = frac * 2^e2, frac in [0.5, 1) */
    uint64_t m = (uint64_t)ldexp(frac, 53);         /* a = m * 2^(e2 - 53) */
    int shift = 53 - e2;                            /* a = m / 2^shift */
    static const uint64_t pow10[20] = {1ULL, 10ULL, 100ULL, 1000ULL, 10000ULL, 100000ULL, 1000000ULL, 10000000ULL,
                                       100000000ULL, 1000000000ULL, 10000000000ULL, 100000000000ULL,
                                       1000000000000ULL, 10000000000000ULL, 100000000000000ULL,
                                       1000000000000000ULL, 10000000000000000ULL, 100000000000000000ULL,
                                       1000000000000000000ULL, 10000000000000000000ULL};
    unsigned __int128 x = (unsigned __int128)m * pow10[prec];
    unsigned __int128 q;
    if (a == 0) {
        q = 0;
    } else if (shift <= 0) {
        if (shift < -10) return -1;
        q = x << -shift;
    } else if (shift >= 127) {
        q = 0;                                      /* far below a unit of the last digit */
    } else {
        q = x >> shift;
        unsigned __int128 rem = x - (q << shift), half = (unsigned __int128)1 << (shift - 1);
        if (rem > half || (rem == half && (q & 1))) q++;
    }
    if (q >> 64) return -1;
    uint64_t n = (uint64_t)q;
    char digits[32];
    char *end = digits + sizeof digits;
    char *d = u64Digits(n, end);
    int len = (int)(end - d);
    int k = 0;
    if (neg) buf[k++] = '-';
    else if (plus) buf[k++] = '+';
    if (len <= prec) {
        buf[k++] = '0';
        if (prec > 0) {
            buf[k++] = '.';
            for (int i = 0; i < prec - len; i++) buf[k++] = '0';
            memcpy(buf + k, d, (size_t)len);
            k += len;
        }
    } else {
        memcpy(buf + k, d, (size_t)(len - prec));
        k += len - prec;
        if (prec > 0) {
            buf[k++] = '.';
            memcpy(buf + k, d + len - prec, (size_t)prec);
            k += prec;
        }
    }
    return k;
}

/* The bytes of the first n characters of p (UTF-8), and how many there are. */
static int64_t runePrefix(const char *p, int64_t len, int64_t n, int64_t *count) {
    int64_t i = 0, c = 0;
    while (i < len && c < n) {
        unsigned char b = (unsigned char)p[i];
        i += b < 0x80 ? 1 : b >> 5 == 6 ? 2 : b >> 4 == 14 ? 3 : b >> 3 == 30 ? 4 : 1;
        c++;
    }
    if (i > len) i = len;
    *count = c;
    return i;
}

static void putRepeated(int sink, IrisString *dst, const char *unit, int ulen, int64_t times) {
    char buf[256];
    while (times > 0) {
        int k = 0;
        while (times > 0 && k + ulen <= (int)sizeof buf) {
            memcpy(buf + k, unit, (size_t)ulen);
            k += ulen;
            times--;
        }
        put(sink, dst, buf, (size_t)k);
    }
}

static void padBytes(int sink, IrisString *dst, const char *p, int64_t len, int64_t width, int fill, int align,
                     int zero, int64_t maxChars);

/* The text in s padded to width characters with fill, by align ('<', '>'
   or '^'); zero puts zeros after a sign instead; maxChars cuts text. Frees s. */
void iris_write_padded(int sink, IrisString *dst, IrisString *s, int64_t width, int fill, int align, int zero,
                       int64_t maxChars) {
    padBytes(sink, dst, s->ptr ? s->ptr : "", s->len, width, fill, align, zero, maxChars);
    iris_string_free(s);
}

/* {n:spec} for an integer, straight into the sink. */
void iris_write_int_spec(int sink, IrisString *dst, int64_t v, int isSigned, int base, int upper, int plus,
                         int64_t width, int fill, int align, int zero) {
    char buf[72];
    char *p = fmtIntBuf(buf, v, isSigned, base, upper, plus);
    padBytes(sink, dst, p, buf + 72 - p, width, fill, align, zero, -1);
}

/* {x:.2} and {x:f} for a float, straight into the sink when it can. */
void iris_write_float_spec(int sink, IrisString *dst, double v, int bits, int prec, int kind, int plus,
                           int64_t width, int fill, int align, int zero) {
    char buf[64];
    int n = kind != 'e' ? fmtFixedBuf(buf, v, prec < 0 ? 6 : prec, plus) : -1;
    if (n >= 0) {
        padBytes(sink, dst, buf, n, width, fill, align, zero, -1);
        return;
    }
    IrisString s = {0};
    iris_fmt_float(&s, v, bits, prec, kind, plus);
    iris_write_padded(sink, dst, &s, width, fill, align, zero, -1);
}

/* {s:>10} for text, straight into the sink. */
void iris_write_str_spec(int sink, IrisString *dst, const char *p, int64_t len, int64_t width, int fill, int align,
                         int64_t maxChars) {
    padBytes(sink, dst, p ? p : "", len, width, fill, align, 0, maxChars);
}

static void padBytes(int sink, IrisString *dst, const char *p, int64_t len, int64_t width, int fill, int align,
                     int zero, int64_t maxChars) {
    int64_t chars;
    if (maxChars >= 0) {
        len = runePrefix(p, len, maxChars, &chars);
    } else if (width > 0) {
        runePrefix(p, len, INT64_MAX, &chars);
    } else {
        put(sink, dst, p, (size_t)len);
        return;
    }
    int64_t pad = width > chars ? width - chars : 0;
    char unit[4];
    int ulen;
    if (zero) {
        unit[0] = '0';
        ulen = 1;
    } else if (fill < 0x80) {
        unit[0] = (char)fill;
        ulen = 1;
    } else if (fill < 0x800) {
        unit[0] = (char)(0xc0 | fill >> 6);
        unit[1] = (char)(0x80 | (fill & 0x3f));
        ulen = 2;
    } else if (fill < 0x10000) {
        unit[0] = (char)(0xe0 | fill >> 12);
        unit[1] = (char)(0x80 | (fill >> 6 & 0x3f));
        unit[2] = (char)(0x80 | (fill & 0x3f));
        ulen = 3;
    } else {
        unit[0] = (char)(0xf0 | fill >> 18);
        unit[1] = (char)(0x80 | (fill >> 12 & 0x3f));
        unit[2] = (char)(0x80 | (fill >> 6 & 0x3f));
        unit[3] = (char)(0x80 | (fill & 0x3f));
        ulen = 4;
    }
    if (zero) {
        /* -0042: the sign first */
        int64_t sign = len > 0 && (p[0] == '-' || p[0] == '+') ? 1 : 0;
        put(sink, dst, p, (size_t)sign);
        putRepeated(sink, dst, unit, ulen, pad);
        put(sink, dst, p + sign, (size_t)(len - sign));
    } else {
        int64_t before = align == '>' ? pad : align == '^' ? pad / 2 : 0;
        putRepeated(sink, dst, unit, ulen, before);
        put(sink, dst, p, (size_t)len);
        putRepeated(sink, dst, unit, ulen, pad - before);
    }
}

/* A float in the fewest digits that read back as the same value, written
 * as a plain decimal unless it is very large or very small: 100, 0.25,
 * 1e+21, 1e-07. */
#include "ryu.h"

void iris_write_float(int sink, IrisString *dst, double v, int bits) {
    char buf[64];
    int n = 0;
    if (v != v) {
        put(sink, dst, "nan", 3);
        return;
    }
    if (v == 1.0 / 0.0 || v == -1.0 / 0.0) {
        put(sink, dst, v > 0 ? "inf" : "-inf", v > 0 ? 3 : 4);
        return;
    }
    if (v == 0) {
        put(sink, dst, signbit(v) ? "-0" : "0", signbit(v) ? 2 : 1);
        return;
    }
    /* the shortest digits, and the power of ten of the first one */
    bool neg = signbit(v);
    uint64_t d;
    int32_t last;
    if (bits == 32) {
        float f = (float)v;
        uint32_t u;
        memcpy(&u, &f, 4);
        d = ryuDigits(u & 0x7fffff, (u >> 23) & 0xff, 23, 127, &last);
    } else {
        uint64_t u;
        memcpy(&u, &v, 8);
        d = ryuDigits(u & ((1ull << 52) - 1), (u >> 52) & 0x7ff, 52, 1023, &last);
    }
    char digits[32];
    int nd = 0;
    for (uint64_t x = d; x; x /= 10) nd++;
    for (int i = nd - 1; i >= 0; i--, d /= 10) digits[i] = (char)('0' + d % 10);
    int exp10 = last + nd - 1;
    if (exp10 < -6 || exp10 >= 21) {
        /* the exponent form, without padding zeros: 1e+21, 1.5e-7 */
        if (neg) buf[n++] = '-';
        buf[n++] = digits[0];
        if (nd > 1) {
            buf[n++] = '.';
            memcpy(buf + n, digits + 1, (size_t)nd - 1);
            n += nd - 1;
        }
        n += snprintf(buf + n, sizeof buf - (size_t)n, "e%c%d", exp10 < 0 ? '-' : '+', exp10 < 0 ? -exp10 : exp10);
        put(sink, dst, buf, (size_t)n);
        return;
    }
    char out[64];
    int o = 0;
    if (neg) out[o++] = '-';
    if (exp10 < 0) {
        out[o++] = '0';
        out[o++] = '.';
        for (int i = 0; i < -exp10 - 1; i++) out[o++] = '0';
        for (int i = 0; i < nd; i++) out[o++] = digits[i];
    } else {
        for (int i = 0; i <= exp10; i++) out[o++] = i < nd ? digits[i] : '0';
        if (nd > exp10 + 1) {
            out[o++] = '.';
            for (int i = exp10 + 1; i < nd; i++) out[o++] = digits[i];
        }
    }
    put(sink, dst, out, (size_t)o);
}

void iris_write_bool(int sink, IrisString *dst, int v) {
    if (v) {
        put(sink, dst, "true", 4);
    } else {
        put(sink, dst, "false", 5);
    }
}

void iris_print_end(void) { fputc('\n', stdout); }

static const char *enumValueName(int64_t kind, int64_t value, const char **enumName) {
    *enumName = "?";
    if (kind < 1 || kind > iris_enum_count) {
        return NULL;
    }
    const IrisEnum *e = &iris_enum_table[kind - 1];
    *enumName = e->name;
    for (int64_t i = 0; i < e->count; i++) {
        if (e->values[i] == value) {
            return e->names[i];
        }
    }
    return NULL;
}

const char *iris_enum_name(int64_t kind, int32_t value) {
    const char *en;
    const char *name = enumValueName(kind, value, &en);
    return name ? name : "?";
}

void iris_write_enum(int sink, IrisString *dst, int64_t kind, int32_t value) {
    const char *en;
    const char *name = enumValueName(kind, value, &en);
    if (name) {
        put(sink, dst, name, strlen(name));
    } else {
        iris_write_i64(sink, dst, value);
    }
}

/* A flag enum's value: the names of its set values, joined by | */
void iris_write_flags(int sink, IrisString *dst, int64_t kind, int32_t value) {
    if (value == 0 || kind < 1 || kind > iris_enum_count) {
        iris_write_i64(sink, dst, value);
        return;
    }
    const IrisEnum *e = &iris_enum_table[kind - 1];
    uint32_t left = (uint32_t)value;
    int written = 0;
    for (int64_t i = 0; i < e->count; i++) {
        uint32_t bit = (uint32_t)e->values[i];
        if (bit && (left & bit) == bit) {
            if (written++) {
                put(sink, dst, "|", 1);
            }
            put(sink, dst, e->names[i], strlen(e->names[i]));
            left &= ~bit;
        }
    }
    if (left) {
        if (written) {
            put(sink, dst, "|", 1);
        }
        iris_write_i64(sink, dst, (int64_t)left);
    }
}

/* An error code as Enum.value */
void iris_write_errcode(int sink, IrisString *dst, int64_t code) {
    const char *en;
    int64_t kind = (int64_t)((uint64_t)code >> 32);
    int32_t value = (int32_t)(uint32_t)code;
    const char *name = enumValueName(kind, value, &en);
    put(sink, dst, en, strlen(en));
    put(sink, dst, ".", 1);
    if (name) {
        put(sink, dst, name, strlen(name));
    } else {
        iris_write_i64(sink, dst, value);
    }
}

void iris_error_begin(void) { msgNextLen = 0; }

/* The current error is kept while deferred code runs on the way out of a
 * function: that code may make and catch errors of its own, or wait while
 * other tasks of the thread make theirs. Keeping costs nothing until the
 * message or trace is about to change: then a copy is made for each keeper
 * that has none yet, and given back when the deferred code is done. A
 * keeper is known by the generation of the error it kept. */
typedef struct {
    size_t msgLen;
    int traceLen, traceDropped;
    Frame trace[];
} Kept;

typedef struct {
    uint64_t gen;
    Kept *copy;
} Keeper;

static _Thread_local uint64_t errGen;
static _Thread_local Keeper *keepers;
static _Thread_local int keepersLen, keepersCap;

static Kept *keptCopy(void) {
    Kept *k = malloc(sizeof(Kept) + (size_t)traceLen * sizeof(Frame) + msgCurLen);
    if (!k) {
        return NULL;
    }
    k->msgLen = msgCurLen;
    k->traceLen = traceLen;
    k->traceDropped = traceDropped;
    memcpy(k->trace, trace, (size_t)traceLen * sizeof(Frame));
    memcpy((char *)(k->trace + traceLen), msgCur, msgCurLen);
    return k;
}

/* Called before the message or the trace changes. */
static inline void errorChanging(void) {
    for (int i = 0; i < keepersLen; i++) {
        if (!keepers[i].copy) {
            keepers[i].copy = keptCopy();
        }
    }
    errGen++;
}

int64_t iris_error_keep(void) {
    if (keepersLen == keepersCap) {
        int cap = keepersCap ? keepersCap * 2 : 8;
        Keeper *k = realloc(keepers, (size_t)cap * sizeof(Keeper));
        if (!k) {
            outOfMemory();
        }
        keepers = k;
        keepersCap = cap;
    }
    keepers[keepersLen++] = (Keeper){errGen, NULL};
    return (int64_t)errGen;
}

void iris_error_back(int64_t gen) {
    int i = keepersLen - 1;
    while (i >= 0 && keepers[i].gen != (uint64_t)gen) {
        i--;
    }
    if (i < 0) {
        return;
    }
    Kept *k = keepers[i].copy;
    memmove(keepers + i, keepers + i + 1, (size_t)(keepersLen - i - 1) * sizeof(Keeper));
    keepersLen--;
    if (!k) {
        return; /* nothing changed */
    }
    errorChanging();
    memcpy(msgCur, (char *)(k->trace + k->traceLen), k->msgLen);
    msgCurLen = k->msgLen;
    memcpy(trace, k->trace, (size_t)k->traceLen * sizeof(Frame));
    traceLen = k->traceLen;
    traceDropped = k->traceDropped;
    free(k);
}

static void addFrame(const char *file, int line, int col, const char *fn) {
    if (keepersLen) {
        errorChanging();
    }
    if (traceLen < TRACE_MAX) {
        trace[traceLen++] = (Frame){file, fn, line, col};
    } else {
        traceDropped++;
    }
}

/* A new error: its message becomes the current one and its trace starts
 * where it was made. */
int64_t iris_error_finish(int64_t code, const char *file, int line, int col, const char *fn) {
    if (keepersLen) {
        errorChanging();
    }
    memcpy(msgCur, msgNext, msgNextLen);
    msgCurLen = msgNextLen;
    traceLen = 0;
    traceDropped = 0;
    addFrame(file, line, col, fn);
    return code;
}

void iris_trace_add(const char *file, int line, int col, const char *fn) { addFrame(file, line, col, fn); }

/* A catch block's own copy of the message, and putting it back when the
 * block returns the error. */
void iris_error_save(char *buf, int64_t *len) {
    memcpy(buf, msgCur, msgCurLen);
    *len = (int64_t)msgCurLen;
}

void iris_error_restore(const char *buf, const int64_t *len) {
    if (keepersLen) {
        errorChanging();
    }
    memcpy(msgCur, buf, (size_t)*len);
    msgCurLen = (size_t)*len;
}

const char *iris_error_message_ptr(void) { return msgCur; }

int64_t iris_error_message_len(void) { return (int64_t)msgCurLen; }

/* An error that got to the end of main. */
void iris_error_report(int64_t code) {
    fflush(stdout);
    IrisString s = {0};
    iris_write_errcode(3, &s, code);
    fprintf(stderr, "error: %.*s", (int)s.len, s.ptr);
    if (msgCurLen) {
        fprintf(stderr, ": %.*s", (int)msgCurLen, msgCur);
    }
    fputc('\n', stderr);
    int width = 0;
    for (int i = 0; i < traceLen; i++) {
        int w = (int)strlen(trace[i].fn);
        if (w > width) {
            width = w;
        }
    }
    for (int i = 0; i < traceLen; i++) {
        fprintf(stderr, "  at %-*s  %s:%d\n", width, trace[i].fn, trace[i].file, trace[i].line);
    }
    if (traceDropped) {
        fprintf(stderr, "  ... and %d more\n", traceDropped);
    }
    iris_string_free(&s);
    exit(1);
}

/* a < b for text: the bytes as unsigned numbers, then the shorter first;
   for UTF-8 that is the order of the code points. */
int iris_str_cmp(const char *a, int64_t na, const char *b, int64_t nb) {
    int64_t n = na < nb ? na : nb;
    int c = n > 0 ? memcmp(a, b, (size_t)n) : 0;
    if (c != 0) return c < 0 ? -1 : 1;
    return na < nb ? -1 : na > nb ? 1 : 0;
}

int iris_str_eq(const char *a, int64_t na, const char *b, int64_t nb) {
    return na == nb && (na == 0 || memcmp(a, b, (size_t)na) == 0);
}

void *iris_alloc(int64_t size, int64_t count, const char *file, int line, int col, const char *fn) {
    if (count < 0) {
        char msg[96];
        int n = snprintf(msg, sizeof msg, "alloc of %lld items", (long long)count);
        stop(msg, (size_t)n, file, line, col, fn);
    }
    if (count == 0) {
        return NULL;
    }
    if (size > 0 && count > INT64_MAX / size) {
        stop("alloc size overflows", 20, file, line, col, fn);
    }
    void *p = calloc((size_t)count, (size_t)(size ? size : 1));
    if (!p) {
        outOfMemory();
    }
    return p;
}

/* List's growSlice: room for n items of size where there were len.
   realloc can often grow in place, or move the pages without copying
   them; a type aligned beyond malloc's gets new memory. */
void *iris_grow(void *p, int64_t len, int64_t n, int64_t size, int64_t align) {
    if (size == 0) {
        size = 1;
    }
    if (n <= len || n > INT64_MAX / size) {
        outOfMemory();
    }
    size_t old = (size_t)len * (size_t)size, total = (size_t)n * (size_t)size;
    char *q;
    if (align > 16) {
        if (posix_memalign((void **)&q, (size_t)align, total) != 0) {
            outOfMemory();
        }
        if (old) {
            memcpy(q, p, old);
        }
        free(p);
    } else {
        q = realloc(p, total);
        if (!q) {
            outOfMemory();
        }
    }
    /* the new items are left as they are: List and Deque write them before
       they read them, and zeroing would fault in pages realloc moved
       untouched */
    return q;
}

/* alloc of a type whose align(n) is more than malloc gives */
void *iris_alloc_aligned(int64_t size, int64_t count, int64_t align, const char *file, int line, int col,
                         const char *fn) {
    if (count < 0) {
        char msg[96];
        int n = snprintf(msg, sizeof msg, "alloc of %lld items", (long long)count);
        stop(msg, (size_t)n, file, line, col, fn);
    }
    if (count == 0) {
        return NULL;
    }
    if (size > 0 && count > INT64_MAX / size) {
        stop("alloc size overflows", 20, file, line, col, fn);
    }
    void *p = NULL;
    size_t total = (size_t)count * (size_t)(size ? size : 1);
    if (posix_memalign(&p, (size_t)align, total) != 0) {
        outOfMemory();
    }
    memset(p, 0, total);
    return p;
}

void iris_panic(const char *file, int line, int col, const char *fn) {
    stop(panicBuf, panicLen, file, line, col, fn);
}

void iris_check_failed(const char *msg, const char *file, int line, int col, const char *fn) {
    stop(msg, strlen(msg), file, line, col, fn);
}

void iris_index_failed(int64_t i, int64_t len, const char *file, int line, int col, const char *fn) {
    char msg[128];
    int n = snprintf(msg, sizeof msg, "index %lld is out of range for length %lld", (long long)i, (long long)len);
    stop(msg, (size_t)n, file, line, col, fn);
}

void iris_slice_failed(int64_t lo, int64_t hi, int64_t len, const char *file, int line, int col, const char *fn) {
    char msg[160];
    int n = snprintf(msg, sizeof msg, "slice [%lld:%lld] is out of range for length %lld", (long long)lo,
                     (long long)hi, (long long)len);
    stop(msg, (size_t)n, file, line, col, fn);
}

/* ---------------------------------------------------------------- std: os, fs, io */

int64_t iris_os_argc(void) { return gArgc; }

void iris_os_arg(int64_t i, const char **p, int64_t *n) {
    *p = gArgv[i];
    *n = (int64_t)strlen(gArgv[i]);
}

/* A C string copy of a (pointer, length) path; NULL if it has a NUL in it. */
static char *cpath(const char *p, int64_t n) {
    if (memchr(p, 0, (size_t)n)) {
        return NULL;
    }
    char *s = malloc((size_t)n + 1);
    if (!s) {
        outOfMemory();
    }
    memcpy(s, p, (size_t)n);
    s[n] = 0;
    return s;
}

/* Sets (value non-NULL) or removes an environment variable. 0 when it
 * worked, 1 for a name that can't be one (empty, with = or a NUL). */
int iris_os_setenv(const char *name, int64_t n, const char *value, int64_t vn, int set) {
    if (n == 0 || memchr(name, '=', (size_t)n)) {
        return 1;
    }
    char *k = cpath(name, n);
    if (!k) {
        return 1;
    }
    int rc;
    if (set) {
        char *v = cpath(value, vn);
        if (!v) {
            free(k);
            return 1;
        }
        rc = setenv(k, v, 1);
        free(v);
    } else {
        rc = unsetenv(k);
    }
    free(k);
    return rc == 0 ? 0 : 1;
}

int iris_os_getenv(const char *name, int64_t n, const char **p, int64_t *len) {
    char *k = cpath(name, n);
    if (!k) {
        return 0;
    }
    const char *v = getenv(k);
    free(k);
    if (!v) {
        return 0;
    }
    *p = v;
    *len = (int64_t)strlen(v);
    return 1;
}

void iris_os_exit(int code) {
    fflush(stdout);
    exit(code);
}

/* errno as fs's error codes: 1 notFound, 2 permission, 3 exists, 4 isDir, 5 other */
static int fsCode(int e) {
    switch (e) {
    case ENOENT: case ENOTDIR: return 1;
    case EACCES: case EPERM: return 2;
    case EEXIST: return 3;
    case EISDIR: return 4;
    case ENOSPC:
#ifdef EDQUOT
    case EDQUOT:
#endif
        return 8;
    case EXDEV: return 9;
    default: return 5;
    }
}

int iris_fs_read_all(const char *path, int64_t n, IrisString *out) {
    char *p = cpath(path, n);
    if (!p) {
        return 1;
    }
    struct stat st;
    if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
        free(p);
        return 4;
    }
    FILE *f = fopen(p, "rb");
    int err = errno;
    free(p);
    if (!f) {
        return fsCode(err);
    }
    char buf[65536];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
        iris_string_append(out, buf, (int64_t)got);
    }
    int bad = ferror(f);
    fclose(f);
    return bad ? 5 : 0;
}

int iris_fs_write_fd(int fd, const char *data, int64_t len);

/* Without stdio, which would stat the file to choose a buffer. */
int iris_fs_write(const char *path, int64_t n, const char *data, int64_t len, int append) {
    char *p = cpath(path, n);
    if (!p) {
        return 1;
    }
    int fd = open(p, O_WRONLY | O_CREAT | O_CLOEXEC | (append ? O_APPEND : O_TRUNC), 0666);
    int err = errno;
    free(p);
    if (fd < 0) {
        return fsCode(err);
    }
    int code = iris_fs_write_fd(fd, data, len);
    if (close(fd) != 0 && !code) code = 5;
    return code;
}

int iris_fs_exists(const char *path, int64_t n) {
    char *p = cpath(path, n);
    if (!p) {
        return 0;
    }
    struct stat st;
    int ok = stat(p, &st) == 0;
    free(p);
    return ok;
}

int iris_fs_remove(const char *path, int64_t n) {
    char *p = cpath(path, n);
    if (!p) {
        return 1;
    }
    int r = remove(p);
    int err = errno;
    free(p);
    return r == 0 ? 0 : fsCode(err);
}

/* size, whether it is a folder, the time it was changed (ns since 1970) */
int iris_fs_stat(const char *path, int64_t n, int64_t *size, int *isDir, int64_t *modified) {
    char *p = cpath(path, n);
    if (!p) {
        return 1;
    }
    struct stat st;
    int r = stat(p, &st);
    int err = errno;
    free(p);
    if (r != 0) {
        return fsCode(err);
    }
    *size = (int64_t)st.st_size;
    *isDir = S_ISDIR(st.st_mode) ? 1 : 0;
#ifdef __APPLE__
    *modified = (int64_t)st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
    *modified = (int64_t)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#endif
    return 0;
}

/* A folder; with parents, also the folders above it, and no error when
   it exists already. */
int iris_fs_mkdir(const char *path, int64_t n, int parents) {
    char *p = cpath(path, n);
    if (!p) {
        return 1;
    }
    int code = 0;
    if (parents && mkdir(p, 0777) == 0) {
        /* the folder above was there: the usual case, in one call */
    } else if (parents && errno == EEXIST) {
        struct stat st;
        if (stat(p, &st) != 0 || !S_ISDIR(st.st_mode)) code = 3;
    } else if (parents) {
        for (char *s = p + 1; *s && !code; s++) {
            if (*s == '/') {
                *s = 0;
                if (mkdir(p, 0777) != 0 && errno != EEXIST) code = fsCode(errno);
                *s = '/';
            }
        }
        if (!code && mkdir(p, 0777) != 0 && errno != EEXIST) code = fsCode(errno);
    } else if (mkdir(p, 0777) != 0) {
        code = fsCode(errno);
    }
    free(p);
    return code;
}

int iris_fs_rename(const char *from, int64_t fn, const char *to, int64_t tn) {
    char *a = cpath(from, fn), *b = cpath(to, tn);
    int code = 1;
    if (a && b) {
        code = rename(a, b) == 0 ? 0 : fsCode(errno);
    }
    free(a);
    free(b);
    return code;
}

/* name in the folder dirfd, a folder (isDir 1), not one (0), or not known
   (-1), and everything in it. Names are taken from the folder each is in,
   so the system doesn't follow the whole path for each one. */
static int removeIn(int dirfd, const char *name, int isDir) {
    if (isDir < 0) {
        struct stat st;
        if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) return errno == ENOENT ? 0 : fsCode(errno);
        isDir = S_ISDIR(st.st_mode);
    }
    if (!isDir) {
        return unlinkat(dirfd, name, 0) == 0 || errno == ENOENT ? 0 : fsCode(errno);
    }
    int fd = openat(dirfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 0 : fsCode(errno);
    DIR *d = fdopendir(fd);
    if (!d) {
        int code = fsCode(errno);
        close(fd);
        return code;
    }
    struct dirent *e;
    int code = 0;
    while (!code && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        int kind = -1;
#ifdef DT_DIR
        if (e->d_type != DT_UNKNOWN) kind = e->d_type == DT_DIR;
#endif
        code = removeIn(fd, e->d_name, kind);
    }
    closedir(d);
    if (code) return code;
    return unlinkat(dirfd, name, AT_REMOVEDIR) == 0 || errno == ENOENT ? 0 : fsCode(errno);
}

static int removeTree(const char *p) {
    struct stat st;
    if (lstat(p, &st) != 0) {
        return errno == ENOENT ? 0 : fsCode(errno);
    }
    return removeIn(AT_FDCWD, p, S_ISDIR(st.st_mode));
}

/* A file, or a folder and everything in it; nothing there is no error. */
int iris_fs_remove_all(const char *path, int64_t n) {
    char *p = cpath(path, n);
    if (!p) {
        return 1;
    }
    int code = removeTree(p);
    free(p);
    return code;
}

void *iris_fs_dir_open(const char *path, int64_t n, int *err) {
    char *p = cpath(path, n);
    if (!p) {
        *err = 1;
        return NULL;
    }
    DIR *d = opendir(p);
    *err = d ? 0 : fsCode(errno);
    free(p);
    return d;
}

/* The next name in the folder (not . or ..): 1, or 0 at the end. */
int iris_fs_dir_next(void *dir, IrisString *name, int *isDir) {
    struct dirent *e;
    while ((e = readdir((DIR *)dir))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        iris_string_append(name, e->d_name, (int64_t)strlen(e->d_name));
#ifdef DT_DIR
        if (e->d_type != DT_UNKNOWN) {
            *isDir = e->d_type == DT_DIR;
            return 1;
        }
#endif
        *isDir = 0;
        int fd = dirfd((DIR *)dir);
        struct stat st;
        if (fstatat(fd, e->d_name, &st, 0) == 0) *isDir = S_ISDIR(st.st_mode);
        return 1;
    }
    return 0;
}

/* fs.find: a name's size, kind and time, relative to the open folder, so
   the system doesn't look the folder's path up again for each file. */
int iris_fs_dir_stat(void *dir, const char *name, int64_t n, int64_t *size, int *isDir, int64_t *modified) {
    char small[256];
    char *p = n < (int64_t)sizeof small ? small : malloc((size_t)n + 1);
    if (!p) outOfMemory();
    memcpy(p, name, (size_t)n);
    p[n] = 0;
    struct stat st;
    int r = fstatat(dirfd((DIR *)dir), p, &st, AT_SYMLINK_NOFOLLOW);
    int err = errno;
    if (p != small) free(p);
    if (r != 0) {
        return fsCode(err);
    }
    *size = (int64_t)st.st_size;
    *isDir = S_ISDIR(st.st_mode) ? 1 : 0;
#ifdef __APPLE__
    *modified = (int64_t)st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
    *modified = (int64_t)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#endif
    return 0;
}

/* What stat knows, as 10 numbers: size, is a folder, modified and
   accessed (ns), permission bits, uid, gid, is a link, link count, inode.
   dir NULL: path alone, following a link at its end when follow; else
   name in dir, not following. 0, or a code. */
static void infoOf(const struct stat *st, int64_t *v) {
    v[0] = (int64_t)st->st_size;
    v[1] = S_ISDIR(st->st_mode) ? 1 : 0;
#ifdef __APPLE__
    v[2] = (int64_t)st->st_mtimespec.tv_sec * 1000000000LL + st->st_mtimespec.tv_nsec;
    v[3] = (int64_t)st->st_atimespec.tv_sec * 1000000000LL + st->st_atimespec.tv_nsec;
#else
    v[2] = (int64_t)st->st_mtim.tv_sec * 1000000000LL + st->st_mtim.tv_nsec;
    v[3] = (int64_t)st->st_atim.tv_sec * 1000000000LL + st->st_atim.tv_nsec;
#endif
    v[4] = (int64_t)(st->st_mode & 07777);
    v[5] = (int64_t)st->st_uid;
    v[6] = (int64_t)st->st_gid;
    v[7] = S_ISLNK(st->st_mode) ? 1 : 0;
    v[8] = (int64_t)st->st_nlink;
    v[9] = (int64_t)st->st_ino;
}

int iris_fs_info(void *dir, const char *path, int64_t n, int follow, int64_t *v) {
    char *p = cpath(path, n);
    if (!p) return 1;
    struct stat st;
    int r = dir ? fstatat(dirfd((DIR *)dir), p, &st, AT_SYMLINK_NOFOLLOW) : follow ? stat(p, &st) : lstat(p, &st);
    int err = errno;
    free(p);
    if (r != 0) return fsCode(err);
    infoOf(&st, v);
    return 0;
}

int iris_fs_file_info(int fd, int64_t *v) {
    struct stat st;
    if (fstat(fd, &st) != 0) return fsCode(errno);
    infoOf(&st, v);
    return 0;
}

/* chmod, chown (-1 leaves an id), times (ns; -1 leaves one), symlink,
   link, readlink, realpath: 0, or a code. */
int iris_fs_chmod(const char *path, int64_t n, int64_t mode) {
    char *p = cpath(path, n);
    if (!p) return 1;
    int r = chmod(p, (mode_t)mode);
    int err = errno;
    free(p);
    return r == 0 ? 0 : fsCode(err);
}

int iris_fs_chown(const char *path, int64_t n, int64_t uid, int64_t gid, int follow) {
    char *p = cpath(path, n);
    if (!p) return 1;
    int r = follow ? chown(p, (uid_t)uid, (gid_t)gid) : lchown(p, (uid_t)uid, (gid_t)gid);
    int err = errno;
    free(p);
    return r == 0 ? 0 : fsCode(err);
}

int iris_fs_times(const char *path, int64_t n, int64_t accessed, int64_t modified) {
    char *p = cpath(path, n);
    if (!p) return 1;
    struct timespec ts[2];
    for (int i = 0; i < 2; i++) {
        int64_t t = i == 0 ? accessed : modified;
        if (t < 0) {
            ts[i].tv_sec = 0;
            ts[i].tv_nsec = UTIME_OMIT;
        } else {
            ts[i].tv_sec = (time_t)(t / 1000000000LL);
            ts[i].tv_nsec = (long)(t % 1000000000LL);
        }
    }
    int r = utimensat(AT_FDCWD, p, ts, 0);
    int err = errno;
    free(p);
    return r == 0 ? 0 : fsCode(err);
}

int iris_fs_link(const char *a, int64_t an, const char *b, int64_t bn, int symbolic) {
    char *pa = cpath(a, an), *pb = cpath(b, bn);
    int r = pa && pb ? (symbolic ? symlink(pa, pb) : link(pa, pb)) : -1;
    int err = pa && pb ? errno : ENOENT;
    free(pa);
    free(pb);
    return r == 0 ? 0 : fsCode(err);
}

int iris_fs_readlink(const char *path, int64_t n, int real, IrisString *out) {
    char *p = cpath(path, n);
    if (!p) return 1;
    char buf[PATH_MAX + 1];
    int code = 0;
    if (real) {
        if (realpath(p, buf)) iris_string_append(out, buf, (int64_t)strlen(buf));
        else code = fsCode(errno);
    } else {
        ssize_t k = readlink(p, buf, PATH_MAX);
        if (k >= 0) iris_string_append(out, buf, (int64_t)k);
        else code = errno == EINVAL ? 7 : fsCode(errno);
    }
    free(p);
    return code;
}

/* The space of the file system path is on: total, free, free for anyone
   (not only root), in bytes, and the files it has room for and has left. */
#include <sys/statvfs.h>
int iris_fs_space(const char *path, int64_t n, int64_t *v) {
    char *p = cpath(path, n);
    if (!p) return 1;
    struct statvfs sv;
    int r = statvfs(p, &sv);
    int err = errno;
    free(p);
    if (r != 0) return fsCode(err);
    uint64_t unit = sv.f_frsize ? sv.f_frsize : sv.f_bsize;
    v[0] = (int64_t)(sv.f_blocks * unit);
    v[1] = (int64_t)(sv.f_bfree * unit);
    v[2] = (int64_t)(sv.f_bavail * unit);
    v[3] = (int64_t)sv.f_files;
    v[4] = (int64_t)sv.f_ffree;
    v[5] = (int64_t)sv.f_bsize;
    return 0;
}

/* Reads and writes at an offset, without moving the file's position:
   bytes read (0 at the end) or -code; 0 or a code. */
int64_t iris_fs_pread(int fd, char *buf, int64_t len, int64_t off) {
    for (;;) {
        ssize_t k = pread(fd, buf, (size_t)len, (off_t)off);
        if (k >= 0) return k;
        if (errno != EINTR) return -fsCode(errno);
    }
}

int iris_fs_pwrite(int fd, const char *buf, int64_t len, int64_t off) {
    while (len > 0) {
        ssize_t k = pwrite(fd, buf, (size_t)len, (off_t)off);
        if (k < 0) {
            if (errno == EINTR) continue;
            return fsCode(errno);
        }
        buf += k;
        len -= k;
        off += k;
    }
    return 0;
}

/* op 1 fsync, 2 fdatasync, 3 truncate to a, 4 allocate a..a+b, 5 seek to a
   (gives the position), 6 the position. 0 (or the position), or -code. */
int64_t iris_fs_ctl(int fd, int op, int64_t a, int64_t b) {
    int r = 0;
    switch (op) {
    case 1:
#ifdef __APPLE__
        r = fcntl(fd, F_FULLFSYNC) == 0 ? 0 : fsync(fd);     /* fsync alone doesn't reach the disk on macOS */
#else
        r = fsync(fd);
#endif
        break;
    case 2:
#ifdef __APPLE__
        r = fcntl(fd, F_FULLFSYNC) == 0 ? 0 : fsync(fd);
#else
        r = fdatasync(fd);
#endif
        break;
    case 3:
        r = ftruncate(fd, (off_t)a);
        break;
    case 4: {
#ifdef __linux__
        r = fallocate(fd, 0, (off_t)a, (off_t)b);
        if (r != 0 && (errno == EOPNOTSUPP || errno == ENOSYS)) r = posix_fallocate(fd, (off_t)a, (off_t)b) == 0 ? 0 : -1;
#elif defined(__APPLE__)
        fstore_t st = {F_ALLOCATECONTIG | F_ALLOCATEALL, F_PEOFPOSMODE, 0, (off_t)(a + b), 0};
        struct stat cur;
        if (fstat(fd, &cur) == 0 && cur.st_size >= a + b) break;
        st.fst_length = (off_t)(a + b - (fstat(fd, &cur) == 0 ? cur.st_size : 0));
        if (fcntl(fd, F_PREALLOCATE, &st) != 0) {
            st.fst_flags = F_ALLOCATEALL;
            fcntl(fd, F_PREALLOCATE, &st);
        }
        r = ftruncate(fd, (off_t)(a + b));
#else
        r = posix_fallocate(fd, (off_t)a, (off_t)b) == 0 ? 0 : -1;
#endif
        break;
    }
    case 5: {
        off_t at = lseek(fd, (off_t)a, SEEK_SET);
        return at < 0 ? -fsCode(errno) : (int64_t)at;
    }
    case 6: {
        off_t at = lseek(fd, 0, SEEK_CUR);
        return at < 0 ? -fsCode(errno) : (int64_t)at;
    }
    default:
        return -5;
    }
    return r == 0 ? 0 : -fsCode(errno);
}

/* fsync of a folder, so a file made, renamed or removed in it stays so
   after a crash. */
int iris_fs_sync_dir(const char *path, int64_t n) {
    char *p = cpath(path, n);
    if (!p) return 1;
    int fd = open(p, O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    int err = errno;
    free(p);
    if (fd < 0) return fsCode(err);
    int r = fsync(fd);
    err = errno;
    close(fd);
    return r == 0 ? 0 : fsCode(err);
}

void iris_fs_dir_close(void *dir) {
    if (dir) closedir((DIR *)dir);
}

/* An open file: mode 0 reading, 1 writing (emptied or made), 2 adding to
   the end (made if needed). The descriptor, or -1 with err set. */
int iris_fs_open(const char *path, int64_t n, int mode, int *err) {
    char *p = cpath(path, n);
    if (!p) {
        *err = 1;
        return -1;
    }
    int flags = mode == 0   ? O_RDONLY
                : mode == 1 ? (O_WRONLY | O_CREAT | O_TRUNC)
                : mode == 2 ? (O_WRONLY | O_CREAT | O_APPEND)
                : mode == 3 ? (O_RDWR | O_CREAT)
                            : O_RDWR;           /* 4: read and write, not made */
    int fd = open(p, flags | O_CLOEXEC, 0666);
    *err = fd < 0 ? fsCode(errno) : 0;
    if (fd >= 0 && mode == 0) {
        struct stat st;
        if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
            close(fd);
            fd = -1;
            *err = 4;
        }
    }
    free(p);
    return fd;
}

/* Maps a file into memory: all of it, grown to size first when write is
 * set and the file is smaller (made if needed). With write, changes go to
 * the file and to every other mapping of it; without, they stay in this
 * mapping. 0, or a code. */
int iris_fs_map(const char *path, int64_t n, int write, int64_t size, char **ptr, int64_t *len) {
    char *p = cpath(path, n);
    *ptr = NULL;
    *len = 0;
    if (!p) return 1;
    int fd = open(p, (write ? (O_RDWR | (size > 0 ? O_CREAT : 0)) : O_RDONLY) | O_CLOEXEC, 0666);
    free(p);
    if (fd < 0) return fsCode(errno);
    struct stat st;
    if (fstat(fd, &st) != 0 || S_ISDIR(st.st_mode)) {
        int code = S_ISDIR(st.st_mode) ? 4 : fsCode(errno);
        close(fd);
        return code;
    }
    int64_t have = st.st_size;
    if (write && size > have) {
        if (ftruncate(fd, (off_t)size) != 0) {
            int code = fsCode(errno);
            close(fd);
            return code;
        }
        have = size;
    }
    if (have == 0) {
        close(fd);
        return 0;
    }
    void *m = mmap(NULL, (size_t)have, PROT_READ | PROT_WRITE, write ? MAP_SHARED : MAP_PRIVATE, fd, 0);
    int e = errno;
    close(fd);
    if (m == MAP_FAILED) return fsCode(e);
    *ptr = m;
    *len = have;
    return 0;
}

/* size bytes of zeroed memory from the OS, or NULL. */
char *iris_mem_map(int64_t size) {
    if (size <= 0) return NULL;
    void *m = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return m == MAP_FAILED ? NULL : m;
}

void iris_mem_unmap(char *ptr, int64_t len) {
    if (ptr && len > 0) munmap(ptr, (size_t)len);
}

/* Writes a file mapping's changes to the file now. 0, or a code. */
int iris_mem_sync(char *ptr, int64_t len) {
    if (!ptr || len <= 0) return 0;
    return msync(ptr, (size_t)len, MS_SYNC) == 0 ? 0 : fsCode(errno);
}

/* Bytes read (0 at the end), or minus an error code. */
int64_t iris_fs_read(int fd, char *buf, int64_t len) {
    for (;;) {
        ssize_t r = read(fd, buf, (size_t)len);
        if (r >= 0) return r;
        if (errno != EINTR) return -fsCode(errno);
    }
}

int iris_fs_write_fd(int fd, const char *data, int64_t len) {
    while (len > 0) {
        ssize_t w = write(fd, data, (size_t)len);
        if (w < 0) {
            if (errno == EINTR) continue;
            return fsCode(errno);
        }
        data += w;
        len -= w;
    }
    return 0;
}

void iris_fs_close(int fd) {
    if (fd >= 0) close(fd);
}

/* ---------------------------------------------------------------- fs (0.66) */
#include <pwd.h>
#include <limits.h>
#include <grp.h>

/* Copies a file's bytes to another path, replaced if it is there, with the
   permissions of the first. 0, or a code. */
int iris_fs_copy(const char *from, int64_t fn, const char *to, int64_t tn) {
    char *a = cpath(from, fn), *b = cpath(to, tn);
    int code = 1;
    if (!a || !b) goto done;
    int in = open(a, O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        code = fsCode(errno);
        goto done;
    }
    struct stat st;
    if (fstat(in, &st) != 0 || S_ISDIR(st.st_mode)) {
        code = S_ISDIR(st.st_mode) ? 4 : fsCode(errno);
        close(in);
        goto done;
    }
    int out = open(b, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st.st_mode & 07777);
    if (out < 0) {
        code = fsCode(errno);
        close(in);
        goto done;
    }
    char buf[65536];
    code = 0;
    for (;;) {
        ssize_t r = read(in, buf, sizeof buf);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) {
            code = fsCode(errno);
            break;
        }
        if (r == 0) break;
        if ((code = iris_fs_write_fd(out, buf, r)) != 0) break;
    }
    if (!code) fchmod(out, st.st_mode & 07777);
    close(in);
    if (close(out) != 0 && !code) code = 5;
done:
    free(a);
    free(b);
    return code;
}

/* A new file (folder set: a folder) named prefix-XXXXXX in dir, or in
   $TMPDIR or /tmp when dir is empty. Its path goes in out. 0, or a code. */
int iris_fs_temp(const char *prefix, int64_t pn, const char *dir, int64_t dn, int folder, IrisString *out) {
    if (memchr(prefix, '/', (size_t)pn) || memchr(prefix, 0, (size_t)pn) || memchr(dir, 0, (size_t)dn)) return 5;
    const char *base = dir;
    size_t bn = (size_t)dn;
    if (bn == 0) {
        base = getenv("TMPDIR");
        if (!base || !*base) base = "/tmp";
        bn = strlen(base);
    }
    while (bn > 1 && base[bn - 1] == '/') bn--;
    size_t len = bn + 1 + (size_t)pn + 8;
    char *t = malloc(len);
    if (!t) outOfMemory();
    snprintf(t, len, "%.*s/%.*s-XXXXXX", (int)bn, base, (int)pn, prefix);
    int code = 0;
    if (folder) {
        if (!mkdtemp(t)) code = fsCode(errno);
    } else {
        int fd = mkstemp(t);
        if (fd < 0) code = fsCode(errno);
        else close(fd);
    }
    if (!code) iris_string_append(out, t, (int64_t)strlen(t));
    free(t);
    return code;
}

/* The working folder, into out. 0, or a code. */
int iris_os_cwd(IrisString *out) {
    char buf[4096];
    if (!getcwd(buf, sizeof buf)) return fsCode(errno);
    iris_string_append(out, buf, (int64_t)strlen(buf));
    return 0;
}

int iris_os_chdir(const char *path, int64_t n) {
    char *p = cpath(path, n);
    if (!p) return 1;
    int code = chdir(p) == 0 ? 0 : fsCode(errno);
    free(p);
    return code;
}

/* Runs the program as user from now on, and as group, or the user's own
   group when group is empty: the user's groups, then the group, then the
   user, so root can't be taken back (and on Linux every capability goes).
   0; 1 no such user, 2 no such group, 3 not allowed, 4 root could be
   taken back, 5 another error. Already that user and group: 0. */
#ifdef __linux__
#include <sys/prctl.h>
#include <linux/capability.h>
#include <sys/syscall.h>

/* The calling thread's capabilities: effective and permitted set to
   keep's bits (CAP_* numbers below 64), none inheritable. */
static int capsSet(uint64_t keep) {
    struct __user_cap_header_struct h = {_LINUX_CAPABILITY_VERSION_3, 0};
    struct __user_cap_data_struct d[2];
    memset(d, 0, sizeof d);
    d[0].effective = d[0].permitted = (uint32_t)keep;
    d[1].effective = d[1].permitted = (uint32_t)(keep >> 32);
    return (int)syscall(SYS_capset, &h, d);
}

/* Whether the calling thread has capability cap in its effective set. */
int iris_os_has_cap(int cap) {
    struct __user_cap_header_struct h = {_LINUX_CAPABILITY_VERSION_3, 0};
    struct __user_cap_data_struct d[2];
    memset(d, 0, sizeof d);
    if (syscall(SYS_capget, &h, d) != 0 || cap < 0 || cap >= 64) return 0;
    return (d[cap / 32].effective >> (cap % 32)) & 1;
}
#else
int iris_os_has_cap(int cap) {
    (void)cap;
    return geteuid() == 0;
}
#endif

int iris_os_run_as(const char *user, int64_t ulen, const char *group, int64_t glen, uint64_t keep) {
    char *u = cpath(user, ulen);
    if (!u) return 1;
    char buf[16384];
    struct passwd pw, *pwp = NULL;
    if (getpwnam_r(u, &pw, buf, sizeof buf, &pwp) != 0 || !pwp) {
        free(u);
        return 1;
    }
    uid_t uid = pw.pw_uid;
    gid_t gid = pw.pw_gid;
    if (glen > 0) {
        char *g = cpath(group, glen);
        char gbuf[16384];
        struct group gr, *grp = NULL;
        int r = g ? getgrnam_r(g, &gr, gbuf, sizeof gbuf, &grp) : 1;
        free(g);
        if (r != 0 || !grp) {
            free(u);
            return 2;
        }
        gid = gr.gr_gid;
    }
    if (getuid() == uid && geteuid() == uid && getgid() == gid && getegid() == gid) {
        free(u);
        return 0;
    }
    int code = 0;
#ifdef __linux__
    /* the capabilities to keep stay permitted through setuid */
    if (keep && prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0) != 0) {
        free(u);
        return 5;
    }
#else
    if (keep) {
        free(u);
        return 6;
    }
#endif
    if (initgroups(u, (int)gid) != 0 || setgid(gid) != 0 || setuid(uid) != 0) {
        code = errno == EPERM ? 3 : 5;
    } else if (uid != 0 && !(keep & (1ULL << 7)) && setuid(0) == 0) {    /* CAP_SETUID could do it */
        code = 4;
    }
#ifdef __linux__
    if (keep) {
        prctl(PR_SET_KEEPCAPS, 0, 0, 0, 0);
        if (!code && capsSet(keep) != 0) code = 5;
    }
#endif
    free(u);
    return code;
}

int64_t iris_os_uid(void) { return (int64_t)geteuid(); }
int64_t iris_os_gid(void) { return (int64_t)getegid(); }

/* The name of the user the program runs as: 0, or 1 when it has none. */
int iris_os_user(IrisString *out) {
    char buf[16384];
    struct passwd pw, *pwp = NULL;
    if (getpwuid_r(geteuid(), &pw, buf, sizeof buf, &pwp) != 0 || !pwp) return 1;
    iris_string_append(out, pw.pw_name, (int64_t)strlen(pw.pw_name));
    return 0;
}

int iris_os_hostname(IrisString *out) {
    char buf[256];
    if (gethostname(buf, sizeof buf) != 0) return 5;
    buf[sizeof buf - 1] = 0;
    iris_string_append(out, buf, (int64_t)strlen(buf));
    return 0;
}

/* Asks for the cache lines of bytes at p, so a search in them doesn't wait
   for one line after another. */
void iris_core_prefetch(const char *p, int64_t bytes) {
    for (int64_t i = 0; i < bytes; i += 64) __builtin_prefetch(p + i);
}

/* a when c, else b, without a branch: for a search where c can't be guessed. */
int64_t iris_core_pick(int32_t c, int64_t a, int64_t b) {
    return __builtin_unpredictable(c != 0) ? a : b;
}

/* Memory grown to n bytes, moved when it must be, and not zeroed: for a
   Deque, which writes each place before it reads it. A large block grows
   by moving its pages, and pages it doesn't use yet stay untouched. */
char *iris_core_grow_raw(char *p, int64_t n) {
    char *q = realloc(p, (size_t)n);
    if (!q) outOfMemory();
    return q;
}

/* $HOME, or the home folder in the user database. 0, or 1 for none. */
int iris_os_home(IrisString *out) {
    const char *h = getenv("HOME");
    if (!h || !*h) {
        struct passwd *pw = getpwuid(getuid());
        h = pw ? pw->pw_dir : NULL;
    }
    if (!h || !*h) return 1;
    iris_string_append(out, h, (int64_t)strlen(h));
    return 0;
}

/* 0 a line, 1 end of input, 2 an error */
int iris_io_read_line(IrisString *out) {
    int c;
    int any = 0;
    while ((c = fgetc(stdin)) != EOF) {
        any = 1;
        if (c == '\n') {
            break;
        }
        char ch = (char)c;
        iris_string_append(out, &ch, 1);
    }
    if (ferror(stdin)) {
        return 2;
    }
    if (!any) {
        return 1;
    }
    if (out->len && out->ptr[out->len - 1] == '\r') {
        out->len--;
    }
    return 0;
}

int iris_io_read_all(IrisString *out) {
    char buf[65536];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, stdin)) > 0) {
        iris_string_append(out, buf, (int64_t)got);
    }
    return ferror(stdin) ? 2 : 0;
}

void iris_io_ewrite(const char *p, int64_t n) {
    fflush(stdout);
    fwrite(p, 1, (size_t)n, stderr);
}

void iris_io_write(const char *p, int64_t n) {
    fwrite(p, 1, (size_t)n, stdout);
}

/* ---------------------------------------------------------------- term */

int iris_term_isatty(int fd) { return isatty(fd); }

static struct termios termSaved;
static int termIsRaw;

/* Raw mode on stdin: each key as it is pressed, nothing shown. */
int iris_term_raw(int on) {
    if (on) {
        if (termIsRaw) return 1;
        if (tcgetattr(0, &termSaved) != 0) return 0;
        struct termios raw = termSaved;
        cfmakeraw(&raw);
        raw.c_oflag |= OPOST;           /* \n still starts a new line */
        if (tcsetattr(0, TCSADRAIN, &raw) != 0) return 0;
        termIsRaw = 1;
        return 1;
    }
    if (termIsRaw) {
        tcsetattr(0, TCSADRAIN, &termSaved);
        termIsRaw = 0;
    }
    return 1;
}

void iris_term_size(int64_t *cols, int64_t *rows) {
    struct winsize ws;
    *cols = 80;
    *rows = 24;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col) {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
    }
}

/* One byte from stdin, -1 at the end. */
int iris_term_read_byte(void) {
    for (;;) {
        int c = fgetc(stdin);
        if (c != EOF) return c;
        if (ferror(stdin) && errno == EINTR) {
            clearerr(stdin);
            continue;
        }
        return -1;
    }
}

/* Writes and flushes, in order with print. */
void iris_term_write(int fd, const char *p, int64_t n) {
    FILE *f = fd == 2 ? stderr : stdout;
    if (fd == 2) fflush(stdout);
    fwrite(p, 1, (size_t)n, f);
    fflush(f);
}

/* str.find: the C library's memmem, which scans many bytes at a time. */
/* memchr for the first byte, then a compare: fast for the short texts
 * programs look for, and memmem's setup isn't paid on each call. Many
 * false starts hand the rest to memmem, which stays linear. */
int64_t iris_str_find(const char *s, int64_t n, const char *sub, int64_t m) {
    if (m == 1) {
        const char *at = memchr(s, sub[0], (size_t)n);
        return at ? (int64_t)(at - s) : -1;
    }
    const char *p = s, *last = s + n - m;
    int misses = 0;
    while (p <= last) {
        p = memchr(p, sub[0], (size_t)(last - p + 1));
        if (!p) {
            return -1;
        }
        if (memcmp(p + 1, sub + 1, (size_t)m - 1) == 0) {
            return (int64_t)(p - s);
        }
        p++;
        if (++misses > 64) {
            const char *at = memmem(p, (size_t)(s + n - p), sub, (size_t)m);
            return at ? (int64_t)(at - s) : -1;
        }
    }
    return -1;
}

/* strtod, with a fast path for a plain decimal number: when its digits fit
   in 53 bits and its power of ten is at most 22, both are exact doubles and
   one multiply or divide gives the correctly rounded value (Clinger 1990).
   Anything else, such as more digits, hex floats or inf, goes to strtod. */
static double irisStrtod(const char *s, char **end) {
    static const double pow10[23] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
                                     1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    const char *p = s;
    bool neg = *p == '-';
    if (neg) p++;
    uint64_t w = 0;
    int nd = 0, exp = 0;
    bool any = false;
    for (; *p >= '0' && *p <= '9'; p++) {
        any = true;
        if (w || *p != '0') {
            if (nd++ == 19) return strtod(s, end);
            w = w * 10 + (uint64_t)(*p - '0');
        }
    }
    if (*p == '.') {
        for (p++; *p >= '0' && *p <= '9'; p++) {
            any = true;
            if (w || *p != '0') {
                if (nd++ == 19) return strtod(s, end);
                w = w * 10 + (uint64_t)(*p - '0');
            }
            exp--;
        }
    }
    if (!any || *p == 'x' || *p == 'X') return strtod(s, end);
    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1;
        bool eneg = *q == '-';
        if (*q == '-' || *q == '+') q++;
        if (*q < '0' || *q > '9') return strtod(s, end);
        int e = 0;
        for (; *q >= '0' && *q <= '9'; q++) {
            if (e < 100000) e = e * 10 + (*q - '0');
        }
        exp += eneg ? -e : e;
        p = q;
    }
    if (w > (1ull << 53) || exp < -22 || exp > 22) return strtod(s, end);
    double v = (double)w;
    v = exp < 0 ? v / pow10[-exp] : v * pow10[exp];
    if (end) *end = (char *)p;
    return neg ? -v : v;
}

/* str.parseFloat: 0 ok, 1 empty, 2 not a number */
int iris_str_parse_float(const char *p, int64_t n, double *out) {
    if (n == 0) {
        return 1;
    }
    char buf[128];
    if (n >= (int64_t)sizeof buf) {
        return 2;
    }
    memcpy(buf, p, (size_t)n);
    buf[n] = 0;
    char *end;
    double v = irisStrtod(buf, &end);
    if (*end || end == buf) {
        return 2;
    }
    *out = v;
    return 0;
}

/* ---------------------------------------------------------------- tasks
 *
 * Every task has its own stack. All the tasks of the program run on one
 * thread, taking turns: a task runs until it waits (for the network, a
 * channel or a timer), and then the next ready one runs. When none is
 * ready, the scheduler waits for the operating system (epoll on Linux,
 * kqueue on macOS) or for the next timer.
 *
 * Switching stacks is a few instructions of assembly per machine: save
 * the registers a function call must keep, swap the stack pointer, load
 * the other task's registers. */

#include <sys/mman.h>
#include <time.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <sys/un.h>
#include <stddef.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <sched.h>
#ifdef __APPLE__
#include <sys/event.h>
#else
#include <sys/epoll.h>
#include <poll.h>
#include <sys/syscall.h>
#include <sys/eventfd.h>
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define IRIS_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define IRIS_ASAN 1
#endif
#ifdef IRIS_ASAN
void __sanitizer_start_switch_fiber(void **fake_stack_save, const void *bottom, size_t size);
void __sanitizer_finish_switch_fiber(void *fake_stack_save, const void **bottom_old, size_t *size_old);
#endif

/* ---------------------------------------------------------------- std: core Arena */

/* An Arena gives out memory from blocks it gets from malloc and frees them
 * all at once. Each block starts with this header; what it gives out
 * follows. In debug builds the memory not given out is poisoned, so
 * AddressSanitizer reports a use after reset or free. */
typedef struct ArenaBlock {
    struct ArenaBlock *next;
    size_t cap;
} ArenaBlock;

typedef struct {
    int64_t blockSize;
    ArenaBlock *blocks;
    uint8_t *at, *end;
    int64_t given;
} IrisArena;

#ifdef IRIS_ASAN
void __asan_poison_memory_region(void const volatile *addr, size_t size);
void __asan_unpoison_memory_region(void const volatile *addr, size_t size);
#define POISON(p, n) __asan_poison_memory_region((p), (n))
#define UNPOISON(p, n) __asan_unpoison_memory_region((p), (n))
#else
#define POISON(p, n) ((void)(p), (void)(n))
#define UNPOISON(p, n) ((void)(p), (void)(n))
#endif

static uint8_t *blockData(ArenaBlock *b) { return (uint8_t *)(b + 1); }

static ArenaBlock *newBlock(size_t cap) {
    ArenaBlock *b = malloc(sizeof(ArenaBlock) + cap);
    if (!b) {
        outOfMemory();
    }
    b->next = NULL;
    b->cap = cap;
    POISON(blockData(b), cap);
    return b;
}

void *iris_arena_alloc(IrisArena *a, int64_t size, int64_t count, int64_t align, const char *file, int line,
                       int col, const char *fn) {
    if (count < 0) {
        char msg[96];
        int n = snprintf(msg, sizeof msg, "alloc of %lld items", (long long)count);
        stop(msg, (size_t)n, file, line, col, fn);
    }
    if (count == 0) {
        return NULL;
    }
    if (size > 0 && count > INT64_MAX / size) {
        stop("alloc size overflows", 20, file, line, col, fn);
    }
    size_t n = (size_t)count * (size_t)(size ? size : 1);
    size_t al = align > 0 ? (size_t)align : 1;
    size_t bs = a->blockSize > 0 ? (size_t)a->blockSize : 64 * 1024;
    uintptr_t p = ((uintptr_t)a->at + al - 1) & ~(uintptr_t)(al - 1);
    if (!a->at || p + n > (uintptr_t)a->end) {
        if (n + al > bs / 4) {
            /* a big one gets a block of its own, and the current block stays */
            ArenaBlock *b = newBlock(n + al);
            if (a->blocks) {
                b->next = a->blocks->next;
                a->blocks->next = b;
            } else {
                a->blocks = b;
            }
            p = ((uintptr_t)blockData(b) + al - 1) & ~(uintptr_t)(al - 1);
            UNPOISON((void *)p, n);
            memset((void *)p, 0, n);
            a->given += (int64_t)n;
            return (void *)p;
        }
        ArenaBlock *b = newBlock(bs);
        b->next = a->blocks;
        a->blocks = b;
        a->at = blockData(b);
        a->end = a->at + bs;
        p = ((uintptr_t)a->at + al - 1) & ~(uintptr_t)(al - 1);
    }
    a->at = (uint8_t *)(p + n);
    UNPOISON((void *)p, n);
    memset((void *)p, 0, n);
    a->given += (int64_t)n;
    return (void *)p;
}

/* Everything given out is given back; the block in use is kept for more. */
void iris_arena_reset(IrisArena *a) {
    ArenaBlock *keep = NULL;
    for (ArenaBlock *b = a->blocks, *next; b; b = next) {
        next = b->next;
        if (!keep && a->at && a->at >= blockData(b) && a->at <= blockData(b) + b->cap) {
            keep = b;
            continue;
        }
        free(b);
    }
    a->blocks = keep;
    a->given = 0;
    if (keep) {
        keep->next = NULL;
        a->at = blockData(keep);
        a->end = a->at + keep->cap;
        POISON(blockData(keep), keep->cap);
    } else {
        a->at = a->end = NULL;
    }
}

void iris_arena_free(IrisArena *a) {
    for (ArenaBlock *b = a->blocks, *next; b; b = next) {
        next = b->next;
        free(b);
    }
    a->blocks = NULL;
    a->at = a->end = NULL;
    a->given = 0;
}

#ifdef __APPLE__
#define SYM(x) "_" #x
#else
#define SYM(x) #x
#endif

/* iris_ctx_switch(void **save, void *next): saves this stack's pointer in
 * *save and continues on next. */
void iris_ctx_switch(void **save, void *next);

#if defined(__x86_64__)
__asm__(".text\n"
        ".globl " SYM(iris_ctx_switch) "\n"
        SYM(iris_ctx_switch) ":\n"
        "    pushq %rbp\n"
        "    pushq %rbx\n"
        "    pushq %r12\n"
        "    pushq %r13\n"
        "    pushq %r14\n"
        "    pushq %r15\n"
        "    movq %rsp, (%rdi)\n"
        "    movq %rsi, %rsp\n"
        "    popq %r15\n"
        "    popq %r14\n"
        "    popq %r13\n"
        "    popq %r12\n"
        "    popq %rbx\n"
        "    popq %rbp\n"
        "    ret\n");
#define CTX_FRAME 64            /* six registers, the return address, padding */
#elif defined(__aarch64__)
__asm__(".text\n"
        ".globl " SYM(iris_ctx_switch) "\n"
        ".p2align 2\n"
        SYM(iris_ctx_switch) ":\n"
        "    sub sp, sp, #176\n"
        "    stp x19, x20, [sp, #0]\n"
        "    stp x21, x22, [sp, #16]\n"
        "    stp x23, x24, [sp, #32]\n"
        "    stp x25, x26, [sp, #48]\n"
        "    stp x27, x28, [sp, #64]\n"
        "    stp x29, x30, [sp, #80]\n"
        "    stp d8, d9, [sp, #96]\n"
        "    stp d10, d11, [sp, #112]\n"
        "    stp d12, d13, [sp, #128]\n"
        "    stp d14, d15, [sp, #144]\n"
        "    mov x2, sp\n"
        "    str x2, [x0]\n"
        "    mov sp, x1\n"
        "    ldp x19, x20, [sp, #0]\n"
        "    ldp x21, x22, [sp, #16]\n"
        "    ldp x23, x24, [sp, #32]\n"
        "    ldp x25, x26, [sp, #48]\n"
        "    ldp x27, x28, [sp, #64]\n"
        "    ldp x29, x30, [sp, #80]\n"
        "    ldp d8, d9, [sp, #96]\n"
        "    ldp d10, d11, [sp, #112]\n"
        "    ldp d12, d13, [sp, #128]\n"
        "    ldp d14, d15, [sp, #144]\n"
        "    add sp, sp, #176\n"
        "    ret\n");
#define CTX_FRAME 176
#else
#error "tasks need a context switch for this machine"
#endif

#define STACK_SIZE (256 * 1024)

typedef struct Task Task;
struct Task {
    void *sp;                   /* the saved stack pointer, while not running */
    char *stack;                /* mmap'd, with a guard page below; NULL for the main task */
    size_t stackSize;
    void (*fn)(void *);
    void *arg;
    Task *next;                 /* in the ready queue */
    int ready;                  /* in the ready queue now */
    int dead;
    int64_t wakeAt;             /* sleeping until (ns, monotonic); 0 if not */
    int ioResult;               /* what the io_uring operation it waited for returned */
    Task *nextSleeper;
    int *sleepClaim;            /* select's claim, which a timeout must win to wake it */
    struct Sched *sched;        /* the thread it belongs to */
    Task *inboxNext;            /* in its thread's inbox */
    void *asanFake;             /* AddressSanitizer's bookkeeping while switched out */
    const void *asanBottom;     /* the main task's stack, learned on the first switch */
    size_t asanSize;
};

/* Each thread has its own scheduler: its tasks never move to another one. */
static _Thread_local Task mainTask;
static _Thread_local Task *currentTask;
static _Thread_local Task *readyHead, *readyTail;
static _Thread_local Task *sleepers;    /* sorted by wakeAt */
static _Thread_local Task *deadTask;    /* to free once off its stack */
static _Thread_local int waitingIo;     /* tasks waiting for a file descriptor */
static _Thread_local int poller = -1;
static _Thread_local Task *switchedFrom;    /* the task the last switch left */
static _Thread_local int liveTasks;         /* spawned and not finished */
static _Thread_local bool waitingForTasks;  /* a thread's first task waits for the others */

/* The tasks waiting on each file descriptor. A descriptor is added to the
   poller once, edge-triggered, and stays there until it is closed. */
typedef struct {
    Task *reader, *writer;
    int added;
} FdWait;
static _Thread_local FdWait *fdWaits;
static _Thread_local int fdWaitsCap;

/* What other threads see of a thread's scheduler: the tasks they woke,
   and a descriptor that wakes the thread's event loop. */
typedef struct Sched {
    int lock;
    Task *inboxHead, *inboxTail;
    int wakeR, wakeW;           /* an eventfd on Linux (both the same), a pipe elsewhere */
    int wakeSent;               /* the descriptor has been written since the loop last read it */
    int stopSeen;               /* the listener stops it has acted on */
} Sched;

static _Thread_local Sched *mySched;
static int liveThreads = 1;

/* Listeners that Listener.interrupt stopped: accept on them fails with the
   connection closed. The socket is left as it is, since a process it was
   handed to may still accept on it; each thread's loop instead cancels its
   own waits on it. Every scheduler is listed, so all can be told. */
static int stopLock;
static int *stoppedFds;
static int stoppedLen, stoppedCap;
static int stopGen;
static Sched **allScheds;
static int schedsLen, schedsCap;

/* A hint to the CPU that this is a spin loop. */
static inline void cpuRelax(void) {
#if defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
}

void iris_cpu_pause(void) { cpuRelax(); }

/* Held for a few instructions at a time: spin a little before giving the
   CPU away. */
static void spinLock(int *l) {
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE)) {
        int spins = 0;
        while (__atomic_load_n(l, __ATOMIC_RELAXED)) {
            if (++spins < 128) {
                cpuRelax();
            } else {
                sched_yield();
            }
        }
    }
}

static void spinUnlock(int *l) {
    __atomic_store_n(l, 0, __ATOMIC_RELEASE);
}

static void outOfMemory(void);

static Task *startThread(void) {
    Sched *s = calloc(1, sizeof(Sched));
    if (!s) {
        outOfMemory();
    }
#ifdef __linux__
    s->wakeR = s->wakeW = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (s->wakeR < 0) {
        outOfMemory();
    }
#else
    int fds[2];
    if (pipe(fds) != 0) {
        outOfMemory();
    }
    for (int i = 0; i < 2; i++) {
        fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL, 0) | O_NONBLOCK);
        fcntl(fds[i], F_SETFD, FD_CLOEXEC);
    }
    s->wakeR = fds[0];
    s->wakeW = fds[1];
#endif
    mySched = s;
    spinLock(&stopLock);
    if (schedsLen == schedsCap) {
        schedsCap = schedsCap ? schedsCap * 2 : 16;
        allScheds = realloc(allScheds, (size_t)schedsCap * sizeof *allScheds);
        if (!allScheds) {
            outOfMemory();
        }
    }
    allScheds[schedsLen++] = s;
    s->stopSeen = stopGen;
    spinUnlock(&stopLock);
    mainTask.sched = s;
    currentTask = &mainTask;
    return currentTask;
}

#define current (currentTask ? currentTask : startThread())

static void makeReady(Task *t);

/* Makes t ready, also when it belongs to another thread. */
static void wakeTask(Task *t) {
    (void)current;
    if (t->sched == mySched) {
        makeReady(t);
        return;
    }
    Sched *s = t->sched;
    spinLock(&s->lock);
    t->inboxNext = NULL;
    if (s->inboxTail) {
        s->inboxTail->inboxNext = t;
    } else {
        s->inboxHead = t;
    }
    s->inboxTail = t;
    int send = !s->wakeSent;
    s->wakeSent = 1;
    spinUnlock(&s->lock);
    if (send) {
        uint64_t one = 1;
        ssize_t r = write(s->wakeW, &one, s->wakeW == s->wakeR ? 8 : 1);
        (void)r;
    }
}

/* Makes the tasks other threads woke ready. */
static void drainInbox(void) {
    Sched *s = mySched;
    if (!s || !__atomic_load_n(&s->inboxHead, __ATOMIC_ACQUIRE)) {
        return;
    }
    spinLock(&s->lock);
    Task *t = s->inboxHead;
    s->inboxHead = s->inboxTail = NULL;
    spinUnlock(&s->lock);
    while (t) {
        Task *n = t->inboxNext;
        makeReady(t);
        t = n;
    }
}

static void checkStops(void);
static bool fdStopped(int fd);

/* The wake descriptor was written: empty it, so the next write is seen. */
static void wakeArrived(void) {
    Sched *s = mySched;
    char buf[64];
    while (read(s->wakeR, buf, sizeof buf) > 0) {
    }
    /* only now: a waker that saw the flag still set has put its task in
       the inbox, which is drained next */
    spinLock(&s->lock);
    s->wakeSent = 0;
    spinUnlock(&s->lock);
    drainInbox();
    checkStops();
}

static int64_t nowNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void makeReady(Task *t) {
    if (t->ready || t->dead) {
        return;
    }
    t->ready = 1;
    t->next = NULL;
    if (readyTail) {
        readyTail->next = t;
    } else {
        readyHead = t;
    }
    readyTail = t;
}

static Task *takeReady(void) {
    Task *t = readyHead;
    if (t) {
        readyHead = t->next;
        if (!readyHead) {
            readyTail = NULL;
        }
        t->ready = 0;
    }
    return t;
}

static void addSleeper(Task *t, int64_t at) {
    t->wakeAt = at;
    Task **p = &sleepers;
    while (*p && (*p)->wakeAt <= at) {
        p = &(*p)->nextSleeper;
    }
    t->nextSleeper = *p;
    *p = t;
}

static void removeSleeper(Task *t) {
    for (Task **p = &sleepers; *p; p = &(*p)->nextSleeper) {
        if (*p == t) {
            *p = t->nextSleeper;
            break;
        }
    }
    t->wakeAt = 0;
}

#ifdef __linux__
/* ------------------------------------------------------------- io_uring

   On Linux the event loop uses io_uring when the kernel allows it: a task
   that reads or accepts hands the operation to the kernel and waits for
   its result, and the loop submits all of them with one system call per
   turn. Where io_uring is missing or blocked (old kernels, containers),
   or IRIS_IO=epoll is set, it uses epoll. The structures are written out
   here so that no kernel headers are needed. */

typedef struct {
    uint32_t sqEntries, cqEntries, flags, sqThreadCpu, sqThreadIdle, features, wqFd, resv[3];
    uint32_t sqHead, sqTail, sqMask, sqCount, sqFlags, sqDropped, sqArray, sqResv;
    uint64_t sqUserAddr;
    uint32_t cqHead, cqTail, cqMask, cqCount, cqOverflow, cqCqes, cqFlags, cqResv;
    uint64_t cqUserAddr;
} RingParams;

typedef struct {
    uint8_t opcode, flags;
    uint16_t ioprio;
    int32_t fd;
    uint64_t off;
    uint64_t addr;
    uint32_t len;
    uint32_t opFlags;
    uint64_t userData;
    uint64_t pad[3];
} RingSqe;

typedef struct {
    uint64_t userData;
    int32_t res;
    uint32_t flags;
} RingCqe;

enum {
    RING_OP_POLL_ADD = 6,
    RING_OP_ACCEPT = 13,
    RING_OP_RECV = 27,
    RING_FEAT_SINGLE_MMAP = 1 << 0,
    RING_FEAT_NODROP = 1 << 1,
    RING_FEAT_EXT_ARG = 1 << 8,
    RING_ENTER_GETEVENTS = 1 << 0,
    RING_ENTER_EXT_ARG = 1 << 3,
    RING_SETUP_SINGLE_ISSUER = 1 << 12,
    RING_SETUP_DEFER_TASKRUN = 1 << 13,
    RING_ENTRIES = 256,         /* a full ring is submitted and refilled */
};

typedef struct {
    int fd;
    uint32_t *sqHead, *sqTail, *sqArray, sqMask, sqCount;
    uint32_t *cqHead, *cqTail, cqMask;
    RingSqe *sqes;
    RingCqe *cqes;
    unsigned pending;           /* written but not yet submitted */
} Ring;

static _Thread_local Ring ring = {.fd = -1};

static void schedule(void);
static void ensurePoller(void);

static bool ringSetup(void) {
    const char *io = getenv("IRIS_IO");
    if (io && strcmp(io, "epoll") == 0) {
        return false;
    }
    RingParams p;
    memset(&p, 0, sizeof p);
    /* only this thread uses its ring, and the kernel finishes its work
       when this thread asks for results, in place of interrupting it */
    p.flags = RING_SETUP_SINGLE_ISSUER | RING_SETUP_DEFER_TASKRUN;
    int fd = (int)syscall(__NR_io_uring_setup, RING_ENTRIES, &p);
    if (fd < 0) {
        memset(&p, 0, sizeof p);    /* a kernel before 6.1 */
        fd = (int)syscall(__NR_io_uring_setup, RING_ENTRIES, &p);
    }
    if (fd < 0) {
        return false;
    }
    uint32_t need = RING_FEAT_SINGLE_MMAP | RING_FEAT_NODROP | RING_FEAT_EXT_ARG;
    if ((p.features & need) != need) {
        close(fd);
        return false;
    }
    size_t sqSize = p.sqArray + p.sqEntries * sizeof(uint32_t);
    size_t cqSize = p.cqCqes + p.cqEntries * sizeof(RingCqe);
    size_t size = sqSize > cqSize ? sqSize : cqSize;
    char *rings = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
    if (rings == MAP_FAILED) {
        close(fd);
        return false;
    }
    void *sqes = mmap(NULL, p.sqEntries * sizeof(RingSqe), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                      fd, 0x10000000);
    if (sqes == MAP_FAILED) {
        munmap(rings, size);
        close(fd);
        return false;
    }
    ring.fd = fd;
    ring.sqHead = (uint32_t *)(rings + p.sqHead);
    ring.sqTail = (uint32_t *)(rings + p.sqTail);
    ring.sqArray = (uint32_t *)(rings + p.sqArray);
    ring.sqMask = *(uint32_t *)(rings + p.sqMask);
    ring.sqCount = p.sqEntries;
    ring.cqHead = (uint32_t *)(rings + p.cqHead);
    ring.cqTail = (uint32_t *)(rings + p.cqTail);
    ring.cqMask = *(uint32_t *)(rings + p.cqMask);
    ring.cqes = (RingCqe *)(rings + p.cqCqes);
    ring.sqes = sqes;
    return true;
}

/* Submits what is written; with wait, also waits up to timeout ns (-1:
   no limit) for at least one result. */
static void ringEnter(bool wait, int64_t timeout) {
    struct {
        uint64_t sigmask;
        uint32_t sigmaskSize, pad;
        uint64_t ts;
    } arg = {0};
    struct {
        int64_t sec, nsec;
    } ts = {timeout / 1000000000LL, timeout % 1000000000LL};
    unsigned flags = wait ? RING_ENTER_GETEVENTS : 0;
    if (wait && timeout >= 0) {
        arg.ts = (uint64_t)(uintptr_t)&ts;
        flags |= RING_ENTER_EXT_ARG;
    }
    long n = syscall(__NR_io_uring_enter, ring.fd, ring.pending, wait ? 1 : 0, flags,
                     flags & RING_ENTER_EXT_ARG ? (void *)&arg : NULL, sizeof arg);
    if (n > 0) {
        ring.pending -= (unsigned)n < ring.pending ? (unsigned)n : ring.pending;
    }
}

/* Writes an operation for the next submit. */
static void ringPushFlags(uint8_t opcode, int fd, void *buf, uint32_t len, uint32_t opFlags, uint64_t userData,
                          uint8_t sqeFlags, unsigned room) {
    uint32_t tail = *ring.sqTail;
    while (tail - __atomic_load_n(ring.sqHead, __ATOMIC_ACQUIRE) + room > ring.sqCount) {
        ringEnter(false, 0);    /* full: submit what is there */
    }
    uint32_t i = tail & ring.sqMask;
    RingSqe *e = &ring.sqes[i];
    memset(e, 0, sizeof *e);
    e->opcode = opcode;
    e->fd = fd;
    e->addr = (uint64_t)(uintptr_t)buf;
    e->len = len;
    e->opFlags = opFlags;
    e->userData = userData;
    e->flags = sqeFlags;
    ring.sqArray[i] = i;
    __atomic_store_n(ring.sqTail, tail + 1, __ATOMIC_RELEASE);
    ring.pending++;
}

static void ringPush(uint8_t opcode, int fd, void *buf, uint32_t len, uint32_t opFlags, uint64_t userData) {
    ringPushFlags(opcode, fd, buf, len, opFlags, userData, 0, 1);
}

/* Hands an operation to the kernel and waits for its result: a count,
   or -errno. */
static int ringOp(uint8_t opcode, int fd, void *buf, uint32_t len, uint32_t opFlags) {
    ringPush(opcode, fd, buf, len, opFlags, (uint64_t)(uintptr_t)current);
    waitingIo++;
    schedule();
    return current->ioResult;
}

/* Makes the tasks whose operations finished ready. */
static void ringReap(void) {
    uint32_t head = *ring.cqHead;
    uint32_t tail = __atomic_load_n(ring.cqTail, __ATOMIC_ACQUIRE);
    while (head != tail) {
        RingCqe *c = &ring.cqes[head & ring.cqMask];
        if (c->userData == 1) {         /* a linked timeout's own result */
            head++;
            continue;
        }
        if (c->userData == 0) {         /* the wake descriptor */
            head++;
            __atomic_store_n(ring.cqHead, head, __ATOMIC_RELEASE);
            wakeArrived();
            ringPush(RING_OP_POLL_ADD, mySched->wakeR, NULL, 0, POLLIN, 0);
            tail = __atomic_load_n(ring.cqTail, __ATOMIC_ACQUIRE);
            continue;
        }
        Task *t = (Task *)(uintptr_t)c->userData;
        t->ioResult = c->res;
        waitingIo--;
        makeReady(t);
        head++;
    }
    __atomic_store_n(ring.cqHead, head, __ATOMIC_RELEASE);
}

static bool useRing(void) {
    ensurePoller();
    return ring.fd >= 0;
}

/* An operation with a linked timeout: -ECANCELED when the time ran out. */
static int ringOpUntil(uint8_t opcode, int fd, void *buf, uint32_t len, uint32_t opFlags, int64_t deadline) {
    if (deadline <= 0) {
        return ringOp(opcode, fd, buf, len, opFlags);
    }
    int64_t left = deadline - nowNs();
    if (left <= 0) {
        return -ECANCELED;
    }
    struct {
        int64_t sec, nsec;
    } ts = {left / 1000000000LL, left % 1000000000LL};
    ringPushFlags(opcode, fd, buf, len, opFlags, (uint64_t)(uintptr_t)current, 1 << 2, 2);
    ringPushFlags(15, -1, &ts, 1, 0, 1, 0, 1);
    waitingIo++;
    schedule();
    return current->ioResult;
}
#endif

static void ensurePoller(void) {
    if (poller >= 0) {
        return;
    }
    Sched *s = current->sched;
#ifdef __APPLE__
    poller = kqueue();
    if (poller >= 0) {
        struct kevent ev;
        EV_SET(&ev, s->wakeR, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
        kevent(poller, &ev, 1, NULL, 0, NULL);
    }
#else
    if (ringSetup()) {
        poller = ring.fd;
        ringPush(RING_OP_POLL_ADD, s->wakeR, NULL, 0, POLLIN, 0);
        return;
    }
    poller = epoll_create1(0);
    if (poller >= 0) {
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = s->wakeR;
        epoll_ctl(poller, EPOLL_CTL_ADD, s->wakeR, &ev);
    }
#endif
    if (poller < 0) {
        fflush(stdout);
        fputs("panic: can't start the event loop\n", stderr);
        exit(2);
    }
}

/* Wakes the tasks waiting to read or write fd. */
static void fdArrived(int fd, int readable, int writable) {
    if (fd == mySched->wakeR) {
        wakeArrived();
        return;
    }
    if (fd < 0 || fd >= fdWaitsCap) {
        return;
    }
    FdWait *w = &fdWaits[fd];
    if (readable && w->reader) {
        makeReady(w->reader);
        w->reader = NULL;
        waitingIo--;
    }
    if (writable && w->writer) {
        makeReady(w->writer);
        w->writer = NULL;
        waitingIo--;
    }
}

/* Forgets what was known about fd: a new or closed descriptor. */
static void fdForget(int fd) {
    if (fd >= 0 && fd < fdWaitsCap) {
        fdWaits[fd] = (FdWait){0};
    }
}

/* Makes the sleepers whose time has come ready. */
static void wakeSleepers(void) {
    int64_t now = nowNs();
    while (sleepers && sleepers->wakeAt <= now) {
        Task *t = sleepers;
        sleepers = t->nextSleeper;
        t->wakeAt = 0;
        if (!t->sleepClaim || !__atomic_exchange_n(t->sleepClaim, 1, __ATOMIC_ACQ_REL)) {
            makeReady(t);
        }
    }
}

/* Waits for the operating system or the next timer, and makes the tasks
 * whose wait is over ready. With noWait it only looks at what is there,
 * for a task that yields while others are ready. */
static void pollEventsHow(bool noWait) {
    int64_t timeout = -1;
    if (sleepers) {
        int64_t d = sleepers->wakeAt - nowNs();
        timeout = d < 0 ? 0 : d;
    }
    if (noWait) timeout = 0;
    /* another thread may still wake a task here: then wait for it */
    int threads = __atomic_load_n(&liveThreads, __ATOMIC_SEQ_CST);
    drainInbox();
    if (readyHead && !noWait) {
        return;
    }
    if (waitingIo == 0 && timeout < 0 && threads == 1) {
        fflush(stdout);
        fputs("panic: every task is waiting and nothing can wake them: a deadlock\n", stderr);
        exit(2);
    }
    bool block = waitingIo > 0 || threads > 1;
    if (timeout != 0) {
        /* the program waits: what it printed goes out now, so a server's
           output is there even when it is stopped with a signal */
        fflush(stdout);
    }
#ifdef __linux__
    if (block && useRing() && waitingIo == 0) {
        /* only another thread can wake this one: it waits on the wake
           descriptor itself, which wakes it sooner than the ring does with
           its completions deferred. The ring's own wait on it ends later,
           and makes no difference then. */
        if (ring.pending) ringEnter(false, 0);
        struct pollfd pf = {mySched->wakeR, POLLIN, 0};
        struct timespec ts = {timeout / 1000000000LL, timeout % 1000000000LL};
        if (ppoll(&pf, 1, timeout < 0 ? NULL : &ts, NULL) > 0) wakeArrived();
    } else if (block && useRing()) {
        ringEnter(true, timeout);
        ringReap();
    } else
#endif
    if (block) {
        ensurePoller();
#ifdef __APPLE__
        struct kevent evs[64];
        struct timespec ts, *tsp = NULL;
        if (timeout >= 0) {
            ts.tv_sec = timeout / 1000000000LL;
            ts.tv_nsec = timeout % 1000000000LL;
            tsp = &ts;
        }
        int n = kevent(poller, NULL, 0, evs, 64, tsp);
        for (int i = 0; i < n; i++) {
            int end = (evs[i].flags & (EV_EOF | EV_ERROR)) != 0;
            fdArrived((int)evs[i].ident, evs[i].filter == EVFILT_READ || end,
                      evs[i].filter == EVFILT_WRITE || end);
        }
#else
        struct epoll_event evs[64];
        int ms = timeout < 0 ? -1 : (int)((timeout + 999999) / 1000000);
        int n = epoll_wait(poller, evs, 64, ms);
        for (int i = 0; i < n; i++) {
            uint32_t e = evs[i].events;
            int end = (e & (EPOLLERR | EPOLLHUP)) != 0;
            fdArrived(evs[i].data.fd, (e & (EPOLLIN | EPOLLRDHUP)) || end, (e & EPOLLOUT) || end);
        }
#endif
    } else if (timeout > 0) {
        struct timespec ts = {timeout / 1000000000LL, timeout % 1000000000LL};
        nanosleep(&ts, NULL);
    }
    wakeSleepers();
}

static void pollEvents(void) { pollEventsHow(false); }

/* Tells AddressSanitizer that this task now runs, after a switch. */
static void arrived(void *fake) {
#ifdef IRIS_ASAN
    const void *bottom;
    size_t size;
    __sanitizer_finish_switch_fiber(fake, &bottom, &size);
    if (switchedFrom && !switchedFrom->stack && !switchedFrom->asanBottom) {
        switchedFrom->asanBottom = bottom;     /* the main stack's bounds */
        switchedFrom->asanSize = size;
    }
#else
    (void)fake;
#endif
}

/* Finished tasks kept with their stacks, so a spawn needs no system call.
   Not under AddressSanitizer, which tracks each stack's memory. */
#define TASK_CACHE_MAX 1024     /* a busy server has hundreds of tasks per thread */
static _Thread_local Task *taskCache[TASK_CACHE_MAX];
static _Thread_local int taskCacheLen;

static void releaseTask(Task *t) {
#ifndef IRIS_ASAN
    if (taskCacheLen < TASK_CACHE_MAX) {
        taskCache[taskCacheLen++] = t;
        return;
    }
#endif
    munmap(t->stack - 4096, t->stackSize + 4096);
    free(t);
}

/* A thread that ends gives back the stacks it kept. */
static void freeTaskCache(void) {
    if (deadTask && deadTask != current) {
        releaseTask(deadTask);
        deadTask = NULL;
    }
    while (taskCacheLen > 0) {
        Task *t = taskCache[--taskCacheLen];
        munmap(t->stack - 4096, t->stackSize + 4096);
        free(t);
    }
}

static void switchTo(Task *next) {
    Task *prev = current;
    if (next == prev) {
        return;
    }
    currentTask = next;
    switchedFrom = prev;
#ifdef IRIS_ASAN
    __sanitizer_start_switch_fiber(prev->dead ? NULL : &prev->asanFake,
                                   next->stack ? (const void *)next->stack : next->asanBottom,
                                   next->stack ? next->stackSize : next->asanSize);
#endif
    iris_ctx_switch(&prev->sp, next->sp);
    arrived(prev->asanFake);
    /* back on this task: free a finished one, now that nobody runs on its stack */
    if (deadTask && deadTask != current) {
        releaseTask(deadTask);
        deadTask = NULL;
    }
}

/* Runs the next ready task; the current one must have arranged to be
 * woken (or be ready already). */
static void schedule(void) {
    Task *next;
    drainInbox();
    while (!(next = takeReady())) {
        pollEvents();
    }
    switchTo(next);
}

static void taskEntry(void) {
    arrived(NULL);
    if (deadTask && deadTask != current) {
        releaseTask(deadTask);
        deadTask = NULL;
    }
    current->fn(current->arg);
    current->dead = 1;
    deadTask = current;
    if (--liveTasks == 0 && waitingForTasks) {
        waitingForTasks = false;
        makeReady(&mainTask);
    }
    schedule();
    abort();                    /* never comes back */
}

/* spawn: a new task that will run fn(arg) */
void iris_spawn(void (*fn)(void *), void *arg) {
    Task *t;
    if (taskCacheLen > 0) {
        t = taskCache[--taskCacheLen];
        char *stack = t->stack;
        *t = (Task){0};
        t->stack = stack;
    } else {
        t = calloc(1, sizeof(Task));
        if (!t) {
            outOfMemory();
        }
        char *mem = mmap(NULL, STACK_SIZE + 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (mem == MAP_FAILED) {
            outOfMemory();
        }
        mprotect(mem, 4096, PROT_NONE);     /* a guard page: overflowing the stack crashes cleanly */
        t->stack = mem + 4096;
    }
    t->stackSize = STACK_SIZE;
    t->sched = current->sched;
    liveTasks++;
    t->fn = fn;
    t->arg = arg;
    char *top = t->stack + t->stackSize;
    char *sp = top - CTX_FRAME;
    memset(sp, 0, CTX_FRAME);
#if defined(__x86_64__)
    ((void **)sp)[6] = (void *)taskEntry;      /* where the first switch returns to */
#else
    ((void **)sp)[11] = (void *)taskEntry;     /* x30, the link register */
#endif
    t->sp = sp;
    makeReady(t);
}

static _Thread_local unsigned yieldCount;

/* lets the other ready tasks run */
void iris_yield(void) {
    makeReady(current);
    /* tasks that yield in a loop still let timers and I/O through every
       64 yields, so a task sleeping or waiting on a descriptor isn't starved */
    if (++yieldCount % 64 == 0) pollEventsHow(true);
    schedule();
}

void iris_sleep(int64_t ns) {
    if (ns <= 0) {
        iris_yield();
        return;
    }
    addSleeper(current, nowNs() + ns);
    schedule();
}

/* Waits until fd can be read (write = 0) or written (write = 1). */
static bool waitFdOn(int fd, int write, int64_t deadline);

static void waitFd(int fd, int write) {
#ifdef __linux__
    if (useRing()) {
        ringOp(RING_OP_POLL_ADD, fd, NULL, 0, write ? POLLOUT : POLLIN);
        return;
    }
#endif
    waitFdOn(fd, write, 0);
}

/* epoll and kqueue: the task waits on fd, and as a sleeper when there is a
   deadline; false when the deadline came first. */
static bool waitFdOn(int fd, int write, int64_t deadline) {
    ensurePoller();
    if (fd >= fdWaitsCap) {
        int cap = fdWaitsCap ? fdWaitsCap : 256;
        while (cap <= fd) {
            cap *= 2;
        }
        FdWait *w = realloc(fdWaits, (size_t)cap * sizeof(FdWait));
        if (!w) {
            outOfMemory();
        }
        memset(w + fdWaitsCap, 0, (size_t)(cap - fdWaitsCap) * sizeof(FdWait));
        fdWaits = w;
        fdWaitsCap = cap;
    }
    FdWait *w = &fdWaits[fd];
    if (!w->added) {
#ifdef __APPLE__
        struct kevent ev[2];
        EV_SET(&ev[0], fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
        EV_SET(&ev[1], fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, NULL);
        kevent(poller, ev, 2, NULL, 0, NULL);
#else
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET;
        ev.data.fd = fd;
        if (epoll_ctl(poller, EPOLL_CTL_ADD, fd, &ev) != 0 && errno == EEXIST) {
            epoll_ctl(poller, EPOLL_CTL_MOD, fd, &ev);
        }
#endif
        w->added = 1;
    }
    if (write) {
        w->writer = current;
    } else {
        w->reader = current;
    }
    waitingIo++;
    if (deadline > 0) {
        addSleeper(current, deadline);
    }
    schedule();
    w = &fdWaits[fd];               /* the table may have grown meanwhile */
    Task **slot = write ? &w->writer : &w->reader;
    if (*slot == current) {
        /* the timer woke it: the descriptor didn't */
        *slot = NULL;
        waitingIo--;
        return false;
    }
    if (current->wakeAt) {
        removeSleeper(current);
    }
    return true;
}

/* Like waitFd, giving up at deadline (ns on the monotonic clock; 0 for
   never). False when the time ran out. */
static bool waitFdUntil(int fd, int write, int64_t deadline) {
    if (deadline <= 0) {
        waitFd(fd, write);
        return true;
    }
    int64_t left = deadline - nowNs();
    if (left <= 0) {
        return false;
    }
#ifdef __linux__
    if (useRing()) {
        /* POLL_ADD linked to a timeout: the poll ends with -ECANCELED when it fires */
        struct {
            int64_t sec, nsec;
        } ts = {left / 1000000000LL, left % 1000000000LL};
        ringPushFlags(RING_OP_POLL_ADD, fd, NULL, 0, write ? POLLOUT : POLLIN, (uint64_t)(uintptr_t)current,
                      1 << 2 /* IOSQE_IO_LINK */, 2);
        ringPushFlags(15 /* IORING_OP_LINK_TIMEOUT */, -1, &ts, 1, 0, 1, 0, 1);
        waitingIo++;
        schedule();
        return current->ioResult >= 0;
    }
#endif
    return waitFdOn(fd, write, deadline);
}

/* ---------------------------------------------------------------- channels */

/* A task waiting on a channel or a lock. Whoever wakes it must first win
   its claim, so a task waiting in select on several channels is woken
   once. */
typedef struct Waiter Waiter;
struct Waiter {
    Task *task;
    int *claimed;
    Waiter *next;
    void *data;                 /* channels with no buffer: the value, or where it goes */
    int64_t *got;               /* set when the value was handed over: the select arm + 1 */
    int64_t arm;
    Waiter *prev;
    bool queued;                /* in a WaitQueue */
};

/* The tasks waiting on one side of a channel, first come first served.
   Taking one out is O(1), so a channel with many waiters stays fast. */
typedef struct {
    Waiter *head, *tail;
} WaitQueue;

static void queuePush(WaitQueue *q, Waiter *w) {
    w->next = NULL;
    w->prev = q->tail;
    if (q->tail) {
        q->tail->next = w;
    } else {
        q->head = w;
    }
    q->tail = w;
    w->queued = true;
}

static void queueRemove(WaitQueue *q, Waiter *w) {
    if (!w->queued) return;
    if (w->prev) {
        w->prev->next = w->next;
    } else {
        q->head = w->next;
    }
    if (w->next) {
        w->next->prev = w->prev;
    } else {
        q->tail = w->prev;
    }
    w->queued = false;
}

static bool claim(Waiter *w) {
    return !__atomic_exchange_n(w->claimed, 1, __ATOMIC_ACQ_REL);
}

/* Channels work between threads: each has a lock, held only briefly. */
typedef struct {
    int lock;
    int64_t elemSize, cap, len, head;
    char *buf;
    WaitQueue recvWaiters, sendWaiters;
} Chan;

void *iris_chan_new(int64_t elemSize, int64_t cap, const char *file, int line, int col, const char *fn) {
    if (cap < 0) {
        stop("a channel can't have room for fewer than 0 values", 48, file, line, col, fn);
    }
    Chan *c = calloc(1, sizeof(Chan));
    if (!c) {
        outOfMemory();
    }
    c->buf = calloc((size_t)(cap ? cap : 1), (size_t)(elemSize ? elemSize : 1));
    if (!c->buf) {
        outOfMemory();
    }
    c->elemSize = elemSize;
    c->cap = cap;
    return c;
}

/* The first waiter in q that can still be claimed, taken out of it. */
static Waiter *takeWaiter(WaitQueue *q) {
    while (q->head) {
        Waiter *w = q->head;
        queueRemove(q, w);
        if (claim(w)) return w;
    }
    return NULL;
}

/* Wakes the first waiter in q that can still be claimed. */
static void wakeOne(WaitQueue *q) {
    Waiter *w = takeWaiter(q);
    if (w) {
        wakeTask(w->task);
    }
}

/* A channel with no buffer: the value goes straight from a sender to a
   receiver, and each waits for the other. */
static void handSend(Chan *c, const void *value) {
    spinLock(&c->lock);
    for (;;) {
        Waiter *r = takeWaiter(&c->recvWaiters);
        if (r) {
            memcpy(r->data, value, (size_t)c->elemSize);
            *r->got = r->arm + 1;
            spinUnlock(&c->lock);
            wakeTask(r->task);
            return;
        }
        int claimed = 0;
        int64_t taken = 0;
        Waiter w = {.task = current, .claimed = &claimed, .data = (void *)value, .got = &taken};
        queuePush(&c->sendWaiters, &w);
        spinUnlock(&c->lock);
        schedule();
        spinLock(&c->lock);
        queueRemove(&c->sendWaiters, &w);
        if (taken) {
            spinUnlock(&c->lock);
            return;
        }
    }
}

/* Takes a waiting sender's value into out; the lock is held. */
static bool handTake(Chan *c, void *out) {
    Waiter *s = takeWaiter(&c->sendWaiters);
    if (!s) return false;
    memcpy(out, s->data, (size_t)c->elemSize);
    *s->got = s->arm + 1;
    wakeTask(s->task);
    return true;
}

static void handRecv(Chan *c, void *out) {
    spinLock(&c->lock);
    for (;;) {
        if (handTake(c, out)) {
            spinUnlock(&c->lock);
            return;
        }
        int claimed = 0;
        int64_t got = 0;
        Waiter w = {.task = current, .claimed = &claimed, .data = out, .got = &got};
        queuePush(&c->recvWaiters, &w);
        spinUnlock(&c->lock);
        schedule();
        spinLock(&c->lock);
        queueRemove(&c->recvWaiters, &w);
        if (got) {
            spinUnlock(&c->lock);
            return;
        }
    }
}

void iris_chan_send(Chan *c, const void *value) {
    if (c->cap == 0) {
        handSend(c, value);
        return;
    }
    spinLock(&c->lock);
    while (c->len == c->cap) {
        int claimed = 0;
        Waiter w = {.task = current, .claimed = &claimed};
        queuePush(&c->sendWaiters, &w);
        spinUnlock(&c->lock);
        schedule();
        spinLock(&c->lock);
        queueRemove(&c->sendWaiters, &w);
    }
    memcpy(c->buf + ((c->head + c->len) % c->cap) * c->elemSize, value, (size_t)c->elemSize);
    c->len++;
    wakeOne(&c->recvWaiters);
    spinUnlock(&c->lock);
}

/* ch.trySend(v): puts the value in when there is room, or hands it to a
   waiting receiver on a channel without a buffer, and never waits. False
   when it couldn't; the value then still belongs to the caller. */
bool iris_chan_try_send(Chan *c, const void *value) {
    spinLock(&c->lock);
    if (c->cap == 0) {
        Waiter *r = takeWaiter(&c->recvWaiters);
        if (!r) {
            spinUnlock(&c->lock);
            return false;
        }
        memcpy(r->data, value, (size_t)c->elemSize);
        *r->got = r->arm + 1;
        spinUnlock(&c->lock);
        wakeTask(r->task);
        return true;
    }
    if (c->len == c->cap) {
        spinUnlock(&c->lock);
        return false;
    }
    memcpy(c->buf + ((c->head + c->len) % c->cap) * c->elemSize, value, (size_t)c->elemSize);
    c->len++;
    wakeOne(&c->recvWaiters);
    spinUnlock(&c->lock);
    return true;
}

/* ch.len(): how many values wait in the channel. */
int64_t iris_chan_len(Chan *c) {
    spinLock(&c->lock);
    int64_t n = c->len;
    spinUnlock(&c->lock);
    return n;
}

/* Takes the next value; the lock is held. */
static void chanTake(Chan *c, void *out) {
    memcpy(out, c->buf + c->head * c->elemSize, (size_t)c->elemSize);
    c->head = (c->head + 1) % c->cap;
    c->len--;
    wakeOne(&c->sendWaiters);
}

void iris_chan_recv(Chan *c, void *out) {
    if (c->cap == 0) {
        handRecv(c, out);
        return;
    }
    spinLock(&c->lock);
    while (c->len == 0) {
        int claimed = 0;
        Waiter w = {.task = current, .claimed = &claimed};
        queuePush(&c->recvWaiters, &w);
        spinUnlock(&c->lock);
        schedule();
        spinLock(&c->lock);
        queueRemove(&c->recvWaiters, &w);
    }
    chanTake(c, out);
    spinUnlock(&c->lock);
}

/* Frees the channel. Values still in it are not freed. A channel
   something waits on can't be freed; one never made is nothing to free. */
void iris_chan_free(Chan *c, const char *file, int line, int col, const char *fn) {
    if (!c) return;
    spinLock(&c->lock);
    bool waiting = c->recvWaiters.head || c->sendWaiters.head;
    spinUnlock(&c->lock);
    if (waiting) {
        static const char msg[] = "a channel was freed while a task was waiting on it";
        stop(msg, sizeof msg - 1, file, line, col, fn);
    }
    free(c->buf);
    free(c);
}

/* Whether q holds a waiter of another task than the one whose claim flag
   is mine: a select that sends and takes on one channel can't meet itself. */
static bool othersWait(WaitQueue *q, int *mine) {
    for (Waiter *w = q->head; w; w = w->next) {
        if (w->claimed != mine) return true;
    }
    return false;
}

/* One select arm, tried without waiting; the channel's lock is held. A
   send arm's value is at bufs[i], a taking arm's goes there. */
static bool selectTry(Chan *c, void *buf, bool send) {
    if (!send) {
        if (c->cap == 0) return handTake(c, buf);
        if (c->len == 0) return false;
        chanTake(c, buf);
        return true;
    }
    if (c->cap == 0) {
        Waiter *r = takeWaiter(&c->recvWaiters);
        if (!r) return false;
        memcpy(r->data, buf, (size_t)c->elemSize);
        *r->got = r->arm + 1;
        wakeTask(r->task);
        return true;
    }
    if (c->len == c->cap) return false;
    memcpy(c->buf + ((c->head + c->len) % c->cap) * c->elemSize, buf, (size_t)c->elemSize);
    c->len++;
    wakeOne(&c->recvWaiters);
    return true;
}

/* select: the first arm in order that can go at once goes: it takes a
 * value from chans[i] into bufs[i], or, when sends[i] is set, sends the
 * value at bufs[i]. Returns its index, or n when timeout (ns, -1 for none,
 * 0 for an else arm) runs out first. */
int64_t iris_select(int64_t n, Chan **chans, void **bufs, const int8_t *sends, int64_t timeout) {
    int64_t deadline = timeout >= 0 ? nowNs() + timeout : -1;
    Waiter ws[64];
    if (n > 64) {
        n = 64;
    }
    for (;;) {
        for (int64_t i = 0; i < n; i++) {
            spinLock(&chans[i]->lock);
            bool went = selectTry(chans[i], bufs[i], sends[i]);
            spinUnlock(&chans[i]->lock);
            if (went) return i;
        }
        if (deadline >= 0 && nowNs() >= deadline) {
            return n;
        }
        /* wait on every channel; one that can go meanwhile means trying
           again, unless a waker already claimed this task */
        int claimed = 0;
        int64_t added = 0, got = 0;
        bool arrived = false;
        for (; added < n && !arrived; added++) {
            Chan *c = chans[added];
            spinLock(&c->lock);
            ws[added] = (Waiter){.task = current, .claimed = &claimed, .data = bufs[added], .got = &got, .arm = added};
            if (sends[added]) {
                queuePush(&c->sendWaiters, &ws[added]);
                arrived = c->cap == 0 ? othersWait(&c->recvWaiters, &claimed) : c->len < c->cap;
            } else {
                queuePush(&c->recvWaiters, &ws[added]);
                arrived = c->cap == 0 ? othersWait(&c->sendWaiters, &claimed) : c->len > 0;
            }
            spinUnlock(&c->lock);
        }
        bool mine = arrived && !__atomic_exchange_n(&claimed, 1, __ATOMIC_ACQ_REL);
        if (!mine) {
            if (deadline >= 0) {
                current->sleepClaim = &claimed;
                addSleeper(current, deadline);
            }
            schedule();
            current->sleepClaim = NULL;
            if (current->wakeAt) {
                removeSleeper(current);
            }
        }
        for (int64_t i = 0; i < added; i++) {
            spinLock(&chans[i]->lock);
            queueRemove(sends[i] ? &chans[i]->sendWaiters : &chans[i]->recvWaiters, &ws[i]);
            spinUnlock(&chans[i]->lock);
        }
        if (got) {
            /* a task on the other side took or handed the value itself */
            return got - 1;
        }
    }
}

/* ---------------------------------------------------------------- locks */

/* Mutex: a zeroed one is unlocked. A task that has to wait lets the other
   tasks of its thread run; unlock hands the lock to the first waiter. */
typedef struct {
    int lock;
    int locked;
    Waiter *head, *tail;
    void *owner;                /* the holder's thread (its mainTask), when locked */
} Mutex;

/* This thread, as a Mutex's owner. */
static inline void *thisThread(void) { return (void *)&mainTask; }

void iris_mutex_lock(Mutex *m) {
    spinLock(&m->lock);
    if (!m->locked) {
        m->locked = 1;
        m->owner = thisThread();
        spinUnlock(&m->lock);
        return;
    }
    /* held by another thread: it is likely to let go within a moment, and
       waiting a moment costs less than sleeping and being woken. Held by
       a task of this thread, it can't let go while this one spins. */
    if (m->owner != thisThread()) {
        for (int i = 0; i < 100; i++) {
            spinUnlock(&m->lock);
            cpuRelax();
            if (!__atomic_load_n(&m->locked, __ATOMIC_RELAXED)) {
                spinLock(&m->lock);
                if (!m->locked) {
                    m->locked = 1;
                    m->owner = thisThread();
                    spinUnlock(&m->lock);
                    return;
                }
            } else {
                spinLock(&m->lock);
            }
        }
    }
    int claimed = 0;
    Waiter w = {.task = current, .claimed = &claimed};
    if (m->tail) {
        m->tail->next = &w;
    } else {
        m->head = &w;
    }
    m->tail = &w;
    spinUnlock(&m->lock);
    schedule();                 /* unlock gave it the lock */
    m->owner = thisThread();
}

bool iris_mutex_try_lock(Mutex *m) {
    spinLock(&m->lock);
    bool got = !m->locked;
    m->locked = 1;
    if (got) {
        m->owner = thisThread();
    }
    spinUnlock(&m->lock);
    return got;
}

void iris_mutex_unlock(Mutex *m, const char *file, int line, int col, const char *fn) {
    spinLock(&m->lock);
    if (!m->locked) {
        spinUnlock(&m->lock);
        stop("unlock of a Mutex that isn't locked", 35, file, line, col, fn);
    }
    Waiter *w = m->head;
    if (w) {
        m->head = w->next;
        if (!m->head) {
            m->tail = NULL;
        }
        claim(w);
        Task *t = w->task;
        m->owner = NULL;        /* the waiter sets itself when it runs */
        spinUnlock(&m->lock);
        wakeTask(t);
        return;
    }
    m->locked = 0;
    m->owner = NULL;
    spinUnlock(&m->lock);
}

/* The spins below are short on purpose: four threads hammering one lock
   went 5 times faster with 3 tries than with 100, since a thread that
   sleeps stops fighting over the lock's cache line. */
/* sync.RwLock: many readers or one writer. One atomic word holds the
   readers, a writer bit and a bit that says tasks are waiting; while that
   bit is set every caller goes the slow way, under the queue lock, so the
   queues and the word agree. Readers and writers that can't enter spin a
   moment, then wait in a queue. A writer that leaves lets the waiting
   readers in first, then the next writer, so neither side waits for ever. */
#define RW_READERS 0x3fffffffLL
#define RW_WRITER (1LL << 30)
#define RW_WAITERS (1LL << 31)
#define RW_PENDING (1LL << 32)          /* a writer spins for the lock: readers keep out */

typedef struct {
    int64_t state;
    int qlock;
    WaitQueue readQ, writeQ;
} RwLock;

/* Someone left and tasks wait: after readers the next writer is woken,
   after a writer the waiting readers are. A woken task tries again, ahead
   of newcomers; nothing is handed over before it runs, so a woken task
   that runs late holds no one up. */
static void rwWakeNext(RwLock *l, bool readersFirst) {
    spinLock(&l->qlock);
    Waiter *wake[64];
    int n = 0;
    if (l->readQ.head && (readersFirst || !l->writeQ.head)) {
        while (l->readQ.head && n < 64) {
            Waiter *r = takeWaiter(&l->readQ);
            if (!r) break;
            wake[n++] = r;
        }
    } else if (l->writeQ.head) {
        Waiter *w = takeWaiter(&l->writeQ);
        if (w) wake[n++] = w;
    }
    if (!l->readQ.head && !l->writeQ.head) {
        __atomic_fetch_and(&l->state, ~RW_WAITERS, __ATOMIC_ACQ_REL);
    }
    spinUnlock(&l->qlock);
    for (int i = 0; i < n; i++) wakeTask(wake[i]->task);
}

/* Marks that tasks wait, against the state s just seen; false when the
   state changed meanwhile and must be looked at again. */
static bool rwMarkWaiting(RwLock *l, int64_t s) {
    return (s & RW_WAITERS) ||
           __atomic_compare_exchange_n(&l->state, &s, s | RW_WAITERS, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
}

void iris_rw_read_lock(RwLock *l) {
    /* an add always succeeds, where a compare-and-swap retries under
       contention; taken back when a writer holds the lock or waits */
    if (!(__atomic_load_n(&l->state, __ATOMIC_RELAXED) & (RW_WRITER | RW_WAITERS | RW_PENDING))) {
        int64_t old = __atomic_fetch_add(&l->state, 1, __ATOMIC_ACQUIRE);
        if (!(old & (RW_WRITER | RW_WAITERS | RW_PENDING))) {
            return;
        }
        int64_t back = __atomic_sub_fetch(&l->state, 1, __ATOMIC_RELEASE);
        if ((back & RW_WAITERS) && (back & RW_READERS) == 0) rwWakeNext(l, false);
    }
    for (int i = 0; i < 3; i++) {
        int64_t s = __atomic_load_n(&l->state, __ATOMIC_RELAXED);
        if (!(s & (RW_WRITER | RW_WAITERS | RW_PENDING))) {
            if (__atomic_compare_exchange_n(&l->state, &s, s + 1, true, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
            continue;
        }
        cpuRelax();
    }
    bool woken = false;
    for (;;) {
        spinLock(&l->qlock);
        for (;;) {
            int64_t s = __atomic_load_n(&l->state, __ATOMIC_ACQUIRE);
            /* a woken reader goes in when no writer holds the lock; a new
               one also waits behind writers that wait */
            if (!(s & RW_WRITER) && (woken || (!(s & RW_PENDING) && !l->writeQ.head))) {
                if (__atomic_compare_exchange_n(&l->state, &s, s + 1, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
                    spinUnlock(&l->qlock);
                    return;
                }
                continue;
            }
            if (rwMarkWaiting(l, s)) break;
        }
        int claimed = 0;
        Waiter w = {.task = current, .claimed = &claimed};
        queuePush(&l->readQ, &w);
        spinUnlock(&l->qlock);
        schedule();
        woken = true;
    }
}

int iris_rw_read_unlock(RwLock *l) {
    int64_t s = __atomic_load_n(&l->state, __ATOMIC_RELAXED);
    if ((s & RW_READERS) == 0) return 0;
    s = __atomic_sub_fetch(&l->state, 1, __ATOMIC_RELEASE);
    if ((s & RW_WAITERS) && (s & RW_READERS) == 0) rwWakeNext(l, false);
    return 1;
}

void iris_rw_write_lock(RwLock *l) {
    for (int i = 0; i < 3; i++) {
        int64_t s = __atomic_load_n(&l->state, __ATOMIC_RELAXED);
        if (s == 0) {
            if (__atomic_compare_exchange_n(&l->state, &s, RW_WRITER, true, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
            continue;
        }
        if (!(s & (RW_WRITER | RW_WAITERS | RW_PENDING))) {
            /* only readers: keep new ones out and wait for these to leave */
            if (!__atomic_compare_exchange_n(&l->state, &s, s | RW_PENDING, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
                continue;
            }
            for (int k = 0; k < 5; k++) {
                int64_t t = __atomic_load_n(&l->state, __ATOMIC_ACQUIRE);
                /* not past tasks in the queue: they go first, by rwWakeNext */
                if ((t & (RW_READERS | RW_WRITER | RW_WAITERS)) == 0 &&
                    __atomic_compare_exchange_n(&l->state, &t, (t & ~RW_PENDING) | RW_WRITER, false, __ATOMIC_ACQ_REL,
                                                __ATOMIC_RELAXED)) {
                    return;
                }
                cpuRelax();
            }
            /* too long: let go, and let in the readers that waited for this */
            int64_t old = __atomic_fetch_and(&l->state, ~RW_PENDING, __ATOMIC_ACQ_REL);
            if (old & RW_WAITERS) rwWakeNext(l, true);
            break;
        }
        cpuRelax();
    }
    bool woken = false;
    for (;;) {
        spinLock(&l->qlock);
        for (;;) {
            int64_t s = __atomic_load_n(&l->state, __ATOMIC_ACQUIRE);
            if ((s & (RW_WRITER | RW_READERS)) == 0 && (woken || (!l->writeQ.head && !l->readQ.head))) {
                if (__atomic_compare_exchange_n(&l->state, &s, s | RW_WRITER, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
                    spinUnlock(&l->qlock);
                    return;
                }
                continue;
            }
            if (rwMarkWaiting(l, s)) break;
        }
        int claimed = 0;
        Waiter w = {.task = current, .claimed = &claimed};
        queuePush(&l->writeQ, &w);
        spinUnlock(&l->qlock);
        schedule();
        woken = true;
    }
}

int iris_rw_write_unlock(RwLock *l) {
    int64_t s = __atomic_load_n(&l->state, __ATOMIC_RELAXED);
    if (!(s & RW_WRITER)) return 0;
    int64_t old = __atomic_fetch_and(&l->state, ~RW_WRITER, __ATOMIC_RELEASE);
    if (old & RW_WAITERS) rwWakeNext(l, true);
    return 1;
}

int iris_rw_size(void) { return (int)sizeof(RwLock); }

/* ---------------------------------------------------------------- net */

/* errno as net's error codes: 1 refused, 2 closed, 3 timeout, 4 notFound, 5 inUse, 6 other,
 * 8 permission, 9 reset (closed, with its own message) */
static int netCode(int e) {
    switch (e) {
    case ECONNREFUSED: return 1;
    case ECONNRESET: return 9;
    case EPIPE: case ENOTCONN: return 2;
    case ETIMEDOUT: return 3;
    case EADDRINUSE: return 5;
    case ENOENT: return 4;          /* a Unix socket's file */
    case EACCES: case EPERM: return 8;
    default: return 6;
    }
}

static void nonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#ifdef __APPLE__
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}

int iris_net_listen(const char *host, int64_t hostLen, int64_t port, int shared, int *err) {
    char *h = cpath(host, hostLen);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    char portStr[16];
    snprintf(portStr, sizeof portStr, "%lld", (long long)port);
    if (h && !*h) {
        /* every address: one IPv6 socket that takes IPv4 too, where the
           system has IPv6 (getaddrinfo gives 0.0.0.0 first, IPv4 only) */
        free(h);
        h = NULL;
        hints.ai_family = AF_INET6;
        if (getaddrinfo(NULL, portStr, &hints, &res) == 0 && res) {
            int fd = socket(AF_INET6, SOCK_STREAM, 0);
            if (fd >= 0) {
                int one = 1, off = 0;
                setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
                setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
                if (shared) setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
                if (bind(fd, res->ai_addr, res->ai_addrlen) == 0 && listen(fd, 1024) == 0) {
                    freeaddrinfo(res);
                    nonBlocking(fd);
                    fdForget(fd);
                    *err = 0;
                    return fd;
                }
                int e = errno;
                close(fd);
                if (e == EADDRINUSE) {
                    freeaddrinfo(res);
                    *err = 5;
                    return -1;
                }
            }
            freeaddrinfo(res);
            res = NULL;
        }
        hints.ai_family = AF_INET;
    }
    int rc = getaddrinfo(h, portStr, &hints, &res);
    free(h);
    if (rc != 0 || !res) {
        *err = 4;
        return -1;
    }
    int fd = socket(res->ai_family, SOCK_STREAM, 0);
    if (fd < 0) {
        *err = netCode(errno);
        freeaddrinfo(res);
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (shared) {
        /* several threads listen on the port; the kernel spreads connections */
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
    }
    if (bind(fd, res->ai_addr, res->ai_addrlen) != 0 || listen(fd, 1024) != 0) {
        *err = netCode(errno);
        close(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    nonBlocking(fd);
    fdForget(fd);
    *err = 0;
    return fd;
}

/* the port a listening socket got (for port 0) */
int64_t iris_net_port(int fd) {
    struct sockaddr_storage ss;
    socklen_t len = sizeof ss;
    if (getsockname(fd, (struct sockaddr *)&ss, &len) != 0) {
        return 0;
    }
    if (ss.ss_family == AF_INET6) {
        return ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
    }
    return ntohs(((struct sockaddr_in *)&ss)->sin_port);
}

static int64_t deadlineOf(int64_t timeout) { return timeout > 0 ? nowNs() + timeout : 0; }

int iris_net_accept(int fd, int *err, int64_t timeout) {
    int64_t deadline = deadlineOf(timeout);
    for (;;) {
        if (fdStopped(fd)) {
            *err = 2;
            return -1;
        }
#ifdef __linux__
        int c;
        if (useRing()) {
            c = ringOpUntil(RING_OP_ACCEPT, fd, NULL, 0, SOCK_NONBLOCK | SOCK_CLOEXEC, deadline);
            if (c == -ECANCELED) {
                *err = fdStopped(fd) ? 2 : 3;
                return -1;
            }
            if (c < 0) {
                errno = -c;
                c = -1;
            }
        } else {
            c = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        }
#else
        int c = accept(fd, NULL, NULL);
#endif
        if (c >= 0) {
            fdForget(c);
#ifndef __linux__
            nonBlocking(c);
#endif
            int one = 1;
            setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            *err = 0;
            return c;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            if (!waitFdUntil(fd, 0, deadline)) {
                *err = 3;
                return -1;
            }
            continue;
        }
        *err = netCode(errno);
        return -1;
    }
}

int iris_net_dial(const char *host, int64_t hostLen, int64_t port, int *err, int64_t timeout) {
    char *h = cpath(host, hostLen);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portStr[16];
    snprintf(portStr, sizeof portStr, "%lld", (long long)port);
    int rc = h ? getaddrinfo(h, portStr, &hints, &res) : -1;
    free(h);
    if (rc != 0 || !res) {
        *err = 4;
        return -1;
    }
    int fd = socket(res->ai_family, SOCK_STREAM, 0);
    if (fd < 0) {
        *err = netCode(errno);
        freeaddrinfo(res);
        return -1;
    }
    nonBlocking(fd);
    fdForget(fd);
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        if (errno != EINPROGRESS) {
            *err = netCode(errno);
            close(fd);
            freeaddrinfo(res);
            return -1;
        }
        if (!waitFdUntil(fd, 1, deadlineOf(timeout))) {
            *err = 3;
            close(fd);
            freeaddrinfo(res);
            return -1;
        }
        int soerr = 0;
        socklen_t sl = sizeof soerr;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
        if (soerr != 0) {
            *err = netCode(soerr);
            close(fd);
            freeaddrinfo(res);
            return -1;
        }
    }
    freeaddrinfo(res);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    *err = 0;
    return fd;
}

/* ---------------------------------------------------------------- addresses and UDP

   An address is 16 bytes, IPv4 as ::ffff:a.b.c.d, and a port. */

static bool isMapped(const uint8_t *ip) {
    static const uint8_t prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    return memcmp(ip, prefix, 12) == 0;
}

static socklen_t toSockaddr(const uint8_t *ip, int64_t port, int family, struct sockaddr_storage *ss) {
    memset(ss, 0, sizeof *ss);
    if (family == AF_INET) {
        struct sockaddr_in *a = (struct sockaddr_in *)ss;
        a->sin_family = AF_INET;
        a->sin_port = htons((uint16_t)port);
        memcpy(&a->sin_addr, ip + 12, 4);
        return sizeof *a;
    }
    struct sockaddr_in6 *a = (struct sockaddr_in6 *)ss;
    a->sin6_family = AF_INET6;
    a->sin6_port = htons((uint16_t)port);
    memcpy(&a->sin6_addr, ip, 16);
    return sizeof *a;
}

static void fromSockaddr(const struct sockaddr_storage *ss, uint8_t *ip, int64_t *port) {
    if (ss->ss_family == AF_INET) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)ss;
        memset(ip, 0, 10);
        ip[10] = ip[11] = 0xff;
        memcpy(ip + 12, &a->sin_addr, 4);
        *port = ntohs(a->sin_port);
        return;
    }
    const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)ss;
    memcpy(ip, &a->sin6_addr, 16);
    *port = ntohs(a->sin6_port);
}

/* A UDP socket: bound to ip and port (connect 0; ip NULL for any), or
 * connected to them (connect 1). family gets AF_INET or AF_INET6. */
int iris_udp_open(const uint8_t *ip, int64_t port, int connectTo, int *family, int *err) {
    int fam = ip && isMapped(ip) ? AF_INET : AF_INET6;
    int fd = socket(fam, SOCK_DGRAM, 0);
    if (fd < 0 && !ip) {
        fam = AF_INET;              /* no IPv6 here */
        fd = socket(fam, SOCK_DGRAM, 0);
    }
    if (fd < 0) {
        *err = netCode(errno);
        return -1;
    }
    if (fam == AF_INET6 && !ip) {
        int off = 0;                /* IPv4 too */
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
    }
    if (!connectTo) {
        /* several sockets, one per thread, can listen on the port; the
         * kernel spreads the datagrams between them */
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
    }
    static const uint8_t any[16] = {0};
    struct sockaddr_storage ss;
    socklen_t len = toSockaddr(ip ? ip : any, port, fam, &ss);
    int rc = connectTo ? connect(fd, (struct sockaddr *)&ss, len) : bind(fd, (struct sockaddr *)&ss, len);
    if (rc != 0) {
        *err = netCode(errno);
        close(fd);
        return -1;
    }
    nonBlocking(fd);
    fdForget(fd);
    *family = fam;
    *err = 0;
    return fd;
}

/* The IPv4 address of the interface named, for IPv4 multicast; false when
   it has none. */
static bool ifaceV4(const char *name, struct in_addr *out) {
    struct ifaddrs *list, *i;
    if (getifaddrs(&list) != 0) return false;
    bool found = false;
    for (i = list; i; i = i->ifa_next) {
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && strcmp(i->ifa_name, name) == 0) {
            *out = ((struct sockaddr_in *)i->ifa_addr)->sin_addr;
            found = true;
            break;
        }
    }
    freeifaddrs(list);
    return found;
}

/* Multicast and broadcast on a UDP socket: op 1 joins group (ip), 2 leaves
   it, on the interface named (iface, "" for the system's choice); 3 sets
   the hops multicast goes (value), 4 whether it comes back to this machine
   (value), 5 the interface it goes out of (iface), 6 broadcast (value). 0,
   errno, or -1 for an interface that doesn't exist or has no IPv4. */
int iris_udp_multicast(int fd, int family, int op, const uint8_t *ip, const char *iface, int64_t ifaceLen,
                       int value) {
    char name[IF_NAMESIZE + 1] = "";
    if (ifaceLen > IF_NAMESIZE) return -1;
    memcpy(name, iface, (size_t)ifaceLen);
    name[ifaceLen] = 0;
    unsigned index = 0;
    if (ifaceLen > 0 && (index = if_nametoindex(name)) == 0) return -1;
    bool v4 = ip ? isMapped(ip) : family == AF_INET;
    int r = 0;
    switch (op) {
    case 1:
    case 2:
        if (v4) {
            struct ip_mreq m;
            memset(&m, 0, sizeof m);
            memcpy(&m.imr_multiaddr, ip + 12, 4);
            m.imr_interface.s_addr = htonl(INADDR_ANY);
            if (ifaceLen > 0 && !ifaceV4(name, &m.imr_interface)) return -1;
            r = setsockopt(fd, IPPROTO_IP, op == 1 ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP, &m, sizeof m);
        } else {
            struct ipv6_mreq m;
            memset(&m, 0, sizeof m);
            memcpy(&m.ipv6mr_multiaddr, ip, 16);
            m.ipv6mr_interface = index;
            r = setsockopt(fd, IPPROTO_IPV6, op == 1 ? IPV6_JOIN_GROUP : IPV6_LEAVE_GROUP, &m, sizeof m);
        }
        break;
    case 3:
        if (v4) {
            unsigned char t = (unsigned char)(value > 255 ? 255 : value);
            r = setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &t, sizeof t);
        } else {
            r = setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &value, sizeof value);
        }
        break;
    case 4:
        if (v4) {
            unsigned char on = value != 0;
            r = setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &on, sizeof on);
        } else {
            unsigned on = value != 0;
            r = setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &on, sizeof on);
        }
        break;
    case 5:
        if (v4) {
            struct in_addr a;
            a.s_addr = htonl(INADDR_ANY);
            if (ifaceLen > 0 && !ifaceV4(name, &a)) return -1;
            r = setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &a, sizeof a);
        } else {
            r = setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &index, sizeof index);
        }
        break;
    case 6: {
        int on = value != 0;
        r = setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
        break;
    }
    default:
        return EINVAL;
    }
    return r == 0 ? 0 : errno;
}

/* Sends one datagram, to ip and port, or where the socket is connected
 * (ip NULL). 0, or a code. A timeout below 0 doesn't wait: 3 when the
 * send buffer is full. */
int iris_udp_send(int fd, int family, const char *buf, int64_t len, const uint8_t *ip, int64_t port,
                  int64_t timeout) {
    int64_t deadline = deadlineOf(timeout);
    struct sockaddr_storage ss;
    socklen_t sl = 0;
    if (ip) {
        if (family == AF_INET && !isMapped(ip)) return 6;      /* an IPv6 address on an IPv4 socket */
        sl = toSockaddr(ip, port, family, &ss);
    }
    for (;;) {
        ssize_t n = ip ? sendto(fd, buf, (size_t)len, 0, (struct sockaddr *)&ss, sl) : send(fd, buf, (size_t)len, 0);
        if (n >= 0) return 0;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (timeout < 0 || !waitFdUntil(fd, 1, deadline)) return 3;
            continue;
        }
        if (errno == EMSGSIZE) return 9;
        return netCode(errno);
    }
}

/* Reads one datagram: its length, or -code. ip and port get the sender. */
int64_t iris_udp_recv(int fd, char *buf, int64_t len, uint8_t *ip, int64_t *port, int64_t timeout) {
    int64_t deadline = deadlineOf(timeout);
    for (;;) {
        struct sockaddr_storage ss;
        socklen_t sl = sizeof ss;
        ssize_t n = recvfrom(fd, buf, (size_t)len, 0, (struct sockaddr *)&ss, &sl);
        if (n >= 0) {
            fromSockaddr(&ss, ip, port);
            return n;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            if (!waitFdUntil(fd, 0, deadline)) return -3;
            continue;
        }
        return -netCode(errno);
    }
}

/* Ancillary data on UDP sockets (RFC 3542 and Linux's): op 1 asks for the
   local address and the TOS byte of each datagram read with recvmsg
   (value on or off), 2 sets the TOS byte of what is sent (IP_TOS,
   IPV6_TCLASS), 3 turns GRO on (Linux), so one read can take several
   datagrams of one size. 0, errno, or -1 off Linux for GRO. */
int iris_udp_info(int fd, int family, int op, int value) {
    int r = 0;
    int on = value != 0;
    if (op == 1) {
        if (family == AF_INET6) {
            r = setsockopt(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &on, sizeof on);
            if (!r) r = setsockopt(fd, IPPROTO_IPV6, IPV6_RECVTCLASS, &on, sizeof on);
#ifdef __linux__
            /* IPv4 datagrams on a dual-stack socket carry IPv4's */
            setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &on, sizeof on);
            setsockopt(fd, IPPROTO_IP, IP_RECVTOS, &on, sizeof on);
#endif
        } else {
#ifdef __linux__
            r = setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &on, sizeof on);
#else
            r = setsockopt(fd, IPPROTO_IP, IP_RECVPKTINFO, &on, sizeof on);
#endif
            if (!r) r = setsockopt(fd, IPPROTO_IP, IP_RECVTOS, &on, sizeof on);
        }
    } else if (op == 2) {
        if (family == AF_INET6) {
            r = setsockopt(fd, IPPROTO_IPV6, IPV6_TCLASS, &value, sizeof value);
#ifdef __linux__
            setsockopt(fd, IPPROTO_IP, IP_TOS, &value, sizeof value);
#endif
        } else {
            r = setsockopt(fd, IPPROTO_IP, IP_TOS, &value, sizeof value);
        }
    } else if (op == 3) {
#ifdef __linux__
        r = setsockopt(fd, IPPROTO_UDP, 104 /* UDP_GRO */, &on, sizeof on);
#else
        return -1;
#endif
    } else {
        return EINVAL;
    }
    return r == 0 ? 0 : errno;
}

/* Reads one datagram with what came with it: its length (cut to len, and
   *flags 1 when it was cut), the sender in ip and port, the local address
   it came to in to (zeros when unknown), its TOS byte (-1 when unknown),
   and with GRO the size of each datagram it holds (0 for one). -code. */
int64_t iris_udp_recvmsg(int fd, char *buf, int64_t len, uint8_t *ip, int64_t *port, uint8_t *to, int64_t *tos,
                         int64_t *segment, int64_t *flags, int64_t timeout) {
    int64_t deadline = deadlineOf(timeout);
    for (;;) {
        struct sockaddr_storage ss;
        struct iovec v = {buf, (size_t)len};
        union {
            char space[256];
            struct cmsghdr align;
        } cm;
        struct msghdr h;
        memset(&h, 0, sizeof h);
        h.msg_name = &ss;
        h.msg_namelen = sizeof ss;
        h.msg_iov = &v;
        h.msg_iovlen = 1;
        h.msg_control = cm.space;
        h.msg_controllen = sizeof cm.space;
        ssize_t n = recvmsg(fd, &h, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                if (!waitFdUntil(fd, 0, deadline)) return -3;
                continue;
            }
            return -netCode(errno);
        }
        fromSockaddr(&ss, ip, port);
        memset(to, 0, 16);
        *tos = -1;
        *segment = 0;
        *flags = (h.msg_flags & MSG_TRUNC) ? 1 : 0;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&h); c; c = CMSG_NXTHDR(&h, c)) {
            if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO) {
                struct in_pktinfo pi;
                memcpy(&pi, CMSG_DATA(c), sizeof pi);
                to[10] = to[11] = 0xff;
                memcpy(to + 12, &pi.ipi_addr, 4);
            } else if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_PKTINFO) {
                struct in6_pktinfo pi;
                memcpy(&pi, CMSG_DATA(c), sizeof pi);
                memcpy(to, &pi.ipi6_addr, 16);
            } else if (c->cmsg_level == IPPROTO_IP && (c->cmsg_type == IP_TOS
#ifdef IP_RECVTOS
                                                      || c->cmsg_type == IP_RECVTOS
#endif
                                                      )) {
                *tos = *(unsigned char *)CMSG_DATA(c);
            } else if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_TCLASS) {
                int t;
                memcpy(&t, CMSG_DATA(c), sizeof t);
                *tos = t;
#ifdef __linux__
            } else if (c->cmsg_level == IPPROTO_UDP && c->cmsg_type == 104 /* UDP_GRO */) {
                int g;
                memcpy(&g, CMSG_DATA(c), sizeof g);
                *segment = g;
#endif
            }
        }
        return n;
    }
}

/* Sends one datagram to ip and port with what goes with it: from, when not
   NULL, the local address it leaves from; tos when not -1; and segment,
   when above 0, the size of each datagram data is cut into by the kernel
   (GSO, Linux). 0, or a code (9 too large, 10 GSO not here). */
int iris_udp_sendmsg(int fd, int family, const char *buf, int64_t len, const uint8_t *ip, int64_t port,
                     const uint8_t *from, int64_t tos, int64_t segment, int64_t timeout) {
    if (family == AF_INET && !isMapped(ip)) return 6;
#ifndef __linux__
    if (segment > 0) return 10;
#endif
    int64_t deadline = deadlineOf(timeout);
    struct sockaddr_storage ss;
    socklen_t sl = toSockaddr(ip, port, family, &ss);
    union {
        char space[256];
        struct cmsghdr align;
    } cm;
    memset(cm.space, 0, sizeof cm.space);
    struct iovec v = {(void *)buf, (size_t)len};
    struct msghdr h;
    memset(&h, 0, sizeof h);
    h.msg_name = &ss;
    h.msg_namelen = sl;
    h.msg_iov = &v;
    h.msg_iovlen = 1;
    h.msg_control = cm.space;
    h.msg_controllen = sizeof cm.space;
    size_t used = 0;
    struct cmsghdr *c = CMSG_FIRSTHDR(&h);
    bool v4 = family == AF_INET || isMapped(ip);
    if (from) {
        if (v4) {
            struct in_pktinfo pi;
            memset(&pi, 0, sizeof pi);
            memcpy(&pi.ipi_spec_dst, from + 12, 4);
            c->cmsg_level = IPPROTO_IP;
            c->cmsg_type = IP_PKTINFO;
            c->cmsg_len = CMSG_LEN(sizeof pi);
            memcpy(CMSG_DATA(c), &pi, sizeof pi);
            used += CMSG_SPACE(sizeof pi);
        } else {
            struct in6_pktinfo pi;
            memset(&pi, 0, sizeof pi);
            memcpy(&pi.ipi6_addr, from, 16);
            c->cmsg_level = IPPROTO_IPV6;
            c->cmsg_type = IPV6_PKTINFO;
            c->cmsg_len = CMSG_LEN(sizeof pi);
            memcpy(CMSG_DATA(c), &pi, sizeof pi);
            used += CMSG_SPACE(sizeof pi);
        }
        c = (struct cmsghdr *)(cm.space + used);
    }
    if (tos >= 0) {
        int t = (int)tos;
        if (v4) {
            c->cmsg_level = IPPROTO_IP;
            c->cmsg_type = IP_TOS;
        } else {
            c->cmsg_level = IPPROTO_IPV6;
            c->cmsg_type = IPV6_TCLASS;
        }
        c->cmsg_len = CMSG_LEN(sizeof t);
        memcpy(CMSG_DATA(c), &t, sizeof t);
        used += CMSG_SPACE(sizeof t);
        c = (struct cmsghdr *)(cm.space + used);
    }
#ifdef __linux__
    if (segment > 0) {
        uint16_t sz = (uint16_t)segment;
        c->cmsg_level = IPPROTO_UDP;
        c->cmsg_type = 103;     /* UDP_SEGMENT */
        c->cmsg_len = CMSG_LEN(sizeof sz);
        memcpy(CMSG_DATA(c), &sz, sizeof sz);
        used += CMSG_SPACE(sizeof sz);
    }
#endif
    h.msg_controllen = used;
    if (!used) h.msg_control = NULL;
    for (;;) {
        ssize_t n = sendmsg(fd, &h, 0);
        if (n >= 0) return 0;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            if (!waitFdUntil(fd, 1, deadline)) return 3;
            continue;
        }
        if (errno == EMSGSIZE) return 9;
        if (errno == EIO && segment > 0) return 10;
        return netCode(errno);
    }
}

/* One datagram of a batch, laid out as Iris's net.Datagram: the space
 * for it, its length and the address it came from or goes to. */
typedef struct {
    char *buf;
    int64_t cap;
    int64_t len;
    uint8_t ip[16];
    uint16_t port;
} IrisDatagram;

/* Waits for a datagram, then reads as many as are waiting, up to n: how
 * many, or -code. */
int64_t iris_udp_recv_many(int fd, IrisDatagram *d, int64_t n, int64_t timeout) {
    if (n <= 0) return 0;
    int64_t deadline = deadlineOf(timeout);
    int64_t got = 0;
    for (;;) {
#ifdef __linux__
        enum { MAX = 64 };
        struct mmsghdr h[MAX];
        struct iovec v[MAX];
        struct sockaddr_storage ss[MAX];
        int k = n - got < MAX ? (int)(n - got) : MAX;
        for (int i = 0; i < k; i++) {
            v[i].iov_base = d[got + i].buf;
            v[i].iov_len = (size_t)d[got + i].cap;
            memset(&h[i], 0, sizeof h[i]);
            h[i].msg_hdr.msg_iov = &v[i];
            h[i].msg_hdr.msg_iovlen = 1;
            h[i].msg_hdr.msg_name = &ss[i];
            h[i].msg_hdr.msg_namelen = sizeof ss[i];
        }
        int r = recvmmsg(fd, h, (unsigned)k, MSG_DONTWAIT, NULL);
        if (r > 0) {
            for (int i = 0; i < r; i++) {
                IrisDatagram *x = &d[got + i];
                int64_t port;
                x->len = h[i].msg_len;
                fromSockaddr(&ss[i], x->ip, &port);
                x->port = (uint16_t)port;
            }
            got += r;
            if (got == n || r < k) return got;
            continue;
        }
#else
        struct sockaddr_storage ss;
        socklen_t sl = sizeof ss;
        IrisDatagram *x = &d[got];
        ssize_t r = recvfrom(fd, x->buf, (size_t)x->cap, MSG_DONTWAIT, (struct sockaddr *)&ss, &sl);
        if (r >= 0) {
            int64_t port;
            x->len = r;
            fromSockaddr(&ss, x->ip, &port);
            x->port = (uint16_t)port;
            got++;
            if (got == n) return got;
            continue;
        }
#endif
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            if (got > 0) return got;
            if (!waitFdUntil(fd, 0, deadline)) return -3;
            continue;
        }
        return got > 0 ? got : -netCode(errno);
    }
}

/* Sends each datagram to its address, or where the socket is connected
 * (connected 1). 0, or a code. */
int iris_udp_send_many(int fd, int family, int connected, IrisDatagram *d, int64_t n, int64_t timeout) {
    int64_t deadline = deadlineOf(timeout);
    for (int64_t i = 0; i < n; i++) {
        if (!connected && family == AF_INET && !isMapped(d[i].ip)) return 6;
    }
    int64_t sent = 0;
    while (sent < n) {
#ifdef __linux__
        enum { MAX = 64 };
        struct mmsghdr h[MAX];
        struct iovec v[MAX];
        struct sockaddr_storage ss[MAX];
        int k = n - sent < MAX ? (int)(n - sent) : MAX;
        for (int i = 0; i < k; i++) {
            IrisDatagram *x = &d[sent + i];
            v[i].iov_base = x->buf;
            v[i].iov_len = (size_t)x->len;
            memset(&h[i], 0, sizeof h[i]);
            h[i].msg_hdr.msg_iov = &v[i];
            h[i].msg_hdr.msg_iovlen = 1;
            if (!connected) {
                h[i].msg_hdr.msg_name = &ss[i];
                h[i].msg_hdr.msg_namelen = toSockaddr(x->ip, x->port, family, &ss[i]);
            }
        }
        int r = sendmmsg(fd, h, (unsigned)k, MSG_DONTWAIT);
        if (r > 0) {
            sent += r;
            continue;
        }
#else
        IrisDatagram *x = &d[sent];
        struct sockaddr_storage ss;
        ssize_t r;
        if (connected) {
            r = send(fd, x->buf, (size_t)x->len, 0);
        } else {
            socklen_t sl = toSockaddr(x->ip, x->port, family, &ss);
            r = sendto(fd, x->buf, (size_t)x->len, 0, (struct sockaddr *)&ss, sl);
        }
        if (r >= 0) {
            sent++;
            continue;
        }
#endif
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            if (!waitFdUntil(fd, 1, deadline)) return 3;
            continue;
        }
        if (errno == EMSGSIZE) return 9;
        return netCode(errno);
    }
    return 0;
}

/* Where a socket is bound. */
void iris_sock_local(int fd, uint8_t *ip, int64_t *port) {
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    memset(ip, 0, 16);
    *port = 0;
    if (getsockname(fd, (struct sockaddr *)&ss, &sl) == 0) fromSockaddr(&ss, ip, port);
}

/* Who a connected socket is connected to: all zeros once it is gone. */
void iris_sock_remote(int fd, uint8_t *ip, int64_t *port) {
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    memset(ip, 0, 16);
    *port = 0;
    if (getpeername(fd, (struct sockaddr *)&ss, &sl) == 0) fromSockaddr(&ss, ip, port);
}

/* A TCP connection to an address. */
int iris_net_dial_ip(const uint8_t *ip, int64_t port, int *err, int64_t timeout) {
    int fam = isMapped(ip) ? AF_INET : AF_INET6;
    int fd = socket(fam, SOCK_STREAM, 0);
    if (fd < 0) {
        *err = netCode(errno);
        return -1;
    }
    nonBlocking(fd);
    fdForget(fd);
    struct sockaddr_storage ss;
    socklen_t sl = toSockaddr(ip, port, fam, &ss);
    if (connect(fd, (struct sockaddr *)&ss, sl) != 0) {
        if (errno != EINPROGRESS) {
            *err = netCode(errno);
            close(fd);
            return -1;
        }
        if (!waitFdUntil(fd, 1, deadlineOf(timeout))) {
            *err = 3;
            close(fd);
            return -1;
        }
        int soerr = 0;
        socklen_t el = sizeof soerr;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &el);
        if (soerr != 0) {
            *err = netCode(soerr);
            close(fd);
            return -1;
        }
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    *err = 0;
    return fd;
}

/* The text of an address's IP, and parsing one: 1 when text is an IP. */
void iris_ip_format(const uint8_t *ip, IrisString *out) {
    char buf[64];
    if (isMapped(ip)) inet_ntop(AF_INET, ip + 12, buf, sizeof buf);
    else inet_ntop(AF_INET6, ip, buf, sizeof buf);
    iris_string_append(out, buf, (int64_t)strlen(buf));
}

int iris_ip_parse(const char *text, int64_t len, uint8_t *ip) {
    char buf[64];
    if (len <= 0 || len >= (int64_t)sizeof buf) return 0;
    memcpy(buf, text, (size_t)len);
    buf[len] = 0;
    uint8_t v4[4];
    if (inet_pton(AF_INET, buf, v4) == 1) {
        memset(ip, 0, 10);
        ip[10] = ip[11] = 0xff;
        memcpy(ip + 12, v4, 4);
        return 1;
    }
    return inet_pton(AF_INET6, buf, ip) == 1;
}

/* bytes read, 0 at the end, -code on an error */
int64_t iris_net_read(int fd, char *buf, int64_t len, int64_t timeout) {
    int64_t deadline = deadlineOf(timeout);
#ifdef __linux__
    if (useRing()) {
        for (;;) {
            int n = ringOpUntil(RING_OP_RECV, fd, buf, len > 0x7fffffff ? 0x7fffffff : (uint32_t)len, 0, deadline);
            if (n >= 0) {
                return n;
            }
            if (n == -ECANCELED) {
                return -3;
            }
            if (n != -EINTR && n != -EAGAIN) {
                return -netCode(-n);
            }
        }
    }
#endif
    for (;;) {
        ssize_t n = read(fd, buf, (size_t)len);
        if (n >= 0) {
            return n;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            if (!waitFdUntil(fd, 0, deadline)) {
                return -3;
            }
            continue;
        }
        return -netCode(errno);
    }
}

/* 0 when everything was written, a code on an error */
int iris_net_write(int fd, const char *buf, int64_t len, int64_t timeout) {
    int64_t deadline = deadlineOf(timeout);
    while (len > 0) {
#ifdef MSG_NOSIGNAL
        ssize_t n = send(fd, buf, (size_t)len, MSG_NOSIGNAL);
#else
        ssize_t n = send(fd, buf, (size_t)len, 0);
#endif
        if (n > 0) {
            buf += n;
            len -= n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            if (!waitFdUntil(fd, 1, deadline)) {
                return 3;
            }
            continue;
        }
        return netCode(errno);
    }
    return 0;
}

/* Whether Listener.interrupt stopped fd. */
static bool fdStopped(int fd) {
    if (!__atomic_load_n(&stoppedLen, __ATOMIC_ACQUIRE)) return false;
    bool found = false;
    spinLock(&stopLock);
    for (int i = 0; i < stoppedLen && !found; i++) found = stoppedFds[i] == fd;
    spinUnlock(&stopLock);
    return found;
}

/* Cancels this thread's waits on the listeners stopped since it last
   looked: the waiting tasks wake, and their accept fails. */
static void checkStops(void) {
    Sched *s = mySched;
    if (!s || __atomic_load_n(&stopGen, __ATOMIC_ACQUIRE) == s->stopSeen) return;
    int fds[64];
    int n = 0;
    spinLock(&stopLock);
    s->stopSeen = stopGen;
    for (int i = 0; i < stoppedLen && n < 64; i++) fds[n++] = stoppedFds[i];
    spinUnlock(&stopLock);
    for (int i = 0; i < n; i++) {
        int fd = fds[i];
#ifdef __linux__
        if (ring.fd >= 0) {
            /* IORING_OP_ASYNC_CANCEL of every operation on fd (5.19+);
               its own result is skipped like a linked timeout's */
            ringPush(14, fd, NULL, 0, (1u << 0) | (1u << 1), 1);
            continue;
        }
#endif
        if (fd < fdWaitsCap && fdWaits[fd].reader) {
            makeReady(fdWaits[fd].reader);
            fdWaits[fd].reader = NULL;
            waitingIo--;
        }
    }
}

/* Listener.interrupt: accept on fd fails from now on, and the tasks that
   wait in it, on any thread, wake up. */
void iris_net_stop_listener(int fd) {
    if (fd < 0) return;
    (void)current;
    spinLock(&stopLock);
    bool there = false;
    for (int i = 0; i < stoppedLen; i++) there = there || stoppedFds[i] == fd;
    if (!there) {
        if (stoppedLen == stoppedCap) {
            stoppedCap = stoppedCap ? stoppedCap * 2 : 8;
            stoppedFds = realloc(stoppedFds, (size_t)stoppedCap * sizeof *stoppedFds);
            if (!stoppedFds) {
                outOfMemory();
            }
        }
        stoppedFds[stoppedLen] = fd;
        __atomic_store_n(&stoppedLen, stoppedLen + 1, __ATOMIC_RELEASE);
    }
    __atomic_add_fetch(&stopGen, 1, __ATOMIC_ACQ_REL);
    int n = schedsLen;
    Sched **list = malloc((size_t)(n ? n : 1) * sizeof *list);
    if (!list) {
        outOfMemory();
    }
    memcpy(list, allScheds, (size_t)n * sizeof *list);
    spinUnlock(&stopLock);
    for (int i = 0; i < n; i++) {
        if (list[i] == mySched) {
            checkStops();
            continue;
        }
        uint64_t one = 1;
        ssize_t r = write(list[i]->wakeW, &one, list[i]->wakeW == list[i]->wakeR ? 8 : 1);
        (void)r;
    }
    free(list);
}

void iris_net_close(int fd) {
    if (fd >= 0) {
        if (__atomic_load_n(&stoppedLen, __ATOMIC_ACQUIRE)) {
            /* the number may come back for another socket */
            spinLock(&stopLock);
            for (int i = 0; i < stoppedLen; i++) {
                if (stoppedFds[i] == fd) {
                    stoppedFds[i] = stoppedFds[--stoppedLen];
                    break;
                }
            }
            spinUnlock(&stopLock);
        }
        fdForget(fd);
        close(fd);
    }
}

/* ---------------------------------------------------------------- sockets (0.67)

   net.Socket: a TCP socket the program sets up itself, options set before
   it binds, listens or connects. Calls that set or read an option give 0 or
   errno; iris_errno_text gives errno's text. */

int iris_net_errno_code(int e) { return netCode(e); }

void iris_errno_text(int e, IrisString *out) {
    const char *s = strerror(e);
    iris_string_append(out, s, (int64_t)strlen(s));
}

/* A TCP socket: IPv6 that takes IPv4 too (family 0), IPv4 (1), or IPv6
   only (2). Family 0 falls back to IPv4 where there is no IPv6. */
int iris_sock_open(int family, int *err) {
    int fam = family == 1 ? AF_INET : AF_INET6;
    int fd = socket(fam, SOCK_STREAM, 0);
    if (fd < 0 && family == 0) {
        fam = AF_INET;
        fd = socket(fam, SOCK_STREAM, 0);
    }
    if (fd < 0) {
        *err = netCode(errno);
        return -1;
    }
    int one = 1;
    if (fam == AF_INET6) {
        int only = family == 2;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &only, sizeof only);
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    nonBlocking(fd);
    fdForget(fd);
    *err = 0;
    return fd;
}

static int sockFamily(int fd) {
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    memset(&ss, 0, sizeof ss);
    if (getsockname(fd, (struct sockaddr *)&ss, &sl) != 0) return AF_INET6;
    return ss.ss_family;
}

/* ip as an address for a socket of family: an IPv4 address on an IPv4
   socket, and :: (any) as 0.0.0.0. -1 when it can't be one. */
static int sockAddrFor(int fd, const uint8_t *ip, int64_t port, struct sockaddr_storage *ss, socklen_t *sl) {
    int fam = sockFamily(fd);
    if (fam == AF_INET) {
        static const uint8_t any[16] = {0};
        uint8_t v4[16];
        if (memcmp(ip, any, 16) == 0) {
            memset(v4, 0, 16);
            v4[10] = v4[11] = 0xff;
            ip = v4;
        } else if (!isMapped(ip)) {
            return -1;
        }
    }
    *sl = toSockaddr(ip, port, fam, ss);
    return 0;
}

/* 0, or errno */
int iris_sock_bind(int fd, const uint8_t *ip, int64_t port) {
    struct sockaddr_storage ss;
    socklen_t sl;
    if (sockAddrFor(fd, ip, port, &ss, &sl) != 0) return EAFNOSUPPORT;
    return bind(fd, (struct sockaddr *)&ss, sl) == 0 ? 0 : errno;
}

int iris_sock_listen(int fd, int64_t backlog) {
    return listen(fd, backlog > 0x7fffffff ? 0x7fffffff : (int)backlog) == 0 ? 0 : errno;
}

/* Connects a non-blocking socket and waits for it: 0, or a net code. */
static int connectWait(int fd, const struct sockaddr_storage *ss, socklen_t sl, int64_t timeout) {
    if (connect(fd, (const struct sockaddr *)ss, sl) != 0) {
        if (errno != EINPROGRESS) return netCode(errno);
        if (!waitFdUntil(fd, 1, deadlineOf(timeout))) return 3;
        int soerr = 0;
        socklen_t el = sizeof soerr;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &el);
        if (soerr != 0) return netCode(soerr);
    }
    return 0;
}

/* 0, or a net code */
int iris_sock_connect(int fd, const uint8_t *ip, int64_t port, int64_t timeout) {
    struct sockaddr_storage ss;
    socklen_t sl;
    if (sockAddrFor(fd, ip, port, &ss, &sl) != 0) return 6;
    return connectWait(fd, &ss, sl, timeout);
}

int iris_sock_set(int fd, int level, int name, int value) {
    return setsockopt(fd, level, name, &value, sizeof value) == 0 ? 0 : errno;
}

int iris_sock_get(int fd, int level, int name, int *value) {
    socklen_t len = sizeof *value;
    *value = 0;
    return getsockopt(fd, level, name, value, &len) == 0 ? 0 : errno;
}

/* Raw bytes, for options that aren't an int (SO_LINGER, TCP_INFO...). */
int iris_sock_set_bytes(int fd, int level, int name, const void *p, int64_t n) {
    return setsockopt(fd, level, name, p, (socklen_t)n) == 0 ? 0 : errno;
}

int iris_sock_get_bytes(int fd, int level, int name, void *p, int64_t *n) {
    socklen_t len = (socklen_t)*n;
    int rc = getsockopt(fd, level, name, p, &len);
    *n = len;
    return rc == 0 ? 0 : errno;
}

int iris_sock_nodelay(int fd, int on) { return iris_sock_set(fd, IPPROTO_TCP, TCP_NODELAY, on); }

/* Keepalive probes after idle seconds without traffic, every interval
   seconds, count of them before the connection is dropped; 0 leaves the
   system's value. idle 0 turns keepalive off. */
int iris_sock_keepalive(int fd, int64_t idle, int64_t interval, int64_t count) {
    if (idle <= 0) return iris_sock_set(fd, SOL_SOCKET, SO_KEEPALIVE, 0);
    int e = iris_sock_set(fd, SOL_SOCKET, SO_KEEPALIVE, 1);
#ifdef __APPLE__
    if (!e) e = iris_sock_set(fd, IPPROTO_TCP, TCP_KEEPALIVE, (int)idle);
#else
    if (!e) e = iris_sock_set(fd, IPPROTO_TCP, TCP_KEEPIDLE, (int)idle);
#endif
    if (!e && interval > 0) e = iris_sock_set(fd, IPPROTO_TCP, TCP_KEEPINTVL, (int)interval);
    if (!e && count > 0) e = iris_sock_set(fd, IPPROTO_TCP, TCP_KEEPCNT, (int)count);
    return e;
}

/* The kernel's buffers, in bytes; 0 leaves one as it is. */
int iris_sock_buffers(int fd, int64_t rd, int64_t wr) {
    int e = 0;
    if (rd > 0) e = iris_sock_set(fd, SOL_SOCKET, SO_RCVBUF, rd > 0x7fffffff ? 0x7fffffff : (int)rd);
    if (!e && wr > 0) e = iris_sock_set(fd, SOL_SOCKET, SO_SNDBUF, wr > 0x7fffffff ? 0x7fffffff : (int)wr);
    return e;
}

/* Options by name, so a program needn't know each system's numbers: 0,
   errno, or -1 where the system has no such option (Linux only ones on
   macOS). which: 1 SO_REUSEPORT, 2 IP_TRANSPARENT (IPV6_TRANSPARENT too on
   an IPv6 socket), 3 IP_FREEBIND, 4 TCP_FASTOPEN (value: the queue of a
   listener), 5 TCP_FASTOPEN_CONNECT, 6 SO_MARK, 7 SO_LINGER (value:
   seconds, -1 off). */
int iris_sock_named(int fd, int which, int64_t value) {
    int v = value > 0x7fffffff ? 0x7fffffff : (int)value;
    switch (which) {
    case 1:
        return iris_sock_set(fd, SOL_SOCKET, SO_REUSEPORT, v);
    case 2: {
#ifdef __linux__
        struct sockaddr_storage ss;
        socklen_t n = sizeof ss;
        bool v6 = getsockname(fd, (struct sockaddr *)&ss, &n) == 0 && ss.ss_family == AF_INET6;
        int e = iris_sock_set(fd, SOL_IP, IP_TRANSPARENT, v);
        if (v6 && (!e || e == ENOPROTOOPT)) e = iris_sock_set(fd, SOL_IPV6, IPV6_TRANSPARENT, v);
        return e;
#else
        return -1;
#endif
    }
    case 3:
#ifdef __linux__
        return iris_sock_set(fd, SOL_IP, IP_FREEBIND, v);
#else
        return -1;
#endif
    case 4:
#ifdef TCP_FASTOPEN
        return iris_sock_set(fd, IPPROTO_TCP, TCP_FASTOPEN, v);
#else
        return -1;
#endif
    case 5:
#ifdef __linux__
        return iris_sock_set(fd, IPPROTO_TCP, 30 /* TCP_FASTOPEN_CONNECT */, v);
#else
        return -1;
#endif
    case 6:
#ifdef __linux__
        return iris_sock_set(fd, SOL_SOCKET, SO_MARK, v);
#else
        return -1;
#endif
    case 7: {
        struct linger l = {v >= 0, v >= 0 ? v : 0};
        return setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof l) == 0 ? 0 : errno;
    }
    }
    return EINVAL;
}

/* Where a connection that iptables redirected (REDIRECT, DNAT) was going:
   0 and the address, errno, or -1 off Linux. */
int iris_sock_original_dst(int fd, uint8_t *ip, int64_t *port) {
#ifdef __linux__
    struct sockaddr_storage ss;
    socklen_t n = sizeof ss;
    if (getsockname(fd, (struct sockaddr *)&ss, &n) != 0) return errno;
    int level = ss.ss_family == AF_INET6 ? SOL_IPV6 : SOL_IP;
    n = sizeof ss;
    memset(&ss, 0, sizeof ss);
    if (getsockopt(fd, level, 80 /* SO_ORIGINAL_DST, IP6T_SO_ORIGINAL_DST */, &ss, &n) != 0) {
        /* an IPv6 socket that took an IPv4 connection keeps it at IPv4's level */
        if (level == SOL_IP) return errno;
        n = sizeof ss;
        if (getsockopt(fd, SOL_IP, 80, &ss, &n) != 0) return errno;
    }
    fromSockaddr(&ss, ip, port);
    return 0;
#else
    (void)fd;
    (void)ip;
    (void)port;
    return -1;
#endif
}

/* A descriptor made elsewhere (inherited, received, from C) handed to the
   event loop: 0, or errno. */
int iris_net_adopt(int fd) {
    if (fd < 0 || fcntl(fd, F_GETFL, 0) < 0) return EBADF;
    nonBlocking(fd);
    fdForget(fd);
    return 0;
}

/* Peeks at the next n bytes, leaving them in the socket for whoever reads
   next, OpenSSL or splice included: how many there are, fewer than n only
   when the other side ended its writes (seen on Linux; elsewhere the
   timeout ends the wait), or -code. A wait for the first byte uses the
   event loop; one for the rest, which comes within a round trip, polls
   with growing sleeps, since the socket stays readable meanwhile. */
int64_t iris_net_peek(int fd, char *buf, int64_t n, int64_t timeout) {
    int64_t deadline = deadlineOf(timeout), pause = 1000000;
    for (;;) {
        ssize_t k = recv(fd, buf, (size_t)n, MSG_PEEK);
        if (k == 0) return 0;
        if (k < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return -netCode(errno);
            if (!waitFdUntil(fd, 0, deadline)) return -3;
            continue;
        }
        if (k >= n) return k;
#ifdef POLLRDHUP
        struct pollfd pf = {fd, POLLIN | POLLRDHUP, 0};
        if (poll(&pf, 1, 0) == 1 && (pf.revents & (POLLRDHUP | POLLHUP))) return k;
#endif
        if (deadline > 0 && nowNs() + pause > deadline) return -3;
        iris_sleep(pause);
        if (pause < 50000000) pause *= 2;
    }
}

/* Ends this side's writes; reads go on until the other side ends its own. */
void iris_net_shutdown_write(int fd) {
    if (fd >= 0) shutdown(fd, SHUT_WR);
}

/* Waits in the event loop until fd can be read (write 0) or written: 1, or
   0 when timeout passed first. For descriptors used through import c,
   after a call on them said it would block. */
int iris_net_wait(int fd, int write, int64_t timeout) {
    return waitFdUntil(fd, write, deadlineOf(timeout)) ? 1 : 0;
}

/* ---------------------------------------------------------------- Unix sockets

   A path starting with @ is in Linux's abstract namespace, with no file. */

static int unixAddr(const char *path, int64_t len, struct sockaddr_un *a, socklen_t *sl) {
    memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    if (len <= 0 || len >= (int64_t)sizeof a->sun_path || memchr(path, 0, (size_t)len)) return -1;
    memcpy(a->sun_path, path, (size_t)len);
#ifdef __linux__
    if (path[0] == '@') a->sun_path[0] = 0;
#endif
    *sl = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + (size_t)len + (path[0] == '@' ? 0 : 1));
    return 0;
}

/* Unix datagram sockets: bound to path when it isn't empty. -1 with *err
   on an error. */
int iris_unixgram_open(const char *path, int64_t len, int *err) {
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        *err = netCode(errno);
        return -1;
    }
    if (len > 0) {
        struct sockaddr_un a;
        socklen_t sl;
        if (unixAddr(path, len, &a, &sl) != 0) {
            close(fd);
            *err = 4;
            return -1;
        }
        if (bind(fd, (struct sockaddr *)&a, sl) != 0) {
            *err = errno == EADDRINUSE ? 5 : errno == ENOENT ? 4 : errno == EACCES ? 8 : netCode(errno);
            close(fd);
            return -1;
        }
    }
    nonBlocking(fd);
    fdForget(fd);
    *err = 0;
    return fd;
}

int iris_unixgram_connect(int fd, const char *path, int64_t len) {
    struct sockaddr_un a;
    socklen_t sl;
    if (unixAddr(path, len, &a, &sl) != 0) return 4;
    return connect(fd, (struct sockaddr *)&a, sl) == 0 ? 0 : errno == ENOENT ? 4 : netCode(errno);
}

/* One datagram to path, or where the socket is connected (len 0): 0, or a
   code (9 too large). */
int iris_unixgram_send(int fd, const char *data, int64_t n, const char *path, int64_t len, int64_t timeout) {
    struct sockaddr_un a;
    socklen_t sl = 0;
    if (len > 0 && unixAddr(path, len, &a, &sl) != 0) return 4;
    int64_t deadline = deadlineOf(timeout);
    for (;;) {
        ssize_t k = len > 0 ? sendto(fd, data, (size_t)n, 0, (struct sockaddr *)&a, sl) : send(fd, data, (size_t)n, 0);
        if (k >= 0) return 0;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ENOBUFS) {
            if (!waitFdUntil(fd, 1, deadline)) return 3;
            continue;
        }
        if (errno == EMSGSIZE) return 9;
        if (errno == ENOENT) return 4;
        if (errno == ECONNREFUSED) return 1;
        return netCode(errno);
    }
}

/* One datagram into buf: its length (cut to len), and the path it came
   from in from ("" for an unbound sender, @name for an abstract one); or
   -code. */
int64_t iris_unixgram_recv(int fd, char *buf, int64_t len, IrisString *from, int64_t timeout) {
    int64_t deadline = deadlineOf(timeout);
    for (;;) {
        struct sockaddr_un a;
        socklen_t sl = sizeof a;
        memset(&a, 0, sizeof a);
        ssize_t k = recvfrom(fd, buf, (size_t)len, 0, (struct sockaddr *)&a, &sl);
        if (k >= 0) {
            size_t pl = sl > offsetof(struct sockaddr_un, sun_path) ? sl - offsetof(struct sockaddr_un, sun_path) : 0;
            if (pl > 0 && a.sun_path[0] == 0) {
                iris_string_append(from, "@", 1);
                iris_string_append(from, a.sun_path + 1, (int64_t)pl - 1);
            } else if (pl > 0) {
                iris_string_append(from, a.sun_path, (int64_t)strnlen(a.sun_path, pl));
            }
            return k;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            if (!waitFdUntil(fd, 0, deadline)) return -3;
            continue;
        }
        return -netCode(errno);
    }
}

/* $NOTIFY_SOCKET, where systemd takes a service's notifications. */
const char *iris_notify_socket_ptr(void) {
    const char *p = getenv("NOTIFY_SOCKET");
    return p ? p : "";
}

int64_t iris_notify_socket_len(void) { return (int64_t)strlen(iris_notify_socket_ptr()); }

/* Two connected Unix stream sockets, for talking to a program started
   with one of them, or between threads: 0, or a code. */
int iris_socket_pair(int *a, int *b) {
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return netCode(errno);
    for (int i = 0; i < 2; i++) {
        fcntl(fds[i], F_SETFD, FD_CLOEXEC);
        nonBlocking(fds[i]);
        fdForget(fds[i]);
    }
    *a = fds[0];
    *b = fds[1];
    return 0;
}

int iris_unix_listen(const char *path, int64_t len, int *err) {
    struct sockaddr_un a;
    socklen_t sl;
    if (unixAddr(path, len, &a, &sl) != 0) {
        *err = 4;
        return -1;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        *err = netCode(errno);
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&a, sl) != 0 || listen(fd, 1024) != 0) {
        *err = errno == EADDRINUSE ? 5 : errno == ENOENT ? 4 : netCode(errno);
        close(fd);
        return -1;
    }
    nonBlocking(fd);
    fdForget(fd);
    *err = 0;
    return fd;
}

int iris_unix_dial(const char *path, int64_t len, int *err, int64_t timeout) {
    struct sockaddr_un a;
    socklen_t sl;
    if (unixAddr(path, len, &a, &sl) != 0) {
        *err = 4;
        return -1;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        *err = netCode(errno);
        return -1;
    }
    nonBlocking(fd);
    fdForget(fd);
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof ss);
    memcpy(&ss, &a, sl);
    int code = connectWait(fd, &ss, sl, timeout);
    if (code != 0) {
        *err = code;
        close(fd);
        return -1;
    }
    *err = 0;
    return fd;
}

/* Sends data with descriptors over a Unix socket, the descriptors with its
   first byte: 0, or a net code. */
int iris_unix_send_fds(int fd, const char *data, int64_t len, const int *fds, int64_t n, int64_t timeout) {
    if (len <= 0 || n < 0 || n > 250) return 6;
    int64_t deadline = deadlineOf(timeout);
    union {
        char buf[CMSG_SPACE(250 * sizeof(int))];
        struct cmsghdr align;
    } u;
    memset(&u, 0, sizeof u);
    struct iovec iov = {(void *)data, (size_t)len};
    struct msghdr m = {0};
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    if (n > 0) {
        m.msg_control = u.buf;
        m.msg_controllen = CMSG_SPACE((size_t)n * sizeof(int));
        struct cmsghdr *c = CMSG_FIRSTHDR(&m);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN((size_t)n * sizeof(int));
        memcpy(CMSG_DATA(c), fds, (size_t)n * sizeof(int));
    }
    for (;;) {
#ifdef MSG_NOSIGNAL
        ssize_t w = sendmsg(fd, &m, MSG_NOSIGNAL);
#else
        ssize_t w = sendmsg(fd, &m, 0);
#endif
        if (w > 0) {
            /* the descriptors went with the first bytes; the rest is plain */
            return w < len ? iris_net_write(fd, data + w, len - w, timeout) : 0;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            if (!waitFdUntil(fd, 1, deadline)) return 3;
            continue;
        }
        return netCode(errno);
    }
}

/* Reads bytes and the descriptors that came with them, up to cap of them
   (more are closed): bytes read, 0 at the end, or -code. */
int64_t iris_unix_recv_fds(int fd, char *buf, int64_t len, int *fds, int64_t cap, int64_t *got, int64_t timeout) {
    int64_t deadline = deadlineOf(timeout);
    union {
        char buf[CMSG_SPACE(250 * sizeof(int))];
        struct cmsghdr align;
    } u;
    *got = 0;
    for (;;) {
        struct iovec iov = {buf, (size_t)len};
        struct msghdr m = {0};
        m.msg_iov = &iov;
        m.msg_iovlen = 1;
        m.msg_control = u.buf;
        m.msg_controllen = sizeof u.buf;
#ifdef MSG_CMSG_CLOEXEC
        ssize_t r = recvmsg(fd, &m, MSG_CMSG_CLOEXEC);
#else
        ssize_t r = recvmsg(fd, &m, 0);
#endif
        if (r >= 0) {
            for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
                if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
                int k = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                int *in = (int *)CMSG_DATA(c);
                for (int i = 0; i < k; i++) {
                    if (*got < cap) {
#ifndef MSG_CMSG_CLOEXEC
                        fcntl(in[i], F_SETFD, FD_CLOEXEC);
#endif
                        fds[(*got)++] = in[i];
                    } else {
                        close(in[i]);
                    }
                }
            }
            return r;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            if (!waitFdUntil(fd, 0, deadline)) return -3;
            continue;
        }
        return -netCode(errno);
    }
}

/* ---------------------------------------------------------------- threads */

/* A thread the program started: it runs fn(arg) on a scheduler of its
   own and ends when fn returns. */
typedef struct {
    int lock;
    int done;
    int detached;               /* nobody will join it */
    Task *joiner;
    void (*fn)(void *);
    void *arg;
} Thread;

static void *threadMain(void *p) {
    Thread *th = p;
    (void)current;
    th->fn(th->arg);
    if (liveTasks > 0) {          /* the thread's tasks finish first */
        waitingForTasks = true;
        schedule();
    }
    freeTaskCache();
    spinLock(&th->lock);
    th->done = 1;
    Task *j = th->joiner;
    int detached = th->detached;
    spinUnlock(&th->lock);
    if (j) {
        wakeTask(j);
    }
    if (detached) {
        free(th);
    }
    __atomic_sub_fetch(&liveThreads, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

/* thread f(args): starts the thunk fn(arg) on a new thread. */
void *iris_thread_new(void (*fn)(void *), void *arg) {
    Thread *th = calloc(1, sizeof(Thread));
    if (!th) {
        outOfMemory();
    }
    th->fn = fn;
    th->arg = arg;
    (void)current;
    __atomic_add_fetch(&liveThreads, 1, __ATOMIC_SEQ_CST);
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, threadMain, th) != 0) {
        fflush(stdout);
        fputs("panic: can't start a thread\n", stderr);
        exit(2);
    }
    pthread_attr_destroy(&attr);
    return th;
}

/* Waits for the thread to finish; other tasks of this thread run meanwhile. */
void iris_thread_join(Thread *th) {
    spinLock(&th->lock);
    if (!th->done) {
        th->joiner = current;
        spinUnlock(&th->lock);
        schedule();
    } else {
        spinUnlock(&th->lock);
    }
    free(th);
}

/* thread f() as a statement: nobody joins it, so it frees itself. */
void iris_thread_detach(Thread *th) {
    spinLock(&th->lock);
    bool done = th->done;
    th->detached = 1;
    spinUnlock(&th->lock);
    if (done) {
        free(th);
    }
}

/* Linux spreads connections over listeners that share a port, so each
   thread gets its own. macOS gives them all to one, so threads share one. */
int iris_listen_per_thread(void) {
#ifdef __linux__
    return 1;
#else
    return 0;
#endif
}

/* The cores this process may run on (taskset limits them on Linux). */
int64_t iris_cpu_count(void) {
#ifdef __linux__
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof set, &set) == 0 && CPU_COUNT(&set) > 0) {
        return CPU_COUNT(&set);
    }
#endif
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? n : 1;
}

/* Pins the calling thread to the nth core this process may run on (n
   from 0 to iris_cpu_count() - 1): 0, -2 for an n out of range, -1 off
   Linux, or errno. */
int iris_pin_thread(int64_t n) {
#ifdef __linux__
    cpu_set_t set, one;
    if (sched_getaffinity(0, sizeof set, &set) != 0) return errno;
    int64_t k = 0;
    for (int c = 0; c < CPU_SETSIZE; c++) {
        if (!CPU_ISSET(c, &set)) continue;
        if (k++ == n) {
            CPU_ZERO(&one);
            CPU_SET(c, &one);
            return pthread_setaffinity_np(pthread_self(), sizeof one, &one);
        }
    }
    return -2;
#else
    (void)n;
    return -1;
#endif
}

/* syslog(3): the system log, at a priority from LOG_EMERG (0) to
   LOG_DEBUG (7), as facility, with ident and the process id on each line. */
#include <syslog.h>

static char *syslogIdent;

void iris_syslog_open(const char *ident, int64_t identLen, int facility) {
    static const int facilities[] = {LOG_USER, LOG_DAEMON, LOG_AUTH, LOG_AUTHPRIV, LOG_LOCAL0, LOG_LOCAL1,
                                     LOG_LOCAL2, LOG_LOCAL3, LOG_LOCAL4, LOG_LOCAL5, LOG_LOCAL6, LOG_LOCAL7};
    int f = facility >= 0 && facility < (int)(sizeof facilities / sizeof facilities[0]) ? facilities[facility]
                                                                                       : LOG_DAEMON;
    char *old = syslogIdent;
    syslogIdent = identLen > 0 ? cpath(ident, identLen) : NULL;   /* openlog keeps the pointer */
    openlog(syslogIdent, LOG_PID | LOG_NDELAY, f);
    free(old);
}

void iris_syslog_write(int priority, const char *msg, int64_t len) {
    syslog(priority, "%.*s", (int)len, msg);
}

void iris_syslog_close(void) { closelog(); }

/* The users and groups of the system (getpwnam_r and friends). A user
   found: 0, with its fields added to the Strings; 1 when there is none. */
int iris_user_lookup(const char *name, int64_t nameLen, int64_t uid, IrisString *outName, int64_t *outUid,
                     int64_t *outGid, IrisString *home, IrisString *shell, IrisString *gecos) {
    char buf[16384];
    struct passwd pw, *pwp = NULL;
    if (nameLen >= 0) {
        char *n = cpath(name, nameLen);
        if (!n) return 1;
        getpwnam_r(n, &pw, buf, sizeof buf, &pwp);
        free(n);
    } else {
        getpwuid_r((uid_t)uid, &pw, buf, sizeof buf, &pwp);
    }
    if (!pwp) return 1;
    iris_string_append(outName, pw.pw_name, (int64_t)strlen(pw.pw_name));
    *outUid = pw.pw_uid;
    *outGid = pw.pw_gid;
    if (pw.pw_dir) iris_string_append(home, pw.pw_dir, (int64_t)strlen(pw.pw_dir));
    if (pw.pw_shell) iris_string_append(shell, pw.pw_shell, (int64_t)strlen(pw.pw_shell));
    if (pw.pw_gecos) iris_string_append(gecos, pw.pw_gecos, (int64_t)strlen(pw.pw_gecos));
    return 0;
}

/* The groups user is in, its own group gid among them, into gids (room
   for cap): how many there are, which may be more than cap. */
int64_t iris_user_groups(const char *name, int64_t nameLen, int64_t gid, int64_t *gids, int64_t cap) {
    char *n = cpath(name, nameLen);
    if (!n) return 0;
    int count = 512;
#ifdef __APPLE__
    int list[512];
    getgrouplist(n, (int)gid, list, &count);
#else
    gid_t list[512];
    getgrouplist(n, (gid_t)gid, list, &count);
#endif
    free(n);
    if (count > 512) count = 512;
    for (int i = 0; i < count && i < cap; i++) gids[i] = (int64_t)list[i];
    return count;
}

/* A group, by name (nameLen >= 0) or id: 0 with its name, id and members
   (each followed by a 0 byte), or 1 when there is none. */
int iris_group_lookup(const char *name, int64_t nameLen, int64_t gid, IrisString *outName, int64_t *outGid,
                      IrisString *members) {
    char buf[65536];
    struct group gr, *grp = NULL;
    if (nameLen >= 0) {
        char *n = cpath(name, nameLen);
        if (!n) return 1;
        getgrnam_r(n, &gr, buf, sizeof buf, &grp);
        free(n);
    } else {
        getgrgid_r((gid_t)gid, &gr, buf, sizeof buf, &grp);
    }
    if (!grp) return 1;
    iris_string_append(outName, gr.gr_name, (int64_t)strlen(gr.gr_name));
    *outGid = gr.gr_gid;
    for (char **m = gr.gr_mem; m && *m; m++) iris_string_append(members, *m, (int64_t)strlen(*m) + 1);
    return 0;
}

/* ---------------------------------------------------------------- json

   json.encode, json.decode and the free of @json structs work from a table
   the compiler writes for each type (ir.c, jsonType). */

enum { J_INT = 1, J_UINT, J_FLOAT, J_BOOL, J_STR, J_STRING, J_STRUCT, J_SLICE, J_ARRAY, J_ENUM, J_LIST, J_MAP };

typedef struct JsonType {
    int32_t kind, size;
    int64_t count;              /* fields, array items or enum values */
    const void *items;          /* JsonField[], the element's JsonType, or JsonName[] */
    const void *init;           /* the default value, or NULL when it is all zeros */
} JsonType;

typedef struct {
    const char *name;
    int64_t nameLen, offset;
    const JsonType *type;
    int64_t required;
} JsonField;

typedef struct {
    const char *name;
    int64_t len, value;
} JsonName;

typedef struct {
    char *ptr;
    int64_t len;
} Slice;

/* A List[T] as JSON sees it: its buffer, whose len is the room it has, and
   how many values are in use. */
typedef struct {
    Slice buf;
    int64_t len;
} JsonList;

/* A Map[K, V] of std/core with str or String keys, which JSON reads and
   writes as an object. Its slots are entries of a key and a value; state
   has 0 for an empty slot, 1 for a removed one and 0x80 | 7 bits of the
   key's hash for a used one (std/core/map.iris). */
typedef struct {
    Slice slots, state;
    int64_t len, removed;
} JsonMap;

typedef struct {
    const JsonType *val;
    int64_t keySize, valOffset, entrySize;  /* keySize: 16 for str, 24 for String */
} JsonMapInfo;

int64_t iris_hash_bytes(const char *p, int64_t n);

static int64_t readInt(const char *p, int size, bool sign) {
    switch (size) {
    case 1: return sign ? (int64_t)*(int8_t *)p : (int64_t)*(uint8_t *)p;
    case 2: return sign ? (int64_t)*(int16_t *)p : (int64_t)*(uint16_t *)p;
    case 4: return sign ? (int64_t)*(int32_t *)p : (int64_t)*(uint32_t *)p;
    default: return *(int64_t *)p;
    }
}

static void writeInt(char *p, int size, int64_t v) {
    switch (size) {
    case 1: *(int8_t *)p = (int8_t)v; break;
    case 2: *(int16_t *)p = (int16_t)v; break;
    case 4: *(int32_t *)p = (int32_t)v; break;
    default: *(int64_t *)p = v; break;
    }
}

/* The first quote, backslash or control character from p on, or end. Eight
   bytes are checked at a time while none of them is one. */
static const char *jsonPlainEnd(const char *p, const char *end) {
    const uint64_t ones = 0x0101010101010101ull, highs = 0x8080808080808080ull;
    while (end - p >= 8) {
        uint64_t w;
        memcpy(&w, p, 8);
        uint64_t q = w ^ (ones * '"'), b = w ^ (ones * '\\');
        uint64_t hit = ((q - ones) & ~q) | ((b - ones) & ~b) | ((w - ones * 0x20) & ~w);
        if (hit & highs) break;
        p += 8;
    }
    while (p < end && *p != '"' && *p != '\\' && (unsigned char)*p >= 0x20) p++;
    return p;
}

static void jsonQuote(IrisString *out, const char *s, int64_t n) {
    iris_string_append(out, "\"", 1);
    const char *p = s, *end = s + n;
    for (;;) {
        const char *q = jsonPlainEnd(p, end);
        iris_string_append(out, p, q - p);
        if (q == end) break;
        unsigned char ch = (unsigned char)*q;
        char buf[8];
        const char *esc = buf;
        switch (ch) {
        case '"': esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n"; break;
        case '\r': esc = "\\r"; break;
        case '\t': esc = "\\t"; break;
        case '\b': esc = "\\b"; break;
        case '\f': esc = "\\f"; break;
        default: snprintf(buf, sizeof buf, "\\u%04x", ch);
        }
        iris_string_append(out, esc, (int64_t)strlen(esc));
        p = q + 1;
    }
    iris_string_append(out, "\"", 1);
}

static void jsonEncode(IrisString *out, const JsonType *t, const char *p) {
    char buf[32];
    switch (t->kind) {
    case J_INT: case J_UINT: {
        int64_t v = readInt(p, t->size, t->kind == J_INT);
        /* signed, or an unsigned type small enough to be read into one */
        bool isSigned = t->kind == J_INT || t->size < 8;
        char *end = buf + sizeof buf;
        char *d = u64Digits(isSigned && v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v, end);
        if (isSigned && v < 0) *--d = '-';
        iris_string_append(out, d, end - d);
        break;
    }
    case J_FLOAT: {
        double v = t->size == 4 ? *(float *)p : *(double *)p;
        if (v != v || v == 1.0 / 0.0 || v == -1.0 / 0.0) {
            iris_string_append(out, "null", 4);     /* JSON has no NaN or infinity */
        } else {
            iris_write_float(3, out, v, t->size * 8);
        }
        break;
    }
    case J_BOOL:
        if (*(uint8_t *)p) {
            iris_string_append(out, "true", 4);
        } else {
            iris_string_append(out, "false", 5);
        }
        break;
    case J_STR: {
        const Slice *s = (const Slice *)p;
        jsonQuote(out, s->ptr, s->len);
        break;
    }
    case J_STRING: {
        const IrisString *s = (const IrisString *)p;
        jsonQuote(out, s->ptr, s->len);
        break;
    }
    case J_ENUM: {
        int64_t v = *(int32_t *)p;
        const JsonName *names = t->items;
        for (int64_t i = 0; i < t->count; i++) {
            if (names[i].value == v) {
                jsonQuote(out, names[i].name, names[i].len);
                return;
            }
        }
        int n = snprintf(buf, sizeof buf, "%lld", (long long)v);
        iris_string_append(out, buf, n);
        break;
    }
    case J_STRUCT: {
        const JsonField *fs = t->items;
        iris_string_append(out, "{", 1);
        for (int64_t i = 0; i < t->count; i++) {
            if (i) iris_string_append(out, ",", 1);
            jsonQuote(out, fs[i].name, fs[i].nameLen);
            iris_string_append(out, ":", 1);
            jsonEncode(out, fs[i].type, p + fs[i].offset);
        }
        iris_string_append(out, "}", 1);
        break;
    }
    case J_MAP: {
        const JsonMapInfo *mi = t->items;
        const JsonMap *m = (const JsonMap *)p;
        bool first = true;
        iris_string_append(out, "{", 1);
        for (int64_t i = 0; i < m->state.len; i++) {
            if ((unsigned char)m->state.ptr[i] < 0x80) continue;
            const char *e = m->slots.ptr + i * mi->entrySize;
            if (!first) iris_string_append(out, ",", 1);
            first = false;
            jsonQuote(out, ((const Slice *)e)->ptr, ((const Slice *)e)->len);
            iris_string_append(out, ":", 1);
            jsonEncode(out, mi->val, e + mi->valOffset);
        }
        iris_string_append(out, "}", 1);
        break;
    }
    case J_SLICE: case J_ARRAY: case J_LIST: {
        const JsonType *et = t->items;
        const char *base = p;
        int64_t n = t->count;
        if (t->kind == J_SLICE) {
            base = ((const Slice *)p)->ptr;
            n = ((const Slice *)p)->len;
        } else if (t->kind == J_LIST) {
            base = ((const JsonList *)p)->buf.ptr;
            n = ((const JsonList *)p)->len;
        }
        iris_string_append(out, "[", 1);
        for (int64_t i = 0; i < n; i++) {
            if (i) iris_string_append(out, ",", 1);
            jsonEncode(out, et, base + i * et->size);
        }
        iris_string_append(out, "]", 1);
        break;
    }
    }
}

void iris_json_encode(const JsonType *t, const char *value, IrisString *out) {
    /* room for a small object at once, instead of growing from 16 bytes */
    if (out->cap - out->len < 128) {
        const char *none = NULL;
        stringGrow(out, &none, 128);
    }
    jsonEncode(out, t, value);
}

static bool jsonOwns(const JsonType *t) {
    switch (t->kind) {
    case J_STRING: case J_SLICE: case J_LIST: case J_MAP: return true;
    case J_ARRAY: return jsonOwns(t->items);
    case J_STRUCT: {
        const JsonField *fs = t->items;
        for (int64_t i = 0; i < t->count; i++) {
            if (jsonOwns(fs[i].type)) return true;
        }
        return false;
    }
    default: return false;
    }
}

static void jsonMapFree(const JsonType *t, char *p);

/* Frees the Strings and slices inside the value at p, and empties them. */
static void jsonFree(const JsonType *t, char *p) {
    switch (t->kind) {
    case J_STRING: {
        IrisString *s = (IrisString *)p;
        if (s->cap) free(s->ptr);
        s->ptr = NULL;
        s->len = s->cap = 0;
        break;
    }
    case J_STRUCT: {
        const JsonField *fs = t->items;
        for (int64_t i = 0; i < t->count; i++) {
            int k = fs[i].type->kind;
            if (k == J_STRING) {
                IrisString *s = (IrisString *)(p + fs[i].offset);
                if (s->cap) free(s->ptr);
                s->ptr = NULL;
                s->len = s->cap = 0;
            } else if (k == J_STRUCT || k == J_SLICE || k == J_LIST || k == J_MAP || k == J_ARRAY) {
                jsonFree(fs[i].type, p + fs[i].offset);
            }
        }
        break;
    }
    case J_ARRAY: {
        const JsonType *et = t->items;
        if (jsonOwns(et)) {
            for (int64_t i = 0; i < t->count; i++) jsonFree(et, p + i * et->size);
        }
        break;
    }
    case J_SLICE: {
        const JsonType *et = t->items;
        Slice *s = (Slice *)p;
        if (jsonOwns(et)) {
            for (int64_t i = 0; i < s->len; i++) jsonFree(et, s->ptr + i * et->size);
        }
        free(s->ptr);
        s->ptr = NULL;
        s->len = 0;
        break;
    }
    case J_MAP:
        jsonMapFree(t, p);
        break;
    case J_LIST: {
        const JsonType *et = t->items;
        JsonList *l = (JsonList *)p;
        if (et->kind == J_STRING || et->kind == J_STRUCT ? l->len > 0 : jsonOwns(et)) {
            for (int64_t i = 0; i < l->len; i++) jsonFree(et, l->buf.ptr + i * et->size);
        }
        free(l->buf.ptr);
        memset(l, 0, sizeof *l);
        break;
    }
    }
}

static void jsonMapFree(const JsonType *t, char *p) {
    const JsonMapInfo *mi = t->items;
    JsonMap *m = (JsonMap *)p;
    bool owns = jsonOwns(mi->val);
    for (int64_t i = 0; i < m->state.len; i++) {
        if ((unsigned char)m->state.ptr[i] < 0x80) continue;
        char *e = m->slots.ptr + i * mi->entrySize;
        free(((Slice *)e)->ptr);
        if (owns) jsonFree(mi->val, e + mi->valOffset);
    }
    free(m->slots.ptr);
    free(m->state.ptr);
    memset(m, 0, sizeof *m);
}

void iris_json_free(const JsonType *t, char *value) {
    jsonFree(t, value);
}

int iris_time_parse_duration(const char *p, int64_t n, int64_t *out);

/* The reader: one pass over the text, filling the value in place. */
typedef struct {
    const char *s, *p, *end;
    int code;                   /* 0, or 1 syntax, 2 type, 3 missing, 4 range */
    int depth;
    char path[256];
    size_t pathLen;
} JsonIn;

static _Thread_local char jsonMsg[512];
static _Thread_local int64_t jsonMsgLen;

const char *iris_json_message_ptr(void) { return jsonMsg; }
int64_t iris_json_message_len(void) { return jsonMsgLen; }

static bool jsonFail(JsonIn *in, int code, const char *what) {
    if (in->code) return false;
    in->code = code;
    if (code == 1) {
        int line = 1, col = 1;
        for (const char *q = in->s; q < in->p && q < in->end; q++) {
            if (*q == '\n') {
                line++;
                col = 1;
            } else {
                col++;
            }
        }
        jsonMsgLen = snprintf(jsonMsg, sizeof jsonMsg, "line %d, column %d: %s", line, col, what);
    } else if (in->pathLen) {
        jsonMsgLen = snprintf(jsonMsg, sizeof jsonMsg, "%s: %s", in->path, what);
    } else {
        jsonMsgLen = snprintf(jsonMsg, sizeof jsonMsg, "%s", what);
    }
    if (jsonMsgLen >= (int64_t)sizeof jsonMsg) jsonMsgLen = sizeof jsonMsg - 1;
    return false;
}

static void jsonWs(JsonIn *in) {
    while (in->p < in->end && (*in->p == ' ' || *in->p == '\t' || *in->p == '\n' || *in->p == '\r')) in->p++;
}

static const char *jsonFound(JsonIn *in) {
    jsonWs(in);
    if (in->p >= in->end) return "the end of the text";
    switch (*in->p) {
    case '"': return "a string";
    case '{': return "an object";
    case '[': return "an array";
    case 't': case 'f': return "a bool";
    case 'n': return "null";
    default: return (*in->p == '-' || (*in->p >= '0' && *in->p <= '9')) ? "a number" : "something else";
    }
}

static bool jsonExpected(JsonIn *in, const char *what) {
    char msg[96];
    const char *found = jsonFound(in);
    if (!strcmp(found, "something else") || !strcmp(found, "the end of the text")) {
        snprintf(msg, sizeof msg, "expected %s, found %s", what, found);
        return jsonFail(in, 1, msg);
    }
    snprintf(msg, sizeof msg, "expected %s, found %s", what, found);
    return jsonFail(in, 2, msg);
}

static bool jsonLit(JsonIn *in, const char *word) {
    size_t n = strlen(word);
    if ((size_t)(in->end - in->p) >= n && !memcmp(in->p, word, n)) {
        in->p += n;
        return true;
    }
    return false;
}

static void putUtf8(IrisString *out, uint32_t c) {
    char b[4];
    int n;
    if (c < 0x80) {
        b[0] = (char)c;
        n = 1;
    } else if (c < 0x800) {
        b[0] = (char)(0xc0 | (c >> 6));
        b[1] = (char)(0x80 | (c & 0x3f));
        n = 2;
    } else if (c < 0x10000) {
        b[0] = (char)(0xe0 | (c >> 12));
        b[1] = (char)(0x80 | ((c >> 6) & 0x3f));
        b[2] = (char)(0x80 | (c & 0x3f));
        n = 3;
    } else {
        b[0] = (char)(0xf0 | (c >> 18));
        b[1] = (char)(0x80 | ((c >> 12) & 0x3f));
        b[2] = (char)(0x80 | ((c >> 6) & 0x3f));
        b[3] = (char)(0x80 | (c & 0x3f));
        n = 4;
    }
    iris_string_append(out, b, n);
}

static int hexDigits(const char *p) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

/* A string, at the opening quote, into out (appended). */
static bool jsonString(JsonIn *in, IrisString *out) {
    in->p++;
    const char *start = in->p;
    while (in->p < in->end) {
        in->p = jsonPlainEnd(in->p, in->end);
        if (in->p >= in->end) break;
        unsigned char c = (unsigned char)*in->p;
        if (c == '"') {
            if (out) iris_string_append(out, start, in->p - start);
            in->p++;
            return true;
        }
        if (c < 0x20) {
            return jsonFail(in, 1, "a control character inside a string");
        }
        if (c != '\\') {
            in->p++;
            continue;
        }
        if (out) iris_string_append(out, start, in->p - start);
        in->p++;
        if (in->p >= in->end) break;
        char e = *in->p++;
        char ch = 0;
        switch (e) {
        case '"': ch = '"'; break;
        case '\\': ch = '\\'; break;
        case '/': ch = '/'; break;
        case 'b': ch = '\b'; break;
        case 'f': ch = '\f'; break;
        case 'n': ch = '\n'; break;
        case 'r': ch = '\r'; break;
        case 't': ch = '\t'; break;
        case 'u': {
            if (in->end - in->p < 4 || hexDigits(in->p) < 0) {
                return jsonFail(in, 1, "a \\u escape needs four hex digits");
            }
            uint32_t cp = (uint32_t)hexDigits(in->p);
            in->p += 4;
            if (cp >= 0xd800 && cp < 0xdc00 && in->end - in->p >= 6 && in->p[0] == '\\' && in->p[1] == 'u' &&
                hexDigits(in->p + 2) >= 0xdc00 && hexDigits(in->p + 2) < 0xe000) {
                cp = 0x10000 + ((cp - 0xd800) << 10) + ((uint32_t)hexDigits(in->p + 2) - 0xdc00);
                in->p += 6;
            } else if (cp >= 0xd800 && cp < 0xe000) {
                cp = 0xfffd;            /* half a pair: the replacement character */
            }
            if (out) putUtf8(out, cp);
            start = in->p;
            continue;
        }
        default:
            in->p--;
            return jsonFail(in, 1, "an unknown escape in a string");
        }
        if (out) iris_string_append(out, &ch, 1);
        start = in->p;
    }
    return jsonFail(in, 1, "a string that doesn't end");
}

/* The extent of a number. */
static bool jsonNumber(JsonIn *in, const char **start, size_t *n, bool *integer) {
    const char *p = in->p;
    *start = p;
    *integer = true;
    if (p < in->end && *p == '-') p++;
    if (p >= in->end || *p < '0' || *p > '9') return jsonFail(in, 1, "a number that isn't one");
    if (*p == '0') {
        p++;
    } else {
        while (p < in->end && *p >= '0' && *p <= '9') p++;
    }
    if (p < in->end && *p == '.') {
        *integer = false;
        p++;
        if (p >= in->end || *p < '0' || *p > '9') return jsonFail(in, 1, "a number that isn't one");
        while (p < in->end && *p >= '0' && *p <= '9') p++;
    }
    if (p < in->end && (*p == 'e' || *p == 'E')) {
        *integer = false;
        p++;
        if (p < in->end && (*p == '+' || *p == '-')) p++;
        if (p >= in->end || *p < '0' || *p > '9') return jsonFail(in, 1, "a number that isn't one");
        while (p < in->end && *p >= '0' && *p <= '9') p++;
    }
    *n = (size_t)(p - *start);
    in->p = p;
    return true;
}

static bool jsonSkip(JsonIn *in);

static bool jsonDeeper(JsonIn *in) {
    if (++in->depth > 512) {
        return jsonFail(in, 1, "the text is nested too deeply");
    }
    return true;
}

static bool jsonSkipContainer(JsonIn *in, char close) {
    if (!jsonDeeper(in)) return false;
    in->p++;
    jsonWs(in);
    if (in->p < in->end && *in->p == close) {
        in->p++;
        in->depth--;
        return true;
    }
    for (;;) {
        jsonWs(in);
        if (close == '}') {
            if (in->p >= in->end || *in->p != '"') return jsonFail(in, 1, "expected a key in quotes");
            if (!jsonString(in, NULL)) return false;
            jsonWs(in);
            if (in->p >= in->end || *in->p != ':') return jsonFail(in, 1, "expected ':' after the key");
            in->p++;
        }
        if (!jsonSkip(in)) return false;
        jsonWs(in);
        if (in->p < in->end && *in->p == ',') {
            in->p++;
            continue;
        }
        if (in->p < in->end && *in->p == close) {
            in->p++;
            in->depth--;
            return true;
        }
        return jsonFail(in, 1, close == '}' ? "expected ',' or '}'" : "expected ',' or ']'");
    }
}

/* Reads past one value of any kind. */
static bool jsonSkip(JsonIn *in) {
    jsonWs(in);
    if (in->p >= in->end) return jsonFail(in, 1, "expected a value, found the end of the text");
    switch (*in->p) {
    case '"': return jsonString(in, NULL);
    case '{': return jsonSkipContainer(in, '}');
    case '[': return jsonSkipContainer(in, ']');
    case 't': if (jsonLit(in, "true")) return true; break;
    case 'f': if (jsonLit(in, "false")) return true; break;
    case 'n': if (jsonLit(in, "null")) return true; break;
    default: {
        const char *s;
        size_t n;
        bool integer;
        return jsonNumber(in, &s, &n, &integer);
    }
    }
    return jsonFail(in, 1, "expected a value");
}

/* The path to the value being read, for error messages: items[2].price.
   It is kept for every value, so it is written byte by byte, without
   printf, and cut short when it doesn't fit. */
static void pathPut(JsonIn *in, const char *s, size_t n) {
    size_t room = sizeof in->path - 1 - in->pathLen;
    if (n > room) n = room;
    memcpy(in->path + in->pathLen, s, n);
    in->pathLen += n;
    in->path[in->pathLen] = 0;
}

static void pathField(JsonIn *in, const char *name, int64_t n) {
    if (in->pathLen) pathPut(in, ".", 1);
    pathPut(in, name, (size_t)n);
}

static void pathIndex(JsonIn *in, int64_t i) {
    char buf[24];
    char *end = buf + sizeof buf;
    char *d = u64Digits((uint64_t)i, end - 1);
    *(end - 1) = ']';
    *--d = '[';
    pathPut(in, d, (size_t)(end - d));
}

static bool jsonValue(JsonIn *in, const JsonType *t, char *p);

static bool jsonStruct(JsonIn *in, const JsonType *t, char *p) {
    const JsonField *fs = t->items;
    unsigned char seenSmall[64];
    unsigned char *seen = t->count <= 64 ? seenSmall : calloc((size_t)t->count, 1);
    if (!seen) outOfMemory();
    memset(seen, 0, t->count <= 64 ? sizeof seenSmall : 0);
    IrisString key = {0};
    int64_t next = 0;
    bool ok = jsonDeeper(in);
    if (ok) {
        in->p++;
        jsonWs(in);
        if (in->p < in->end && *in->p == '}') {
            in->p++;
        } else {
            for (;;) {
                jsonWs(in);
                if (in->p >= in->end || *in->p != '"') {
                    ok = jsonFail(in, 1, "expected a key in quotes");
                    break;
                }
                /* a key without escapes is compared where it is */
                const char *kp;
                int64_t kn;
                const char *q = jsonPlainEnd(in->p + 1, in->end);
                if (q < in->end && *q == '"') {
                    kp = in->p + 1;
                    kn = q - kp;
                    in->p = q + 1;
                } else {
                    key.len = 0;
                    if (!jsonString(in, &key)) {
                        ok = false;
                        break;
                    }
                    kp = key.ptr;
                    kn = key.len;
                }
                jsonWs(in);
                if (in->p >= in->end || *in->p != ':') {
                    ok = jsonFail(in, 1, "expected ':' after the key");
                    break;
                }
                in->p++;
                /* keys mostly come in the struct's order: look from the
                   field after the last one first */
                int64_t f = -1;
                for (int64_t k = 0; k < t->count; k++) {
                    int64_t i = next + k < t->count ? next + k : next + k - t->count;
                    if (fs[i].nameLen == kn && !memcmp(fs[i].name, kp, (size_t)kn)) {
                        f = i;
                        next = i + 1;
                        break;
                    }
                }
                if (f < 0) {
                    ok = jsonSkip(in);  /* a key the struct doesn't have */
                } else {
                    size_t saved = in->pathLen;
                    pathField(in, fs[f].name, fs[f].nameLen);
                    ok = jsonValue(in, fs[f].type, p + fs[f].offset);
                    in->pathLen = saved;
                    in->path[saved] = 0;
                    seen[f] = 1;
                }
                if (!ok) break;
                jsonWs(in);
                if (in->p < in->end && *in->p == ',') {
                    in->p++;
                    continue;
                }
                if (in->p < in->end && *in->p == '}') {
                    in->p++;
                    break;
                }
                ok = jsonFail(in, 1, "expected ',' or '}'");
                break;
            }
        }
        in->depth--;
    }
    for (int64_t i = 0; ok && i < t->count; i++) {
        if ((fs[i].required & 1) && !seen[i]) {
            char msg[160];
            snprintf(msg, sizeof msg, "the required field \"%.*s\" is missing", (int)fs[i].nameLen, fs[i].name);
            ok = jsonFail(in, 3, msg);
        }
    }
    iris_string_free(&key);
    if (seen != seenSmall) free(seen);
    return ok;
}

/* The slot of a key in a map, or of the place it would go, as
   Map[K, V].slot does it. */
static int64_t jsonMapSlot(const JsonMap *m, const JsonMapInfo *mi, const char *k, int64_t n, bool *found,
                           unsigned char *tag) {
    uint64_t h = (uint64_t)iris_hash_bytes(k, n);
    int64_t mask = m->state.len - 1, i = (int64_t)(h & (uint64_t)mask), firstRemoved = -1;
    *tag = (unsigned char)(h >> 57) | 0x80;
    for (;;) {
        unsigned char s = (unsigned char)m->state.ptr[i];
        if (s == 0) {
            *found = false;
            return firstRemoved >= 0 ? firstRemoved : i;
        }
        if (s == *tag) {
            const Slice *key = (const Slice *)(m->slots.ptr + i * mi->entrySize);
            if (key->len == n && !memcmp(key->ptr, k, (size_t)n)) {
                *found = true;
                return i;
            }
        } else if (s == 1 && firstRemoved < 0) {
            firstRemoved = i;
        }
        i = (i + 1) & mask;
    }
}

/* Map[K, V].grow: twice the room once half of it is used. */
static void jsonMapGrow(JsonMap *m, const JsonMapInfo *mi) {
    JsonMap old = *m;
    int64_t size = old.slots.len == 0 ? 8 : m->len * 2 >= old.slots.len ? old.slots.len * 2 : old.slots.len;
    m->slots.ptr = calloc((size_t)size, (size_t)mi->entrySize);
    m->state.ptr = calloc((size_t)size, 1);
    if (!m->slots.ptr || !m->state.ptr) outOfMemory();
    m->slots.len = m->state.len = size;
    m->removed = 0;
    for (int64_t i = 0; i < old.state.len; i++) {
        if ((unsigned char)old.state.ptr[i] < 0x80) continue;
        const char *e = old.slots.ptr + i * mi->entrySize;
        bool found;
        unsigned char tag;
        int64_t at = jsonMapSlot(m, mi, ((const Slice *)e)->ptr, ((const Slice *)e)->len, &found, &tag);
        memcpy(m->slots.ptr + at * mi->entrySize, e, (size_t)mi->entrySize);
        m->state.ptr[at] = (char)tag;
    }
    free(old.slots.ptr);
    free(old.state.ptr);
}

/* A JSON object into a map: each key's value read in place in its slot. */
static bool jsonMap(JsonIn *in, const JsonType *t, char *p) {
    const JsonMapInfo *mi = t->items;
    JsonMap *m = (JsonMap *)p;
    jsonMapFree(t, p);          /* what was there before */
    if (!jsonDeeper(in)) return false;
    in->p++;
    jsonWs(in);
    bool ok = true;
    IrisString key = {0};
    if (in->p < in->end && *in->p == '}') {
        in->p++;
    } else {
        for (;;) {
            jsonWs(in);
            if (in->p >= in->end || *in->p != '"') {
                ok = jsonFail(in, 1, "expected a key in quotes");
                break;
            }
            key.len = 0;
            if (!jsonString(in, &key)) {
                ok = false;
                break;
            }
            jsonWs(in);
            if (in->p >= in->end || *in->p != ':') {
                ok = jsonFail(in, 1, "expected ':' after the key");
                break;
            }
            in->p++;
            if ((m->len + m->removed + 1) * 4 > m->state.len * 3) jsonMapGrow(m, mi);
            bool found;
            unsigned char tag;
            const char *kp = key.ptr ? key.ptr : "";
            int64_t at = jsonMapSlot(m, mi, kp, key.len, &found, &tag);
            char *e = m->slots.ptr + at * mi->entrySize;
            char *v = e + mi->valOffset;
            if (found) {
                jsonFree(mi->val, v); /* a key given twice: the last value wins */
                memset(v, 0, (size_t)mi->val->size);
            } else {
                if (m->state.ptr[at] == 1) m->removed--;
                Slice *k = (Slice *)e;
                k->ptr = malloc(key.len ? (size_t)key.len : 1);
                if (!k->ptr) outOfMemory();
                memcpy(k->ptr, kp, (size_t)key.len);
                k->len = key.len;
                if (mi->keySize == 24) ((IrisString *)e)->cap = key.len;
                m->state.ptr[at] = (char)tag;
                m->len++;
            }
            if (mi->val->init) memcpy(v, mi->val->init, (size_t)mi->val->size);
            size_t saved = in->pathLen;
            pathField(in, kp, key.len);
            ok = jsonValue(in, mi->val, v);
            in->pathLen = saved;
            in->path[saved] = 0;
            if (!ok) break;
            jsonWs(in);
            if (in->p < in->end && *in->p == ',') {
                in->p++;
                continue;
            }
            if (in->p < in->end && *in->p == '}') {
                in->p++;
                break;
            }
            ok = jsonFail(in, 1, "expected ',' or '}'");
            break;
        }
    }
    in->depth--;
    iris_string_free(&key);
    return ok;
}

static bool jsonList(JsonIn *in, const JsonType *t, char *p) {
    const JsonType *et = t->items;
    bool slice = t->kind == J_SLICE || t->kind == J_LIST;
    char *buf = slice ? NULL : p;
    int64_t n = 0, cap = slice ? 0 : t->count;
    if (slice) {
        jsonFree(t, p);         /* what was there before */
    }
    if (!jsonDeeper(in)) return false;
    in->p++;
    jsonWs(in);
    bool ok = true;
    if (in->p < in->end && *in->p == ']') {
        in->p++;
    } else {
        for (;;) {
            if (n == cap) {
                if (!slice) {
                    char msg[96];
                    snprintf(msg, sizeof msg, "more than %lld items", (long long)t->count);
                    ok = jsonFail(in, 2, msg);
                    break;
                }
                int64_t ncap = cap ? cap * 2 : 8;
                char *nb = realloc(buf, (size_t)(ncap * et->size));
                if (!nb) outOfMemory();
                memset(nb + cap * et->size, 0, (size_t)((ncap - cap) * et->size));
                buf = nb;
                cap = ncap;
            }
            size_t saved = in->pathLen;
            pathIndex(in, n);
            if (slice && et->init) memcpy(buf + n * et->size, et->init, (size_t)et->size);
            ok = jsonValue(in, et, buf + n * et->size);
            in->pathLen = saved;
            in->path[saved] = 0;
            n++;
            if (!ok) break;
            jsonWs(in);
            if (in->p < in->end && *in->p == ',') {
                in->p++;
                jsonWs(in);
                continue;
            }
            if (in->p < in->end && *in->p == ']') {
                in->p++;
                break;
            }
            ok = jsonFail(in, 1, "expected ',' or ']'");
            break;
        }
    }
    in->depth--;
    if (t->kind == J_LIST) {
        /* the list keeps the room it grew, for the values pushed later */
        JsonList *l = (JsonList *)p;
        l->buf.ptr = buf;
        l->buf.len = cap;
        l->len = n;
    } else if (slice) {
        Slice *s = (Slice *)p;
        s->ptr = buf;
        s->len = n;
        if (!n) {
            free(buf);
            s->ptr = NULL;
        }
    }
    return ok;
}

static bool jsonValue(JsonIn *in, const JsonType *t, char *p) {
    jsonWs(in);
    if (in->p >= in->end) return jsonFail(in, 1, "expected a value, found the end of the text");
    if (*in->p == 'n' && jsonLit(in, "null")) {
        return true;            /* null leaves the value as it is */
    }
    switch (t->kind) {
    case J_INT: case J_UINT: case J_FLOAT: {
        if (t->kind == J_INT && t->count == 1 && *in->p == '"') {
            /* a Duration as text: "30s", "1m30s", "250ms" */
            IrisString text = {0};
            bool ok = jsonString(in, &text);
            int64_t d = 0;
            if (ok && !(text.len > 0 && iris_time_parse_duration(text.ptr, text.len, &d))) {
                ok = jsonFail(in, 2, "expected a duration like 30s or 1m30s");
            }
            iris_string_free(&text);
            if (ok) *(int64_t *)p = d;
            return ok;
        }
        if (*in->p != '-' && (*in->p < '0' || *in->p > '9')) return jsonExpected(in, t->count == 1 ? "a number or a duration like 30s" : "a number");
        const char *s;
        size_t n;
        bool integer;
        if (!jsonNumber(in, &s, &n, &integer)) return false;
        char num[64];
        if (n >= sizeof num) n = sizeof num - 1;
        memcpy(num, s, n);
        num[n] = 0;
        if (t->kind == J_FLOAT) {
            double v = irisStrtod(num, NULL);
            if (t->size == 4) *(float *)p = (float)v;
            else *(double *)p = v;
            return true;
        }
        if (!integer) return jsonFail(in, 2, "expected a whole number");
        errno = 0;
        char msg[128];
        /* up to 18 digits can't overflow: read them here, without strtoll */
        bool neg = num[0] == '-';
        bool shortInt = n - neg <= 18;
        uint64_t u = 0;
        for (size_t i = neg; shortInt && i < n; i++) u = u * 10 + (uint64_t)(num[i] - '0');
        if (t->kind == J_INT) {
            long long v = shortInt ? (neg ? -(long long)u : (long long)u) : strtoll(num, NULL, 10);
            int bits = t->size * 8;
            bool fits = errno == 0 && (bits == 64 || (v >= -(1LL << (bits - 1)) && v < (1LL << (bits - 1))));
            if (!fits) {
                snprintf(msg, sizeof msg, "%s doesn't fit in i%d", num, bits);
                return jsonFail(in, 4, msg);
            }
            writeInt(p, t->size, v);
        } else {
            unsigned long long v = neg ? 0 : shortInt ? u : strtoull(num, NULL, 10);
            int bits = t->size * 8;
            bool fits = num[0] != '-' && errno == 0 && (bits == 64 || v < (1ULL << bits));
            if (!fits) {
                snprintf(msg, sizeof msg, "%s doesn't fit in u%d", num, bits);
                return jsonFail(in, 4, msg);
            }
            writeInt(p, t->size, (int64_t)v);
        }
        return true;
    }
    case J_BOOL:
        if (jsonLit(in, "true")) {
            *(uint8_t *)p = 1;
            return true;
        }
        if (jsonLit(in, "false")) {
            *(uint8_t *)p = 0;
            return true;
        }
        return jsonExpected(in, "true or false");
    case J_STRING: {
        if (*in->p != '"') return jsonExpected(in, "a string");
        IrisString *s = (IrisString *)p;
        iris_string_free(s);
        bool ok = jsonString(in, s);
        if (ok && !s->ptr) iris_string_append(s, "", 0);
        return ok;
    }
    case J_ENUM: {
        if (*in->p != '"') return jsonExpected(in, "a string");
        IrisString name = {0};
        bool ok = jsonString(in, &name);
        if (ok) {
            const JsonName *names = t->items;
            int64_t i = 0;
            while (i < t->count && !(names[i].len == name.len && !memcmp(names[i].name, name.ptr, (size_t)name.len))) {
                i++;
            }
            if (i < t->count) {
                *(int32_t *)p = (int32_t)names[i].value;
            } else {
                char msg[160];
                snprintf(msg, sizeof msg, "\"%.*s\" isn't one of the enum's values", (int)(name.len > 60 ? 60 : name.len),
                         name.ptr ? name.ptr : "");
                ok = jsonFail(in, 2, msg);
            }
        }
        iris_string_free(&name);
        return ok;
    }
    case J_STRUCT:
        if (*in->p != '{') return jsonExpected(in, "an object");
        return jsonStruct(in, t, p);
    case J_MAP:
        if (*in->p != '{') return jsonExpected(in, "an object");
        return jsonMap(in, t, p);
    case J_SLICE: case J_ARRAY: case J_LIST:
        if (*in->p != '[') return jsonExpected(in, "an array");
        return jsonList(in, t, p);
    default:
        return jsonFail(in, 2, "this kind of value can't be read from JSON");
    }
}

/* json.decode: fills the value at out from text. 0, or the code of the
   error, whose message iris_json_message_ptr/len give. On an error what
   was filled in is freed. */
int iris_json_decode(const JsonType *t, const char *text, int64_t len, char *out) {
    JsonIn in = {.s = text, .p = text, .end = text + len};
    bool ok = jsonValue(&in, t, out);
    if (ok) {
        jsonWs(&in);
        if (in.p < in.end) {
            ok = jsonFail(&in, 1, "more text after the value");
        }
    }
    if (!ok) {
        jsonFree(t, out);
        return in.code ? in.code : 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- wire

   The compiler writes a decode, a size and an encode function for each
   @wire struct (ir.c, wireFns) and a table of them. These functions run
   them, and give the generated code its messages and memory. */

typedef int (*WireDec)(const unsigned char **pos, const unsigned char *end, char *out);
typedef int64_t (*WireSize)(const char *value);
typedef void (*WireEnc)(unsigned char **pos, const char *value);

typedef struct {
    WireDec dec;
    WireSize size;
    WireEnc enc;
    void (*release)(char *value);
} WireFns;

/* What went wrong, as the generated code says it. */
enum {
    WIRE_SHORT = 1, WIRE_MAX_BYTES, WIRE_MAX_ITEMS, WIRE_BOOL, WIRE_ENUM, WIRE_FLAGS, WIRE_NEGATIVE,
    WIRE_LIMIT_BYTES, WIRE_LIMIT_ITEMS
};

static _Thread_local char wireMsg[256];
static _Thread_local int64_t wireMsgLen;
static _Thread_local uintptr_t wireNeedAt;     /* after a short message: where its bytes would end */

const char *iris_wire_message_ptr(void) { return wireMsg; }
int64_t iris_wire_message_len(void) { return wireMsgLen; }

/* Keeps the message and gives the WireError: 1 short, 2 tooLong, 3 badValue.
   A short message at `at` needs value * limit more bytes. */
int iris_wire_fail(int what, const char *name, int64_t nameLen, int64_t value, int64_t limit, const unsigned char *at) {
    char what_[160];
    int code = 3;
    unsigned long long u = (unsigned long long)value;
    switch (what) {
    case WIRE_SHORT: {
        uint64_t n;
        if (__builtin_mul_overflow((uint64_t)value, (uint64_t)limit, &n) ||
            __builtin_add_overflow((uint64_t)(uintptr_t)at, n, &n)) {
            n = UINTPTR_MAX;
        }
        wireNeedAt = (uintptr_t)n;
        snprintf(what_, sizeof what_, "the bytes end in the middle of the message");
        code = 1;
        break;
    }
    case WIRE_MAX_BYTES: case WIRE_MAX_ITEMS: case WIRE_LIMIT_BYTES: case WIRE_LIMIT_ITEMS:
        snprintf(what_, sizeof what_, "%llu %s, more than the %s of %lld", u,
                 what == WIRE_MAX_BYTES || what == WIRE_LIMIT_BYTES ? "bytes" : "items",
                 what <= WIRE_MAX_ITEMS ? "max" : "length's limit", (long long)limit);
        code = 2;
        break;
    case WIRE_BOOL: snprintf(what_, sizeof what_, "%llu isn't a bool, which is 0 or 1", u); break;
    case WIRE_ENUM: snprintf(what_, sizeof what_, "%lld isn't one of the enum's values", (long long)value); break;
    case WIRE_FLAGS: snprintf(what_, sizeof what_, "%lld has bits that aren't values of the enum", (long long)value); break;
    default: snprintf(what_, sizeof what_, "a length can't be %lld", (long long)value); break;
    }
    int n = nameLen ? snprintf(wireMsg, sizeof wireMsg, "%.*s: %s", (int)(nameLen > 60 ? 60 : nameLen), name, what_)
                    : snprintf(wireMsg, sizeof wireMsg, "%s", what_);
    wireMsgLen = n < 0 ? 0 : n >= (int)sizeof wireMsg ? (int64_t)sizeof wireMsg - 1 : n;
    return code;
}

/* A String field: a copy of the n bytes at p. */
void iris_wire_string(IrisString *s, const char *p, int64_t n) {
    char *q = malloc((size_t)(n ? n : 1));
    if (!q) outOfMemory();
    memcpy(q, p, (size_t)n);
    s->ptr = q;
    s->len = n;
    s->cap = n ? n : 1;
}

/* The memory of a slice field of n items, all zeros when zero is set:
   items that are read in place, which a failure half way frees. */
void *iris_wire_alloc(int64_t n, int64_t size, bool zero) {
    if (n == 0) return NULL;
    void *p = zero ? calloc((size_t)n, (size_t)size) : malloc((size_t)(n * size));
    if (!p) outOfMemory();
    return p;
}

/* Reads one message from the len bytes at p into out: 0 and its size in
   *n, or the WireError. After a short one, *n is how many bytes it needs
   at least. On an error what was filled in is freed. */
int iris_wire_decode(const WireFns *t, const unsigned char *p, int64_t len, char *out, int64_t *n) {
    const unsigned char *pos = p;
    int code = t->dec(&pos, p + len, out);
    if (code == 0) {
        *n = pos - p;
        return 0;
    }
    t->release(out);
    *n = 0;
    if (code == 1) {
        uintptr_t need = wireNeedAt - (uintptr_t)p;
        *n = need > (uintptr_t)INT64_MAX || wireNeedAt < (uintptr_t)p ? INT64_MAX : (int64_t)need;
    }
    return code;
}

void iris_wire_free(const WireFns *t, char *value) {
    t->release(value);
}

/* Appends the bytes of the value at p to out: 0, or 2 (tooLong) when a
   String or slice doesn't fit its length or max. */
int iris_wire_encode(const WireFns *t, const char *p, IrisString *out) {
    int64_t n = t->size(p);
    if (n < 0) return 2;
    iris_string_reserve(out, n);
    unsigned char *pos = (unsigned char *)out->ptr + out->len;
    t->enc(&pos, p);
    out->len += n;
    return 0;
}

/* ---------------------------------------------------------------- tests

   iris test: main calls iris_test_start before each test (false: the
   filter, the program's first argument, leaves it out), the test, then
   iris_test_done with the error it returned. */

static int testsRun, testsFailed;
static int testFailedNow;

bool iris_test_start(const char *name) {
    if (gArgc > 1 && !strstr(name, gArgv[1])) {
        return false;
    }
    testFailedNow = 0;
    testsRun++;
    return true;
}

void iris_expect_failed(const char *file, int line, int col, IrisString *msg) {
    fflush(stdout);
    if (msg && msg->len) {
        fprintf(stderr, "    %s:%d:%d: expect failed: %.*s\n", file, line, col, (int)msg->len, msg->ptr);
    } else {
        fprintf(stderr, "    %s:%d:%d: expect failed\n", file, line, col);
    }
    testFailedNow = 1;
}

void iris_test_done(const char *name, int64_t err) {
    fflush(stdout);
    if (err) {
        IrisString s = {0};
        iris_write_errcode(3, &s, err);
        fprintf(stderr, "    error: %.*s", (int)s.len, s.ptr);
        if (msgCurLen) {
            fprintf(stderr, ": %.*s", (int)msgCurLen, msgCur);
        }
        fputc('\n', stderr);
        for (int i = 0; i < traceLen; i++) {
            fprintf(stderr, "      at %s  %s:%d\n", trace[i].fn, trace[i].file, trace[i].line);
        }
        iris_string_free(&s);
        testFailedNow = 1;
    }
    if (testFailedNow) {
        testsFailed++;
        printf("FAIL %s\n", name);
    } else {
        printf("ok   %s\n", name);
    }
    fflush(stdout);
}

int iris_test_summary(void) {
    printf("%d test%s, %d passed, %d failed\n", testsRun, testsRun == 1 ? "" : "s", testsRun - testsFailed,
           testsFailed);
    return testsFailed ? 1 : 0;
}

/* iris bench: main calls iris_bench_start before each bench (false: the
   filter, the program's first argument, leaves it out), then
   iris_bench_run with the function the bench block became. It is called
   with a count, 1 first, then larger ones until one call takes the bench
   time (IRIS_BENCH_TIME nanoseconds, 1s when not set); that call's time
   over its count is what one run takes. */

static int benchesRun, benchesFailed;

bool iris_bench_start(const char *name) {
    if (gArgc > 1 && !strstr(name, gArgv[1])) {
        return false;
    }
    benchesRun++;
    return true;
}

static void benchPerRun(char *buf, size_t size, double ns) {
    if (ns < 1000) {
        snprintf(buf, size, "%.2fns", ns);
    } else if (ns < 1e6) {
        snprintf(buf, size, "%.2fus", ns / 1e3);
    } else if (ns < 1e9) {
        snprintf(buf, size, "%.2fms", ns / 1e6);
    } else {
        snprintf(buf, size, "%.2fs", ns / 1e9);
    }
}

void iris_bench_run(const char *name, int64_t (*fn)(int64_t)) {
    int64_t target = 1000000000LL;
    const char *env = getenv("IRIS_BENCH_TIME");
    if (env && *env && atoll(env) > 0) {
        target = atoll(env);
    }
    int64_t n = 1, took = 0;
    for (;;) {
        int64_t start = nowNs();
        int64_t err = fn(n);
        took = nowNs() - start;
        if (err) {
            fflush(stdout);
            IrisString s = {0};
            iris_write_errcode(3, &s, err);
            fprintf(stderr, "    error: %.*s", (int)s.len, s.ptr);
            if (msgCurLen) {
                fprintf(stderr, ": %.*s", (int)msgCurLen, msgCur);
            }
            fputc('\n', stderr);
            for (int i = 0; i < traceLen; i++) {
                fprintf(stderr, "      at %s  %s:%d\n", trace[i].fn, trace[i].file, trace[i].line);
            }
            iris_string_free(&s);
            benchesFailed++;
            printf("FAIL %s\n", name);
            fflush(stdout);
            return;
        }
        if (took >= target || n >= 1000000000LL) {
            break;
        }
        /* aim a little past the target, growing at most 100 times */
        double want = took > 0 ? (double)n * (double)target / (double)took * 1.2 : (double)n * 100;
        int64_t next = want > (double)n * 100 ? n * 100 : (int64_t)want;
        n = next > n ? next : n + 1;
        if (n > 1000000000LL) {
            n = 1000000000LL;
        }
    }
    char per[32];
    benchPerRun(per, sizeof per, (double)took / (double)n);
    printf("%-32s %12lld runs %12s/run\n", name, (long long)n, per);
    fflush(stdout);
}

int iris_bench_summary(void) {
    if (benchesFailed) {
        printf("%d bench%s, %d failed\n", benchesRun, benchesRun == 1 ? "" : "es", benchesFailed);
    }
    return benchesFailed ? 1 : 0;
}

/* ---------------------------------------------------------------- hashing (Map) */

int64_t iris_hash_u64(int64_t v) {
    uint64_t x = (uint64_t)v + 0x9e3779b97f4a7c15ULL;     /* splitmix64 */
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return (int64_t)(x ^ (x >> 31));
}

int64_t iris_hash_bytes(const char *p, int64_t n) {
    uint64_t h = 0xcbf29ce484222325ULL;                     /* FNV-1a, then mixed */
    for (int64_t i = 0; i < n; i++) {
        h = (h ^ (unsigned char)p[i]) * 0x100000001b3ULL;
    }
    return iris_hash_u64((int64_t)h);
}

char *iris_bytes_dup(const char *p, int64_t n) {
    char *c = malloc(n ? (size_t)n : 1);
    if (!c) outOfMemory();
    if (n) memcpy(c, p, (size_t)n);
    return c;
}

/* ---------------------------------------------------------------- time */

int64_t iris_time_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int64_t iris_time_clock(void) {
    return nowNs();
}

/* The computer's time zone offset, in seconds east of UTC, at a moment. */
int32_t iris_time_offset(int64_t unixSeconds) {
    time_t t = (time_t)unixSeconds;
    struct tm tm;
    if (!localtime_r(&t, &tm)) {
        return 0;
    }
    return (int32_t)tm.tm_gmtoff;
}

/* ---------------------------------------------------------------- processes */

#include <sys/wait.h>
#include <poll.h>

extern char **environ;

/* Runs argv[0] with argv, writing input to its standard input and taking
   its standard output and error. The other tasks of the thread run while
   it waits. 0 when it ran (status: its exit status, or 128 + the signal
   that ended it), 1 when it can't be found, 2 when it can't be started. */
int iris_process_run(const Slice *argv, int64_t argc, const char *input, int64_t inputLen, IrisString *out,
                     IrisString *errOut, int *status) {
    if (argc < 1) return 1;
    char **args = calloc((size_t)argc + 1, sizeof(char *));
    if (!args) outOfMemory();
    for (int64_t i = 0; i < argc; i++) {
        args[i] = cpath(argv[i].ptr, argv[i].len);
    }
    int inP[2], outP[2], errP[2], execP[2];
    if (pipe(inP) || pipe(outP) || pipe(errP) || pipe(execP)) {
        for (int64_t i = 0; i < argc; i++) free(args[i]);
        free(args);
        return 2;
    }
    fcntl(execP[1], F_SETFD, FD_CLOEXEC);
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(inP[0], 0);
        dup2(outP[1], 1);
        dup2(errP[1], 2);
        close(inP[0]); close(inP[1]); close(outP[0]); close(outP[1]); close(errP[0]); close(errP[1]);
        close(execP[0]);
        execvp(args[0], args);
        int e = errno;
        ssize_t w = write(execP[1], &e, sizeof e);
        (void)w;
        _exit(127);
    }
    close(inP[0]); close(outP[1]); close(errP[1]); close(execP[1]);
    for (int64_t i = 0; i < argc; i++) free(args[i]);
    free(args);
    if (pid < 0) {
        close(inP[1]); close(outP[0]); close(errP[0]); close(execP[0]);
        return 2;
    }
    int execErr = 0;
    ssize_t got = read(execP[0], &execErr, sizeof execErr);
    close(execP[0]);
    if (got == (ssize_t)sizeof execErr) {
        close(inP[1]); close(outP[0]); close(errP[0]);
        waitpid(pid, NULL, 0);
        return execErr == ENOENT ? 1 : 2;
    }
    fcntl(inP[1], F_SETFL, O_NONBLOCK);
    fcntl(outP[0], F_SETFL, O_NONBLOCK);
    fcntl(errP[0], F_SETFL, O_NONBLOCK);
    int fdIn = inputLen > 0 ? inP[1] : -1;
    if (fdIn < 0) close(inP[1]);
    int fdOut = outP[0], fdErr = errP[0];
    int64_t sent = 0;
    char buf[65536];
    while (fdOut >= 0 || fdErr >= 0 || fdIn >= 0) {
        struct pollfd p[3];
        int np = 0;
        if (fdOut >= 0) p[np++] = (struct pollfd){fdOut, POLLIN, 0};
        if (fdErr >= 0) p[np++] = (struct pollfd){fdErr, POLLIN, 0};
        if (fdIn >= 0) p[np++] = (struct pollfd){fdIn, POLLOUT, 0};
        int r = poll(p, (nfds_t)np, 0);
        if (r == 0) {
            iris_sleep(1000000);        /* nothing yet: let the other tasks run */
            continue;
        }
        if (r < 0 && errno != EINTR) break;
        for (int i = 0; i < np; i++) {
            if (!p[i].revents) continue;
            if (p[i].fd == fdIn) {
                ssize_t w = write(fdIn, input + sent, (size_t)(inputLen - sent));
                if (w > 0) sent += w;
                if (sent >= inputLen || (w < 0 && errno != EAGAIN)) {
                    close(fdIn);
                    fdIn = -1;
                }
                continue;
            }
            ssize_t n = read(p[i].fd, buf, sizeof buf);
            if (n > 0) {
                iris_string_append(p[i].fd == fdOut ? out : errOut, buf, n);
            } else if (n == 0 || errno != EAGAIN) {
                close(p[i].fd);
                if (p[i].fd == fdOut) fdOut = -1;
                else fdErr = -1;
            }
        }
    }
    int st = 0;
    while (waitpid(pid, &st, WNOHANG) == 0) {
        iris_sleep(1000000);
    }
    *status = WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 1;
    return 0;
}

/* Starts argv without waiting for it: it shares this program's standard
   input, output and error, and gets keep[i] as descriptor 3 + i. 0 with
   *pid and *handle (a pidfd on Linux, else -1), 1 when it can't be found,
   2 when it can't be started. */
int iris_process_start(const Slice *argv, int64_t argc, const int *keep, int64_t nkeep, int64_t *pid,
                       int *handle) {
    if (argc < 1) return 1;
    char **args = calloc((size_t)argc + 1, sizeof(char *));
    if (!args) outOfMemory();
    for (int64_t i = 0; i < argc; i++) args[i] = cpath(argv[i].ptr, argv[i].len);
    int execP[2];
    if (pipe(execP)) {
        for (int64_t i = 0; i < argc; i++) free(args[i]);
        free(args);
        return 2;
    }
    fcntl(execP[0], F_SETFD, FD_CLOEXEC);
    fcntl(execP[1], F_SETFD, FD_CLOEXEC);
    fflush(stdout);
    fflush(stderr);
    pid_t p = fork();
    if (p == 0) {
        /* each kept descriptor goes out of the way first, so moving one
           to 3 + i can't close another that is still to move */
        int base = 3 + (int)nkeep + 1;
        for (int64_t i = 0; i < nkeep; i++) {
            if (keep[i] >= base) base = keep[i] + 1;
        }
        int moved[256];
        for (int64_t i = 0; i < nkeep && i < 256; i++) moved[i] = fcntl(keep[i], F_DUPFD, base);
        for (int64_t i = 0; i < nkeep && i < 256; i++) {
            if (moved[i] < 0 || dup2(moved[i], 3 + (int)i) < 0) _exit(126);
            close(moved[i]);
        }
        /* threads that splice hold SIGPIPE: the program starts without that */
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        execvp(args[0], args);
        int e = errno;
        ssize_t w = write(execP[1], &e, sizeof e);
        (void)w;
        _exit(127);
    }
    close(execP[1]);
    for (int64_t i = 0; i < argc; i++) free(args[i]);
    free(args);
    if (p < 0) {
        close(execP[0]);
        return 2;
    }
    int execErr = 0;
    ssize_t got = read(execP[0], &execErr, sizeof execErr);
    close(execP[0]);
    if (got == (ssize_t)sizeof execErr) {
        waitpid(p, NULL, 0);
        return execErr == ENOENT ? 1 : 2;
    }
    *pid = p;
    *handle = -1;
#ifdef __linux__
    int h = (int)syscall(434 /* pidfd_open */, p, 0);
    if (h >= 0) {
        fcntl(h, F_SETFD, FD_CLOEXEC);
        *handle = h;
    }
#endif
    return 0;
}

/* The executable for name: itself when it has a /, else the first one on
   path (a PATH value); NULL when there is none. */
static char *findProgram(const char *name, const char *path) {
    if (strchr(name, '/')) return strdup(name);
    if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";
    char buf[PATH_MAX];
    while (*path) {
        const char *end = strchr(path, ':');
        size_t len = end ? (size_t)(end - path) : strlen(path);
        int n = snprintf(buf, sizeof buf, "%.*s%s%s", (int)len, path, len ? "/" : "", name);
        if (n > 0 && n < (int)sizeof buf && access(buf, X_OK) == 0) {
            struct stat st;
            if (stat(buf, &st) == 0 && S_ISREG(st.st_mode)) return strdup(buf);
        }
        if (!end) break;
        path = end + 1;
    }
    return NULL;
}

/* process.startWith: argv with the options set. env holds NAME=value
   entries that replace or add to this program's environment, or are the
   whole of it with cleanEnv. user, when not empty, is who it runs as (its
   groups, its group, then it). terminal, when not -1, is a pty's terminal
   side: the program gets it as 0, 1 and 2 and as its controlling terminal,
   in a session of its own. pipes gives *io three sockets, the other ends
   of its standard input, output and error. 0, 1 not found, 2 can't start,
   3 no such user. */
int iris_process_spawn(const Slice *argv, int64_t argc, const int *keep, int64_t nkeep, const Slice *env,
                       int64_t nenv, int cleanEnv, const char *dir, int64_t dirLen, const char *argv0,
                       int64_t argv0Len, int pipes, int newSession, const char *user, int64_t userLen,
                       int terminal, int64_t *pid, int *handle, int *io) {
    if (argc < 1) return 1;
    /* the environment: this one's, with env's entries replacing those of
       the same name, or env alone */
    int64_t cap = nenv + 1;
    int64_t have = 0;
    if (!cleanEnv) {
        for (char **e = environ; *e; e++) cap++;
    }
    char **envp = calloc((size_t)cap + 1, sizeof(char *));
    if (!envp) outOfMemory();
    if (!cleanEnv) {
        for (char **e = environ; *e; e++) {
            const char *eq = strchr(*e, '=');
            size_t nl = eq ? (size_t)(eq - *e) : strlen(*e);
            bool replaced = false;
            for (int64_t i = 0; i < nenv && !replaced; i++) {
                const char *q = memchr(env[i].ptr, '=', (size_t)env[i].len);
                size_t il = q ? (size_t)(q - (const char *)env[i].ptr) : (size_t)env[i].len;
                replaced = il == nl && memcmp(env[i].ptr, *e, nl) == 0;
            }
            if (!replaced) envp[have++] = strdup(*e);
        }
    }
    const char *pathVar = getenv("PATH");
    for (int64_t i = 0; i < nenv; i++) {
        envp[have] = cpath(env[i].ptr, env[i].len);
        if (envp[have] && strncmp(envp[have], "PATH=", 5) == 0) pathVar = envp[have] + 5;
        have++;
    }
    char **args = calloc((size_t)argc + 1, sizeof(char *));
    if (!args) outOfMemory();
    for (int64_t i = 0; i < argc; i++) args[i] = cpath(argv[i].ptr, argv[i].len);
    char *prog = findProgram(args[0], pathVar);
    if (argv0Len > 0) {
        free(args[0]);
        args[0] = cpath(argv0, argv0Len);
    }
    char *wd = dirLen > 0 ? cpath(dir, dirLen) : NULL;
    int code = 0;
    uid_t uid = 0;
    gid_t gid = 0;
    gid_t groups[512];
    int ngroups = 0;
    if (userLen > 0) {
        char *u = cpath(user, userLen);
        char buf[16384];
        struct passwd pw, *pwp = NULL;
        if (u && getpwnam_r(u, &pw, buf, sizeof buf, &pwp) == 0 && pwp) {
            uid = pw.pw_uid;
            gid = pw.pw_gid;
            ngroups = 512;
#ifdef __APPLE__
            getgrouplist(u, (int)gid, (int *)groups, &ngroups);
#else
            getgrouplist(u, gid, groups, &ngroups);
#endif
            if (ngroups > 512) ngroups = 512;
        } else {
            code = 3;
        }
        free(u);
    }
    if (!prog && !code) code = 1;
    int sv[3][2] = {{-1, -1}, {-1, -1}, {-1, -1}};
    int execP[2] = {-1, -1};
    if (!code && pipes) {
        for (int i = 0; i < 3 && !code; i++) {
            if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv[i]) != 0) {
                code = 2;
            } else {
                /* neither end may stay open in the child past exec: it would
                   hold its own stdin open and never see the end of it */
                fcntl(sv[i][0], F_SETFD, FD_CLOEXEC);
                fcntl(sv[i][1], F_SETFD, FD_CLOEXEC);
            }
        }
    }
    if (!code && pipe(execP) != 0) code = 2;
    pid_t p = -1;
    if (!code) {
        fcntl(execP[0], F_SETFD, FD_CLOEXEC);
        fcntl(execP[1], F_SETFD, FD_CLOEXEC);
        fflush(stdout);
        fflush(stderr);
        p = fork();
        if (p == 0) {
            if (newSession || terminal >= 0) setsid();
            if (terminal >= 0) {
                ioctl(terminal, TIOCSCTTY, 0);
                for (int i = 0; i < 3; i++) dup2(terminal, i);
            } else if (pipes) {
                for (int i = 0; i < 3; i++) {
                    if (sv[i][1] == i) {
                        fcntl(i, F_SETFD, 0);
                    } else {
                        dup2(sv[i][1], i);
                    }
                }
            }
            /* the kept descriptors go out of the way first, then to 3 + i */
            int base = 3 + (int)nkeep + 1;
            for (int64_t i = 0; i < nkeep; i++) {
                if (keep[i] >= base) base = keep[i] + 1;
            }
            int moved[256];
            for (int64_t i = 0; i < nkeep && i < 256; i++) moved[i] = fcntl(keep[i], F_DUPFD, base);
            for (int64_t i = 0; i < nkeep && i < 256; i++) {
                if (moved[i] < 0 || dup2(moved[i], 3 + (int)i) < 0) _exit(126);
                close(moved[i]);
            }
            sigset_t none;
            sigemptyset(&none);
            sigprocmask(SIG_SETMASK, &none, NULL);
            int e = 0;
            if (userLen > 0 && (setgroups((size_t)ngroups, groups) != 0 || setgid(gid) != 0 || setuid(uid) != 0)) {
                e = errno ? errno : EPERM;
            }
            if (!e && wd && chdir(wd) != 0) e = errno;
            if (!e) {
                execve(prog, args, envp);
                e = errno;
            }
            ssize_t w = write(execP[1], &e, sizeof e);
            (void)w;
            _exit(127);
        }
        if (p < 0) code = 2;
    }
    if (execP[1] >= 0) close(execP[1]);
    for (int i = 0; i < 3; i++) {
        if (sv[i][1] >= 0) close(sv[i][1]);
    }
    if (!code) {
        int execErr = 0;
        ssize_t got = read(execP[0], &execErr, sizeof execErr);
        if (got == (ssize_t)sizeof execErr) {
            waitpid(p, NULL, 0);
            code = execErr == ENOENT ? 1 : 2;
        }
    }
    if (execP[0] >= 0) close(execP[0]);
    if (!code) {
        *pid = p;
        *handle = -1;
#ifdef __linux__
        int h = (int)syscall(434 /* pidfd_open */, p, 0);
        if (h >= 0) {
            fcntl(h, F_SETFD, FD_CLOEXEC);
            *handle = h;
        }
#endif
        for (int i = 0; i < 3; i++) {
            io[i] = -1;
            if (pipes) {
                fcntl(sv[i][0], F_SETFD, FD_CLOEXEC);
                nonBlocking(sv[i][0]);
                fdForget(sv[i][0]);
                io[i] = sv[i][0];
            }
        }
    } else {
        for (int i = 0; i < 3; i++) {
            if (sv[i][0] >= 0) close(sv[i][0]);
        }
    }
    for (int64_t i = 0; i < have; i++) free(envp[i]);
    free(envp);
    for (int64_t i = 0; i < argc; i++) free(args[i]);
    free(args);
    free(prog);
    free(wd);
    return code;
}

/* ---------------------------------------------------------------- passwords (crypt)

   crypt_r from libcrypt, loaded with dlopen, checks a password against a
   hash in the format of /etc/shadow ($6$... and the rest). The runtime
   doesn't link libcrypt otherwise. */

#include <dlfcn.h>
void iris_wipe(void *p, int64_t n);
int iris_const_equal(const char *a, int64_t na, const char *b, int64_t nb);

struct cryptData {               /* the start of struct crypt_data */
    char out[256];
    char setting[256];
    char input[256];
    char rest[32768];
};

static char *(*cryptFn)(const char *, const char *, struct cryptData *);
static int cryptTried;

static bool cryptReady(void) {
    if (!cryptTried) {
        cryptTried = 1;
        void *lib = dlopen("libcrypt.so.2", RTLD_NOW);
        if (!lib) lib = dlopen("libcrypt.so.1", RTLD_NOW);
        if (!lib) lib = dlopen("libcrypt.so", RTLD_NOW);
        if (lib) *(void **)&cryptFn = dlsym(lib, "crypt_r");
    }
    return cryptFn != NULL;
}

/* 1 when password matches the crypt hash, 0 when it doesn't, -1 when the
   hash's method isn't available (no libcrypt, or an unknown prefix). The
   compare is in constant time once the hash is made. */
int iris_crypt_check(const char *password, int64_t plen, const char *hash, int64_t hlen) {
    if (!cryptReady() || hlen <= 0 || hlen >= 256 || plen >= 256) return -1;
    struct cryptData *d = calloc(1, sizeof *d);
    if (!d) outOfMemory();
    memcpy(d->setting, hash, (size_t)hlen);
    char pw[256];
    memcpy(pw, password, (size_t)plen);
    pw[plen] = 0;
    char *got = cryptFn(pw, d->setting, d);
    int result = -1;
    if (got && got[0] && got[0] != '*') {
        result = iris_const_equal(got, (int64_t)strlen(got), hash, hlen) ? 1 : 0;
    }
    iris_wipe(pw, sizeof pw);
    iris_wipe(d, sizeof *d);
    free(d);
    return result;
}

/* Hashes password with setting (a prefix like "$6$salt" or a whole hash):
   the hash into out (room for 256) and *len, or -1 when it can't. */
int iris_crypt_hash(const char *password, int64_t plen, const char *setting, int64_t slen, char *out, int64_t *len) {
    if (!cryptReady() || slen <= 0 || slen >= 256 || plen >= 256) return -1;
    struct cryptData *d = calloc(1, sizeof *d);
    if (!d) outOfMemory();
    memcpy(d->setting, setting, (size_t)slen);
    char pw[256];
    memcpy(pw, password, (size_t)plen);
    pw[plen] = 0;
    char *got = cryptFn(pw, d->setting, d);
    int code = -1;
    if (got && got[0] && got[0] != '*') {
        int64_t n = (int64_t)strlen(got);
        if (n < 256) {
            memcpy(out, got, (size_t)n);
            *len = n;
            code = 0;
        }
    }
    iris_wipe(pw, sizeof pw);
    iris_wipe(d, sizeof *d);
    free(d);
    return code;
}

/* Reads user's hash from /etc/shadow into out (room for 256): 0 and sets
   *len, 1 for no such user, 2 when it can't be read (not root). */
int iris_shadow_hash(const char *user, int64_t ulen, char *out, int64_t *len) {
    FILE *f = fopen("/etc/shadow", "r");
    if (!f) return 2;
    char line[4096];
    int code = 1;
    while (fgets(line, sizeof line, f)) {
        char *colon = strchr(line, ':');
        if (!colon) continue;
        if ((int64_t)(colon - line) == ulen && memcmp(line, user, (size_t)ulen) == 0) {
            char *end = strchr(colon + 1, ':');
            int64_t n = end ? (int64_t)(end - colon - 1) : (int64_t)strlen(colon + 1);
            if (n > 0 && n < 256) {
                memcpy(out, colon + 1, (size_t)n);
                *len = n;
                code = 0;
            } else {
                code = 1;       /* no password set, or locked (* or !) */
            }
            break;
        }
    }
    iris_wipe(line, sizeof line);
    fclose(f);
    return code;
}

/* ---------------------------------------------------------------- limits and sandboxes/* ---------------------------------------------------------------- limits and sandboxes

   os.chroot, os.setLimit, os.noNewPrivileges, and os.allowSyscalls: a
   seccomp filter (Linux) that lets the system calls named through, with
   those the runtime itself makes, and fails the rest with EPERM or ends
   the process. The numbers come from this system's headers. */

#include <sys/resource.h>

extern int chroot(const char *);
int iris_os_chroot(const char *path, int64_t len) {
    char *p = cpath(path, len);
    if (!p) return ENOENT;
    int r = chroot(p);
    int e = errno;
    free(p);
    if (r != 0) return e;
    return chdir("/") == 0 ? 0 : errno;
}

static const int limitIds[] = {RLIMIT_NOFILE, RLIMIT_NPROC, RLIMIT_FSIZE, RLIMIT_AS, RLIMIT_CORE, RLIMIT_CPU,
                               RLIMIT_STACK, RLIMIT_DATA, RLIMIT_MEMLOCK};

/* -1 for no limit. 0, or errno. */
int iris_os_set_limit(int which, int64_t soft, int64_t hard) {
    if (which < 0 || which >= (int)(sizeof limitIds / sizeof limitIds[0])) return EINVAL;
    struct rlimit r;
    r.rlim_cur = soft < 0 ? RLIM_INFINITY : (rlim_t)soft;
    r.rlim_max = hard < 0 ? RLIM_INFINITY : (rlim_t)hard;
    return setrlimit(limitIds[which], &r) == 0 ? 0 : errno;
}

int iris_os_get_limit(int which, int64_t *soft, int64_t *hard) {
    if (which < 0 || which >= (int)(sizeof limitIds / sizeof limitIds[0])) return EINVAL;
    struct rlimit r;
    if (getrlimit(limitIds[which], &r) != 0) return errno;
    *soft = r.rlim_cur == RLIM_INFINITY ? -1 : (int64_t)r.rlim_cur;
    *hard = r.rlim_max == RLIM_INFINITY ? -1 : (int64_t)r.rlim_max;
    return 0;
}

int iris_os_no_new_privs(void) {
#ifdef __linux__
    return prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 ? 0 : errno;
#else
    return -1;
#endif
}

#ifdef __linux__
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/audit.h>

static const struct {
    const char *name;
    int nr;
} sysNames[] = {
#ifdef SYS_accept
    {"accept", SYS_accept},
#endif
#ifdef SYS_accept4
    {"accept4", SYS_accept4},
#endif
#ifdef SYS_access
    {"access", SYS_access},
#endif
#ifdef SYS_acct
    {"acct", SYS_acct},
#endif
#ifdef SYS_add_key
    {"add_key", SYS_add_key},
#endif
#ifdef SYS_adjtimex
    {"adjtimex", SYS_adjtimex},
#endif
#ifdef SYS_afs_syscall
    {"afs_syscall", SYS_afs_syscall},
#endif
#ifdef SYS_alarm
    {"alarm", SYS_alarm},
#endif
#ifdef SYS_arch_prctl
    {"arch_prctl", SYS_arch_prctl},
#endif
#ifdef SYS_bind
    {"bind", SYS_bind},
#endif
#ifdef SYS_bpf
    {"bpf", SYS_bpf},
#endif
#ifdef SYS_brk
    {"brk", SYS_brk},
#endif
#ifdef SYS_cachestat
    {"cachestat", SYS_cachestat},
#endif
#ifdef SYS_capget
    {"capget", SYS_capget},
#endif
#ifdef SYS_capset
    {"capset", SYS_capset},
#endif
#ifdef SYS_chdir
    {"chdir", SYS_chdir},
#endif
#ifdef SYS_chmod
    {"chmod", SYS_chmod},
#endif
#ifdef SYS_chown
    {"chown", SYS_chown},
#endif
#ifdef SYS_chroot
    {"chroot", SYS_chroot},
#endif
#ifdef SYS_clock_adjtime
    {"clock_adjtime", SYS_clock_adjtime},
#endif
#ifdef SYS_clock_getres
    {"clock_getres", SYS_clock_getres},
#endif
#ifdef SYS_clock_gettime
    {"clock_gettime", SYS_clock_gettime},
#endif
#ifdef SYS_clock_nanosleep
    {"clock_nanosleep", SYS_clock_nanosleep},
#endif
#ifdef SYS_clock_settime
    {"clock_settime", SYS_clock_settime},
#endif
#ifdef SYS_clone
    {"clone", SYS_clone},
#endif
#ifdef SYS_clone3
    {"clone3", SYS_clone3},
#endif
#ifdef SYS_close
    {"close", SYS_close},
#endif
#ifdef SYS_close_range
    {"close_range", SYS_close_range},
#endif
#ifdef SYS_connect
    {"connect", SYS_connect},
#endif
#ifdef SYS_copy_file_range
    {"copy_file_range", SYS_copy_file_range},
#endif
#ifdef SYS_creat
    {"creat", SYS_creat},
#endif
#ifdef SYS_create_module
    {"create_module", SYS_create_module},
#endif
#ifdef SYS_delete_module
    {"delete_module", SYS_delete_module},
#endif
#ifdef SYS_dup
    {"dup", SYS_dup},
#endif
#ifdef SYS_dup2
    {"dup2", SYS_dup2},
#endif
#ifdef SYS_dup3
    {"dup3", SYS_dup3},
#endif
#ifdef SYS_epoll_create
    {"epoll_create", SYS_epoll_create},
#endif
#ifdef SYS_epoll_create1
    {"epoll_create1", SYS_epoll_create1},
#endif
#ifdef SYS_epoll_ctl
    {"epoll_ctl", SYS_epoll_ctl},
#endif
#ifdef SYS_epoll_ctl_old
    {"epoll_ctl_old", SYS_epoll_ctl_old},
#endif
#ifdef SYS_epoll_pwait
    {"epoll_pwait", SYS_epoll_pwait},
#endif
#ifdef SYS_epoll_pwait2
    {"epoll_pwait2", SYS_epoll_pwait2},
#endif
#ifdef SYS_epoll_wait
    {"epoll_wait", SYS_epoll_wait},
#endif
#ifdef SYS_epoll_wait_old
    {"epoll_wait_old", SYS_epoll_wait_old},
#endif
#ifdef SYS_eventfd
    {"eventfd", SYS_eventfd},
#endif
#ifdef SYS_eventfd2
    {"eventfd2", SYS_eventfd2},
#endif
#ifdef SYS_execve
    {"execve", SYS_execve},
#endif
#ifdef SYS_execveat
    {"execveat", SYS_execveat},
#endif
#ifdef SYS_exit
    {"exit", SYS_exit},
#endif
#ifdef SYS_exit_group
    {"exit_group", SYS_exit_group},
#endif
#ifdef SYS_faccessat
    {"faccessat", SYS_faccessat},
#endif
#ifdef SYS_faccessat2
    {"faccessat2", SYS_faccessat2},
#endif
#ifdef SYS_fadvise64
    {"fadvise64", SYS_fadvise64},
#endif
#ifdef SYS_fallocate
    {"fallocate", SYS_fallocate},
#endif
#ifdef SYS_fanotify_init
    {"fanotify_init", SYS_fanotify_init},
#endif
#ifdef SYS_fanotify_mark
    {"fanotify_mark", SYS_fanotify_mark},
#endif
#ifdef SYS_fchdir
    {"fchdir", SYS_fchdir},
#endif
#ifdef SYS_fchmod
    {"fchmod", SYS_fchmod},
#endif
#ifdef SYS_fchmodat
    {"fchmodat", SYS_fchmodat},
#endif
#ifdef SYS_fchmodat2
    {"fchmodat2", SYS_fchmodat2},
#endif
#ifdef SYS_fchown
    {"fchown", SYS_fchown},
#endif
#ifdef SYS_fchownat
    {"fchownat", SYS_fchownat},
#endif
#ifdef SYS_fcntl
    {"fcntl", SYS_fcntl},
#endif
#ifdef SYS_fdatasync
    {"fdatasync", SYS_fdatasync},
#endif
#ifdef SYS_fgetxattr
    {"fgetxattr", SYS_fgetxattr},
#endif
#ifdef SYS_finit_module
    {"finit_module", SYS_finit_module},
#endif
#ifdef SYS_flistxattr
    {"flistxattr", SYS_flistxattr},
#endif
#ifdef SYS_flock
    {"flock", SYS_flock},
#endif
#ifdef SYS_fork
    {"fork", SYS_fork},
#endif
#ifdef SYS_fremovexattr
    {"fremovexattr", SYS_fremovexattr},
#endif
#ifdef SYS_fsconfig
    {"fsconfig", SYS_fsconfig},
#endif
#ifdef SYS_fsetxattr
    {"fsetxattr", SYS_fsetxattr},
#endif
#ifdef SYS_fsmount
    {"fsmount", SYS_fsmount},
#endif
#ifdef SYS_fsopen
    {"fsopen", SYS_fsopen},
#endif
#ifdef SYS_fspick
    {"fspick", SYS_fspick},
#endif
#ifdef SYS_fstat
    {"fstat", SYS_fstat},
#endif
#ifdef SYS_fstatfs
    {"fstatfs", SYS_fstatfs},
#endif
#ifdef SYS_fsync
    {"fsync", SYS_fsync},
#endif
#ifdef SYS_ftruncate
    {"ftruncate", SYS_ftruncate},
#endif
#ifdef SYS_futex
    {"futex", SYS_futex},
#endif
#ifdef SYS_futex_requeue
    {"futex_requeue", SYS_futex_requeue},
#endif
#ifdef SYS_futex_wait
    {"futex_wait", SYS_futex_wait},
#endif
#ifdef SYS_futex_waitv
    {"futex_waitv", SYS_futex_waitv},
#endif
#ifdef SYS_futex_wake
    {"futex_wake", SYS_futex_wake},
#endif
#ifdef SYS_futimesat
    {"futimesat", SYS_futimesat},
#endif
#ifdef SYS_getcpu
    {"getcpu", SYS_getcpu},
#endif
#ifdef SYS_getcwd
    {"getcwd", SYS_getcwd},
#endif
#ifdef SYS_getdents
    {"getdents", SYS_getdents},
#endif
#ifdef SYS_getdents64
    {"getdents64", SYS_getdents64},
#endif
#ifdef SYS_getegid
    {"getegid", SYS_getegid},
#endif
#ifdef SYS_geteuid
    {"geteuid", SYS_geteuid},
#endif
#ifdef SYS_getgid
    {"getgid", SYS_getgid},
#endif
#ifdef SYS_getgroups
    {"getgroups", SYS_getgroups},
#endif
#ifdef SYS_getitimer
    {"getitimer", SYS_getitimer},
#endif
#ifdef SYS_get_kernel_syms
    {"get_kernel_syms", SYS_get_kernel_syms},
#endif
#ifdef SYS_get_mempolicy
    {"get_mempolicy", SYS_get_mempolicy},
#endif
#ifdef SYS_getpeername
    {"getpeername", SYS_getpeername},
#endif
#ifdef SYS_getpgid
    {"getpgid", SYS_getpgid},
#endif
#ifdef SYS_getpgrp
    {"getpgrp", SYS_getpgrp},
#endif
#ifdef SYS_getpid
    {"getpid", SYS_getpid},
#endif
#ifdef SYS_getpmsg
    {"getpmsg", SYS_getpmsg},
#endif
#ifdef SYS_getppid
    {"getppid", SYS_getppid},
#endif
#ifdef SYS_getpriority
    {"getpriority", SYS_getpriority},
#endif
#ifdef SYS_getrandom
    {"getrandom", SYS_getrandom},
#endif
#ifdef SYS_getresgid
    {"getresgid", SYS_getresgid},
#endif
#ifdef SYS_getresuid
    {"getresuid", SYS_getresuid},
#endif
#ifdef SYS_getrlimit
    {"getrlimit", SYS_getrlimit},
#endif
#ifdef SYS_get_robust_list
    {"get_robust_list", SYS_get_robust_list},
#endif
#ifdef SYS_getrusage
    {"getrusage", SYS_getrusage},
#endif
#ifdef SYS_getsid
    {"getsid", SYS_getsid},
#endif
#ifdef SYS_getsockname
    {"getsockname", SYS_getsockname},
#endif
#ifdef SYS_getsockopt
    {"getsockopt", SYS_getsockopt},
#endif
#ifdef SYS_get_thread_area
    {"get_thread_area", SYS_get_thread_area},
#endif
#ifdef SYS_gettid
    {"gettid", SYS_gettid},
#endif
#ifdef SYS_gettimeofday
    {"gettimeofday", SYS_gettimeofday},
#endif
#ifdef SYS_getuid
    {"getuid", SYS_getuid},
#endif
#ifdef SYS_getxattr
    {"getxattr", SYS_getxattr},
#endif
#ifdef SYS_init_module
    {"init_module", SYS_init_module},
#endif
#ifdef SYS_inotify_add_watch
    {"inotify_add_watch", SYS_inotify_add_watch},
#endif
#ifdef SYS_inotify_init
    {"inotify_init", SYS_inotify_init},
#endif
#ifdef SYS_inotify_init1
    {"inotify_init1", SYS_inotify_init1},
#endif
#ifdef SYS_inotify_rm_watch
    {"inotify_rm_watch", SYS_inotify_rm_watch},
#endif
#ifdef SYS_io_cancel
    {"io_cancel", SYS_io_cancel},
#endif
#ifdef SYS_ioctl
    {"ioctl", SYS_ioctl},
#endif
#ifdef SYS_io_destroy
    {"io_destroy", SYS_io_destroy},
#endif
#ifdef SYS_io_getevents
    {"io_getevents", SYS_io_getevents},
#endif
#ifdef SYS_ioperm
    {"ioperm", SYS_ioperm},
#endif
#ifdef SYS_io_pgetevents
    {"io_pgetevents", SYS_io_pgetevents},
#endif
#ifdef SYS_iopl
    {"iopl", SYS_iopl},
#endif
#ifdef SYS_ioprio_get
    {"ioprio_get", SYS_ioprio_get},
#endif
#ifdef SYS_ioprio_set
    {"ioprio_set", SYS_ioprio_set},
#endif
#ifdef SYS_io_setup
    {"io_setup", SYS_io_setup},
#endif
#ifdef SYS_io_submit
    {"io_submit", SYS_io_submit},
#endif
#ifdef SYS_io_uring_enter
    {"io_uring_enter", SYS_io_uring_enter},
#endif
#ifdef SYS_io_uring_register
    {"io_uring_register", SYS_io_uring_register},
#endif
#ifdef SYS_io_uring_setup
    {"io_uring_setup", SYS_io_uring_setup},
#endif
#ifdef SYS_kcmp
    {"kcmp", SYS_kcmp},
#endif
#ifdef SYS_kexec_file_load
    {"kexec_file_load", SYS_kexec_file_load},
#endif
#ifdef SYS_kexec_load
    {"kexec_load", SYS_kexec_load},
#endif
#ifdef SYS_keyctl
    {"keyctl", SYS_keyctl},
#endif
#ifdef SYS_kill
    {"kill", SYS_kill},
#endif
#ifdef SYS_landlock_add_rule
    {"landlock_add_rule", SYS_landlock_add_rule},
#endif
#ifdef SYS_landlock_create_ruleset
    {"landlock_create_ruleset", SYS_landlock_create_ruleset},
#endif
#ifdef SYS_landlock_restrict_self
    {"landlock_restrict_self", SYS_landlock_restrict_self},
#endif
#ifdef SYS_lchown
    {"lchown", SYS_lchown},
#endif
#ifdef SYS_lgetxattr
    {"lgetxattr", SYS_lgetxattr},
#endif
#ifdef SYS_link
    {"link", SYS_link},
#endif
#ifdef SYS_linkat
    {"linkat", SYS_linkat},
#endif
#ifdef SYS_listen
    {"listen", SYS_listen},
#endif
#ifdef SYS_listmount
    {"listmount", SYS_listmount},
#endif
#ifdef SYS_listxattr
    {"listxattr", SYS_listxattr},
#endif
#ifdef SYS_llistxattr
    {"llistxattr", SYS_llistxattr},
#endif
#ifdef SYS_lookup_dcookie
    {"lookup_dcookie", SYS_lookup_dcookie},
#endif
#ifdef SYS_lremovexattr
    {"lremovexattr", SYS_lremovexattr},
#endif
#ifdef SYS_lseek
    {"lseek", SYS_lseek},
#endif
#ifdef SYS_lsetxattr
    {"lsetxattr", SYS_lsetxattr},
#endif
#ifdef SYS_lsm_get_self_attr
    {"lsm_get_self_attr", SYS_lsm_get_self_attr},
#endif
#ifdef SYS_lsm_list_modules
    {"lsm_list_modules", SYS_lsm_list_modules},
#endif
#ifdef SYS_lsm_set_self_attr
    {"lsm_set_self_attr", SYS_lsm_set_self_attr},
#endif
#ifdef SYS_lstat
    {"lstat", SYS_lstat},
#endif
#ifdef SYS_madvise
    {"madvise", SYS_madvise},
#endif
#ifdef SYS_map_shadow_stack
    {"map_shadow_stack", SYS_map_shadow_stack},
#endif
#ifdef SYS_mbind
    {"mbind", SYS_mbind},
#endif
#ifdef SYS_membarrier
    {"membarrier", SYS_membarrier},
#endif
#ifdef SYS_memfd_create
    {"memfd_create", SYS_memfd_create},
#endif
#ifdef SYS_memfd_secret
    {"memfd_secret", SYS_memfd_secret},
#endif
#ifdef SYS_migrate_pages
    {"migrate_pages", SYS_migrate_pages},
#endif
#ifdef SYS_mincore
    {"mincore", SYS_mincore},
#endif
#ifdef SYS_mkdir
    {"mkdir", SYS_mkdir},
#endif
#ifdef SYS_mkdirat
    {"mkdirat", SYS_mkdirat},
#endif
#ifdef SYS_mknod
    {"mknod", SYS_mknod},
#endif
#ifdef SYS_mknodat
    {"mknodat", SYS_mknodat},
#endif
#ifdef SYS_mlock
    {"mlock", SYS_mlock},
#endif
#ifdef SYS_mlock2
    {"mlock2", SYS_mlock2},
#endif
#ifdef SYS_mlockall
    {"mlockall", SYS_mlockall},
#endif
#ifdef SYS_mmap
    {"mmap", SYS_mmap},
#endif
#ifdef SYS_modify_ldt
    {"modify_ldt", SYS_modify_ldt},
#endif
#ifdef SYS_mount
    {"mount", SYS_mount},
#endif
#ifdef SYS_mount_setattr
    {"mount_setattr", SYS_mount_setattr},
#endif
#ifdef SYS_move_mount
    {"move_mount", SYS_move_mount},
#endif
#ifdef SYS_move_pages
    {"move_pages", SYS_move_pages},
#endif
#ifdef SYS_mprotect
    {"mprotect", SYS_mprotect},
#endif
#ifdef SYS_mq_getsetattr
    {"mq_getsetattr", SYS_mq_getsetattr},
#endif
#ifdef SYS_mq_notify
    {"mq_notify", SYS_mq_notify},
#endif
#ifdef SYS_mq_open
    {"mq_open", SYS_mq_open},
#endif
#ifdef SYS_mq_timedreceive
    {"mq_timedreceive", SYS_mq_timedreceive},
#endif
#ifdef SYS_mq_timedsend
    {"mq_timedsend", SYS_mq_timedsend},
#endif
#ifdef SYS_mq_unlink
    {"mq_unlink", SYS_mq_unlink},
#endif
#ifdef SYS_mremap
    {"mremap", SYS_mremap},
#endif
#ifdef SYS_mseal
    {"mseal", SYS_mseal},
#endif
#ifdef SYS_msgctl
    {"msgctl", SYS_msgctl},
#endif
#ifdef SYS_msgget
    {"msgget", SYS_msgget},
#endif
#ifdef SYS_msgrcv
    {"msgrcv", SYS_msgrcv},
#endif
#ifdef SYS_msgsnd
    {"msgsnd", SYS_msgsnd},
#endif
#ifdef SYS_msync
    {"msync", SYS_msync},
#endif
#ifdef SYS_munlock
    {"munlock", SYS_munlock},
#endif
#ifdef SYS_munlockall
    {"munlockall", SYS_munlockall},
#endif
#ifdef SYS_munmap
    {"munmap", SYS_munmap},
#endif
#ifdef SYS_name_to_handle_at
    {"name_to_handle_at", SYS_name_to_handle_at},
#endif
#ifdef SYS_nanosleep
    {"nanosleep", SYS_nanosleep},
#endif
#ifdef SYS_newfstatat
    {"newfstatat", SYS_newfstatat},
#endif
#ifdef SYS_nfsservctl
    {"nfsservctl", SYS_nfsservctl},
#endif
#ifdef SYS_open
    {"open", SYS_open},
#endif
#ifdef SYS_openat
    {"openat", SYS_openat},
#endif
#ifdef SYS_openat2
    {"openat2", SYS_openat2},
#endif
#ifdef SYS_open_by_handle_at
    {"open_by_handle_at", SYS_open_by_handle_at},
#endif
#ifdef SYS_open_tree
    {"open_tree", SYS_open_tree},
#endif
#ifdef SYS_pause
    {"pause", SYS_pause},
#endif
#ifdef SYS_perf_event_open
    {"perf_event_open", SYS_perf_event_open},
#endif
#ifdef SYS_personality
    {"personality", SYS_personality},
#endif
#ifdef SYS_pidfd_getfd
    {"pidfd_getfd", SYS_pidfd_getfd},
#endif
#ifdef SYS_pidfd_open
    {"pidfd_open", SYS_pidfd_open},
#endif
#ifdef SYS_pidfd_send_signal
    {"pidfd_send_signal", SYS_pidfd_send_signal},
#endif
#ifdef SYS_pipe
    {"pipe", SYS_pipe},
#endif
#ifdef SYS_pipe2
    {"pipe2", SYS_pipe2},
#endif
#ifdef SYS_pivot_root
    {"pivot_root", SYS_pivot_root},
#endif
#ifdef SYS_pkey_alloc
    {"pkey_alloc", SYS_pkey_alloc},
#endif
#ifdef SYS_pkey_free
    {"pkey_free", SYS_pkey_free},
#endif
#ifdef SYS_pkey_mprotect
    {"pkey_mprotect", SYS_pkey_mprotect},
#endif
#ifdef SYS_poll
    {"poll", SYS_poll},
#endif
#ifdef SYS_ppoll
    {"ppoll", SYS_ppoll},
#endif
#ifdef SYS_prctl
    {"prctl", SYS_prctl},
#endif
#ifdef SYS_pread64
    {"pread64", SYS_pread64},
#endif
#ifdef SYS_preadv
    {"preadv", SYS_preadv},
#endif
#ifdef SYS_preadv2
    {"preadv2", SYS_preadv2},
#endif
#ifdef SYS_prlimit64
    {"prlimit64", SYS_prlimit64},
#endif
#ifdef SYS_process_madvise
    {"process_madvise", SYS_process_madvise},
#endif
#ifdef SYS_process_mrelease
    {"process_mrelease", SYS_process_mrelease},
#endif
#ifdef SYS_process_vm_readv
    {"process_vm_readv", SYS_process_vm_readv},
#endif
#ifdef SYS_process_vm_writev
    {"process_vm_writev", SYS_process_vm_writev},
#endif
#ifdef SYS_pselect6
    {"pselect6", SYS_pselect6},
#endif
#ifdef SYS_ptrace
    {"ptrace", SYS_ptrace},
#endif
#ifdef SYS_putpmsg
    {"putpmsg", SYS_putpmsg},
#endif
#ifdef SYS_pwrite64
    {"pwrite64", SYS_pwrite64},
#endif
#ifdef SYS_pwritev
    {"pwritev", SYS_pwritev},
#endif
#ifdef SYS_pwritev2
    {"pwritev2", SYS_pwritev2},
#endif
#ifdef SYS_query_module
    {"query_module", SYS_query_module},
#endif
#ifdef SYS_quotactl
    {"quotactl", SYS_quotactl},
#endif
#ifdef SYS_quotactl_fd
    {"quotactl_fd", SYS_quotactl_fd},
#endif
#ifdef SYS_read
    {"read", SYS_read},
#endif
#ifdef SYS_readahead
    {"readahead", SYS_readahead},
#endif
#ifdef SYS_readlink
    {"readlink", SYS_readlink},
#endif
#ifdef SYS_readlinkat
    {"readlinkat", SYS_readlinkat},
#endif
#ifdef SYS_readv
    {"readv", SYS_readv},
#endif
#ifdef SYS_reboot
    {"reboot", SYS_reboot},
#endif
#ifdef SYS_recvfrom
    {"recvfrom", SYS_recvfrom},
#endif
#ifdef SYS_recvmmsg
    {"recvmmsg", SYS_recvmmsg},
#endif
#ifdef SYS_recvmsg
    {"recvmsg", SYS_recvmsg},
#endif
#ifdef SYS_remap_file_pages
    {"remap_file_pages", SYS_remap_file_pages},
#endif
#ifdef SYS_removexattr
    {"removexattr", SYS_removexattr},
#endif
#ifdef SYS_rename
    {"rename", SYS_rename},
#endif
#ifdef SYS_renameat
    {"renameat", SYS_renameat},
#endif
#ifdef SYS_renameat2
    {"renameat2", SYS_renameat2},
#endif
#ifdef SYS_request_key
    {"request_key", SYS_request_key},
#endif
#ifdef SYS_restart_syscall
    {"restart_syscall", SYS_restart_syscall},
#endif
#ifdef SYS_rmdir
    {"rmdir", SYS_rmdir},
#endif
#ifdef SYS_rseq
    {"rseq", SYS_rseq},
#endif
#ifdef SYS_rt_sigaction
    {"rt_sigaction", SYS_rt_sigaction},
#endif
#ifdef SYS_rt_sigpending
    {"rt_sigpending", SYS_rt_sigpending},
#endif
#ifdef SYS_rt_sigprocmask
    {"rt_sigprocmask", SYS_rt_sigprocmask},
#endif
#ifdef SYS_rt_sigqueueinfo
    {"rt_sigqueueinfo", SYS_rt_sigqueueinfo},
#endif
#ifdef SYS_rt_sigreturn
    {"rt_sigreturn", SYS_rt_sigreturn},
#endif
#ifdef SYS_rt_sigsuspend
    {"rt_sigsuspend", SYS_rt_sigsuspend},
#endif
#ifdef SYS_rt_sigtimedwait
    {"rt_sigtimedwait", SYS_rt_sigtimedwait},
#endif
#ifdef SYS_rt_tgsigqueueinfo
    {"rt_tgsigqueueinfo", SYS_rt_tgsigqueueinfo},
#endif
#ifdef SYS_sched_getaffinity
    {"sched_getaffinity", SYS_sched_getaffinity},
#endif
#ifdef SYS_sched_getattr
    {"sched_getattr", SYS_sched_getattr},
#endif
#ifdef SYS_sched_getparam
    {"sched_getparam", SYS_sched_getparam},
#endif
#ifdef SYS_sched_get_priority_max
    {"sched_get_priority_max", SYS_sched_get_priority_max},
#endif
#ifdef SYS_sched_get_priority_min
    {"sched_get_priority_min", SYS_sched_get_priority_min},
#endif
#ifdef SYS_sched_getscheduler
    {"sched_getscheduler", SYS_sched_getscheduler},
#endif
#ifdef SYS_sched_rr_get_interval
    {"sched_rr_get_interval", SYS_sched_rr_get_interval},
#endif
#ifdef SYS_sched_setaffinity
    {"sched_setaffinity", SYS_sched_setaffinity},
#endif
#ifdef SYS_sched_setattr
    {"sched_setattr", SYS_sched_setattr},
#endif
#ifdef SYS_sched_setparam
    {"sched_setparam", SYS_sched_setparam},
#endif
#ifdef SYS_sched_setscheduler
    {"sched_setscheduler", SYS_sched_setscheduler},
#endif
#ifdef SYS_sched_yield
    {"sched_yield", SYS_sched_yield},
#endif
#ifdef SYS_seccomp
    {"seccomp", SYS_seccomp},
#endif
#ifdef SYS_security
    {"security", SYS_security},
#endif
#ifdef SYS_select
    {"select", SYS_select},
#endif
#ifdef SYS_semctl
    {"semctl", SYS_semctl},
#endif
#ifdef SYS_semget
    {"semget", SYS_semget},
#endif
#ifdef SYS_semop
    {"semop", SYS_semop},
#endif
#ifdef SYS_semtimedop
    {"semtimedop", SYS_semtimedop},
#endif
#ifdef SYS_sendfile
    {"sendfile", SYS_sendfile},
#endif
#ifdef SYS_sendmmsg
    {"sendmmsg", SYS_sendmmsg},
#endif
#ifdef SYS_sendmsg
    {"sendmsg", SYS_sendmsg},
#endif
#ifdef SYS_sendto
    {"sendto", SYS_sendto},
#endif
#ifdef SYS_setdomainname
    {"setdomainname", SYS_setdomainname},
#endif
#ifdef SYS_setfsgid
    {"setfsgid", SYS_setfsgid},
#endif
#ifdef SYS_setfsuid
    {"setfsuid", SYS_setfsuid},
#endif
#ifdef SYS_setgid
    {"setgid", SYS_setgid},
#endif
#ifdef SYS_setgroups
    {"setgroups", SYS_setgroups},
#endif
#ifdef SYS_sethostname
    {"sethostname", SYS_sethostname},
#endif
#ifdef SYS_setitimer
    {"setitimer", SYS_setitimer},
#endif
#ifdef SYS_set_mempolicy
    {"set_mempolicy", SYS_set_mempolicy},
#endif
#ifdef SYS_set_mempolicy_home_node
    {"set_mempolicy_home_node", SYS_set_mempolicy_home_node},
#endif
#ifdef SYS_setns
    {"setns", SYS_setns},
#endif
#ifdef SYS_setpgid
    {"setpgid", SYS_setpgid},
#endif
#ifdef SYS_setpriority
    {"setpriority", SYS_setpriority},
#endif
#ifdef SYS_setregid
    {"setregid", SYS_setregid},
#endif
#ifdef SYS_setresgid
    {"setresgid", SYS_setresgid},
#endif
#ifdef SYS_setresuid
    {"setresuid", SYS_setresuid},
#endif
#ifdef SYS_setreuid
    {"setreuid", SYS_setreuid},
#endif
#ifdef SYS_setrlimit
    {"setrlimit", SYS_setrlimit},
#endif
#ifdef SYS_set_robust_list
    {"set_robust_list", SYS_set_robust_list},
#endif
#ifdef SYS_setsid
    {"setsid", SYS_setsid},
#endif
#ifdef SYS_setsockopt
    {"setsockopt", SYS_setsockopt},
#endif
#ifdef SYS_set_thread_area
    {"set_thread_area", SYS_set_thread_area},
#endif
#ifdef SYS_set_tid_address
    {"set_tid_address", SYS_set_tid_address},
#endif
#ifdef SYS_settimeofday
    {"settimeofday", SYS_settimeofday},
#endif
#ifdef SYS_setuid
    {"setuid", SYS_setuid},
#endif
#ifdef SYS_setxattr
    {"setxattr", SYS_setxattr},
#endif
#ifdef SYS_shmat
    {"shmat", SYS_shmat},
#endif
#ifdef SYS_shmctl
    {"shmctl", SYS_shmctl},
#endif
#ifdef SYS_shmdt
    {"shmdt", SYS_shmdt},
#endif
#ifdef SYS_shmget
    {"shmget", SYS_shmget},
#endif
#ifdef SYS_shutdown
    {"shutdown", SYS_shutdown},
#endif
#ifdef SYS_sigaltstack
    {"sigaltstack", SYS_sigaltstack},
#endif
#ifdef SYS_signalfd
    {"signalfd", SYS_signalfd},
#endif
#ifdef SYS_signalfd4
    {"signalfd4", SYS_signalfd4},
#endif
#ifdef SYS_socket
    {"socket", SYS_socket},
#endif
#ifdef SYS_socketpair
    {"socketpair", SYS_socketpair},
#endif
#ifdef SYS_splice
    {"splice", SYS_splice},
#endif
#ifdef SYS_stat
    {"stat", SYS_stat},
#endif
#ifdef SYS_statfs
    {"statfs", SYS_statfs},
#endif
#ifdef SYS_statmount
    {"statmount", SYS_statmount},
#endif
#ifdef SYS_statx
    {"statx", SYS_statx},
#endif
#ifdef SYS_swapoff
    {"swapoff", SYS_swapoff},
#endif
#ifdef SYS_swapon
    {"swapon", SYS_swapon},
#endif
#ifdef SYS_symlink
    {"symlink", SYS_symlink},
#endif
#ifdef SYS_symlinkat
    {"symlinkat", SYS_symlinkat},
#endif
#ifdef SYS_sync
    {"sync", SYS_sync},
#endif
#ifdef SYS_sync_file_range
    {"sync_file_range", SYS_sync_file_range},
#endif
#ifdef SYS_syncfs
    {"syncfs", SYS_syncfs},
#endif
#ifdef SYS__sysctl
    {"_sysctl", SYS__sysctl},
#endif
#ifdef SYS_sysfs
    {"sysfs", SYS_sysfs},
#endif
#ifdef SYS_sysinfo
    {"sysinfo", SYS_sysinfo},
#endif
#ifdef SYS_syslog
    {"syslog", SYS_syslog},
#endif
#ifdef SYS_tee
    {"tee", SYS_tee},
#endif
#ifdef SYS_tgkill
    {"tgkill", SYS_tgkill},
#endif
#ifdef SYS_time
    {"time", SYS_time},
#endif
#ifdef SYS_timer_create
    {"timer_create", SYS_timer_create},
#endif
#ifdef SYS_timer_delete
    {"timer_delete", SYS_timer_delete},
#endif
#ifdef SYS_timerfd_create
    {"timerfd_create", SYS_timerfd_create},
#endif
#ifdef SYS_timerfd_gettime
    {"timerfd_gettime", SYS_timerfd_gettime},
#endif
#ifdef SYS_timerfd_settime
    {"timerfd_settime", SYS_timerfd_settime},
#endif
#ifdef SYS_timer_getoverrun
    {"timer_getoverrun", SYS_timer_getoverrun},
#endif
#ifdef SYS_timer_gettime
    {"timer_gettime", SYS_timer_gettime},
#endif
#ifdef SYS_timer_settime
    {"timer_settime", SYS_timer_settime},
#endif
#ifdef SYS_times
    {"times", SYS_times},
#endif
#ifdef SYS_tkill
    {"tkill", SYS_tkill},
#endif
#ifdef SYS_truncate
    {"truncate", SYS_truncate},
#endif
#ifdef SYS_tuxcall
    {"tuxcall", SYS_tuxcall},
#endif
#ifdef SYS_umask
    {"umask", SYS_umask},
#endif
#ifdef SYS_umount2
    {"umount2", SYS_umount2},
#endif
#ifdef SYS_uname
    {"uname", SYS_uname},
#endif
#ifdef SYS_unlink
    {"unlink", SYS_unlink},
#endif
#ifdef SYS_unlinkat
    {"unlinkat", SYS_unlinkat},
#endif
#ifdef SYS_unshare
    {"unshare", SYS_unshare},
#endif
#ifdef SYS_uretprobe
    {"uretprobe", SYS_uretprobe},
#endif
#ifdef SYS_uselib
    {"uselib", SYS_uselib},
#endif
#ifdef SYS_userfaultfd
    {"userfaultfd", SYS_userfaultfd},
#endif
#ifdef SYS_ustat
    {"ustat", SYS_ustat},
#endif
#ifdef SYS_utime
    {"utime", SYS_utime},
#endif
#ifdef SYS_utimensat
    {"utimensat", SYS_utimensat},
#endif
#ifdef SYS_utimes
    {"utimes", SYS_utimes},
#endif
#ifdef SYS_vfork
    {"vfork", SYS_vfork},
#endif
#ifdef SYS_vhangup
    {"vhangup", SYS_vhangup},
#endif
#ifdef SYS_vmsplice
    {"vmsplice", SYS_vmsplice},
#endif
#ifdef SYS_vserver
    {"vserver", SYS_vserver},
#endif
#ifdef SYS_wait4
    {"wait4", SYS_wait4},
#endif
#ifdef SYS_waitid
    {"waitid", SYS_waitid},
#endif
#ifdef SYS_write
    {"write", SYS_write},
#endif
#ifdef SYS_writev
    {"writev", SYS_writev},
#endif
};

/* What the runtime calls on its own: memory, threads, signals, time, the
   event loop, sockets it already has, and leaving. */
static const char *const runtimeCalls[] = {
    "read", "write", "readv", "writev", "pread64", "pwrite64", "close", "mmap", "munmap", "mremap", "mprotect",
    "madvise", "brk", "futex", "rt_sigreturn", "rt_sigprocmask", "rt_sigaction", "rt_sigtimedwait", "rt_sigpending",
    "sigaltstack", "exit", "exit_group", "clock_gettime", "clock_nanosleep", "nanosleep", "gettid", "getpid",
    "sched_yield", "sched_getaffinity", "io_uring_enter", "epoll_wait", "epoll_pwait", "epoll_ctl", "poll", "ppoll",
    "recvfrom", "sendto", "recvmsg", "sendmsg", "recvmmsg", "sendmmsg", "getrandom", "restart_syscall",
    "membarrier", "tgkill", "rseq", "set_robust_list", "fstat", "newfstatat", "lseek", "fcntl", "eventfd2",
    "getsockname", "getpeername", "getsockopt", "setsockopt", "shutdown", "splice", "dup", "uname", NULL};

static int sysNumber(const char *name, size_t len) {
    for (size_t i = 0; i < sizeof sysNames / sizeof sysNames[0]; i++) {
        if (strlen(sysNames[i].name) == len && memcmp(sysNames[i].name, name, len) == 0) return sysNames[i].nr;
    }
    return -1;
}
#endif

/* The filter: names holds the calls let through, each followed by a 0
   byte. 0, -1 off Linux, -2 with the first unknown name in bad, or errno. */
int iris_os_allow_syscalls(const char *names, int64_t len, int kill, IrisString *bad) {
#ifdef __linux__
    int nrs[1024];
    int n = 0;
    for (const char *const *r = runtimeCalls; *r; r++) {
        int nr = sysNumber(*r, strlen(*r));
        if (nr >= 0 && n < 1024) nrs[n++] = nr;
    }
    for (int64_t at = 0; at < len;) {
        const char *e = memchr(names + at, 0, (size_t)(len - at));
        size_t l = e ? (size_t)(e - (names + at)) : (size_t)(len - at);
        int nr = sysNumber(names + at, l);
        if (nr < 0) {
            iris_string_append(bad, names + at, (int64_t)l);
            return -2;
        }
        if (n < 1024) nrs[n++] = nr;
        at += (int64_t)l + 1;
    }
#if defined(__x86_64__)
    uint32_t arch = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
    uint32_t arch = AUDIT_ARCH_AARCH64;
#else
    return -1;
#endif
    struct sock_filter *f = calloc((size_t)(2 * n) + 8, sizeof *f);
    if (!f) outOfMemory();
    int k = 0;
    /* another architecture's calls would mean other numbers: ended */
    f[k++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch));
    f[k++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, arch, 1, 0);
    f[k++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
    f[k++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr));
    for (int i = 0; i < n; i++) {
        /* a match goes on to the allow after it, any other number skips it */
        f[k++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)nrs[i], 0, 1);
        f[k++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    }
    f[k++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, kill ? SECCOMP_RET_KILL_PROCESS : SECCOMP_RET_ERRNO | EPERM);
    struct sock_fprog prog = {(unsigned short)k, f};
    int r = 0;
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) r = errno;
    /* every thread of the program, not only this one */
    else if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, &prog) != 0) r = errno;
    free(f);
    return r;
#else
    (void)names;
    (void)len;
    (void)kill;
    (void)bad;
    return -1;
#endif
}

/* ---------------------------------------------------------------- TUN and TAP devices

   A TUN device carries IP packets between the kernel and this program, a
   TAP device Ethernet frames (Linux, /dev/net/tun; CAP_NET_ADMIN to make
   one). Reads and writes are one packet each, and wait in the event loop. */

#ifdef __linux__
#include <linux/if_tun.h>
#endif

/* The device's descriptor, its name in name; -1 with *err (errno, or -2
   off Linux). */
int iris_tun_open(const char *want, int64_t len, int tap, int multi, IrisString *name, int *err) {
#ifdef __linux__
    int fd = open("/dev/net/tun", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        *err = errno;
        return -1;
    }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    ifr.ifr_flags = (short)((tap ? IFF_TAP : IFF_TUN) | IFF_NO_PI | (multi ? IFF_MULTI_QUEUE : 0));
    if (len >= IFNAMSIZ) {
        close(fd);
        *err = ENAMETOOLONG;
        return -1;
    }
    memcpy(ifr.ifr_name, want, (size_t)len);
    if (ioctl(fd, TUNSETIFF, &ifr) != 0) {
        *err = errno;
        close(fd);
        return -1;
    }
    nonBlocking(fd);
    fdForget(fd);
    iris_string_append(name, ifr.ifr_name, (int64_t)strnlen(ifr.ifr_name, IFNAMSIZ));
    *err = 0;
    return fd;
#else
    (void)want;
    (void)len;
    (void)tap;
    (void)multi;
    (void)name;
    *err = -2;
    return -1;
#endif
}

/* One packet: its length, or -errno. */
int64_t iris_tun_read(int fd, char *buf, int64_t len, int64_t timeout) {
    int64_t deadline = deadlineOf(timeout);
    for (;;) {
        ssize_t n = read(fd, buf, (size_t)len);
        if (n >= 0) return n;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            if (!waitFdUntil(fd, 0, deadline)) return -ETIMEDOUT;
            continue;
        }
        return -errno;
    }
}

int iris_tun_write(int fd, const char *buf, int64_t len) {
    for (;;) {
        ssize_t n = write(fd, buf, (size_t)len);
        if (n >= 0) return 0;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            waitFd(fd, 1);
            continue;
        }
        return errno;
    }
}

/* ---------------------------------------------------------------- netlink (Linux)

   rtnetlink: the kernel's interfaces, addresses and routes, set with
   requests the kernel acknowledges, and read with dumps; events come to a
   socket bound to their groups. Each record the dumps and events give is
   a line of tab-separated fields:

     link   index name mtu up(0/1) running(0/1) mac
     addr   index family address/prefix
     route  table protocol dest/prefix gateway dev-index metric

   with "new" or "del" in front for an event. */

#ifdef __linux__
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if_arp.h>

static _Atomic uint32_t nlSeq = 1;

int iris_nl_open(uint32_t groups, int *err) {
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) {
        *err = errno;
        return -1;
    }
    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof sa);
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = groups;
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        *err = errno;
        close(fd);
        return -1;
    }
    int big = 1 << 20;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
    nonBlocking(fd);
    fdForget(fd);
    *err = 0;
    return fd;
}

static void nlAttr(char *buf, size_t *len, unsigned short type, const void *data, size_t n) {
    struct rtattr *a = (struct rtattr *)(buf + NLMSG_ALIGN(*len));
    a->rta_type = type;
    a->rta_len = (unsigned short)RTA_LENGTH(n);
    memcpy(RTA_DATA(a), data, n);
    *len = NLMSG_ALIGN(*len) + RTA_ALIGN(a->rta_len);
}

/* Reads until the answer to seq: 0, or the kernel's errno. */
static int nlWaitAck(int fd, uint32_t seq) {
    char buf[16384];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                waitFd(fd, 0);
                continue;
            }
            return errno;
        }
        for (struct nlmsghdr *h = (struct nlmsghdr *)buf; NLMSG_OK(h, (size_t)n); h = NLMSG_NEXT(h, n)) {
            if (h->nlmsg_seq != seq) continue;
            if (h->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *e = NLMSG_DATA(h);
                return -e->error;
            }
            if (h->nlmsg_type == NLMSG_DONE) return 0;
        }
    }
}

static int nlRequest(char *msg, size_t len) {
    int err;
    int fd = iris_nl_open(0, &err);
    if (fd < 0) return err;
    struct nlmsghdr *h = (struct nlmsghdr *)msg;
    h->nlmsg_len = (uint32_t)len;
    h->nlmsg_seq = nlSeq++;
    struct sockaddr_nl k;
    memset(&k, 0, sizeof k);
    k.nl_family = AF_NETLINK;
    int r = sendto(fd, msg, len, 0, (struct sockaddr *)&k, sizeof k) < 0 ? errno : nlWaitAck(fd, h->nlmsg_seq);
    fdForget(fd);
    close(fd);
    return r;
}

static int ifIndex(const char *name, int64_t len) {
    char n[IF_NAMESIZE + 1];
    if (len <= 0 || len > IF_NAMESIZE) return 0;
    memcpy(n, name, (size_t)len);
    n[len] = 0;
    return (int)if_nametoindex(n);
}

/* An interface up (1) or down (0) or as it is (-1), and its MTU when not
   -1: 0, errno, or -1 when there is no such interface. */
int iris_nl_link_set(const char *name, int64_t len, int up, int64_t mtu) {
    int index = ifIndex(name, len);
    if (!index) return -1;
    char msg[512];
    memset(msg, 0, sizeof msg);
    struct nlmsghdr *h = (struct nlmsghdr *)msg;
    h->nlmsg_type = RTM_NEWLINK;
    h->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    struct ifinfomsg *i = NLMSG_DATA(h);
    i->ifi_family = AF_UNSPEC;
    i->ifi_index = index;
    if (up >= 0) {
        i->ifi_change = IFF_UP;
        i->ifi_flags = up ? IFF_UP : 0;
    }
    size_t l = NLMSG_LENGTH(sizeof *i);
    if (mtu >= 0) {
        uint32_t m = (uint32_t)mtu;
        nlAttr(msg, &l, IFLA_MTU, &m, sizeof m);
    }
    return nlRequest(msg, l);
}

/* Adds (1) or removes (0) an address, ip with prefix bits, on an
   interface. */
int iris_nl_addr(int add, const char *name, int64_t len, const uint8_t *ip, int prefix) {
    int index = ifIndex(name, len);
    if (!index) return -1;
    bool v4 = isMapped(ip);
    char msg[512];
    memset(msg, 0, sizeof msg);
    struct nlmsghdr *h = (struct nlmsghdr *)msg;
    h->nlmsg_type = add ? RTM_NEWADDR : RTM_DELADDR;
    h->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | (add ? NLM_F_CREATE | NLM_F_EXCL : 0);
    struct ifaddrmsg *a = NLMSG_DATA(h);
    a->ifa_family = v4 ? AF_INET : AF_INET6;
    a->ifa_prefixlen = (unsigned char)prefix;
    a->ifa_index = (unsigned)index;
    size_t l = NLMSG_LENGTH(sizeof *a);
    nlAttr(msg, &l, IFA_LOCAL, v4 ? ip + 12 : ip, v4 ? 4 : 16);
    nlAttr(msg, &l, IFA_ADDRESS, v4 ? ip + 12 : ip, v4 ? 4 : 16);
    return nlRequest(msg, l);
}

/* Adds (1, replace when replace), or removes (0), a route to dest/prefix
   via gateway (NULL: none) out of the interface named (len 0: none). */
int iris_nl_route(int add, int replace, const uint8_t *dest, int prefix, const uint8_t *gateway, const char *name,
                  int64_t len, int table, int protocol, int64_t metric) {
    int index = len > 0 ? ifIndex(name, len) : 0;
    if (len > 0 && !index) return -1;
    bool v4 = isMapped(dest);
    char msg[512];
    memset(msg, 0, sizeof msg);
    struct nlmsghdr *h = (struct nlmsghdr *)msg;
    h->nlmsg_type = add ? RTM_NEWROUTE : RTM_DELROUTE;
    h->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | (add ? NLM_F_CREATE | (replace ? NLM_F_REPLACE : NLM_F_EXCL) : 0);
    struct rtmsg *r = NLMSG_DATA(h);
    r->rtm_family = v4 ? AF_INET : AF_INET6;
    r->rtm_dst_len = (unsigned char)prefix;
    r->rtm_table = table < 256 ? (unsigned char)table : RT_TABLE_UNSPEC;
    r->rtm_protocol = (unsigned char)(protocol ? protocol : RTPROT_STATIC);
    r->rtm_scope = gateway ? RT_SCOPE_UNIVERSE : RT_SCOPE_LINK;
    r->rtm_type = RTN_UNICAST;
    size_t l = NLMSG_LENGTH(sizeof *r);
    nlAttr(msg, &l, RTA_DST, v4 ? dest + 12 : dest, v4 ? 4 : 16);
    if (gateway) nlAttr(msg, &l, RTA_GATEWAY, v4 ? gateway + 12 : gateway, v4 ? 4 : 16);
    if (index) nlAttr(msg, &l, RTA_OIF, &index, sizeof index);
    if (table >= 256) {
        uint32_t t = (uint32_t)table;
        nlAttr(msg, &l, RTA_TABLE, &t, sizeof t);
    }
    if (metric >= 0) {
        uint32_t m = (uint32_t)metric;
        nlAttr(msg, &l, RTA_PRIORITY, &m, sizeof m);
    }
    return nlRequest(msg, l);
}

static void nlIp(IrisString *out, int family, const void *p) {
    char t[64];
    inet_ntop(family, p, t, sizeof t);
    iris_string_append(out, t, (int64_t)strlen(t));
}

static void nlNum(IrisString *out, int64_t v) {
    char t[32];
    int n = snprintf(t, sizeof t, "%lld", (long long)v);
    iris_string_append(out, t, n);
}

/* One message as a line, as the comment above has them; false for one
   that isn't a link, an address or a route. */
static bool nlLine(struct nlmsghdr *h, IrisString *out, bool event) {
    if (h->nlmsg_type == RTM_NEWLINK || h->nlmsg_type == RTM_DELLINK) {
        struct ifinfomsg *i = NLMSG_DATA(h);
        if (event) iris_string_append(out, h->nlmsg_type == RTM_NEWLINK ? "new\t" : "del\t", 4);
        iris_string_append(out, "link\t", 5);
        nlNum(out, i->ifi_index);
        const char *name = "";
        int64_t mtu = 0;
        char mac[32] = "";
        int len = (int)IFLA_PAYLOAD(h);
        for (struct rtattr *a = IFLA_RTA(i); RTA_OK(a, len); a = RTA_NEXT(a, len)) {
            if (a->rta_type == IFLA_IFNAME) name = RTA_DATA(a);
            else if (a->rta_type == IFLA_MTU) mtu = *(uint32_t *)RTA_DATA(a);
            else if (a->rta_type == IFLA_ADDRESS && RTA_PAYLOAD(a) == 6) {
                const unsigned char *m = RTA_DATA(a);
                snprintf(mac, sizeof mac, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
            }
        }
        iris_string_append(out, "\t", 1);
        iris_string_append(out, name, (int64_t)strlen(name));
        iris_string_append(out, "\t", 1);
        nlNum(out, mtu);
        iris_string_append(out, (i->ifi_flags & IFF_UP) ? "\t1" : "\t0", 2);
        iris_string_append(out, (i->ifi_flags & IFF_RUNNING) ? "\t1\t" : "\t0\t", 3);
        iris_string_append(out, mac, (int64_t)strlen(mac));
        iris_string_append(out, "\n", 1);
        return true;
    }
    if (h->nlmsg_type == RTM_NEWADDR || h->nlmsg_type == RTM_DELADDR) {
        struct ifaddrmsg *a = NLMSG_DATA(h);
        if (event) iris_string_append(out, h->nlmsg_type == RTM_NEWADDR ? "new\t" : "del\t", 4);
        iris_string_append(out, "addr\t", 5);
        nlNum(out, a->ifa_index);
        iris_string_append(out, a->ifa_family == AF_INET ? "\t4\t" : "\t6\t", 3);
        int len = (int)IFA_PAYLOAD(h);
        const void *ip = NULL;
        for (struct rtattr *r = IFA_RTA(a); RTA_OK(r, len); r = RTA_NEXT(r, len)) {
            if (r->rta_type == IFA_LOCAL) ip = RTA_DATA(r);
            else if (r->rta_type == IFA_ADDRESS && !ip) ip = RTA_DATA(r);
        }
        if (ip) nlIp(out, a->ifa_family, ip);
        iris_string_append(out, "/", 1);
        nlNum(out, a->ifa_prefixlen);
        iris_string_append(out, "\n", 1);
        return true;
    }
    if (h->nlmsg_type == RTM_NEWROUTE || h->nlmsg_type == RTM_DELROUTE) {
        struct rtmsg *r = NLMSG_DATA(h);
        if (r->rtm_family != AF_INET && r->rtm_family != AF_INET6) return false;
        if (event) iris_string_append(out, h->nlmsg_type == RTM_NEWROUTE ? "new\t" : "del\t", 4);
        iris_string_append(out, "route\t", 6);
        int64_t table = r->rtm_table, oif = 0, metric = 0;
        const void *dst = NULL, *gw = NULL;
        int len = (int)RTM_PAYLOAD(h);
        for (struct rtattr *a = RTM_RTA(r); RTA_OK(a, len); a = RTA_NEXT(a, len)) {
            if (a->rta_type == RTA_TABLE) table = *(uint32_t *)RTA_DATA(a);
            else if (a->rta_type == RTA_DST) dst = RTA_DATA(a);
            else if (a->rta_type == RTA_GATEWAY) gw = RTA_DATA(a);
            else if (a->rta_type == RTA_OIF) oif = *(int *)RTA_DATA(a);
            else if (a->rta_type == RTA_PRIORITY) metric = *(uint32_t *)RTA_DATA(a);
        }
        nlNum(out, table);
        iris_string_append(out, "\t", 1);
        nlNum(out, r->rtm_protocol);
        iris_string_append(out, "\t", 1);
        if (dst) {
            nlIp(out, r->rtm_family, dst);
        } else {
            iris_string_append(out, r->rtm_family == AF_INET ? "0.0.0.0" : "::", r->rtm_family == AF_INET ? 7 : 2);
        }
        iris_string_append(out, "/", 1);
        nlNum(out, r->rtm_dst_len);
        iris_string_append(out, "\t", 1);
        if (gw) nlIp(out, r->rtm_family, gw);
        iris_string_append(out, "\t", 1);
        nlNum(out, oif);
        iris_string_append(out, "\t", 1);
        nlNum(out, metric);
        iris_string_append(out, "\n", 1);
        return true;
    }
    return false;
}

/* A dump: 0 links, 1 addresses, 2 routes; a line each, added to out. */
int iris_nl_dump(int what, IrisString *out) {
    int err;
    int fd = iris_nl_open(0, &err);
    if (fd < 0) return err;
    struct {
        struct nlmsghdr h;
        union {
            struct ifinfomsg link;
            struct ifaddrmsg addr;
            struct rtmsg route;
        } body;
    } req;
    memset(&req, 0, sizeof req);
    req.h.nlmsg_type = what == 0 ? RTM_GETLINK : what == 1 ? RTM_GETADDR : RTM_GETROUTE;
    req.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.h.nlmsg_len = NLMSG_LENGTH(what == 0 ? sizeof req.body.link : what == 1 ? sizeof req.body.addr : sizeof req.body.route);
    req.h.nlmsg_seq = nlSeq++;
    struct sockaddr_nl k;
    memset(&k, 0, sizeof k);
    k.nl_family = AF_NETLINK;
    if (sendto(fd, &req, req.h.nlmsg_len, 0, (struct sockaddr *)&k, sizeof k) < 0) {
        err = errno;
        fdForget(fd);
        close(fd);
        return err;
    }
    char *buf = malloc(65536);
    if (!buf) outOfMemory();
    int code = 0;
    for (bool done = false; !done;) {
        ssize_t n = recv(fd, buf, 65536, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                waitFd(fd, 0);
                continue;
            }
            code = errno;
            break;
        }
        for (struct nlmsghdr *h = (struct nlmsghdr *)buf; NLMSG_OK(h, (size_t)n); h = NLMSG_NEXT(h, n)) {
            if (h->nlmsg_type == NLMSG_DONE) {
                done = true;
                break;
            }
            if (h->nlmsg_type == NLMSG_ERROR) {
                code = -((struct nlmsgerr *)NLMSG_DATA(h))->error;
                done = true;
                break;
            }
            nlLine(h, out, false);
        }
    }
    free(buf);
    fdForget(fd);
    close(fd);
    return code;
}

/* The next event on a socket bound to groups: its line in out. 0, 3 when
   the time ran out, or errno. */
int iris_nl_next(int fd, IrisString *out, IrisString *pending, int64_t timeout) {
    int64_t deadline = deadlineOf(timeout);
    for (;;) {
        if (pending->len > 0) {
            const char *nl = memchr(pending->ptr, '\n', (size_t)pending->len);
            int64_t k = nl ? (int64_t)(nl - pending->ptr) + 1 : pending->len;
            iris_string_append(out, pending->ptr, k - 1);
            memmove(pending->ptr, pending->ptr + k, (size_t)(pending->len - k));
            pending->len -= k;
            return 0;
        }
        char buf[65536];
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                if (!waitFdUntil(fd, 0, deadline)) return 3;
                continue;
            }
            return errno;
        }
        for (struct nlmsghdr *h = (struct nlmsghdr *)buf; NLMSG_OK(h, (size_t)n); h = NLMSG_NEXT(h, n)) {
            nlLine(h, pending, true);
        }
    }
}
#else
int iris_nl_open(uint32_t groups, int *err) {
    (void)groups;
    *err = -2;
    return -1;
}
int iris_nl_link_set(const char *name, int64_t len, int up, int64_t mtu) { return -2; }
int iris_nl_addr(int add, const char *name, int64_t len, const uint8_t *ip, int prefix) { return -2; }
int iris_nl_route(int add, int replace, const uint8_t *dest, int prefix, const uint8_t *gateway, const char *name,
                  int64_t len, int table, int protocol, int64_t metric) {
    return -2;
}
int iris_nl_dump(int what, IrisString *out) { return -2; }
int iris_nl_next(int fd, IrisString *out, IrisString *pending, int64_t timeout) { return -2; }
#endif

/* ---------------------------------------------------------------- watching folders

   fs.watch: inotify on Linux, waiting in the event loop; elsewhere scans of
   the tree compared every 200 ms. Each change is (kind, path): 1 created,
   2 removed, 3 changed. */

typedef struct {
    char *path;
    int64_t size, mtime;
    int isDir;
} WatchSeen;

typedef struct {
    char *root;
    int recursive;
#ifdef __linux__
    int fd;
    char buf[65536];
    int len, at;
    char **paths;               /* by watch descriptor */
    int npaths;
#else
    WatchSeen *seen;
    int64_t nseen;
#endif
    /* changes found and not given yet */
    int *kinds;
    char **queue;
    int *queueDir;
    int64_t qn, qcap, qat;
} Watcher;

static void watchPush(Watcher *w, int kind, const char *path, int isDir) {
    if (w->qn == w->qcap) {
        w->qcap = w->qcap ? w->qcap * 2 : 16;
        w->kinds = realloc(w->kinds, (size_t)w->qcap * sizeof(int));
        w->queue = realloc(w->queue, (size_t)w->qcap * sizeof(char *));
        w->queueDir = realloc(w->queueDir, (size_t)w->qcap * sizeof(int));
        if (!w->kinds || !w->queue || !w->queueDir) outOfMemory();
    }
    w->kinds[w->qn] = kind;
    w->queue[w->qn] = strdup(path);
    w->queueDir[w->qn] = isDir;
    w->qn++;
}

#ifdef __linux__
#include <sys/inotify.h>

static void watchAdd(Watcher *w, const char *path) {
    int wd = inotify_add_watch(w->fd, path, IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_FROM |
                                                IN_MOVED_TO | IN_ATTRIB | IN_DELETE_SELF | IN_ONLYDIR);
    if (wd < 0) return;
    if (wd >= w->npaths) {
        int n = wd + 16;
        w->paths = realloc(w->paths, (size_t)n * sizeof(char *));
        if (!w->paths) outOfMemory();
        for (int i = w->npaths; i < n; i++) w->paths[i] = NULL;
        w->npaths = n;
    }
    free(w->paths[wd]);
    w->paths[wd] = strdup(path);
    if (!w->recursive) return;
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2]))) continue;
        char sub[PATH_MAX];
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        struct stat st;
        if (lstat(sub, &st) == 0 && S_ISDIR(st.st_mode)) watchAdd(w, sub);
    }
    closedir(d);
}
#else
static int seenCmp(const void *a, const void *b) {
    return strcmp(((const WatchSeen *)a)->path, ((const WatchSeen *)b)->path);
}

static void scanTree(const char *path, int recursive, WatchSeen **out, int64_t *n, int64_t *cap) {
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2]))) continue;
        char sub[PATH_MAX];
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        struct stat st;
        if (lstat(sub, &st) != 0) continue;
        if (*n == *cap) {
            *cap = *cap ? *cap * 2 : 64;
            *out = realloc(*out, (size_t)*cap * sizeof(WatchSeen));
            if (!*out) outOfMemory();
        }
        WatchSeen *s = &(*out)[(*n)++];
        s->path = strdup(sub);
        s->size = st.st_size;
        s->mtime = (int64_t)st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
        s->isDir = S_ISDIR(st.st_mode);
        if (s->isDir && recursive) scanTree(sub, recursive, out, n, cap);
    }
    closedir(d);
}

static void scanSorted(Watcher *w, WatchSeen **out, int64_t *n) {
    int64_t cap = 0;
    *out = NULL;
    *n = 0;
    scanTree(w->root, w->recursive, out, n, &cap);
    if (*n) qsort(*out, (size_t)*n, sizeof(WatchSeen), seenCmp);
}

static void seenFree(WatchSeen *s, int64_t n) {
    for (int64_t i = 0; i < n; i++) free(s[i].path);
    free(s);
}
#endif

void *iris_watch_open(const char *path, int64_t len, int recursive, int *err) {
    char *root = cpath(path, len);
    struct stat st;
    if (!root || stat(root, &st) != 0) {
        *err = fsCode(errno);
        free(root);
        return NULL;
    }
    if (!S_ISDIR(st.st_mode)) {
        *err = 1;
        free(root);
        return NULL;
    }
    Watcher *w = calloc(1, sizeof *w);
    if (!w) outOfMemory();
    w->root = root;
    w->recursive = recursive;
#ifdef __linux__
    w->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (w->fd < 0) {
        *err = fsCode(errno);
        free(root);
        free(w);
        return NULL;
    }
    fdForget(w->fd);
    watchAdd(w, root);
#else
    scanSorted(w, &w->seen, &w->nseen);
#endif
    *err = 0;
    return w;
}

/* The next change: 0 with its kind, path and whether it is a folder; 3
   when the time ran out (timeout 0: no limit). */
int iris_watch_next(void *wp, int *kind, IrisString *path, int *isDir, int64_t timeout) {
    Watcher *w = wp;
    int64_t deadline = deadlineOf(timeout);
    for (;;) {
        if (w->qat < w->qn) {
            *kind = w->kinds[w->qat];
            *isDir = w->queueDir[w->qat];
            iris_string_append(path, w->queue[w->qat], (int64_t)strlen(w->queue[w->qat]));
            free(w->queue[w->qat]);
            w->qat++;
            if (w->qat == w->qn) w->qat = w->qn = 0;
            return 0;
        }
#ifdef __linux__
        if (w->at >= w->len) {
            ssize_t n = read(w->fd, w->buf, sizeof w->buf);
            if (n <= 0) {
                if (n < 0 && errno != EAGAIN && errno != EINTR) return 5;
                if (!waitFdUntil(w->fd, 0, deadline)) return 3;
                continue;
            }
            w->len = (int)n;
            w->at = 0;
        }
        while (w->at < w->len) {
            struct inotify_event *e = (struct inotify_event *)(w->buf + w->at);
            w->at += (int)(sizeof *e + e->len);
            if (e->wd < 0 || e->wd >= w->npaths || !w->paths[e->wd]) continue;
            if (e->mask & (IN_IGNORED | IN_DELETE_SELF)) {
                continue;
            }
            char full[PATH_MAX];
            if (e->len) snprintf(full, sizeof full, "%s/%s", w->paths[e->wd], e->name);
            else snprintf(full, sizeof full, "%s", w->paths[e->wd]);
            int dir = (e->mask & IN_ISDIR) != 0;
            int k = (e->mask & (IN_CREATE | IN_MOVED_TO)) ? 1 : (e->mask & (IN_DELETE | IN_MOVED_FROM)) ? 2 : 3;
            if (k == 1 && dir && w->recursive) watchAdd(w, full);
            if (k == 3 && dir) continue;    /* a folder's own times: its entries tell more */
            watchPush(w, k, full, dir);
        }
#else
        WatchSeen *now;
        int64_t n;
        scanSorted(w, &now, &n);
        int64_t i = 0, j = 0;
        while (i < w->nseen || j < n) {
            int c = i >= w->nseen ? 1 : j >= n ? -1 : strcmp(w->seen[i].path, now[j].path);
            if (c < 0) {
                watchPush(w, 2, w->seen[i].path, w->seen[i].isDir);
                i++;
            } else if (c > 0) {
                watchPush(w, 1, now[j].path, now[j].isDir);
                j++;
            } else {
                if (!now[j].isDir && (now[j].size != w->seen[i].size || now[j].mtime != w->seen[i].mtime)) {
                    watchPush(w, 3, now[j].path, 0);
                }
                i++;
                j++;
            }
        }
        seenFree(w->seen, w->nseen);
        w->seen = now;
        w->nseen = n;
        if (w->qn > 0) continue;
        if (deadline > 0 && nowNs() >= deadline) return 3;
        iris_sleep(200000000);
#endif
    }
}

void iris_watch_close(void *wp) {
    Watcher *w = wp;
    if (!w) return;
#ifdef __linux__
    fdForget(w->fd);
    close(w->fd);
    for (int i = 0; i < w->npaths; i++) free(w->paths[i]);
    free(w->paths);
#else
    seenFree(w->seen, w->nseen);
#endif
    for (int64_t i = w->qat; i < w->qn; i++) free(w->queue[i]);
    free(w->queue);
    free(w->kinds);
    free(w->queueDir);
    free(w->root);
    free(w);
}

/* ---------------------------------------------------------------- pseudo-terminals

   A pty is a pair: the program side, which a shell runs on as its
   terminal, and this side (the master), which reads what the program
   writes and writes what it reads, as an SSH server or terminal emulator
   does. The master doesn't block: reads and writes wait in the event loop. */

int iris_pty_open(int rows, int cols, int *master, int *terminal, IrisString *name) {
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0) return errno;
    if (grantpt(m) != 0 || unlockpt(m) != 0) {
        int e = errno;
        close(m);
        return e;
    }
    char path[256];
#ifdef __linux__
    if (ptsname_r(m, path, sizeof path) != 0) {
        int e = errno;
        close(m);
        return e;
    }
#else
    const char *pn = ptsname(m);
    if (!pn) {
        close(m);
        return ENOTTY;
    }
    snprintf(path, sizeof path, "%s", pn);
#endif
    int t = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (t < 0) {
        int e = errno;
        close(m);
        return e;
    }
    fcntl(m, F_SETFD, FD_CLOEXEC);
    struct winsize ws = {(unsigned short)rows, (unsigned short)cols, 0, 0};
    ioctl(m, TIOCSWINSZ, &ws);
    nonBlocking(m);
    fdForget(m);
    *master = m;
    *terminal = t;
    iris_string_append(name, path, (int64_t)strlen(path));
    return 0;
}

/* What the program wrote: bytes read, 0 once it closed its terminal side,
   or -errno. */
int64_t iris_pty_read(int fd, char *buf, int64_t len) {
    for (;;) {
        ssize_t n = read(fd, buf, (size_t)len);
        if (n >= 0) return n;
        if (errno == EIO) return 0;     /* Linux: every program side is closed */
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            waitFd(fd, 0);
            continue;
        }
        return -errno;
    }
}

int iris_pty_write(int fd, const char *data, int64_t len) {
    while (len > 0) {
        ssize_t n = write(fd, data, (size_t)len);
        if (n > 0) {
            data += n;
            len -= n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            waitFd(fd, 1);
            continue;
        }
        return n < 0 ? errno : EIO;
    }
    return 0;
}

int iris_pty_resize(int fd, int rows, int cols, int xpixel, int ypixel) {
    struct winsize ws = {(unsigned short)rows, (unsigned short)cols, (unsigned short)xpixel, (unsigned short)ypixel};
    return ioctl(fd, TIOCSWINSZ, &ws) == 0 ? 0 : errno;
}

/* The terminal's settings, by the case numbers of pty.Flag and pty.Char:
   which flag set (0 input, 1 output, 2 control, 3 local) and its bit. */
static const struct { int set; tcflag_t bit; } ptyFlags[] = {
    {0, IGNPAR}, {0, PARMRK}, {0, INPCK}, {0, ISTRIP}, {0, INLCR}, {0, IGNCR}, {0, ICRNL},
#ifdef IUCLC
    {0, IUCLC},
#else
    {0, 0},
#endif
    {0, IXON}, {0, IXANY}, {0, IXOFF}, {0, IMAXBEL},
#ifdef IUTF8
    {0, IUTF8},
#else
    {0, 0},
#endif
    {3, ISIG}, {3, ICANON},
#ifdef XCASE
    {3, XCASE},
#else
    {3, 0},
#endif
    {3, ECHO}, {3, ECHOE}, {3, ECHOK}, {3, ECHONL}, {3, NOFLSH}, {3, TOSTOP}, {3, IEXTEN}, {3, ECHOCTL},
    {3, ECHOKE}, {3, PENDIN}, {1, OPOST},
#ifdef OLCUC
    {1, OLCUC},
#else
    {1, 0},
#endif
    {1, ONLCR}, {1, OCRNL}, {1, ONOCR}, {1, ONLRET}, {2, CS7}, {2, CS8}, {2, PARENB}, {2, PARODD},
};

static const int ptyChars[] = {VINTR, VQUIT, VERASE, VKILL, VEOF, VEOL, VEOL2, VSTART, VSTOP, VSUSP,
#ifdef VDSUSP
                               VDSUSP,
#else
                               -1,
#endif
                               VREPRINT, VWERASE, VLNEXT,
#ifdef VDISCARD
                               VDISCARD,
#else
                               -1,
#endif
#ifdef VSTATUS
                               VSTATUS,
#else
                               -1,
#endif
                               VMIN, VTIME};

/* op 0 a flag (which, on), 1 a control character (which, value), 2 the
   speed (value, in and out): 0, errno, or -1 when this system has none. */
int iris_pty_set(int fd, int op, int which, int64_t value) {
    struct termios t;
    if (tcgetattr(fd, &t) != 0) return errno;
    if (op == 0) {
        if (which < 0 || which >= (int)(sizeof ptyFlags / sizeof ptyFlags[0]) || !ptyFlags[which].bit) return -1;
        tcflag_t *set = ptyFlags[which].set == 0 ? &t.c_iflag : ptyFlags[which].set == 1 ? &t.c_oflag
                       : ptyFlags[which].set == 2 ? &t.c_cflag : &t.c_lflag;
        if (ptyFlags[which].set == 2 && (ptyFlags[which].bit == CS7 || ptyFlags[which].bit == CS8)) {
            *set &= ~(tcflag_t)CSIZE;
            if (value) *set |= ptyFlags[which].bit;
        } else if (value) {
            *set |= ptyFlags[which].bit;
        } else {
            *set &= ~ptyFlags[which].bit;
        }
    } else if (op == 1) {
        if (which < 0 || which >= (int)(sizeof ptyChars / sizeof ptyChars[0]) || ptyChars[which] < 0) return -1;
        t.c_cc[ptyChars[which]] = (cc_t)value;
    } else {
        speed_t sp = (speed_t)value;
#ifdef __linux__
        static const struct { int64_t baud; speed_t code; } speeds[] = {
            {0, B0}, {50, B50}, {75, B75}, {110, B110}, {134, B134}, {150, B150}, {200, B200}, {300, B300},
            {600, B600}, {1200, B1200}, {1800, B1800}, {2400, B2400}, {4800, B4800}, {9600, B9600},
            {19200, B19200}, {38400, B38400}, {57600, B57600}, {115200, B115200}, {230400, B230400}};
        sp = (speed_t)-1;
        for (size_t i = 0; i < sizeof speeds / sizeof speeds[0]; i++) {
            if (speeds[i].baud == value) sp = speeds[i].code;
        }
        if (sp == (speed_t)-1) return -1;
#endif
        cfsetispeed(&t, sp);
        cfsetospeed(&t, sp);
    }
    return tcsetattr(fd, TCSANOW, &t) == 0 ? 0 : errno;
}

/* Whether a started program ended: 1 with *status (its exit status, or
   128 + the signal), 0 while it runs. */
int iris_process_poll(int64_t pid, int *status) {
    int st = 0;
    pid_t r = waitpid((pid_t)pid, &st, WNOHANG);
    if (r == 0) return 0;
    if (r < 0) {
        *status = -1;
        return 1;
    }
    *status = WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 1;
    return 1;
}

/* Waits for a started program to end, letting the other tasks run: on
   Linux its pidfd in the event loop, elsewhere checks with growing sleeps.
   Gives its status; closes handle. */
int iris_process_wait(int64_t pid, int handle) {
    int status = 0;
    int64_t pause = 1000000;
    while (!iris_process_poll(pid, &status)) {
        if (handle >= 0) {
            waitFd(handle, 0);
            continue;
        }
        iris_sleep(pause);
        if (pause < 50000000) pause *= 2;
    }
    if (handle >= 0) {
        fdForget(handle);
        close(handle);
    }
    return status;
}

void iris_process_signal(int64_t pid, int kill9) { kill((pid_t)pid, kill9 ? SIGKILL : SIGTERM); }

void iris_process_release(int handle) {
    if (handle >= 0) {
        fdForget(handle);
        close(handle);
    }
}

/* ---------------------------------------------------------------- TLS

   TLS uses OpenSSL, loaded with dlopen the first time a program uses it,
   so building needs no OpenSSL headers and programs without TLS don't need
   OpenSSL at all. IRIS_OPENSSL can name libssl; libcrypto is looked for
   next to it. The sockets stay non-blocking: when OpenSSL wants to read or
   write, the task waits on the socket and the other tasks run. */

#include <dlfcn.h>

static struct {
    int loaded;                 /* 1 ready, -1 not found */
    void *(*clientMethod)(void);
    void *(*serverMethod)(void);
    void *(*ctxNew)(void *);
    void (*ctxFree)(void *);
    int (*ctxDefaultPaths)(void *);
    int (*ctxLoadVerify)(void *, const char *, const char *);
    void (*ctxSetVerify)(void *, int, void *);
    long (*ctxCtrl)(void *, int, long, void *);
    int (*ctxUseChain)(void *, const char *);
    int (*ctxUseKey)(void *, const char *, int);
    int (*ctxCheckKey)(void *);
    void *(*sslNew)(void *);
    void (*sslFree)(void *);
    int (*sslSetFd)(void *, int);
    long (*sslCtrl)(void *, int, long, void *);
    int (*sslSet1Host)(void *, const char *);
    int (*sslConnect)(void *);
    int (*sslAccept)(void *);
    int (*sslRead)(void *, void *, int);
    int (*sslWrite)(void *, const void *, int);
    int (*sslGetError)(void *, int);
    int (*sslShutdown)(void *);
    long (*sslVerifyResult)(void *);
    int (*sslGetFd)(void *);
    unsigned long (*errGet)(void);
    void (*errString)(unsigned long, char *, size_t);
    void (*errClear)(void);
    const char *(*verifyString)(long);
    /* ALPN, which old OpenSSLs lack */
    void (*ctxSetAlpnSelect)(void *, int (*)(void *, const unsigned char **, unsigned char *,
                                            const unsigned char *, unsigned int, void *), void *);
    int (*sslSetAlpnProtos)(void *, const unsigned char *, unsigned int);
    void (*sslGetAlpn)(const void *, const unsigned char **, unsigned int *);
    /* several certificates, client certificates (0.67) */
    long (*ctxCallbackCtrl)(void *, int, void (*)(void));
    const char *(*getServername)(const void *, int);
    void *(*setSslCtx)(void *, void *);
    void *(*ctxGetCert)(const void *);
    int (*checkHost)(void *, const char *, size_t, unsigned, char **);
    int (*ctxSessionId)(void *, const unsigned char *, unsigned);
    void *(*loadClientCaFile)(const char *);
    void (*ctxSetClientCaList)(void *, void *);
    void *(*getPeerCert)(const void *);
    void *(*x509Subject)(const void *);
    char *(*nameOneline)(const void *, char *, int);
    void (*x509Free)(void *);
    uint64_t (*ctxSetOptions)(void *, uint64_t);
    void *(*ctxGetKey)(const void *);
    int (*ctxUseCert)(void *, void *);
    int (*ctxUsePkey)(void *, void *);
    /* the connection's socket as a BIO of the runtime's own (1.1.0) */
    void *(*bioMethNew)(int, const char *);
    int (*bioMethSetWrite)(void *, int (*)(void *, const char *, int));
    int (*bioMethSetRead)(void *, int (*)(void *, char *, int));
    int (*bioMethSetCtrl)(void *, long (*)(void *, int, long, void *));
    int (*bioMethSetCreate)(void *, int (*)(void *));
    void *(*bioNew)(const void *);
    void (*bioSetData)(void *, void *);
    void *(*bioGetData)(void *);
    void (*bioSetInit)(void *, int);
    void (*bioSetFlags)(void *, int);
    void (*bioClearFlags)(void *, int);
    void (*sslSetBio)(void *, void *, void *);
    void *sockMeth;
    void *cryptoLib;
} ossl;

static pthread_once_t osslOnce = PTHREAD_ONCE_INIT;
static void *clientCtx;
static _Thread_local char tlsMsg[512];
static _Thread_local int64_t tlsMsgLen;

const char *iris_tls_message_ptr(void) { return tlsMsg; }
int64_t iris_tls_message_len(void) { return tlsMsgLen; }

static void tlsSay(const char *what) {
    tlsMsgLen = snprintf(tlsMsg, sizeof tlsMsg, "%s", what);
    if (tlsMsgLen >= (int64_t)sizeof tlsMsg) tlsMsgLen = sizeof tlsMsg - 1;
}

/* The message: what was being done, and OpenSSL's reason when it has one. */
static void tlsSayOpenssl(const char *what) {
    unsigned long e = ossl.errGet ? ossl.errGet() : 0;
    if (e && ossl.errString) {
        char reason[256];
        ossl.errString(e, reason, sizeof reason);
        tlsMsgLen = snprintf(tlsMsg, sizeof tlsMsg, "%s: %s", what, reason);
    } else {
        tlsMsgLen = snprintf(tlsMsg, sizeof tlsMsg, "%s", what);
    }
    if (tlsMsgLen >= (int64_t)sizeof tlsMsg) tlsMsgLen = sizeof tlsMsg - 1;
    if (ossl.errClear) ossl.errClear();
}

static void *openLib(const char *const *names, const char *dirOf, const char *base) {
    if (dirOf) {
        /* next to the libssl that IRIS_OPENSSL named */
        char path[4096];
        const char *slash = strrchr(dirOf, '/');
        int n = slash ? (int)(slash - dirOf) : 0;
        snprintf(path, sizeof path, "%.*s%s%s", n, dirOf, n ? "/" : "", base);
        void *h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
        if (h) return h;
    }
    for (int i = 0; names[i]; i++) {
        void *h = dlopen(names[i], RTLD_NOW | RTLD_GLOBAL);
        if (h) return h;
    }
    return NULL;
}

/* A connection's socket as OpenSSL's BIO: like OpenSSL's own socket BIO,
   but its sends can't raise SIGPIPE (MSG_NOSIGNAL), so no call into
   OpenSSL has to hold the signal. The BIO's data is the descriptor. */
enum { BIO_TYPE_IRIS_SOCK = 0x0505, BIO_FL_READ = 1, BIO_FL_WRITE = 2, BIO_FL_RWS = 7, BIO_FL_RETRY = 8,
       BIO_CTRL_FLUSH_ = 11, BIO_C_GET_FD_ = 105 };

static int sockBioWrite(void *b, const char *buf, int len) {
    int fd = (int)(intptr_t)ossl.bioGetData(b);
    ossl.bioClearFlags(b, BIO_FL_RWS | BIO_FL_RETRY);
    errno = 0;
#ifdef MSG_NOSIGNAL
    ssize_t n = send(fd, buf, (size_t)len, MSG_NOSIGNAL);
#else
    ssize_t n = send(fd, buf, (size_t)len, 0);     /* macOS sockets have SO_NOSIGPIPE */
#endif
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        ossl.bioSetFlags(b, BIO_FL_WRITE | BIO_FL_RETRY);
    }
    return (int)n;
}

static int sockBioRead(void *b, char *buf, int len) {
    int fd = (int)(intptr_t)ossl.bioGetData(b);
    ossl.bioClearFlags(b, BIO_FL_RWS | BIO_FL_RETRY);
    errno = 0;
    ssize_t n = recv(fd, buf, (size_t)len, 0);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        ossl.bioSetFlags(b, BIO_FL_READ | BIO_FL_RETRY);
    }
    return (int)n;
}

static long sockBioCtrl(void *b, int cmd, long num, void *ptr) {
    (void)num;
    if (cmd == BIO_CTRL_FLUSH_) return 1;
    if (cmd == BIO_C_GET_FD_) {
        int fd = (int)(intptr_t)ossl.bioGetData(b);
        if (ptr) *(int *)ptr = fd;
        return fd;
    }
    return 0;
}

static int sockBioCreate(void *b) {
    ossl.bioSetInit(b, 1);
    return 1;
}

static void sockBioLoad(void *ssl, void *crypto) {
    void **f[] = {(void **)&ossl.bioMethNew, (void **)&ossl.bioMethSetWrite, (void **)&ossl.bioMethSetRead,
                  (void **)&ossl.bioMethSetCtrl, (void **)&ossl.bioMethSetCreate, (void **)&ossl.bioNew,
                  (void **)&ossl.bioSetData, (void **)&ossl.bioGetData, (void **)&ossl.bioSetInit,
                  (void **)&ossl.bioSetFlags, (void **)&ossl.bioClearFlags};
    const char *names[] = {"BIO_meth_new", "BIO_meth_set_write", "BIO_meth_set_read", "BIO_meth_set_ctrl",
                           "BIO_meth_set_create", "BIO_new", "BIO_set_data", "BIO_get_data", "BIO_set_init",
                           "BIO_set_flags", "BIO_clear_flags"};
    for (size_t i = 0; i < sizeof f / sizeof f[0]; i++) {
        *f[i] = dlsym(crypto, names[i]);
        if (!*f[i]) return;
    }
    *(void **)&ossl.sslSetBio = dlsym(ssl, "SSL_set_bio");
    if (!ossl.sslSetBio) return;
    void *m = ossl.bioMethNew(BIO_TYPE_IRIS_SOCK, "iris socket");
    if (!m) return;
    ossl.bioMethSetWrite(m, sockBioWrite);
    ossl.bioMethSetRead(m, sockBioRead);
    ossl.bioMethSetCtrl(m, sockBioCtrl);
    ossl.bioMethSetCreate(m, sockBioCreate);
    ossl.sockMeth = m;
}

static void osslLoad(void) {
#ifdef __APPLE__
    static const char *const sslNames[] = {"/opt/homebrew/opt/openssl@3/lib/libssl.3.dylib",
                                           "/usr/local/opt/openssl@3/lib/libssl.3.dylib",
                                           "/opt/homebrew/lib/libssl.3.dylib", "/usr/local/lib/libssl.3.dylib", NULL};
    static const char *const cryptoNames[] = {"/opt/homebrew/opt/openssl@3/lib/libcrypto.3.dylib",
                                              "/usr/local/opt/openssl@3/lib/libcrypto.3.dylib",
                                              "/opt/homebrew/lib/libcrypto.3.dylib", "/usr/local/lib/libcrypto.3.dylib",
                                              NULL};
    const char *cryptoBase = "libcrypto.3.dylib";
#else
    static const char *const sslNames[] = {"libssl.so.3", "libssl.so.1.1", "libssl.so", NULL};
    static const char *const cryptoNames[] = {"libcrypto.so.3", "libcrypto.so.1.1", "libcrypto.so", NULL};
    const char *cryptoBase = "libcrypto.so.3";
#endif
    const char *env = getenv("IRIS_OPENSSL");
    void *crypto = openLib(cryptoNames, env && *env ? env : NULL, cryptoBase);
    void *ssl = NULL;
    if (env && *env) ssl = dlopen(env, RTLD_NOW | RTLD_GLOBAL);
    if (!ssl) ssl = openLib(sslNames, NULL, NULL);
    if (!ssl || !crypto) {
        ossl.loaded = -1;
        return;
    }
#define SYM(lib, field, name)                                   \
    do {                                                        \
        *(void **)&ossl.field = dlsym(lib, name);               \
        if (!ossl.field) {                                      \
            ossl.loaded = -1;                                   \
            return;                                             \
        }                                                       \
    } while (0)
    SYM(ssl, clientMethod, "TLS_client_method");
    SYM(ssl, serverMethod, "TLS_server_method");
    SYM(ssl, ctxNew, "SSL_CTX_new");
    SYM(ssl, ctxFree, "SSL_CTX_free");
    SYM(ssl, ctxDefaultPaths, "SSL_CTX_set_default_verify_paths");
    SYM(ssl, ctxLoadVerify, "SSL_CTX_load_verify_locations");
    SYM(ssl, ctxSetVerify, "SSL_CTX_set_verify");
    SYM(ssl, ctxCtrl, "SSL_CTX_ctrl");
    SYM(ssl, ctxUseChain, "SSL_CTX_use_certificate_chain_file");
    SYM(ssl, ctxUseKey, "SSL_CTX_use_PrivateKey_file");
    SYM(ssl, ctxCheckKey, "SSL_CTX_check_private_key");
    SYM(ssl, sslNew, "SSL_new");
    SYM(ssl, sslFree, "SSL_free");
    SYM(ssl, sslSetFd, "SSL_set_fd");
    SYM(ssl, sslCtrl, "SSL_ctrl");
    SYM(ssl, sslSet1Host, "SSL_set1_host");
    SYM(ssl, sslConnect, "SSL_connect");
    SYM(ssl, sslAccept, "SSL_accept");
    SYM(ssl, sslRead, "SSL_read");
    SYM(ssl, sslWrite, "SSL_write");
    SYM(ssl, sslGetError, "SSL_get_error");
    SYM(ssl, sslShutdown, "SSL_shutdown");
    SYM(ssl, sslVerifyResult, "SSL_get_verify_result");
    SYM(ssl, sslGetFd, "SSL_get_fd");
    SYM(crypto, errGet, "ERR_get_error");
    SYM(crypto, errString, "ERR_error_string_n");
    SYM(crypto, errClear, "ERR_clear_error");
    SYM(crypto, verifyString, "X509_verify_cert_error_string");
    SYM(ssl, ctxCallbackCtrl, "SSL_CTX_callback_ctrl");
    SYM(ssl, getServername, "SSL_get_servername");
    SYM(ssl, setSslCtx, "SSL_set_SSL_CTX");
    SYM(ssl, ctxGetCert, "SSL_CTX_get0_certificate");
    SYM(ssl, ctxSessionId, "SSL_CTX_set_session_id_context");
    SYM(ssl, loadClientCaFile, "SSL_load_client_CA_file");
    SYM(ssl, ctxSetClientCaList, "SSL_CTX_set_client_CA_list");
    SYM(crypto, checkHost, "X509_check_host");
    SYM(crypto, x509Subject, "X509_get_subject_name");
    SYM(crypto, nameOneline, "X509_NAME_oneline");
    SYM(crypto, x509Free, "X509_free");
    *(void **)&ossl.getPeerCert = dlsym(ssl, "SSL_get1_peer_certificate");     /* OpenSSL 3 */
    if (!ossl.getPeerCert) SYM(ssl, getPeerCert, "SSL_get_peer_certificate");
    SYM(ssl, ctxSetOptions, "SSL_CTX_set_options");
    SYM(ssl, ctxGetKey, "SSL_CTX_get0_privatekey");
    SYM(ssl, ctxUseCert, "SSL_CTX_use_certificate");
    SYM(ssl, ctxUsePkey, "SSL_CTX_use_PrivateKey");
#undef SYM
    *(void **)&ossl.ctxSetAlpnSelect = dlsym(ssl, "SSL_CTX_set_alpn_select_cb");
    sockBioLoad(ssl, crypto);
    ossl.cryptoLib = crypto;
    *(void **)&ossl.sslSetAlpnProtos = dlsym(ssl, "SSL_set_alpn_protos");
    *(void **)&ossl.sslGetAlpn = dlsym(ssl, "SSL_get0_alpn_selected");
    ossl.loaded = 1;
}

static bool tlsReady(void) {
    pthread_once(&osslOnce, osslLoad);
    if (ossl.loaded != 1) {
#ifdef __APPLE__
        tlsSay("TLS needs OpenSSL 3: brew install openssl@3, or set IRIS_OPENSSL to its libssl");
#else
        tlsSay("TLS needs OpenSSL (libssl3): install it, or set IRIS_OPENSSL to its libssl");
#endif
        return false;
    }
    return true;
}

enum { TLS_VERIFY_PEER = 1, TLS_CTRL_MIN_VERSION = 123, TLS_V12 = 0x0303, TLS_CTRL_SNI = 55,
       TLS_WANT_READ = 2, TLS_WANT_WRITE = 3, TLS_ERROR_SYSCALL = 5, TLS_ZERO_RETURN = 6 };

/* A context for clients: the system's certificates, or the ones in ca;
   with a certificate and key of its own when cert isn't NULL. NULL with the
   message set on an error. */
static void *newClientCtx(const char *ca, const char *cert, const char *key) {
    void *ctx = ossl.ctxNew(ossl.clientMethod());
    if (!ctx) {
        tlsSayOpenssl("starting TLS");
        return NULL;
    }
    ossl.ctxCtrl(ctx, TLS_CTRL_MIN_VERSION, TLS_V12, NULL);
    ossl.ctxSetVerify(ctx, TLS_VERIFY_PEER, NULL);
    char what[300];
    if (ca ? ossl.ctxLoadVerify(ctx, ca, NULL) != 1 : ossl.ctxDefaultPaths(ctx) != 1) {
        snprintf(what, sizeof what, "reading the certificates to trust %s", ca ? ca : "");
        tlsSayOpenssl(what);
        ossl.ctxFree(ctx);
        return NULL;
    }
    if (cert && ossl.ctxUseChain(ctx, cert) != 1) {
        snprintf(what, sizeof what, "reading the certificate %s", cert);
        tlsSayOpenssl(what);
        ossl.ctxFree(ctx);
        return NULL;
    }
    if (cert && (ossl.ctxUseKey(ctx, key, 1) != 1 || ossl.ctxCheckKey(ctx) != 1)) {
        snprintf(what, sizeof what, "reading the key %s", key);
        tlsSayOpenssl(what);
        ossl.ctxFree(ctx);
        return NULL;
    }
    return ctx;
}

static int lockedClientCtx;
static void *sharedClientCtx(void) {
    spinLock(&lockedClientCtx);
    if (!clientCtx) clientCtx = newClientCtx(NULL, NULL, NULL);
    void *c = clientCtx;
    spinUnlock(&lockedClientCtx);
    return c;
}

/* ---------------------------------------------------------------- public-key signatures

   ECDSA (P-256, P-384) and RSA (PKCS#1 v1.5 and PSS) through libcrypto's
   EVP calls, which OpenSSL 1.1 and 3 both have. A key is an EVP_PKEY. */

static struct {
    int loaded;
    void *(*bioMemBuf)(const void *, int);
    void *(*bioNew)(const void *);
    const void *(*bioSMem)(void);
    void (*bioFree)(void *);
    long (*bioCtrl)(void *, int, long, void *);
    void *(*readPriv)(void *, void **, void *, void *);
    void *(*readPub)(void *, void **, void *, void *);
    int (*writePriv)(void *, void *, const void *, void *, int, void *, void *);
    int (*writePub)(void *, void *);
    void *(*d2iPriv)(void **, const unsigned char **, long);
    void *(*d2iPub)(void **, const unsigned char **, long);
    int (*i2dPub)(void *, unsigned char **);
    void (*pkeyFree)(void *);
    int (*baseId)(const void *);
    int (*bits)(const void *);
    void *(*keygen)(void *, const char *, const char *, ...);
    void *(*mdNew)(void);
    void (*mdFree)(void *);
    int (*signInit)(void *, void **, const void *, void *, void *);
    int (*sign)(void *, unsigned char *, size_t *, const unsigned char *, size_t);
    int (*verifyInit)(void *, void **, const void *, void *, void *);
    int (*verify)(void *, const unsigned char *, size_t, const unsigned char *, size_t);
    const void *(*md[4])(void);
    int (*setPadding)(void *, int);
    int (*setSaltLen)(void *, int);
    int (*pkeyCtrl)(void *, int, int, int, int, void *);
    void (*cryptoFree)(void *, const char *, int);
} pk;

static pthread_once_t pkOnce = PTHREAD_ONCE_INIT;

static void pkLoad(void) {
    if (!tlsReady() || !ossl.cryptoLib) return;
    void *c = ossl.cryptoLib;
#define PK(field, name)                                         \
    do {                                                        \
        *(void **)&pk.field = dlsym(c, name);                   \
        if (!pk.field) return;                                  \
    } while (0)
    PK(bioMemBuf, "BIO_new_mem_buf");
    PK(bioNew, "BIO_new");
    PK(bioSMem, "BIO_s_mem");
    PK(bioFree, "BIO_free");
    PK(bioCtrl, "BIO_ctrl");
    PK(readPriv, "PEM_read_bio_PrivateKey");
    PK(readPub, "PEM_read_bio_PUBKEY");
    PK(writePriv, "PEM_write_bio_PKCS8PrivateKey");
    PK(writePub, "PEM_write_bio_PUBKEY");
    PK(d2iPriv, "d2i_AutoPrivateKey");
    PK(d2iPub, "d2i_PUBKEY");
    PK(i2dPub, "i2d_PUBKEY");
    PK(pkeyFree, "EVP_PKEY_free");
    PK(mdNew, "EVP_MD_CTX_new");
    PK(mdFree, "EVP_MD_CTX_free");
    PK(signInit, "EVP_DigestSignInit");
    PK(sign, "EVP_DigestSign");
    PK(verifyInit, "EVP_DigestVerifyInit");
    PK(verify, "EVP_DigestVerify");
    PK(md[0], "EVP_sha1");
    PK(md[1], "EVP_sha256");
    PK(md[2], "EVP_sha384");
    PK(md[3], "EVP_sha512");
    PK(cryptoFree, "CRYPTO_free");
    *(void **)&pk.baseId = dlsym(c, "EVP_PKEY_get_base_id");
    if (!pk.baseId) *(void **)&pk.baseId = dlsym(c, "EVP_PKEY_base_id");
    *(void **)&pk.bits = dlsym(c, "EVP_PKEY_get_bits");
    if (!pk.bits) *(void **)&pk.bits = dlsym(c, "EVP_PKEY_bits");
    *(void **)&pk.keygen = dlsym(c, "EVP_PKEY_Q_keygen");       /* OpenSSL 3 */
    *(void **)&pk.setPadding = dlsym(c, "EVP_PKEY_CTX_set_rsa_padding");
    *(void **)&pk.setSaltLen = dlsym(c, "EVP_PKEY_CTX_set_rsa_pss_saltlen");
    *(void **)&pk.pkeyCtrl = dlsym(c, "EVP_PKEY_CTX_ctrl");
    if (!pk.baseId || !pk.bits) return;
#undef PK
    pk.loaded = 1;
}

static bool pkReady(void) {
    pthread_once(&pkOnce, pkLoad);
    if (!pk.loaded) {
        if (tlsMsgLen == 0) tlsSay("public-key signatures need OpenSSL's libcrypto");
        return false;
    }
    return true;
}

/* A private key from PEM (PKCS#8, or SEC1 and PKCS#1 with their headers)
   or DER: NULL with the message set when it isn't one. */
void *iris_pk_load_private(const char *data, int64_t len) {
    if (!pkReady()) return NULL;
    void *key = NULL;
    if (len > 10 && memmem(data, (size_t)len, "-----BEGIN", 10)) {
        void *b = pk.bioMemBuf(data, (int)len);
        if (b) {
            key = pk.readPriv(b, NULL, NULL, NULL);
            pk.bioFree(b);
        }
    } else {
        const unsigned char *p = (const unsigned char *)data;
        key = pk.d2iPriv(NULL, &p, (long)len);
    }
    if (!key) tlsSayOpenssl("reading the private key");
    return key;
}

/* A public key from PEM or DER (SubjectPublicKeyInfo). */
void *iris_pk_load_public(const char *data, int64_t len) {
    if (!pkReady()) return NULL;
    void *key = NULL;
    if (len > 10 && memmem(data, (size_t)len, "-----BEGIN", 10)) {
        void *b = pk.bioMemBuf(data, (int)len);
        if (b) {
            key = pk.readPub(b, NULL, NULL, NULL);
            pk.bioFree(b);
        }
    } else {
        const unsigned char *p = (const unsigned char *)data;
        key = pk.d2iPub(NULL, &p, (long)len);
    }
    if (!key) tlsSayOpenssl("reading the public key");
    return key;
}

/* A DER length at out (room for 5): its size in bytes. */
static int derLen(unsigned char *out, size_t n) {
    if (n < 128) {
        out[0] = (unsigned char)n;
        return 1;
    }
    int k = 0;
    for (size_t m = n; m; m >>= 8) k++;
    out[0] = (unsigned char)(0x80 | k);
    for (int i = 0; i < k; i++) out[1 + i] = (unsigned char)(n >> (8 * (k - 1 - i)));
    return 1 + k;
}

/* An INTEGER of the big-endian bytes v, with a 0 in front when the top
   bit is set; at out (room for n + 7). */
static size_t derInt(unsigned char *out, const unsigned char *v, size_t n) {
    while (n > 1 && v[0] == 0) {
        v++;
        n--;
    }
    bool pad = n > 0 && (v[0] & 0x80);
    size_t at = 0;
    out[at++] = 0x02;
    at += (size_t)derLen(out + at, n + pad);
    if (pad) out[at++] = 0;
    memcpy(out + at, v, n);
    return at + n;
}

/* An EC public key from its point, uncompressed (04, x, y) or x and y
   alone: curve 0 P-256, 1 P-384, 2 P-521. */
void *iris_pk_ec_public(int curve, const char *point, int64_t len) {
    if (!pkReady()) return NULL;
    static const unsigned char p256[] = {0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02,
                                         0x01, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03,
                                         0x42, 0x00, 0x04};
    static const unsigned char p384[] = {0x30, 0x76, 0x30, 0x10, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02,
                                         0x01, 0x06, 0x05, 0x2b, 0x81, 0x04, 0x00, 0x22, 0x03, 0x62, 0x00, 0x04};
    static const unsigned char p521[] = {0x30, 0x81, 0x9b, 0x30, 0x10, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02,
                                         0x01, 0x06, 0x05, 0x2b, 0x81, 0x04, 0x00, 0x23, 0x03, 0x81, 0x86, 0x00, 0x04};
    size_t coord = curve == 0 ? 32 : curve == 1 ? 48 : 66;
    const unsigned char *prefix = curve == 0 ? p256 : curve == 1 ? p384 : p521;
    size_t plen = curve == 0 ? sizeof p256 : curve == 1 ? sizeof p384 : sizeof p521;
    const unsigned char *xy = (const unsigned char *)point;
    if ((size_t)len == 2 * coord + 1 && xy[0] == 4) {
        xy++;
    } else if ((size_t)len != 2 * coord) {
        tlsSay("an EC point is 04, x and y, or x and y");
        return NULL;
    }
    unsigned char der[160];
    memcpy(der, prefix, plen);
    memcpy(der + plen, xy, 2 * coord);
    const unsigned char *p = der;
    void *key = pk.d2iPub(NULL, &p, (long)(plen + 2 * coord));
    if (!key) tlsSayOpenssl("the EC point isn't on the curve");
    return key;
}

/* An RSA public key from its modulus n and exponent e, big-endian. */
void *iris_pk_rsa_public(const char *n, int64_t nLen, const char *e, int64_t eLen) {
    if (!pkReady()) return NULL;
    if (nLen <= 0 || eLen <= 0 || nLen > 2048 || eLen > 64) {
        tlsSay("an RSA key's n is up to 16384 bits and e up to 512");
        return NULL;
    }
    unsigned char *inner = malloc((size_t)(nLen + eLen + 32));
    unsigned char *der = malloc((size_t)(nLen + eLen + 96));
    if (!inner || !der) outOfMemory();
    /* RSAPublicKey: SEQUENCE { n, e } */
    unsigned char ints[2200];
    size_t il = derInt(ints, (const unsigned char *)n, (size_t)nLen);
    il += derInt(ints + il, (const unsigned char *)e, (size_t)eLen);
    size_t at = 0;
    inner[at++] = 0x30;
    at += (size_t)derLen(inner + at, il);
    memcpy(inner + at, ints, il);
    at += il;
    /* SubjectPublicKeyInfo: SEQUENCE { AlgorithmIdentifier rsaEncryption NULL, BIT STRING } */
    static const unsigned char alg[] = {0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01, 0x05, 0x00};
    unsigned char bits[8];
    size_t bl = 0;
    bits[bl++] = 0x03;
    bl += (size_t)derLen(bits + bl, at + 1);
    bits[bl++] = 0;
    size_t body = sizeof alg + bl + at;
    size_t dl = 0;
    der[dl++] = 0x30;
    dl += (size_t)derLen(der + dl, body);
    memcpy(der + dl, alg, sizeof alg);
    dl += sizeof alg;
    memcpy(der + dl, bits, bl);
    dl += bl;
    memcpy(der + dl, inner, at);
    dl += at;
    const unsigned char *p = der;
    void *key = pk.d2iPub(NULL, &p, (long)dl);
    free(inner);
    free(der);
    if (!key) tlsSayOpenssl("the RSA key isn't one");
    return key;
}

/* A new key: kind 0 P-256, 1 P-384, 3 P-521, 2 RSA of bits. OpenSSL 3. */
void *iris_pk_generate(int kind, int64_t bits) {
    if (!pkReady()) return NULL;
    if (!pk.keygen) {
        tlsSay("making keys needs OpenSSL 3");
        return NULL;
    }
    /* EVP_PKEY_Q_keygen(library context, properties, type, its parameter) */
    void *key = kind == 2 ? pk.keygen(NULL, NULL, "RSA", (size_t)bits)
                          : pk.keygen(NULL, NULL, "EC", kind == 0 ? "P-256" : kind == 1 ? "P-384" : "P-521");
    if (!key) tlsSayOpenssl("making the key");
    return key;
}

/* 1 EC, 2 RSA, 0 another kind; and its size in bits. */
int iris_pk_kind(void *key, int64_t *bits) {
    *bits = pk.bits(key);
    int id = pk.baseId(key);
    return id == 408 ? 1 : (id == 6 || id == 912) ? 2 : 0;     /* EVP_PKEY_EC, RSA, RSA_PSS */
}

/* The key as PEM (private: PKCS#8; public: SubjectPublicKeyInfo), or as
   DER for a public key with der; added to out. */
int iris_pk_export(void *key, int private, int der, IrisString *out) {
    if (der) {
        unsigned char *buf = NULL;
        int n = pk.i2dPub(key, &buf);
        if (n <= 0) return 0;
        iris_string_append(out, (const char *)buf, n);
        pk.cryptoFree(buf, "", 0);
        return 1;
    }
    void *b = pk.bioNew(pk.bioSMem());
    if (!b) return 0;
    int ok = private ? pk.writePriv(b, key, NULL, NULL, 0, NULL, NULL) : pk.writePub(b, key);
    char *data = NULL;
    long n = pk.bioCtrl(b, 3 /* BIO_CTRL_INFO */, 0, &data);
    if (ok && n > 0) iris_string_append(out, data, n);
    pk.bioFree(b);
    return ok;
}

void iris_pk_free(void *key) {
    if (key && pk.loaded) pk.pkeyFree(key);
}

static void *pkCtx(void *key, int hash, int pss, bool signing, void **mdOut) {
    if (hash < 0 || hash > 3) return NULL;
    void *md = pk.mdNew();
    if (!md) return NULL;
    void *pctx = NULL;
    int r = signing ? pk.signInit(md, &pctx, pk.md[hash](), NULL, key) : pk.verifyInit(md, &pctx, pk.md[hash](), NULL, key);
    if (r != 1) {
        pk.mdFree(md);
        return NULL;
    }
    if (pss) {
        /* PSS with a salt as long as the hash, as TLS and QUIC use */
        bool ok = pk.setPadding ? pk.setPadding(pctx, 6) == 1 && pk.setSaltLen(pctx, -1) == 1
                                : pk.pkeyCtrl(pctx, 6, -1, 0x1001, 6, NULL) == 1 &&
                                      pk.pkeyCtrl(pctx, 6, 24, 0x1002, -1, NULL) == 1;
        if (!ok) {
            pk.mdFree(md);
            return NULL;
        }
    }
    *mdOut = md;
    return md;
}

/* Signs data: ECDSA's signature in DER, or RSA's. 0 with the message set
   on an error. hash: 0 SHA-1, 1 SHA-256, 2 SHA-384, 3 SHA-512. */
int iris_pk_sign(void *key, const char *data, int64_t len, int hash, int pss, IrisString *out) {
    void *md = NULL;
    if (!pkCtx(key, hash, pss, true, &md)) {
        tlsSayOpenssl("signing");
        return 0;
    }
    size_t n = 0;
    int ok = pk.sign(md, NULL, &n, (const unsigned char *)data, (size_t)len) == 1;
    unsigned char *sig = ok ? malloc(n ? n : 1) : NULL;
    ok = ok && sig && pk.sign(md, sig, &n, (const unsigned char *)data, (size_t)len) == 1;
    if (ok) iris_string_append(out, (const char *)sig, (int64_t)n);
    else tlsSayOpenssl("signing");
    free(sig);
    pk.mdFree(md);
    return ok;
}

/* 1 when sig is key's signature of data, 0 when it isn't. */
int iris_pk_verify(void *key, const char *data, int64_t len, const char *sig, int64_t sigLen, int hash, int pss) {
    void *md = NULL;
    if (!pkCtx(key, hash, pss, false, &md)) return 0;
    int r = pk.verify(md, (const unsigned char *)sig, (size_t)sigLen, (const unsigned char *)data, (size_t)len);
    pk.mdFree(md);
    if (ossl.errClear) ossl.errClear();
    return r == 1;
}

/* ECDSA's DER signature (SEQUENCE { r, s }) as r and s of size bytes
   each, and back: 1, or 0 for one that isn't. */
int iris_ecdsa_der_to_raw(const unsigned char *d, int64_t n, int64_t size, IrisString *out) {
    int64_t at = 0;
    if (n < 8 || d[at++] != 0x30) return 0;
    int64_t seqLen = d[at++];
    if (seqLen & 0x80) {
        if (seqLen != 0x81 || at >= n) return 0;
        seqLen = d[at++];
    }
    if (at + seqLen != n) return 0;
    unsigned char raw[132] = {0};
    if (size > 66) return 0;
    for (int k = 0; k < 2; k++) {
        if (at + 2 > n || d[at++] != 0x02) return 0;
        int64_t il = d[at++];
        if (il <= 0 || at + il > n) return 0;
        const unsigned char *v = d + at;
        int64_t vl = il;
        while (vl > 0 && v[0] == 0) {
            v++;
            vl--;
        }
        if (vl > size) return 0;
        memcpy(raw + k * size + (size - vl), v, (size_t)vl);
        at += il;
    }
    iris_string_append(out, (const char *)raw, 2 * size);
    return 1;
}

void iris_ecdsa_raw_to_der(const unsigned char *raw, int64_t n, IrisString *out) {
    int64_t size = n / 2;
    unsigned char ints[150];
    size_t il = derInt(ints, raw, (size_t)size);
    il += derInt(ints + il, raw + size, (size_t)size);
    unsigned char head[4];
    size_t hl = 0;
    head[hl++] = 0x30;
    hl += (size_t)derLen(head + hl, il);
    iris_string_append(out, (const char *)head, (int64_t)hl);
    iris_string_append(out, (const char *)ints, (int64_t)il);
}

/* Waits for what OpenSSL asked for; false for any other answer. *timedOut
   says whether the deadline passed. */
static bool tlsWaitUntil(void *ssl, int r, int64_t deadline, bool *timedOut) {
    int e = ossl.sslGetError(ssl, r);
    if (e == TLS_WANT_READ || e == TLS_WANT_WRITE) {
        if (!waitFdUntil(ossl.sslGetFd(ssl), e == TLS_WANT_WRITE, deadline)) {
            *timedOut = true;
            return false;
        }
        return true;
    }
    return false;
}

/* OpenSSL writes to the socket itself, and on Linux a write to a closed
   connection raises SIGPIPE, which ends the program. While it writes the
   signal is held, and one it raised is taken back. macOS sockets have
   SO_NOSIGPIPE instead. */
typedef struct {
    sigset_t old;
    bool pending;
} PipeGuard;

static void pipeHold(PipeGuard *g) {
#ifdef __linux__
    if (ossl.sockMeth) return;      /* the runtime's BIO sends without the signal */
    sigset_t set, now;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    sigpending(&now);
    g->pending = sigismember(&now, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &set, &g->old);
#else
    (void)g;
#endif
}

static void pipeRelease(PipeGuard *g) {
#ifdef __linux__
    if (ossl.sockMeth) return;
    if (!g->pending) {
        sigset_t set, now;
        sigemptyset(&set);
        sigaddset(&set, SIGPIPE);
        sigpending(&now);
        if (sigismember(&now, SIGPIPE)) {
            struct timespec zero = {0, 0};
            sigtimedwait(&set, NULL, &zero);
        }
    }
    pthread_sigmask(SIG_SETMASK, &g->old, NULL);
#else
    (void)g;
#endif
}

/* Gives ssl the socket fd, through the runtime's BIO when there is one;
   records are read ahead, as many as one read brings. */
static void tlsSetSock(void *ssl, int fd) {
    void *b = ossl.sockMeth ? ossl.bioNew(ossl.sockMeth) : NULL;
    if (b) {
        ossl.bioSetData(b, (void *)(intptr_t)fd);
        ossl.sslSetBio(ssl, b, b);
    } else {
        ossl.sslSetFd(ssl, fd);
    }
    ossl.sslCtrl(ssl, 41 /* SSL_CTRL_SET_READ_AHEAD */, 1, NULL);
    /* an idle connection gives its record buffers back */
    ossl.sslCtrl(ssl, 33 /* SSL_CTRL_MODE */, 0x10 /* SSL_MODE_RELEASE_BUFFERS */, NULL);
}

static bool tlsWait(void *ssl, int r) {
    bool t = false;
    return tlsWaitUntil(ssl, r, 0, &t);
}

/* The client side of a handshake on a connected socket: the server must
   have a certificate for host. ca, when not empty, is a file of the
   certificates to trust instead of the system's; cert and key, when not
   empty, the client's own certificate. NULL on an error, with the message
   set. */
void *iris_tls_connect(int fd, const char *host, int64_t hostLen, const char *ca, int64_t caLen, const char *alpn,
                       int64_t alpnLen, const char *cert, int64_t certLen, const char *key, int64_t keyLen) {
    if (!tlsReady()) return NULL;
    if (hostLen <= 0) {
        /* the name is what the certificate is checked against */
        tlsSay("a TLS client needs the server's name or IP to check its certificate");
        return NULL;
    }
    char *h = cpath(host, hostLen), *caPath = caLen > 0 ? cpath(ca, caLen) : NULL;
    char *certPath = certLen > 0 ? cpath(cert, certLen) : NULL, *keyPath = keyLen > 0 ? cpath(key, keyLen) : NULL;
    bool own = caPath || certPath;
    void *ctx = own ? newClientCtx(caPath, certPath, keyPath ? keyPath : certPath) : sharedClientCtx();
    if (!ctx && !own) tlsSayOpenssl("starting TLS");
    void *ssl = ctx ? ossl.sslNew(ctx) : NULL;
    if (ctx && !ssl) tlsSayOpenssl("starting TLS");
    if (own && ctx) ossl.ctxFree(ctx);      /* the connection keeps its own reference */
    free(caPath);
    free(certPath);
    free(keyPath);
    if (!ssl) {
        free(h);
        return NULL;
    }
    tlsSetSock(ssl, fd);
    unsigned char ipBuf[16];
    bool isIp = inet_pton(AF_INET, h, ipBuf) == 1 || inet_pton(AF_INET6, h, ipBuf) == 1;
    if (!isIp) ossl.sslCtrl(ssl, TLS_CTRL_SNI, 0, h);     /* SNI names hosts, never IPs */
    ossl.sslSet1Host(ssl, h);
    if (alpnLen > 0 && ossl.sslSetAlpnProtos) {
        /* the protocols to offer, each with its length in front: "\x02h2\x08http/1.1" */
        ossl.sslSetAlpnProtos(ssl, (const unsigned char *)alpn, (unsigned)alpnLen);
    }
    for (;;) {
        PipeGuard g;
        pipeHold(&g);
        int r = ossl.sslConnect(ssl);
        pipeRelease(&g);
        if (r == 1) break;
        if (tlsWait(ssl, r)) continue;
        long v = ossl.sslVerifyResult(ssl);
        if (v != 0) {
            char what[400];
            snprintf(what, sizeof what, "its certificate can't be trusted: %s", ossl.verifyString(v));
            tlsSay(what);
            if (ossl.errClear) ossl.errClear();
        } else {
            tlsSayOpenssl("the TLS handshake failed");
        }
        ossl.sslFree(ssl);
        free(h);
        return NULL;
    }
    free(h);
    return ssl;
}

/* The server picks the first of its protocols that the client offers. */
static int alpnSelect(void *ssl, const unsigned char **out, unsigned char *outLen, const unsigned char *in,
                      unsigned int inLen, void *arg) {
    (void)ssl;
    const unsigned char *ours = arg;
    for (unsigned i = 0; ours[i]; i += 1u + ours[i]) {
        for (unsigned j = 0; j < inLen; j += 1u + in[j]) {
            if (in[j] == ours[i] && j + 1u + in[j] <= inLen && memcmp(in + j + 1, ours + i + 1, ours[i]) == 0) {
                *out = in + j + 1;
                *outLen = in[j];
                return 0;               /* SSL_TLSEXT_ERR_OK */
            }
        }
    }
    return 3;                           /* SSL_TLSEXT_ERR_NOACK: no protocol, the connection goes on */
}

/* A server's TLS setup: its certificates (files, read again on reload),
   the CA its clients' certificates must be signed by, and the protocols it
   offers with ALPN. What OpenSSL uses is built from that as a generation,
   one context per certificate; a change builds a new one and swaps it in,
   so handshakes going on keep theirs. The first certificate is the default
   and the others are picked by the name the client asks for (SNI). */

typedef struct {
    int refs;                   /* the server's own, and one per handshake */
    int n;
    void **ctxs;
    void **leafs;               /* each context's certificate, for names */
    unsigned char *alpn;        /* NUL after the last protocol */
} TlsGen;

typedef struct {
    int lock;
    TlsGen *gen;
    int n;
    char **certs, **keys;
    char *clientCa;
    int clientRequired;
    unsigned char *alpn;
} TlsServer;

enum { TLS_VERIFY_FAIL_NO_CERT = 2, TLS_CTRL_SERVERNAME_CB = 53, TLS_CTRL_SERVERNAME_ARG = 54,
       TLS_CTRL_GET_CHAIN = 82, TLS_CTRL_CHAIN = 88 };

static void genRelease(TlsGen *g) {
    if (!g || __atomic_sub_fetch(&g->refs, 1, __ATOMIC_ACQ_REL) != 0) return;
    for (int i = 0; i < g->n; i++) {
        if (g->ctxs[i]) ossl.ctxFree(g->ctxs[i]);
    }
    free(g->ctxs);
    free(g->leafs);
    free(g->alpn);
    free(g);
}

/* Picks the certificate for the name the client asked for: the first whose
   names cover it, else the default. */
static int pickCert(void *ssl, int *alert, void *arg) {
    (void)alert;
    TlsGen *g = arg;
    const char *name = ossl.getServername(ssl, 0);
    if (!name || g->n < 2) return 0;
    size_t len = strlen(name);
    for (int i = 0; i < g->n; i++) {
        if (g->leafs[i] && ossl.checkHost(g->leafs[i], name, len, 0, NULL) == 1) {
            if (i > 0) ossl.setSslCtx(ssl, g->ctxs[i]);
            return 0;
        }
    }
    return 0;
}

/* A context for one certificate: read from its files, or, when from isn't
   NULL, the certificate, key and chain that context loaded already. */
static void *serverCtx(TlsServer *s, TlsGen *g, const char *cert, const char *key, void *from) {
    void *ctx = ossl.ctxNew(ossl.serverMethod());
    if (!ctx) {
        tlsSayOpenssl("starting TLS");
        return NULL;
    }
    char what[300];
    ossl.ctxCtrl(ctx, TLS_CTRL_MIN_VERSION, TLS_V12, NULL);
    ossl.ctxSessionId(ctx, (const unsigned char *)"iris", 4);
    /* a renegotiation would call pickCert after the handshake let go of
       its generation */
    ossl.ctxSetOptions(ctx, 1u << 30 /* SSL_OP_NO_RENEGOTIATION */);
    if (from) {
        void *chain = NULL;
        ossl.ctxCtrl(from, TLS_CTRL_GET_CHAIN, 0, &chain);
        if (ossl.ctxUseCert(ctx, ossl.ctxGetCert(from)) != 1 || ossl.ctxUsePkey(ctx, ossl.ctxGetKey(from)) != 1 ||
            (chain && ossl.ctxCtrl(ctx, TLS_CTRL_CHAIN, 1, chain) != 1)) {
            snprintf(what, sizeof what, "using the certificate %s", cert);
            goto fail;
        }
    } else if (ossl.ctxUseChain(ctx, cert) != 1) {
        snprintf(what, sizeof what, "reading the certificate %s", cert);
        goto fail;
    } else if (ossl.ctxUseKey(ctx, key, 1) != 1 || ossl.ctxCheckKey(ctx) != 1) {
        snprintf(what, sizeof what, "reading the key %s", key);
        goto fail;
    }
    if (s->clientCa) {
        if (ossl.ctxLoadVerify(ctx, s->clientCa, NULL) != 1) {
            snprintf(what, sizeof what, "reading the certificates to trust %s", s->clientCa);
            goto fail;
        }
        ossl.ctxSetVerify(ctx, TLS_VERIFY_PEER | (s->clientRequired ? TLS_VERIFY_FAIL_NO_CERT : 0), NULL);
        void *names = ossl.loadClientCaFile(s->clientCa);
        if (names) ossl.ctxSetClientCaList(ctx, names);
    }
    if (g->alpn && ossl.ctxSetAlpnSelect) ossl.ctxSetAlpnSelect(ctx, alpnSelect, g->alpn);
    return ctx;
fail:
    tlsSayOpenssl(what);
    ossl.ctxFree(ctx);
    return NULL;
}

/* Builds a generation from the server's settings and swaps it in: 0, or -1
   with the message set and the old one kept. With files, every certificate
   is read from its files; without, the ones loaded already are kept, and
   only those past them are read. */
static int serverBuild(TlsServer *s, bool files) {
    TlsGen *g = calloc(1, sizeof *g);
    if (!g) outOfMemory();
    g->refs = 1;
    g->ctxs = calloc((size_t)s->n, sizeof(void *));
    g->leafs = calloc((size_t)s->n, sizeof(void *));
    if (!g->ctxs || !g->leafs) outOfMemory();
    if (s->alpn) {
        size_t len = strlen((const char *)s->alpn);
        g->alpn = malloc(len + 1);
        if (!g->alpn) outOfMemory();
        memcpy(g->alpn, s->alpn, len + 1);
    }
    TlsGen *old = s->gen;           /* changes come one at a time */
    for (int i = 0; i < s->n; i++) {
        void *from = !files && old && i < old->n ? old->ctxs[i] : NULL;
        g->ctxs[i] = serverCtx(s, g, s->certs[i], s->keys[i], from);
        g->n = i + 1;
        if (!g->ctxs[i]) {
            genRelease(g);
            return -1;
        }
        g->leafs[i] = ossl.ctxGetCert(g->ctxs[i]);
    }
    ossl.ctxCallbackCtrl(g->ctxs[0], TLS_CTRL_SERVERNAME_CB, (void (*)(void))pickCert);
    ossl.ctxCtrl(g->ctxs[0], TLS_CTRL_SERVERNAME_ARG, 0, g);
    spinLock(&s->lock);
    s->gen = g;
    spinUnlock(&s->lock);
    genRelease(old);
    return 0;
}

static void serverAddFiles(TlsServer *s, char *cert, char *key) {
    char **c = realloc(s->certs, (size_t)(s->n + 1) * sizeof(char *));
    if (!c) outOfMemory();
    s->certs = c;
    char **k = realloc(s->keys, (size_t)(s->n + 1) * sizeof(char *));
    if (!k) outOfMemory();
    s->keys = k;
    s->certs[s->n] = cert;
    s->keys[s->n] = key;
    s->n++;
}

/* A server with one certificate chain and its key, from PEM files. */
void *iris_tls_server_ctx(const char *cert, int64_t certLen, const char *key, int64_t keyLen) {
    if (!tlsReady()) return NULL;
    char *c = cpath(cert, certLen), *k = cpath(key, keyLen);
    if (!c || !k) {
        tlsSay("a file name with a NUL in it");
        free(c);
        free(k);
        return NULL;
    }
    TlsServer *s = calloc(1, sizeof *s);
    if (!s) outOfMemory();
    serverAddFiles(s, c, k);
    if (serverBuild(s, true) != 0) {
        free(c);
        free(k);
        free(s->certs);
        free(s->keys);
        free(s);
        return NULL;
    }
    return s;
}

/* Another certificate, used for the names it has: 0, or -1. */
int iris_tls_server_add(void *server, const char *cert, int64_t certLen, const char *key, int64_t keyLen) {
    TlsServer *s = server;
    char *c = cpath(cert, certLen), *k = cpath(key, keyLen);
    if (!c || !k) {
        tlsSay("a file name with a NUL in it");
        free(c);
        free(k);
        return -1;
    }
    serverAddFiles(s, c, k);
    if (serverBuild(s, false) != 0) {
        s->n--;
        free(c);
        free(k);
        return -1;
    }
    return 0;
}

/* Reads every certificate and key again. */
int iris_tls_server_reload(void *server) { return serverBuild(server, true); }

/* Asks clients for a certificate signed by one in ca; required makes a
   client without one fail the handshake. */
int iris_tls_server_clients(void *server, const char *ca, int64_t caLen, int required) {
    TlsServer *s = server;
    char *path = cpath(ca, caLen), *old = s->clientCa;
    int oldRequired = s->clientRequired;
    if (!path) {
        tlsSay("a file name with a NUL in it");
        return -1;
    }
    s->clientCa = path;
    s->clientRequired = required;
    if (serverBuild(s, false) != 0) {
        s->clientCa = old;
        s->clientRequired = oldRequired;
        free(path);
        return -1;
    }
    free(old);
    return 0;
}

/* The protocols to offer with ALPN, each with its length in front. */
int iris_tls_server_alpn(void *server, const char *protos, int64_t len) {
    TlsServer *s = server;
    if (memchr(protos, 0, (size_t)len)) {
        tlsSay("a protocol list with a 0 byte in it");
        return -1;
    }
    for (int64_t i = 0; i < len; i += 1 + (unsigned char)protos[i]) {
        if (i + 1 + (unsigned char)protos[i] > len) {
            tlsSay("a protocol list whose lengths don't add up");
            return -1;
        }
    }
    if (s->alpn && strlen((const char *)s->alpn) == (size_t)len && memcmp(s->alpn, protos, (size_t)len) == 0) {
        return 0;
    }
    unsigned char *old = s->alpn, *p = malloc((size_t)len + 1);
    if (!p) outOfMemory();
    memcpy(p, protos, (size_t)len);
    p[len] = 0;
    s->alpn = len > 0 ? p : NULL;
    if (len == 0) free(p);
    if (serverBuild(s, false) != 0) {
        free(s->alpn);
        s->alpn = old;
        return -1;
    }
    free(old);
    return 0;
}

/* Lets clients of a server ask for HTTP/2. */
void iris_tls_ctx_http2(void *server) {
    static const char protos[] = "\x02h2\x08http/1.1";
    if (server) iris_tls_server_alpn(server, protos, (int64_t)sizeof protos - 1);
}

void iris_tls_ctx_free(void *server) {
    TlsServer *s = server;
    if (!s || ossl.loaded != 1) return;
    genRelease(s->gen);
    for (int i = 0; i < s->n; i++) {
        free(s->certs[i]);
        free(s->keys[i]);
    }
    free(s->certs);
    free(s->keys);
    free(s->clientCa);
    free(s->alpn);
    free(s);
}

/* The protocol the two sides picked with ALPN: "" for none. */
void iris_tls_alpn(void *ssl, const char **p, int64_t *n) {
    const unsigned char *data = NULL;
    unsigned len = 0;
    if (ssl && ossl.sslGetAlpn) ossl.sslGetAlpn(ssl, &data, &len);
    *p = data ? (const char *)data : "";
    *n = data ? len : 0;
}

/* The name the client asked for (SNI): "" when it sent none. */
void iris_tls_server_name(void *ssl, const char **p, int64_t *n) {
    const char *name = ssl ? ossl.getServername(ssl, 0) : NULL;
    *p = name ? name : "";
    *n = name ? (int64_t)strlen(name) : 0;
}

/* The subject of the other side's certificate, "/CN=..." : 0 when it sent
   none. */
int iris_tls_peer_subject(void *ssl, IrisString *out) {
    void *x = ssl ? ossl.getPeerCert(ssl) : NULL;
    if (!x) return 0;
    char buf[512];
    char *t = ossl.nameOneline(ossl.x509Subject(x), buf, sizeof buf);
    if (t) iris_string_append(out, t, (int64_t)strlen(t));
    ossl.x509Free(x);
    return 1;
}

/* The server side of a handshake on an accepted socket. */
void *iris_tls_accept(void *server, int fd, int64_t timeout) {
    int64_t deadline = deadlineOf(timeout);
    if (!tlsReady()) return NULL;
    TlsServer *s = server;
    spinLock(&s->lock);
    TlsGen *g = s->gen;
    __atomic_add_fetch(&g->refs, 1, __ATOMIC_ACQ_REL);
    spinUnlock(&s->lock);
    void *ssl = ossl.sslNew(g->ctxs[0]);
    if (!ssl) {
        tlsSayOpenssl("starting TLS");
        genRelease(g);
        return NULL;
    }
    tlsSetSock(ssl, fd);
    for (;;) {
        PipeGuard pg;
        pipeHold(&pg);
        int r = ossl.sslAccept(ssl);
        pipeRelease(&pg);
        if (r == 1) {
            genRelease(g);      /* the connection holds its contexts itself */
            return ssl;
        }
        bool timedOut = false;
        if (tlsWaitUntil(ssl, r, deadline, &timedOut)) continue;
        long v = ossl.sslVerifyResult(ssl);
        if (v != 0) {
            char what[400];
            snprintf(what, sizeof what, "the client's certificate can't be trusted: %s", ossl.verifyString(v));
            tlsSay(what);
            if (ossl.errClear) ossl.errClear();
        } else {
            tlsSayOpenssl(timedOut ? "the TLS handshake timed out" : "the TLS handshake failed");
        }
        ossl.sslFree(ssl);
        genRelease(g);
        return NULL;
    }
}

/* Bytes read (0 when the other side closed), or minus net's error code. */
int64_t iris_tls_read(void *ssl, char *buf, int64_t len, int64_t timeout) {
    int n = len > 0x40000000 ? 0x40000000 : (int)len;
    int64_t deadline = deadlineOf(timeout);
    for (;;) {
        PipeGuard g;
        pipeHold(&g);
        int r = ossl.sslRead(ssl, buf, n);
        pipeRelease(&g);
        if (r > 0) return r;
        bool timedOut = false;
        if (tlsWaitUntil(ssl, r, deadline, &timedOut)) continue;
        if (timedOut) return -3;
        int e = ossl.sslGetError(ssl, r);
        if (e == TLS_ZERO_RETURN || (e == TLS_ERROR_SYSCALL && errno == 0)) {
            if (ossl.errClear) ossl.errClear();
            return 0;
        }
        tlsSayOpenssl("reading over TLS");
        return -7;
    }
}

/* 0 when everything was written, else net's error code. */
int iris_tls_write(void *ssl, const char *data, int64_t len, int64_t timeout) {
    int64_t deadline = deadlineOf(timeout);
    while (len > 0) {
        int n = len > 0x40000000 ? 0x40000000 : (int)len;
        PipeGuard g;
        pipeHold(&g);
        int r = ossl.sslWrite(ssl, data, n);
        pipeRelease(&g);
        if (r > 0) {
            data += r;
            len -= r;
            continue;
        }
        bool timedOut = false;
        if (tlsWaitUntil(ssl, r, deadline, &timedOut)) continue;
        if (timedOut) return 3;
        tlsSayOpenssl("writing over TLS");
        return 7;
    }
    return 0;
}

void iris_tls_close(void *ssl) {
    if (!ssl) return;
    PipeGuard g;
    pipeHold(&g);
    ossl.sslShutdown(ssl);      /* one try: the socket closes right after */
    pipeRelease(&g);
    if (ossl.errClear) ossl.errClear();
    ossl.sslFree(ssl);
}

/* Ends this side's writes over TLS: close_notify, then the socket's write
   side. Reads go on. 0, or a net code. */
int iris_tls_close_write(void *ssl, int64_t timeout) {
    int64_t deadline = deadlineOf(timeout);
    for (;;) {
        PipeGuard g;
        pipeHold(&g);
        int r = ossl.sslShutdown(ssl);
        pipeRelease(&g);
        if (r >= 0) break;
        bool timedOut = false;
        if (tlsWaitUntil(ssl, r, deadline, &timedOut)) continue;
        if (ossl.errClear) ossl.errClear();
        if (timedOut) return 3;
        break;              /* the other side is gone: nothing left to end */
    }
    shutdown(ossl.sslGetFd(ssl), SHUT_WR);
    return 0;
}

/* ---------------------------------------------------------------- copying between connections (0.67)

   net.copy on Linux moves the bytes from one socket to the other through a
   pipe with splice, so they never come up to the program. Each thread keeps
   a few empty pipes for it. A splice into a socket the other side closed
   raises SIGPIPE, which splice can't be told not to; a thread that splices
   keeps SIGPIPE blocked from then on, and one that comes is taken back.

   A pipe is made 256 KiB (the default is 64 KiB), so a stream moves in
   fewer, larger splices of 256 KiB. The kernel lets a user keep only so
   many pages in pipes (fs/pipe-user-pages-soft, 64 MiB unless set), so at
   most 128 pipes in the process are made larger; the others keep the
   default size, as does one the kernel won't grow. */

#ifdef __linux__
#define PIPE_BIG (1 << 18)
#define PIPE_BIG_MAX 128
#define SPLICE_CHUNK (1 << 18)
static _Atomic int pipesBig;
static _Thread_local int pipeFree[16][2];
static _Thread_local int pipeFreeN;
static _Thread_local bool pipeBlocked;

static bool pipeGet(int p[2]) {
    if (pipeFreeN > 0) {
        pipeFreeN--;
        p[0] = pipeFree[pipeFreeN][0];
        p[1] = pipeFree[pipeFreeN][1];
        return true;
    }
    if (pipe2(p, O_NONBLOCK | O_CLOEXEC) != 0) return false;
    if (atomic_fetch_add(&pipesBig, 1) < PIPE_BIG_MAX && fcntl(p[1], F_SETPIPE_SZ, PIPE_BIG) >= PIPE_BIG) {
        return true;
    }
    atomic_fetch_sub(&pipesBig, 1);
    return true;
}

static void pipePut(int p[2], bool empty) {
    if (empty && pipeFreeN < 16) {
        pipeFree[pipeFreeN][0] = p[0];
        pipeFree[pipeFreeN][1] = p[1];
        pipeFreeN++;
        return;
    }
    if (fcntl(p[1], F_GETPIPE_SZ) >= PIPE_BIG) atomic_fetch_sub(&pipesBig, 1);
    close(p[0]);
    close(p[1]);
}

static void sigpipeQuiet(void) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    if (!pipeBlocked) {
        pthread_sigmask(SIG_BLOCK, &set, NULL);
        pipeBlocked = true;
    }
}

static void sigpipeTake(void) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    struct timespec zero = {0, 0};
    sigtimedwait(&set, NULL, &zero);
}
#endif

/* Copies from one socket to another until the first ends or limit bytes
   (0 for no limit) have gone: the bytes copied. *err is 0, or a net code,
   and *side says which socket failed: 0 reading, 1 writing.

   Small messages go through buf with a plain read, which waits in the
   event loop for data. A read that fills buf means a stream is flowing,
   and on Linux the copy then moves the bytes with splice through a pipe,
   in the kernel, waiting between splices, until several splices in a row
   move little; then it reads again.

   With active (net.relay), the two directions of a connection share an
   idle limit: active is when bytes last moved either way, set on each
   move, and a wait gives up only once nothing moved for idle. */
static int64_t relayLeft(const int64_t *active, int64_t idle, int64_t timeout) {
    if (!active) return timeout;
    int64_t left = *active + idle - nowNs();
    return left > 0 ? left : -1;
}

/* After a wait that ran out: whether the other direction moved meanwhile,
   so the wait goes on. */
static bool relayMoved(const int64_t *active, int64_t idle) { return active && *active + idle > nowNs(); }

int64_t iris_net_relay(int to, int from, char *buf, int64_t bufLen, int64_t limit, int64_t readTimeout,
                       int64_t writeTimeout, int *err, int *side, int64_t *active, int64_t idle) {
    *err = 0;
    *side = 0;
    int64_t total = 0;
#ifdef __linux__
    /* a copy holds a pipe only while bytes are in it, so a thread needs as
       many pipes as copies moving a stream at once */
    int p[2] = {-1, -1};
    int64_t inPipe = 0;
    bool stream = false;
    int small = 0;          /* splices in a row that moved under 4 KiB */
#endif
    for (;;) {
#ifdef __linux__
        if (stream || inPipe > 0) {
            if (inPipe == 0) {
                if (limit > 0 && total >= limit) break;
                if (p[0] < 0) {
                    if (!pipeGet(p)) {
                        stream = false;
                        continue;
                    }
                    sigpipeQuiet();
                }
                size_t want = limit > 0 && limit - total < SPLICE_CHUNK ? (size_t)(limit - total) : SPLICE_CHUNK;
                ssize_t n = splice(from, NULL, p[1], NULL, want, SPLICE_F_MOVE | SPLICE_F_NONBLOCK);
                if (n == 0) break;
                if (n < 0) {
                    if (errno == EAGAIN || errno == EINTR) {
                        pipePut(p, true);
                        p[0] = p[1] = -1;
                        if (small >= 8) {
                            /* small messages again: back to reads, which wait in the loop */
                            stream = false;
                            small = 0;
                            continue;
                        }
                        /* a stream that paused: wait for it here */
                        bool ready;
                        do {
                            int64_t t = relayLeft(active, idle, readTimeout);
                            ready = t >= 0 && waitFdUntil(from, 0, deadlineOf(t));
                        } while (!ready && relayMoved(active, idle));
                        if (!ready) {
                            *err = 3;
                            break;
                        }
                        continue;
                    }
                    *err = netCode(errno);
                    break;
                }
                inPipe = n;
                small = n < 4096 ? small + 1 : 0;
            }
            ssize_t m = splice(p[0], NULL, to, NULL, (size_t)inPipe, SPLICE_F_MOVE | SPLICE_F_NONBLOCK);
            if (m < 0) {
                if (errno == EAGAIN || errno == EINTR) {
                    bool ready;
                    do {
                        int64_t t = relayLeft(active, idle, writeTimeout);
                        ready = t >= 0 && waitFdUntil(to, 1, deadlineOf(t));
                    } while (!ready && relayMoved(active, idle));
                    if (!ready) {
                        *err = 3;
                        *side = 1;
                        break;
                    }
                    continue;
                }
                if (errno == EPIPE) sigpipeTake();
                *err = netCode(errno);
                *side = 1;
                break;
            }
            inPipe -= m;
            total += m;
            if (active) *active = nowNs();
            continue;
        }
#endif
        if (limit > 0 && total >= limit) break;
        int64_t want = limit > 0 && limit - total < bufLen ? limit - total : bufLen;
        int64_t n;
        do {
            int64_t t = relayLeft(active, idle, readTimeout);
            n = t >= 0 ? iris_net_read(from, buf, want, t) : -3;
        } while (n == -3 && relayMoved(active, idle));
        if (n == 0) break;
        if (n < 0) {
            *err = (int)-n;
            break;
        }
        if (active) *active = nowNs();
        int64_t wt = relayLeft(active, idle, writeTimeout);
        int code = wt >= 0 ? iris_net_write(to, buf, n, wt) : 3;
        if (code != 0) {
            *err = code;
            *side = 1;
            break;
        }
        total += n;
        if (active) *active = nowNs();
#ifdef __linux__
        stream = n == bufLen;
#endif
    }
#ifdef __linux__
    if (p[0] >= 0) pipePut(p, inPipe == 0);
#endif
    return total;
}

/* ---------------------------------------------------------------- random and crypto */

#ifdef __linux__
#include <sys/random.h>
#endif

/* Bytes from the operating system's secure source. */
void iris_random_os(char *buf, int64_t len) {
#ifdef __APPLE__
    arc4random_buf(buf, (size_t)len);
#else
    while (len > 0) {
        ssize_t n = getrandom(buf, (size_t)(len > 256 ? 256 : len), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            fflush(stdout);
            fputs("panic: the system's random source failed\n", stderr);
            exit(2);
        }
        buf += n;
        len -= n;
    }
#endif
}

/* SHA-256 (FIPS 180-4). */
typedef struct {
    uint32_t h[8];
    uint64_t len;
    unsigned char block[64];
    size_t used;
} Sha256;

static const uint32_t shaK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void shaBlock(Sha256 *s, const unsigned char *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + shaK[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void shaInit(Sha256 *s) {
    static const uint32_t start[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, start, sizeof start);
    s->len = 0;
    s->used = 0;
}

static int shaHwBlocks(uint32_t h[8], const unsigned char *p, size_t blocks);

static void shaAdd(Sha256 *s, const unsigned char *p, size_t n) {
    s->len += n;
    if (s->used == 0 && n >= 64) {
        /* whole blocks straight from p, with the CPU's SHA instructions
           when it has them */
        size_t blocks = n / 64;
        if (!shaHwBlocks(s->h, p, blocks)) {
            for (size_t i = 0; i < blocks; i++) shaBlock(s, p + 64 * i);
        }
        p += 64 * blocks;
        n -= 64 * blocks;
    }
    while (n > 0) {
        size_t take = 64 - s->used < n ? 64 - s->used : n;
        memcpy(s->block + s->used, p, take);
        s->used += take;
        p += take;
        n -= take;
        if (s->used == 64) {
            if (!shaHwBlocks(s->h, s->block, 1)) shaBlock(s, s->block);
            s->used = 0;
        }
    }
}

static void shaEnd(Sha256 *s, unsigned char out[32]) {
    uint64_t bits = s->len * 8;
    /* 0x80, zeros up to 56 bytes into a block, then the length in bits */
    unsigned char pad[72] = {0x80};
    size_t zeros = (s->used < 56 ? 56 - s->used : 120 - s->used) - 1;
    for (int i = 0; i < 8; i++) pad[1 + zeros + i] = (unsigned char)(bits >> (56 - 8 * i));
    shaAdd(s, pad, 1 + zeros + 8);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (unsigned char)(s->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(s->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(s->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)s->h[i];
    }
}

void iris_sha256(const char *data, int64_t len, unsigned char *out) {
    Sha256 s;
    shaInit(&s);
    shaAdd(&s, (const unsigned char *)data, (size_t)len);
    shaEnd(&s, out);
}

/* HMAC-SHA256 (RFC 2104). */
void iris_hmac_sha256(const char *key, int64_t keyLen, const char *data, int64_t len, unsigned char *out) {
    unsigned char k[64] = {0};
    if (keyLen > 64) {
        iris_sha256(key, keyLen, k);
    } else {
        memcpy(k, key, (size_t)keyLen);
    }
    unsigned char ipad[64], opad[64], inner[32];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    Sha256 s;
    shaInit(&s);
    shaAdd(&s, ipad, 64);
    shaAdd(&s, (const unsigned char *)data, (size_t)len);
    shaEnd(&s, inner);
    shaInit(&s);
    shaAdd(&s, opad, 64);
    shaAdd(&s, inner, 32);
    shaEnd(&s, out);
}

/* Overwrites n bytes at p with zeros in a way the compiler can't leave
   out, for keys and passwords that are done with. */
void iris_wipe(void *p, int64_t n) {
    volatile unsigned char *v = p;
    for (int64_t i = 0; i < n; i++) v[i] = 0;
    __asm__ __volatile__("" : : "r"(p) : "memory");
}

/* Equal without stopping at the first difference, so the time says
   nothing about where they differ. */
int iris_const_equal(const char *a, int64_t na, const char *b, int64_t nb) {
    if (na != nb) return 0;
    unsigned char d = 0;
    for (int64_t i = 0; i < na; i++) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}

/* SHA-512 (FIPS 180-4). */
typedef struct {
    uint64_t h[8];
    unsigned char block[128];
    size_t used;
    uint64_t len;
} Sha512;

static const uint64_t sha512K[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL,
    0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
    0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL, 0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL, 0x983e5152ee66dfabULL,
    0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL,
    0x53380d139d95b3dfULL, 0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL, 0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
    0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL,
    0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL, 0xca273eceea26619cULL,
    0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
    0x113f9804bef90daeULL, 0x1b710b35131c471bULL, 0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

#define ROR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

/* One round, with the eight working variables renamed instead of moved:
   Ch as g ^ (e & (f ^ g)), Maj as (a & b) | (c & (a | b)). */
#define SHA512_ROUND(a, b, c, d, e, f, g, h, k)                                                         \
    do {                                                                                                 \
        uint64_t t1 = h + (k);                                                                           \
        t1 += g ^ (e & (f ^ g));                                                                         \
        t1 += ROR64(e, 14) ^ ROR64(e, 18) ^ ROR64(e, 41);                                                \
        uint64_t t2 = (ROR64(a, 28) ^ ROR64(a, 34) ^ ROR64(a, 39)) + ((a & b) | (c & (a | b)));        \
        d += t1;                                                                                         \
        h = t1 + t2;                                                                                     \
    } while (0)

#define SHA512_W(w, i)                                                                                    \
    (w[(i) & 15] += w[((i) - 7) & 15] +                                                                  \
                    (ROR64(w[((i) - 15) & 15], 1) ^ ROR64(w[((i) - 15) & 15], 8) ^ (w[((i) - 15) & 15] >> 7)) + \
                    (ROR64(w[((i) - 2) & 15], 19) ^ ROR64(w[((i) - 2) & 15], 61) ^ (w[((i) - 2) & 15] >> 6)))

/* Each word of the message schedule is made in the round that takes it,
   so the CPU works on it while the round waits for the one before. */
static inline __attribute__((always_inline)) void sha512BlockBody(Sha512 *s, const unsigned char *p) {
    uint64_t w[16];
    for (int i = 0; i < 16; i++) {
        uint64_t v;
        memcpy(&v, p + 8 * i, 8);
        w[i] = __builtin_bswap64(v);
    }
    uint64_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    SHA512_ROUND(a, b, c, d, e, f, g, h, sha512K[0] + w[0]);
    SHA512_ROUND(h, a, b, c, d, e, f, g, sha512K[1] + w[1]);
    SHA512_ROUND(g, h, a, b, c, d, e, f, sha512K[2] + w[2]);
    SHA512_ROUND(f, g, h, a, b, c, d, e, sha512K[3] + w[3]);
    SHA512_ROUND(e, f, g, h, a, b, c, d, sha512K[4] + w[4]);
    SHA512_ROUND(d, e, f, g, h, a, b, c, sha512K[5] + w[5]);
    SHA512_ROUND(c, d, e, f, g, h, a, b, sha512K[6] + w[6]);
    SHA512_ROUND(b, c, d, e, f, g, h, a, sha512K[7] + w[7]);
    SHA512_ROUND(a, b, c, d, e, f, g, h, sha512K[8] + w[8]);
    SHA512_ROUND(h, a, b, c, d, e, f, g, sha512K[9] + w[9]);
    SHA512_ROUND(g, h, a, b, c, d, e, f, sha512K[10] + w[10]);
    SHA512_ROUND(f, g, h, a, b, c, d, e, sha512K[11] + w[11]);
    SHA512_ROUND(e, f, g, h, a, b, c, d, sha512K[12] + w[12]);
    SHA512_ROUND(d, e, f, g, h, a, b, c, sha512K[13] + w[13]);
    SHA512_ROUND(c, d, e, f, g, h, a, b, sha512K[14] + w[14]);
    SHA512_ROUND(b, c, d, e, f, g, h, a, sha512K[15] + w[15]);
#pragma clang loop unroll(disable)
    for (int i = 16; i < 80; i += 16) {
        SHA512_ROUND(a, b, c, d, e, f, g, h, sha512K[i + 0] + SHA512_W(w, 0));
        SHA512_ROUND(h, a, b, c, d, e, f, g, sha512K[i + 1] + SHA512_W(w, 1));
        SHA512_ROUND(g, h, a, b, c, d, e, f, sha512K[i + 2] + SHA512_W(w, 2));
        SHA512_ROUND(f, g, h, a, b, c, d, e, sha512K[i + 3] + SHA512_W(w, 3));
        SHA512_ROUND(e, f, g, h, a, b, c, d, sha512K[i + 4] + SHA512_W(w, 4));
        SHA512_ROUND(d, e, f, g, h, a, b, c, sha512K[i + 5] + SHA512_W(w, 5));
        SHA512_ROUND(c, d, e, f, g, h, a, b, sha512K[i + 6] + SHA512_W(w, 6));
        SHA512_ROUND(b, c, d, e, f, g, h, a, sha512K[i + 7] + SHA512_W(w, 7));
        SHA512_ROUND(a, b, c, d, e, f, g, h, sha512K[i + 8] + SHA512_W(w, 8));
        SHA512_ROUND(h, a, b, c, d, e, f, g, sha512K[i + 9] + SHA512_W(w, 9));
        SHA512_ROUND(g, h, a, b, c, d, e, f, sha512K[i + 10] + SHA512_W(w, 10));
        SHA512_ROUND(f, g, h, a, b, c, d, e, sha512K[i + 11] + SHA512_W(w, 11));
        SHA512_ROUND(e, f, g, h, a, b, c, d, sha512K[i + 12] + SHA512_W(w, 12));
        SHA512_ROUND(d, e, f, g, h, a, b, c, sha512K[i + 13] + SHA512_W(w, 13));
        SHA512_ROUND(c, d, e, f, g, h, a, b, sha512K[i + 14] + SHA512_W(w, 14));
        SHA512_ROUND(b, c, d, e, f, g, h, a, sha512K[i + 15] + SHA512_W(w, 15));
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

/* With BMI2, rorx rotates into another register, which saves a mov for
   most of the rotates. */
#if defined(__x86_64__)
__attribute__((target("bmi2"))) static void sha512BlockBmi2(Sha512 *s, const unsigned char *p) {
    sha512BlockBody(s, p);
}

static int sha512Bmi2 = -1;
#endif

static void sha512Block(Sha512 *s, const unsigned char *p) {
#if defined(__x86_64__)
    if (sha512Bmi2 < 0) {
        __builtin_cpu_init();
        sha512Bmi2 = __builtin_cpu_supports("bmi2");
    }
    if (sha512Bmi2) {
        sha512BlockBmi2(s, p);
        return;
    }
#endif
    sha512BlockBody(s, p);
}

static void sha512Init(Sha512 *s) {
    static const uint64_t start[8] = {0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
                                      0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
                                      0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};
    memcpy(s->h, start, sizeof start);
    s->len = 0;
    s->used = 0;
}

static void sha512Add(Sha512 *s, const unsigned char *p, size_t n) {
    s->len += n;
    if (s->used > 0) {
        size_t take = 128 - s->used < n ? 128 - s->used : n;
        memcpy(s->block + s->used, p, take);
        s->used += take;
        p += take;
        n -= take;
        if (s->used < 128) return;
        sha512Block(s, s->block);
        s->used = 0;
    }
    /* whole blocks straight from p */
    for (; n >= 128; p += 128, n -= 128) sha512Block(s, p);
    memcpy(s->block, p, n);
    s->used = n;
}

static void sha512End(Sha512 *s, unsigned char out[64]) {
    uint64_t bits = s->len * 8;
    s->block[s->used++] = 0x80;
    if (s->used > 112) {
        memset(s->block + s->used, 0, 128 - s->used);
        sha512Block(s, s->block);
        s->used = 0;
    }
    memset(s->block + s->used, 0, 120 - s->used);
    for (int i = 0; i < 8; i++) s->block[120 + i] = (unsigned char)(bits >> (56 - 8 * i));
    sha512Block(s, s->block);
    for (int i = 0; i < 8; i++) {
        for (int k = 0; k < 8; k++) out[i * 8 + k] = (unsigned char)(s->h[i] >> (56 - 8 * k));
    }
}

void iris_sha512(const char *data, int64_t len, unsigned char *out) {
    Sha512 s;
    sha512Init(&s);
    sha512Add(&s, (const unsigned char *)data, (size_t)len);
    sha512End(&s, out);
}

/* HMAC-SHA512 (RFC 2104), with SHA-512's 128-byte blocks. */
void iris_hmac_sha512(const char *key, int64_t keyLen, const char *data, int64_t len, unsigned char *out) {
    unsigned char k[128] = {0};
    if (keyLen > 128) {
        iris_sha512(key, keyLen, k);
    } else {
        memcpy(k, key, (size_t)keyLen);
    }
    unsigned char ipad[128], opad[128], inner[64];
    for (int i = 0; i < 128; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    Sha512 s;
    sha512Init(&s);
    sha512Add(&s, ipad, 128);
    sha512Add(&s, (const unsigned char *)data, (size_t)len);
    sha512End(&s, inner);
    sha512Init(&s);
    sha512Add(&s, opad, 128);
    sha512Add(&s, inner, 64);
    sha512End(&s, out);
}

/* X25519 key exchange (RFC 7748). Its field elements have five limbs of
   51 bits, multiplied into 128-bit sums. No branch or index depends on the
   secret. */
typedef unsigned __int128 X128;
typedef uint64_t F51[5];
#define M51 0x7ffffffffffffULL

static uint64_t x25519Le64(const unsigned char *p) {
    uint64_t v;
    memcpy(&v, p, 8);
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = __builtin_bswap64(v);
#endif
    return v;
}

static void f51Unpack(F51 r, const unsigned char *s) {
    r[0] = x25519Le64(s) & M51;
    r[1] = (x25519Le64(s + 6) >> 3) & M51;
    r[2] = (x25519Le64(s + 12) >> 6) & M51;
    r[3] = (x25519Le64(s + 19) >> 1) & M51;
    r[4] = (x25519Le64(s + 24) >> 12) & M51;   /* the top bit is ignored */
}

/* the five 128-bit sums carried into limbs of 51 bits */
static void f51Carry(F51 r, X128 t0, X128 t1, X128 t2, X128 t3, X128 t4) {
    t1 += t0 >> 51;
    t2 += t1 >> 51;
    t3 += t2 >> 51;
    t4 += t3 >> 51;
    X128 r0 = ((uint64_t)t0 & M51) + (t4 >> 51) * 19;
    r[1] = ((uint64_t)t1 & M51) + (uint64_t)(r0 >> 51);
    r[0] = (uint64_t)r0 & M51;
    r[2] = (uint64_t)t2 & M51;
    r[3] = (uint64_t)t3 & M51;
    r[4] = (uint64_t)t4 & M51;
}

static void f51Mul(F51 r, const F51 a, const F51 b) {
    uint64_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3], a4 = a[4];
    uint64_t b0 = b[0], b1 = b[1], b2 = b[2], b3 = b[3], b4 = b[4];
    uint64_t b1_19 = b1 * 19, b2_19 = b2 * 19, b3_19 = b3 * 19, b4_19 = b4 * 19;
    X128 t0 = (X128)a0 * b0 + (X128)a1 * b4_19 + (X128)a2 * b3_19 + (X128)a3 * b2_19 + (X128)a4 * b1_19;
    X128 t1 = (X128)a0 * b1 + (X128)a1 * b0 + (X128)a2 * b4_19 + (X128)a3 * b3_19 + (X128)a4 * b2_19;
    X128 t2 = (X128)a0 * b2 + (X128)a1 * b1 + (X128)a2 * b0 + (X128)a3 * b4_19 + (X128)a4 * b3_19;
    X128 t3 = (X128)a0 * b3 + (X128)a1 * b2 + (X128)a2 * b1 + (X128)a3 * b0 + (X128)a4 * b4_19;
    X128 t4 = (X128)a0 * b4 + (X128)a1 * b3 + (X128)a2 * b2 + (X128)a3 * b1 + (X128)a4 * b0;
    f51Carry(r, t0, t1, t2, t3, t4);
}

static void f51Sq(F51 r, const F51 a) {
    uint64_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3], a4 = a[4];
    uint64_t d0 = a0 * 2, d1 = a1 * 2, d2_19 = a2 * 38, a3_19 = a3 * 19, a4_19 = a4 * 19, d4_19 = a4 * 38;
    X128 t0 = (X128)a0 * a0 + (X128)d4_19 * a1 + (X128)d2_19 * a3;
    X128 t1 = (X128)d0 * a1 + (X128)d4_19 * a2 + (X128)a3 * a3_19;
    X128 t2 = (X128)d0 * a2 + (X128)a1 * a1 + (X128)d4_19 * a3;
    X128 t3 = (X128)d0 * a3 + (X128)d1 * a2 + (X128)a4 * a4_19;
    X128 t4 = (X128)d0 * a4 + (X128)d1 * a3 + (X128)a2 * a2;
    f51Carry(r, t0, t1, t2, t3, t4);
}

static void f51SqN(F51 r, const F51 a, int n) {
    f51Sq(r, a);
    while (--n > 0) f51Sq(r, r);
}

static void f51MulSmall(F51 r, const F51 a, uint64_t k) {
    f51Carry(r, (X128)a[0] * k, (X128)a[1] * k, (X128)a[2] * k, (X128)a[3] * k, (X128)a[4] * k);
}

static void f51Add(F51 r, const F51 a, const F51 b) {
    for (int i = 0; i < 5; i++) r[i] = a[i] + b[i];
}

/* a - b, with 4p added so no limb goes below zero: b's limbs must be under
   2^53, as what f51Mul, f51Sq and f51Carry give and sums of two of them are */
static void f51Sub(F51 r, const F51 a, const F51 b) {
    r[0] = a[0] + 0x1fffffffffffb4ULL - b[0];
    for (int i = 1; i < 5; i++) r[i] = a[i] + 0x1ffffffffffffcULL - b[i];
}

static void f51Swap(F51 a, F51 b, uint64_t bit) {
    uint64_t mask = 0 - bit;
    for (int i = 0; i < 5; i++) {
        uint64_t t = mask & (a[i] ^ b[i]);
        a[i] ^= t;
        b[i] ^= t;
    }
}

/* z^(p - 2), which is 1/z */
static void f51Invert(F51 r, const F51 z) {
    F51 z2, z9, z11, z5, z10, z20, z50, z100, t;
    f51Sq(z2, z);
    f51SqN(t, z2, 2);
    f51Mul(z9, t, z);
    f51Mul(z11, z9, z2);
    f51Sq(t, z11);
    f51Mul(z5, t, z9);          /* 2^5 - 1 */
    f51SqN(t, z5, 5);
    f51Mul(z10, t, z5);         /* 2^10 - 1 */
    f51SqN(t, z10, 10);
    f51Mul(z20, t, z10);
    f51SqN(t, z20, 20);
    f51Mul(t, t, z20);          /* 2^40 - 1 */
    f51SqN(t, t, 10);
    f51Mul(z50, t, z10);
    f51SqN(t, z50, 50);
    f51Mul(z100, t, z50);
    f51SqN(t, z100, 100);
    f51Mul(t, t, z100);         /* 2^200 - 1 */
    f51SqN(t, t, 50);
    f51Mul(t, t, z50);          /* 2^250 - 1 */
    f51SqN(t, t, 5);
    f51Mul(r, t, z11);          /* 2^255 - 21 */
}

/* the one value below p, as 32 bytes */
static void f51Pack(unsigned char *out, const F51 a) {
    uint64_t t[5] = {a[0], a[1], a[2], a[3], a[4]};
    for (int k = 0; k < 2; k++) {
        for (int i = 0; i < 4; i++) {
            t[i + 1] += t[i] >> 51;
            t[i] &= M51;
        }
        t[0] += 19 * (t[4] >> 51);
        t[4] &= M51;
    }
    /* t + 19 reaches 2^255 when t >= p: then that is the carry to take away */
    t[0] += 19;
    for (int i = 0; i < 4; i++) {
        t[i + 1] += t[i] >> 51;
        t[i] &= M51;
    }
    t[0] += 19 * (t[4] >> 51);
    t[4] &= M51;
    /* take the 19 back: add 2^255 - 19 and drop 2^255 */
    t[0] += 0x8000000000000ULL - 19;
    for (int i = 1; i < 5; i++) t[i] += 0x8000000000000ULL - 1;
    for (int i = 0; i < 4; i++) {
        t[i + 1] += t[i] >> 51;
        t[i] &= M51;
    }
    t[4] &= M51;
    uint64_t o[4] = {t[0] | t[1] << 51, t[1] >> 13 | t[2] << 38, t[2] >> 26 | t[3] << 25, t[3] >> 39 | t[4] << 12};
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 8; j++) out[8 * i + j] = (unsigned char)(o[i] >> (8 * j));
    }
}

/* Gives 0 when the result is all zeros: the peer's key was one of the few
   points that give away nothing secret. */
int iris_x25519(const unsigned char *scalar, const unsigned char *point, unsigned char *out) {
    unsigned char z[32];
    memcpy(z, scalar, 32);
    z[31] = (unsigned char)((z[31] & 127) | 64);
    z[0] &= 248;
    F51 x, a = {1}, b, c = {0}, d = {1}, e, f;
    f51Unpack(x, point);
    memcpy(b, x, sizeof b);
    uint64_t swap = 0;
    for (int i = 254; i >= 0; i--) {
        uint64_t bit = (z[i >> 3] >> (i & 7)) & 1;
        swap ^= bit;
        f51Swap(a, b, swap);
        f51Swap(c, d, swap);
        swap = bit;
        f51Add(e, a, c);
        f51Sub(a, a, c);
        f51Add(c, b, d);
        f51Sub(b, b, d);
        f51Sq(d, e);
        f51Sq(f, a);
        f51Mul(a, c, a);
        f51Mul(c, b, e);
        f51Add(e, a, c);
        f51Sub(a, a, c);
        f51Sq(b, a);
        f51Sub(c, d, f);
        f51MulSmall(a, c, 121665);
        f51Add(a, a, d);
        f51Mul(c, c, a);
        f51Mul(a, d, f);
        f51Mul(d, b, x);
        f51Sq(b, e);
    }
    f51Swap(a, b, swap);
    f51Swap(c, d, swap);
    f51Invert(c, c);
    f51Mul(a, a, c);
    f51Pack(out, a);
    iris_wipe(z, sizeof z);
    unsigned char any = 0;
    for (int i = 0; i < 32; i++) any |= out[i];
    return any != 0;
}


/* Ed25519 signatures (RFC 8032) on the 51-bit field of X25519 above. A
   point is (X, Y, Z, T) in extended coordinates: x = X/Z, y = Y/Z and
   xy = T/Z. Public keys and signatures multiply the base point with a
   table of its multiples, read whole for each digit, so no branch or index
   depends on the secret. Checking a signature works on public values only
   and takes a path that branches on them. */
typedef struct { F51 X, Y, Z, T; } EdPoint;
/* a point to add: y + x, y - x and 2dxy, with z = 1 */
typedef struct { F51 yp, ym, t2d; } EdNiels;
/* the same with its Z, for a point that isn't affine */
typedef struct { F51 yp, ym, Z, t2d; } EdCached;

static const unsigned char edDBytes[32] = {
    0xa3, 0x78, 0x59, 0x13, 0xca, 0x4d, 0xeb, 0x75, 0xab, 0xd8, 0x41, 0x41, 0x4d, 0x0a, 0x70, 0x00,
    0x98, 0xe8, 0x79, 0x77, 0x79, 0x40, 0xc7, 0x8c, 0x73, 0xfe, 0x6f, 0x2b, 0xee, 0x6c, 0x03, 0x52};
static const unsigned char edD2Bytes[32] = {
    0x59, 0xf1, 0xb2, 0x26, 0x94, 0x9b, 0xd6, 0xeb, 0x56, 0xb1, 0x83, 0x82, 0x9a, 0x14, 0xe0, 0x00,
    0x30, 0xd1, 0xf3, 0xee, 0xf2, 0x80, 0x8e, 0x19, 0xe7, 0xfc, 0xdf, 0x56, 0xdc, 0xd9, 0x06, 0x24};
/* the square root of -1 */
static const unsigned char edIBytes[32] = {
    0xb0, 0xa0, 0x0e, 0x4a, 0x27, 0x1b, 0xee, 0xc4, 0x78, 0xe4, 0x2f, 0xad, 0x06, 0x18, 0x43, 0x2f,
    0xa7, 0xd7, 0xfb, 0x3d, 0x99, 0x00, 0x4d, 0x2b, 0x0b, 0xdf, 0xc1, 0x4f, 0x80, 0x24, 0x83, 0x2b};
static const unsigned char edBxBytes[32] = {
    0x1a, 0xd5, 0x25, 0x8f, 0x60, 0x2d, 0x56, 0xc9, 0xb2, 0xa7, 0x25, 0x95, 0x60, 0xc7, 0x2c, 0x69,
    0x5c, 0xdc, 0xd6, 0xfd, 0x31, 0xe2, 0xa4, 0xc0, 0xfe, 0x53, 0x6e, 0xcd, 0xd3, 0x36, 0x69, 0x21};
static const unsigned char edByBytes[32] = {
    0x58, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66};
/* the order of the group, little end first */
static const int64_t edL[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7,
                                0xa2, 0xde, 0xf9, 0xde, 0x14, 0, 0, 0, 0, 0, 0, 0, 0,
                                0, 0, 0, 0, 0, 0, 0, 0x10};

static F51 edD51, edD251, edI51;
/* edTable[i][j] is (j + 1) * 256^i * B */
static EdNiels edTable[32][8];
static pthread_once_t edOnce = PTHREAD_ONCE_INIT;

static const F51 f51Zero = {0}, f51One = {1};

static void f51Neg(F51 r, const F51 a) { f51Sub(r, f51Zero, a); }

/* r = a when mask is all ones, unchanged when it is zero */
static void f51Cmov(F51 r, const F51 a, uint64_t mask) {
    for (int i = 0; i < 5; i++) r[i] ^= mask & (r[i] ^ a[i]);
}

static int f51Parity(const F51 a) {
    unsigned char b[32];
    f51Pack(b, a);
    return b[0] & 1;
}

static int f51Equal(const F51 a, const F51 b) {
    unsigned char x[32], y[32];
    f51Pack(x, a);
    f51Pack(y, b);
    return iris_const_equal((const char *)x, 32, (const char *)y, 32);
}

/* a^(2^252 - 3), that is a^((p - 5) / 8), for square roots */
static void f51Pow22523(F51 r, const F51 z) {
    F51 t0, t1, t2;
    f51Sq(t0, z);
    f51SqN(t1, t0, 2);
    f51Mul(t1, z, t1);          /* z^9 */
    f51Mul(t0, t0, t1);         /* z^11 */
    f51Sq(t0, t0);
    f51Mul(t0, t1, t0);         /* 2^5 - 1 */
    f51SqN(t1, t0, 5);
    f51Mul(t0, t1, t0);         /* 2^10 - 1 */
    f51SqN(t1, t0, 10);
    f51Mul(t1, t1, t0);         /* 2^20 - 1 */
    f51SqN(t2, t1, 20);
    f51Mul(t1, t2, t1);         /* 2^40 - 1 */
    f51SqN(t1, t1, 10);
    f51Mul(t0, t1, t0);         /* 2^50 - 1 */
    f51SqN(t1, t0, 50);
    f51Mul(t1, t1, t0);         /* 2^100 - 1 */
    f51SqN(t2, t1, 100);
    f51Mul(t1, t2, t1);         /* 2^200 - 1 */
    f51SqN(t1, t1, 50);
    f51Mul(t0, t1, t0);         /* 2^250 - 1 */
    f51SqN(t0, t0, 2);
    f51Mul(r, t0, z);           /* 2^252 - 3 */
}

static void edIdentity(EdPoint *p) {
    memcpy(p->X, f51Zero, sizeof(F51));
    memcpy(p->Y, f51One, sizeof(F51));
    memcpy(p->Z, f51One, sizeof(F51));
    memcpy(p->T, f51Zero, sizeof(F51));
}

/* 2p (dbl-2008-hwcd with a = -1) */
static void edDouble(EdPoint *r, const EdPoint *p) {
    F51 a, b, c, e, f, g, h, s;
    f51Sq(a, p->X);
    f51Sq(b, p->Y);
    f51Sq(c, p->Z);
    f51Add(c, c, c);
    f51Add(e, p->X, p->Y);
    f51Sq(e, e);
    f51Add(s, a, b);
    f51Sub(e, e, s);            /* 2xy */
    f51Sub(g, b, a);
    f51Sub(f, g, c);
    f51Neg(h, s);
    f51Mul(r->X, e, f);
    f51Mul(r->Y, g, h);
    f51Mul(r->T, e, h);
    f51Mul(r->Z, f, g);
}

/* p + q, for q with its Z (add-2008-hwcd-3) */
static void edAddCached(EdPoint *r, const EdPoint *p, const EdCached *q) {
    F51 a, b, c, d, e, f, g, h;
    f51Sub(a, p->Y, p->X);
    f51Mul(a, a, q->ym);
    f51Add(b, p->Y, p->X);
    f51Mul(b, b, q->yp);
    f51Mul(c, p->T, q->t2d);
    f51Mul(d, p->Z, q->Z);
    f51Add(d, d, d);
    f51Sub(e, b, a);
    f51Sub(f, d, c);
    f51Add(g, d, c);
    f51Add(h, b, a);
    f51Mul(r->X, e, f);
    f51Mul(r->Y, g, h);
    f51Mul(r->T, e, h);
    f51Mul(r->Z, f, g);
}

/* p - q */
static void edSubCached(EdPoint *r, const EdPoint *p, const EdCached *q) {
    EdCached n;
    memcpy(n.yp, q->ym, sizeof(F51));
    memcpy(n.ym, q->yp, sizeof(F51));
    memcpy(n.Z, q->Z, sizeof(F51));
    f51Neg(n.t2d, q->t2d);
    edAddCached(r, p, &n);
}

/* p + q, for an affine q */
static void edAddNiels(EdPoint *r, const EdPoint *p, const EdNiels *q) {
    F51 a, b, c, d, e, f, g, h;
    f51Sub(a, p->Y, p->X);
    f51Mul(a, a, q->ym);
    f51Add(b, p->Y, p->X);
    f51Mul(b, b, q->yp);
    f51Mul(c, p->T, q->t2d);
    f51Add(d, p->Z, p->Z);
    f51Sub(e, b, a);
    f51Sub(f, d, c);
    f51Add(g, d, c);
    f51Add(h, b, a);
    f51Mul(r->X, e, f);
    f51Mul(r->Y, g, h);
    f51Mul(r->T, e, h);
    f51Mul(r->Z, f, g);
}

static void edToCached(EdCached *c, const EdPoint *p) {
    f51Add(c->yp, p->Y, p->X);
    f51Sub(c->ym, p->Y, p->X);
    memcpy(c->Z, p->Z, sizeof(F51));
    f51Mul(c->t2d, p->T, edD251);
}

static void edMakeTable(void) {
    unsigned char b[32];
    memcpy(b, edDBytes, 32);
    f51Unpack(edD51, b);
    memcpy(b, edD2Bytes, 32);
    f51Unpack(edD251, b);
    memcpy(b, edIBytes, 32);
    f51Unpack(edI51, b);
    EdPoint base;
    f51Unpack(base.X, edBxBytes);
    f51Unpack(base.Y, edByBytes);
    memcpy(base.Z, f51One, sizeof(F51));
    f51Mul(base.T, base.X, base.Y);
    /* the 256 points in extended coordinates, then all made affine with
       one inversion */
    static EdPoint all[256];
    EdPoint row = base;
    for (int i = 0; i < 32; i++) {
        EdCached step;
        edToCached(&step, &row);
        all[i * 8] = row;
        for (int j = 1; j < 8; j++) edAddCached(&all[i * 8 + j], &all[i * 8 + j - 1], &step);
        for (int k = 0; k < 8; k++) edDouble(&row, &row);
    }
    static F51 prefix[256];
    memcpy(prefix[0], all[0].Z, sizeof(F51));
    for (int i = 1; i < 256; i++) f51Mul(prefix[i], prefix[i - 1], all[i].Z);
    F51 inv;
    f51Invert(inv, prefix[255]);
    for (int i = 255; i >= 0; i--) {
        F51 zi, x, y, xy;
        if (i > 0) {
            f51Mul(zi, inv, prefix[i - 1]);
            f51Mul(inv, inv, all[i].Z);
        } else {
            memcpy(zi, inv, sizeof(F51));
        }
        f51Mul(x, all[i].X, zi);
        f51Mul(y, all[i].Y, zi);
        f51Mul(xy, x, y);
        EdNiels *n = &edTable[i / 8][i % 8];
        f51Add(n->yp, y, x);
        f51Sub(n->ym, y, x);
        f51Mul(n->t2d, xy, edD251);
    }
}

static void edReady(void) { pthread_once(&edOnce, edMakeTable); }

/* b * 256^pos * B for a digit b from -8 to 8, reading every entry */
static void edSelect(EdNiels *t, int pos, signed char b) {
    uint64_t neg = (uint64_t)(unsigned char)b >> 7;
    unsigned char babs = (unsigned char)(b - ((-neg & (uint64_t)b) << 1));
    memcpy(t->yp, f51One, sizeof(F51));
    memcpy(t->ym, f51One, sizeof(F51));
    memcpy(t->t2d, f51Zero, sizeof(F51));
    for (int j = 0; j < 8; j++) {
        uint64_t eq = (uint64_t)(babs ^ (unsigned char)(j + 1));
        uint64_t mask = 0 - ((eq - 1) >> 63);
        f51Cmov(t->yp, edTable[pos][j].yp, mask);
        f51Cmov(t->ym, edTable[pos][j].ym, mask);
        f51Cmov(t->t2d, edTable[pos][j].t2d, mask);
    }
    /* a negative digit: y + x and y - x change places, and 2dxy its sign */
    uint64_t m = 0 - neg;
    F51 yp, nt;
    memcpy(yp, t->yp, sizeof(F51));
    f51Cmov(t->yp, t->ym, m);
    f51Cmov(t->ym, yp, m);
    f51Neg(nt, t->t2d);
    f51Cmov(t->t2d, nt, m);
}

/* the digits of a, from -8 to 7 in base 16, low first; a below 2^255 */
static void edDigits(signed char e[64], const unsigned char *a) {
    for (int i = 0; i < 32; i++) {
        e[2 * i] = (signed char)(a[i] & 15);
        e[2 * i + 1] = (signed char)(a[i] >> 4);
    }
    signed char carry = 0;
    for (int i = 0; i < 63; i++) {
        e[i] = (signed char)(e[i] + carry);
        carry = (signed char)((e[i] + 8) >> 4);
        e[i] = (signed char)(e[i] - carry * 16);
    }
    e[63] = (signed char)(e[63] + carry);
}

/* a * B */
static void edMulBase(EdPoint *h, const unsigned char *a) {
    edReady();
    signed char e[64];
    edDigits(e, a);
    edIdentity(h);
    EdNiels t;
    for (int i = 1; i < 64; i += 2) {
        edSelect(&t, i / 2, e[i]);
        edAddNiels(h, h, &t);
    }
    for (int k = 0; k < 4; k++) edDouble(h, h);
    for (int i = 0; i < 64; i += 2) {
        edSelect(&t, i / 2, e[i]);
        edAddNiels(h, h, &t);
    }
    iris_wipe(e, sizeof e);
}

/* a * p, for public a and p: it branches on the digits */
static void edMulPublic(EdPoint *r, const EdPoint *p, const unsigned char *a) {
    EdCached table[8];
    EdPoint m = *p, two;
    edToCached(&table[0], p);
    edDouble(&two, p);
    for (int j = 1; j < 8; j++) {
        EdPoint next;
        if (j == 1) {
            next = two;
        } else {
            edAddCached(&next, &m, &table[0]);
        }
        m = next;
        edToCached(&table[j], &m);
    }
    signed char e[64];
    edDigits(e, a);
    edIdentity(r);
    for (int i = 63; i >= 0; i--) {
        if (i != 63) {
            for (int k = 0; k < 4; k++) edDouble(r, r);
        }
        if (e[i] > 0) {
            edAddCached(r, r, &table[e[i] - 1]);
        } else if (e[i] < 0) {
            edSubCached(r, r, &table[-e[i] - 1]);
        }
    }
}

static void edPack(unsigned char *out, const EdPoint *p) {
    F51 zi, x, y;
    f51Invert(zi, p->Z);
    f51Mul(x, p->X, zi);
    f51Mul(y, p->Y, zi);
    f51Pack(out, y);
    out[31] ^= (unsigned char)(f51Parity(x) << 7);
}

/* -A from a public key, or -1 when it isn't a point of the curve in its
   one encoding */
static int edUnpackNeg(EdPoint *r, const unsigned char *s) {
    edReady();
    F51 u, v, v3, x, vxx, t;
    f51Unpack(r->Y, s);
    unsigned char back[32];
    f51Pack(back, r->Y);
    back[31] |= s[31] & 0x80;
    if (memcmp(back, s, 32) != 0) return -1;
    memcpy(r->Z, f51One, sizeof(F51));
    F51 yy;
    f51Sq(yy, r->Y);
    f51Mul(v, yy, edD51);
    f51Sub(u, yy, f51One);      /* y^2 - 1 */
    f51Add(v, v, f51One);       /* dy^2 + 1 */
    f51Sq(v3, v);
    f51Mul(v3, v3, v);          /* v^3 */
    f51Sq(x, v3);
    f51Mul(x, x, v);
    f51Mul(x, x, u);            /* uv^7 */
    f51Pow22523(x, x);
    f51Mul(x, x, v3);
    f51Mul(x, x, u);            /* uv^3 (uv^7)^((p-5)/8) */
    f51Sq(vxx, x);
    f51Mul(vxx, vxx, v);
    if (!f51Equal(vxx, u)) {
        /* -u as 1 - y^2: f51Sub takes away only values under 2^53 a limb */
        f51Sub(t, f51One, yy);
        if (!f51Equal(vxx, t)) return -1;
        f51Mul(x, x, edI51);
    }
    int sign = s[31] >> 7;
    if (sign && f51Equal(x, f51Zero)) return -1;
    if (f51Parity(x) == sign) {
        f51Neg(x, x);
        /* back under 2^52 a limb, as f51Sub needs of what it takes away */
        f51Carry(x, x[0], x[1], x[2], x[3], x[4]);
    }
    memcpy(r->X, x, sizeof(F51));
    f51Mul(r->T, r->X, r->Y);
    return 0;
}

/* r = x mod L, for x of 64 limbs of a byte each */
static void edModL(unsigned char *r, int64_t x[64]) {
    int64_t carry;
    for (int i = 63; i >= 32; i--) {
        carry = 0;
        int j;
        for (j = i - 32; j < i - 12; j++) {
            x[j] += carry - 16 * x[i] * edL[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry * 256;
        }
        x[j] += carry;
        x[i] = 0;
    }
    carry = 0;
    for (int j = 0; j < 32; j++) {
        x[j] += carry - (x[31] >> 4) * edL[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (int j = 0; j < 32; j++) x[j] -= carry * edL[j];
    for (int i = 0; i < 32; i++) {
        x[i + 1] += x[i] >> 8;
        r[i] = (unsigned char)(x[i] & 255);
    }
}

static void edReduce(unsigned char *r) {
    int64_t x[64];
    for (int i = 0; i < 64; i++) x[i] = r[i];
    memset(r, 0, 64);
    edModL(r, x);
}

/* the secret scalar (clamped) and the prefix, from the 32-byte seed */
static void edExpand(const unsigned char *seed, unsigned char d[64]) {
    iris_sha512((const char *)seed, 32, d);
    d[0] &= 248;
    d[31] &= 127;
    d[31] |= 64;
}

void iris_ed25519_public(const unsigned char *seed, unsigned char *pub) {
    unsigned char d[64];
    EdPoint p;
    edExpand(seed, d);
    edMulBase(&p, d);
    edPack(pub, &p);
    iris_wipe(d, sizeof d);
}

void iris_ed25519_sign(const unsigned char *seed, const char *msg, int64_t len, unsigned char *sig) {
    unsigned char d[64], r[64], h[64], pub[32];
    EdPoint p;
    edExpand(seed, d);
    edMulBase(&p, d);
    edPack(pub, &p);
    Sha512 s;
    sha512Init(&s);
    sha512Add(&s, d + 32, 32);
    sha512Add(&s, (const unsigned char *)msg, (size_t)len);
    sha512End(&s, r);
    edReduce(r);
    edMulBase(&p, r);
    edPack(sig, &p);
    sha512Init(&s);
    sha512Add(&s, sig, 32);
    sha512Add(&s, pub, 32);
    sha512Add(&s, (const unsigned char *)msg, (size_t)len);
    sha512End(&s, h);
    edReduce(h);
    int64_t x[64] = {0};
    for (int i = 0; i < 32; i++) x[i] = r[i];
    for (int i = 0; i < 32; i++) {
        for (int j = 0; j < 32; j++) x[i + j] += h[i] * (int64_t)d[j];
    }
    edModL(sig + 32, x);
    iris_wipe(d, sizeof d);
    iris_wipe(r, sizeof r);
    iris_wipe(x, sizeof x);
}

/* The X25519 public key, from the Ed25519 base point's table: the
   Montgomery u of the Edwards point (x, y) is (1 + y) / (1 - y). */
void iris_x25519_public(const unsigned char *scalar, unsigned char *out) {
    unsigned char z[32];
    memcpy(z, scalar, 32);
    z[31] = (unsigned char)((z[31] & 127) | 64);
    z[0] &= 248;
    EdPoint p;
    edMulBase(&p, z);
    F51 num, den;
    f51Add(num, p.Z, p.Y);
    f51Sub(den, p.Z, p.Y);
    f51Invert(den, den);
    f51Mul(num, num, den);
    f51Pack(out, num);
    iris_wipe(z, sizeof z);
}

int iris_ed25519_verify(const unsigned char *pub, const char *msg, int64_t len, const unsigned char *sig) {
    /* S must be less than L, so a signature has one form */
    for (int i = 31; i >= 0; i--) {
        if (sig[32 + i] < edL[i]) break;
        if (sig[32 + i] > edL[i] || i == 0) return 0;
    }
    EdPoint a, p, q;
    if (edUnpackNeg(&a, pub)) return 0;
    unsigned char h[64], t[32];
    Sha512 s;
    sha512Init(&s);
    sha512Add(&s, sig, 32);
    sha512Add(&s, pub, 32);
    sha512Add(&s, (const unsigned char *)msg, (size_t)len);
    sha512End(&s, h);
    edReduce(h);
    /* R = sB - hA */
    edMulPublic(&p, &a, h);
    edMulBase(&q, sig + 32);
    EdCached c;
    edToCached(&c, &q);
    edAddCached(&p, &p, &c);
    edPack(t, &p);
    return iris_const_equal((const char *)sig, 32, (const char *)t, 32);
}

/* ChaCha20 (RFC 8439, 2.3): one 64-byte block of key stream. */
static uint32_t le32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

#define ROL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define QROUND(a, b, c, d) \
    a += b; d ^= a; d = ROL32(d, 16); \
    c += d; b ^= c; b = ROL32(b, 12); \
    a += b; d ^= a; d = ROL32(d, 8); \
    c += d; b ^= c; b = ROL32(b, 7)

static void chachaBlock(const unsigned char key[32], uint32_t counter, const unsigned char nonce[12],
                        unsigned char out[64]) {
    uint32_t s[16], x[16];
    s[0] = 0x61707865;
    s[1] = 0x3320646e;
    s[2] = 0x79622d32;
    s[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) s[4 + i] = le32(key + 4 * i);
    s[12] = counter;
    for (int i = 0; i < 3; i++) s[13 + i] = le32(nonce + 4 * i);
    memcpy(x, s, sizeof s);
    for (int i = 0; i < 10; i++) {
        QROUND(x[0], x[4], x[8], x[12]);
        QROUND(x[1], x[5], x[9], x[13]);
        QROUND(x[2], x[6], x[10], x[14]);
        QROUND(x[3], x[7], x[11], x[15]);
        QROUND(x[0], x[5], x[10], x[15]);
        QROUND(x[1], x[6], x[11], x[12]);
        QROUND(x[2], x[7], x[8], x[13]);
        QROUND(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) {
        uint32_t v = x[i] + s[i];
        out[4 * i] = (unsigned char)v;
        out[4 * i + 1] = (unsigned char)(v >> 8);
        out[4 * i + 2] = (unsigned char)(v >> 16);
        out[4 * i + 3] = (unsigned char)(v >> 24);
    }
}

/* 8 blocks of key stream side by side, one in each lane of the vectors,
   xored into 512 bytes of in: AVX2 on x86_64, NEON (two halves) on arm64. */
#if defined(__x86_64__) || defined(__aarch64__)
#define CHACHA_WIDE 1
typedef uint32_t ChachaVec __attribute__((vector_size(32)));
#if defined(__x86_64__)
#define CHACHA_TARGET __attribute__((target("avx2")))
#else
#define CHACHA_TARGET
#endif

static int chachaWide(void) {
#if defined(__x86_64__)
    static int wide = -1;
    if (wide < 0) {
        __builtin_cpu_init();
        wide = __builtin_cpu_supports("avx2") && !getenv("IRIS_CHACHA_SOFT");
    }
    return wide;
#else
    return 1;
#endif
}

CHACHA_TARGET static void chacha8(const uint32_t s[16], const unsigned char *in, unsigned char *out) {
    ChachaVec x[16], start[16];
    for (int i = 0; i < 16; i++) {
        uint32_t v = s[i];
        start[i] = (ChachaVec){v, v, v, v, v, v, v, v};
    }
    start[12] += (ChachaVec){0, 1, 2, 3, 4, 5, 6, 7};
    memcpy(x, start, sizeof x);
    for (int i = 0; i < 10; i++) {
        QROUND(x[0], x[4], x[8], x[12]);
        QROUND(x[1], x[5], x[9], x[13]);
        QROUND(x[2], x[6], x[10], x[14]);
        QROUND(x[3], x[7], x[11], x[15]);
        QROUND(x[0], x[5], x[10], x[15]);
        QROUND(x[1], x[6], x[11], x[12]);
        QROUND(x[2], x[7], x[8], x[13]);
        QROUND(x[3], x[4], x[9], x[14]);
    }
    uint32_t words[16][8];
    for (int i = 0; i < 16; i++) {
        ChachaVec v = x[i] + start[i];
        memcpy(words[i], &v, sizeof v);
    }
    for (int b = 0; b < 8; b++) {
        for (int i = 0; i < 16; i++) {
            uint32_t d;
            memcpy(&d, in + 64 * b + 4 * i, 4);
            d ^= words[i][b];
            memcpy(out + 64 * b + 4 * i, &d, 4);
        }
    }
}
#endif

/* data XOR the key stream from block counter on */
static void chachaXor(const unsigned char key[32], uint32_t counter, const unsigned char nonce[12],
                      const unsigned char *in, unsigned char *out, size_t n) {
#ifdef CHACHA_WIDE
    if (n >= 512 && chachaWide()) {
        uint32_t s[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
        for (int i = 0; i < 8; i++) s[4 + i] = le32(key + 4 * i);
        for (int i = 0; i < 3; i++) s[13 + i] = le32(nonce + 4 * i);
        for (; n >= 512; in += 512, out += 512, n -= 512, counter += 8) {
            s[12] = counter;
            chacha8(s, in, out);
        }
        iris_wipe(s, sizeof s);
    }
#endif
    unsigned char block[64];
    for (size_t at = 0; at < n; at += 64) {
        chachaBlock(key, counter++, nonce, block);
        size_t take = n - at < 64 ? n - at : 64;
        for (size_t i = 0; i < take; i++) out[at + i] = in[at + i] ^ block[i];
    }
}

/* Poly1305 (RFC 8439, 2.5), with 44-bit limbs. */
static uint64_t le64(const unsigned char *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }

typedef struct {
    uint64_t r[3], h[3], pad[2];
} Poly1305;

static void polyInit(Poly1305 *p, const unsigned char key[32]) {
    uint64_t t0 = le64(key), t1 = le64(key + 8);
    p->r[0] = t0 & 0xffc0fffffffULL;
    p->r[1] = ((t0 >> 44) | (t1 << 20)) & 0xfffffc0ffffULL;
    p->r[2] = (t1 >> 24) & 0x00ffffffc0fULL;
    p->h[0] = p->h[1] = p->h[2] = 0;
    p->pad[0] = le64(key + 16);
    p->pad[1] = le64(key + 24);
}

/* one 16-byte block; a last short one comes padded with its 1 already */
static void polyBlock(Poly1305 *p, const unsigned char m[16], uint64_t hibit) {
    typedef unsigned __int128 u128;
    uint64_t r0 = p->r[0], r1 = p->r[1], r2 = p->r[2];
    uint64_t s1 = r1 * (5 << 2), s2 = r2 * (5 << 2);
    uint64_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2];
    uint64_t t0 = le64(m), t1 = le64(m + 8);
    h0 += t0 & 0xfffffffffffULL;
    h1 += ((t0 >> 44) | (t1 << 20)) & 0xfffffffffffULL;
    h2 += ((t1 >> 24) & 0x3ffffffffffULL) | hibit;
    u128 d0 = (u128)h0 * r0 + (u128)h1 * s2 + (u128)h2 * s1;
    u128 d1 = (u128)h0 * r1 + (u128)h1 * r0 + (u128)h2 * s2;
    u128 d2 = (u128)h0 * r2 + (u128)h1 * r1 + (u128)h2 * r0;
    uint64_t c = (uint64_t)(d0 >> 44);
    h0 = (uint64_t)d0 & 0xfffffffffffULL;
    d1 += c;
    c = (uint64_t)(d1 >> 44);
    h1 = (uint64_t)d1 & 0xfffffffffffULL;
    d2 += c;
    c = (uint64_t)(d2 >> 42);
    h2 = (uint64_t)d2 & 0x3ffffffffffULL;
    h0 += c * 5;
    c = h0 >> 44;
    h0 &= 0xfffffffffffULL;
    h1 += c;
    p->h[0] = h0;
    p->h[1] = h1;
    p->h[2] = h2;
}

/* the data in 16-byte blocks, the last one padded with zeros (the AEAD's way) */
static void polyPadded(Poly1305 *p, const unsigned char *m, size_t n) {
    while (n >= 16) {
        polyBlock(p, m, 1ULL << 40);
        m += 16;
        n -= 16;
    }
    if (n) {
        unsigned char last[16] = {0};
        memcpy(last, m, n);
        polyBlock(p, last, 1ULL << 40);
    }
}

static void polyFinish(Poly1305 *p, unsigned char mac[16]) {
    uint64_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], c, g0, g1, g2;
    c = h1 >> 44;
    h1 &= 0xfffffffffffULL;
    h2 += c;
    c = h2 >> 42;
    h2 &= 0x3ffffffffffULL;
    h0 += c * 5;
    c = h0 >> 44;
    h0 &= 0xfffffffffffULL;
    h1 += c;
    c = h1 >> 44;
    h1 &= 0xfffffffffffULL;
    h2 += c;
    c = h2 >> 42;
    h2 &= 0x3ffffffffffULL;
    h0 += c * 5;
    c = h0 >> 44;
    h0 &= 0xfffffffffffULL;
    h1 += c;
    /* h - p, used when it doesn't go below zero */
    g0 = h0 + 5;
    c = g0 >> 44;
    g0 &= 0xfffffffffffULL;
    g1 = h1 + c;
    c = g1 >> 44;
    g1 &= 0xfffffffffffULL;
    g2 = h2 + c - (1ULL << 42);
    c = (g2 >> 63) - 1;
    g0 &= c;
    g1 &= c;
    g2 &= c;
    c = ~c;
    h0 = (h0 & c) | g0;
    h1 = (h1 & c) | g1;
    h2 = (h2 & c) | g2;
    uint64_t t0 = p->pad[0], t1 = p->pad[1];
    h0 += t0 & 0xfffffffffffULL;
    c = h0 >> 44;
    h0 &= 0xfffffffffffULL;
    h1 += (((t0 >> 44) | (t1 << 20)) & 0xfffffffffffULL) + c;
    c = h1 >> 44;
    h1 &= 0xfffffffffffULL;
    h2 += ((t1 >> 24) & 0x3ffffffffffULL) + c;
    h2 &= 0x3ffffffffffULL;
    h0 = h0 | (h1 << 44);
    h1 = (h1 >> 20) | (h2 << 24);
    for (int i = 0; i < 8; i++) {
        mac[i] = (unsigned char)(h0 >> (8 * i));
        mac[8 + i] = (unsigned char)(h1 >> (8 * i));
    }
}

/* the AEAD's tag over the extra data and the sealed text (RFC 8439, 2.8) */
static void aeadTag(const unsigned char key[32], const unsigned char nonce[12], const unsigned char *aad,
                    size_t aadLen, const unsigned char *ct, size_t ctLen, unsigned char tag[16]) {
    unsigned char block[64];
    chachaBlock(key, 0, nonce, block);
    Poly1305 p;
    polyInit(&p, block);
    polyPadded(&p, aad, aadLen);
    polyPadded(&p, ct, ctLen);
    unsigned char lens[16];
    for (int i = 0; i < 8; i++) {
        lens[i] = (unsigned char)((uint64_t)aadLen >> (8 * i));
        lens[8 + i] = (unsigned char)((uint64_t)ctLen >> (8 * i));
    }
    polyBlock(&p, lens, 1ULL << 40);
    polyFinish(&p, tag);
}

/* Room for n more bytes at the end of out. data and aad may point into
   out: they are moved along with it. */
static unsigned char *aeadRoom(IrisString *out, int64_t n, const unsigned char **data, const unsigned char **aad) {
    if (out->len + n > out->cap) {
        int64_t at = -1;
        if (*aad && out->ptr && (const char *)*aad >= out->ptr && (const char *)*aad < out->ptr + out->len) {
            at = (const char *)*aad - out->ptr;
        }
        const char *d = (const char *)*data;
        stringGrow(out, &d, n);
        *data = (const unsigned char *)d;
        if (at >= 0) *aad = (const unsigned char *)out->ptr + at;
    }
    return (unsigned char *)out->ptr + out->len;
}

/* ChaCha20-Poly1305: the data sealed, then its 16-byte tag, added to out */
void iris_chacha_seal(const unsigned char *key, const unsigned char *nonce, const unsigned char *data, int64_t len,
                      const unsigned char *aad, int64_t aadLen, IrisString *out) {
    unsigned char *ct = aeadRoom(out, len + 16, &data, &aad);
    chachaXor(key, 1, nonce, data, ct, (size_t)len);
    aeadTag(key, nonce, aad, (size_t)aadLen, ct, (size_t)len, ct + len);
    out->len += len + 16;
}

/* Opens what seal made: 1 and the data added to out, or 0 when the tag
   doesn't match, and then out gets nothing. */
int iris_chacha_open(const unsigned char *key, const unsigned char *nonce, const unsigned char *sealed, int64_t len,
                     const unsigned char *aad, int64_t aadLen, IrisString *out) {
    if (len < 16) {
        return 0;
    }
    size_t n = (size_t)len - 16;
    unsigned char tag[16];
    aeadTag(key, nonce, aad, (size_t)aadLen, sealed, n, tag);
    if (!iris_const_equal((const char *)tag, 16, (const char *)sealed + n, 16)) {
        return 0;
    }
    if (n == 0) {
        return 1;
    }
    unsigned char *pt = aeadRoom(out, (int64_t)n, &sealed, &aad);
    chachaXor(key, 1, nonce, sealed, pt, n);
    out->len += (int64_t)n;
    return 1;
}

/* Keccak-f[1600] and its sponge (FIPS 202): SHA3-256, SHA3-512, SHAKE128
   and SHAKE256, as ML-KEM uses them. */
static const uint64_t keccakRound[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL, 0x8000000080008000ULL,
    0x000000000000808bULL, 0x0000000080000001ULL, 0x8000000080008081ULL, 0x8000000000008009ULL,
    0x000000000000008aULL, 0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL, 0x8000000000008003ULL,
    0x8000000000008002ULL, 0x8000000000000080ULL, 0x000000000000800aULL, 0x800000008000000aULL,
    0x8000000080008081ULL, 0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL};
static const int keccakRot[24] = {1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14, 27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44};
static const int keccakLane[24] = {10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4, 15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1};

static void keccakF(uint64_t s[25]) {
    for (int r = 0; r < 24; r++) {
        uint64_t c[5];
        for (int i = 0; i < 5; i++) c[i] = s[i] ^ s[i + 5] ^ s[i + 10] ^ s[i + 15] ^ s[i + 20];
        for (int i = 0; i < 5; i++) {
            uint64_t d = c[(i + 4) % 5] ^ (c[(i + 1) % 5] << 1 | c[(i + 1) % 5] >> 63);
            for (int j = 0; j < 25; j += 5) s[j + i] ^= d;
        }
        uint64_t t = s[1];
        for (int i = 0; i < 24; i++) {
            int j = keccakLane[i];
            uint64_t u = s[j];
            s[j] = t << keccakRot[i] | t >> (64 - keccakRot[i]);
            t = u;
        }
        for (int j = 0; j < 25; j += 5) {
            uint64_t a[5];
            for (int i = 0; i < 5; i++) a[i] = s[j + i];
            for (int i = 0; i < 5; i++) s[j + i] = a[i] ^ (~a[(i + 1) % 5] & a[(i + 2) % 5]);
        }
        s[0] ^= keccakRound[r];
    }
}

typedef struct {
    uint64_t s[25];
    size_t rate, at;
} Keccak;

static void keccakInit(Keccak *k, size_t rate) {
    memset(k, 0, sizeof *k);
    k->rate = rate;
}

static void keccakAbsorb(Keccak *k, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        k->s[k->at >> 3] ^= (uint64_t)p[i] << (8 * (k->at & 7));
        if (++k->at == k->rate) {
            keccakF(k->s);
            k->at = 0;
        }
    }
}

/* pad is 0x06 for SHA-3, 0x1f for SHAKE */
static void keccakFinish(Keccak *k, uint8_t pad) {
    k->s[k->at >> 3] ^= (uint64_t)pad << (8 * (k->at & 7));
    k->s[(k->rate - 1) >> 3] ^= (uint64_t)0x80 << (8 * ((k->rate - 1) & 7));
    keccakF(k->s);
    k->at = 0;
}

static void keccakSqueeze(Keccak *k, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (k->at == k->rate) {
            keccakF(k->s);
            k->at = 0;
        }
        out[i] = (uint8_t)(k->s[k->at >> 3] >> (8 * (k->at & 7)));
        k->at++;
    }
}

/* SHA3 (pad 0x06) or SHAKE (0x1f) of a and b one after the other */
static void keccakHash(size_t rate, uint8_t pad, const uint8_t *a, size_t an, const uint8_t *b, size_t bn, uint8_t *out,
                       size_t outLen) {
    Keccak k;
    keccakInit(&k, rate);
    keccakAbsorb(&k, a, an);
    keccakAbsorb(&k, b, bn);
    keccakFinish(&k, pad);
    keccakSqueeze(&k, out, outLen);
    iris_wipe(&k, sizeof k);
}

#define SHA3_256(a, an, b, bn, out) keccakHash(136, 0x06, a, an, b, bn, out, 32)
#define SHA3_512(a, an, b, bn, out) keccakHash(72, 0x06, a, an, b, bn, out, 64)

void iris_sha3(int bits, const char *data, int64_t len, char *out) {
    keccakHash(200 - (size_t)bits / 4, 0x06, (const uint8_t *)data, (size_t)len, NULL, 0, (uint8_t *)out, (size_t)bits / 8);
}

void iris_shake(int bits, const char *data, int64_t len, IrisString *out, int64_t outLen) {
    iris_string_reserve(out, outLen);
    keccakHash(bits == 128 ? 168 : 136, 0x1f, (const uint8_t *)data, (size_t)len, NULL, 0, (uint8_t *)out->ptr + out->len,
               (size_t)outLen);
    out->len += outLen;
}

/* ML-KEM-768 (FIPS 203): polynomials of 256 numbers mod 3329, in a 3 by 3
   module. Numbers stay in 0..q-1 and every step reduces with % by the
   constant, which compiles to multiplications. */
#define MLKEM_Q 3329
#define MLKEM_K 3
#define MLKEM_PUBLIC 1184
#define MLKEM_CIPHERTEXT 1088

typedef struct {
    uint16_t c[256];
} MlkemPoly;

static uint16_t mlkemZeta[128];  /* 17^BitRev7(i) */
static uint16_t mlkemGamma[128]; /* 17^(2 BitRev7(i) + 1) */
static pthread_once_t mlkemOnce = PTHREAD_ONCE_INIT;

static void mlkemTables(void) {
    for (int i = 0; i < 128; i++) {
        int r = 0;
        for (int b = 0; b < 7; b++) {
            if (i >> b & 1) r |= 1 << (6 - b);
        }
        uint32_t z = 1;
        for (int e = 0; e < r; e++) z = z * 17 % MLKEM_Q;
        mlkemZeta[i] = (uint16_t)z;
        mlkemGamma[i] = (uint16_t)(z * z % MLKEM_Q * 17 % MLKEM_Q);
    }
}

static uint16_t modq(uint32_t x) { return (uint16_t)(x % MLKEM_Q); }

static void mlkemNtt(MlkemPoly *f) {
    int i = 1;
    for (int len = 128; len >= 2; len >>= 1) {
        for (int start = 0; start < 256; start += 2 * len) {
            uint32_t z = mlkemZeta[i++];
            for (int j = start; j < start + len; j++) {
                uint16_t t = modq(z * f->c[j + len]);
                f->c[j + len] = modq((uint32_t)f->c[j] + MLKEM_Q - t);
                f->c[j] = modq((uint32_t)f->c[j] + t);
            }
        }
    }
}

static void mlkemInvNtt(MlkemPoly *f) {
    int i = 127;
    for (int len = 2; len <= 128; len <<= 1) {
        for (int start = 0; start < 256; start += 2 * len) {
            uint32_t z = mlkemZeta[i--];
            for (int j = start; j < start + len; j++) {
                uint16_t t = f->c[j];
                f->c[j] = modq((uint32_t)t + f->c[j + len]);
                f->c[j + len] = modq(z * ((uint32_t)f->c[j + len] + MLKEM_Q - t));
            }
        }
    }
    for (int j = 0; j < 256; j++) f->c[j] = modq(3303u * f->c[j]); /* 128^-1 */
}

/* h += f g, both in the NTT form */
static void mlkemMulAdd(MlkemPoly *h, const MlkemPoly *f, const MlkemPoly *g) {
    for (int i = 0; i < 128; i++) {
        uint32_t a0 = f->c[2 * i], a1 = f->c[2 * i + 1], b0 = g->c[2 * i], b1 = g->c[2 * i + 1];
        uint32_t c0 = modq(a0 * b0 + (uint32_t)modq(a1 * b1) * mlkemGamma[i]);
        uint32_t c1 = modq(a0 * b1 + a1 * b0);
        h->c[2 * i] = modq(h->c[2 * i] + c0);
        h->c[2 * i + 1] = modq(h->c[2 * i + 1] + c1);
    }
}

static void mlkemAdd(MlkemPoly *h, const MlkemPoly *f) {
    for (int i = 0; i < 256; i++) h->c[i] = modq((uint32_t)h->c[i] + f->c[i]);
}

/* A[i][j] of the matrix, in the NTT form, from SHAKE128(rho || j || i) */
static void mlkemSampleNtt(MlkemPoly *a, const uint8_t rho[32], int i, int j) {
    Keccak k;
    keccakInit(&k, 168);
    uint8_t b[34];
    memcpy(b, rho, 32);
    b[32] = (uint8_t)j;
    b[33] = (uint8_t)i;
    keccakAbsorb(&k, b, 34);
    keccakFinish(&k, 0x1f);
    int n = 0;
    uint8_t c[168];
    while (n < 256) {
        keccakSqueeze(&k, c, sizeof c);
        for (int at = 0; at < 168 && n < 256; at += 3) {
            uint16_t d1 = (uint16_t)(c[at] | (c[at + 1] & 15) << 8);
            uint16_t d2 = (uint16_t)(c[at + 1] >> 4 | c[at + 2] << 4);
            if (d1 < MLKEM_Q) a->c[n++] = d1;
            if (d2 < MLKEM_Q && n < 256) a->c[n++] = d2;
        }
    }
}

/* A small polynomial, each number a difference of two sums of 2 bits of
   SHAKE256(seed || nonce) (eta 2) */
static void mlkemSampleCbd(MlkemPoly *f, const uint8_t seed[32], uint8_t nonce) {
    uint8_t b[128];
    keccakHash(136, 0x1f, seed, 32, &nonce, 1, b, sizeof b);
    for (int i = 0; i < 256; i++) {
        int bits = b[i / 2] >> (4 * (i & 1)) & 15;
        int x = (bits & 1) + (bits >> 1 & 1), y = (bits >> 2 & 1) + (bits >> 3 & 1);
        f->c[i] = (uint16_t)(x - y + MLKEM_Q) % MLKEM_Q;
    }
    iris_wipe(b, sizeof b);
}

/* d bits a number, the low bits first */
static void mlkemEncode(uint8_t *out, const MlkemPoly *f, int d) {
    uint32_t acc = 0;
    int have = 0;
    size_t at = 0;
    for (int i = 0; i < 256; i++) {
        acc |= (uint32_t)f->c[i] << have;
        have += d;
        while (have >= 8) {
            out[at++] = (uint8_t)acc;
            acc >>= 8;
            have -= 8;
        }
    }
}

static void mlkemDecode(MlkemPoly *f, const uint8_t *in, int d) {
    uint32_t acc = 0;
    int have = 0;
    size_t at = 0;
    for (int i = 0; i < 256; i++) {
        while (have < d) {
            acc |= (uint32_t)in[at++] << have;
            have += 8;
        }
        f->c[i] = (uint16_t)(acc & ((1u << d) - 1));
        acc >>= d;
        have -= d;
    }
}

static void mlkemCompress(MlkemPoly *f, int d) {
    for (int i = 0; i < 256; i++) f->c[i] = (uint16_t)((((uint32_t)f->c[i] << d) + MLKEM_Q / 2) / MLKEM_Q & ((1u << d) - 1));
}

static void mlkemDecompress(MlkemPoly *f, int d) {
    for (int i = 0; i < 256; i++) f->c[i] = (uint16_t)(((uint32_t)f->c[i] * MLKEM_Q + (1u << (d - 1))) >> d);
}

/* K-PKE.KeyGen: the public key and the secret s, in the NTT form */
static void mlkemKeyGen(const uint8_t d[32], uint8_t ek[MLKEM_PUBLIC], MlkemPoly s[MLKEM_K]) {
    pthread_once(&mlkemOnce, mlkemTables);
    uint8_t g[64], k = MLKEM_K;
    SHA3_512(d, 32, &k, 1, g);
    const uint8_t *rho = g, *sigma = g + 32;
    MlkemPoly e, t, a;
    for (int i = 0; i < MLKEM_K; i++) {
        mlkemSampleCbd(&s[i], sigma, (uint8_t)i);
        mlkemNtt(&s[i]);
    }
    for (int i = 0; i < MLKEM_K; i++) {
        mlkemSampleCbd(&e, sigma, (uint8_t)(MLKEM_K + i));
        mlkemNtt(&e);
        t = e;
        for (int j = 0; j < MLKEM_K; j++) {
            mlkemSampleNtt(&a, rho, i, j);
            mlkemMulAdd(&t, &a, &s[j]);
        }
        mlkemEncode(ek + 384 * i, &t, 12);
    }
    memcpy(ek + 384 * MLKEM_K, rho, 32);
    iris_wipe(g, sizeof g);
    iris_wipe(&e, sizeof e);
}

/* K-PKE.Encrypt of the 32 bytes m with the randomness r */
static void mlkemEncrypt(const uint8_t ek[MLKEM_PUBLIC], const uint8_t m[32], const uint8_t r[32],
                         uint8_t c[MLKEM_CIPHERTEXT]) {
    MlkemPoly t[MLKEM_K], y[MLKEM_K], u, e, a;
    const uint8_t *rho = ek + 384 * MLKEM_K;
    for (int i = 0; i < MLKEM_K; i++) {
        mlkemDecode(&t[i], ek + 384 * i, 12);
        mlkemSampleCbd(&y[i], r, (uint8_t)i);
        mlkemNtt(&y[i]);
    }
    for (int i = 0; i < MLKEM_K; i++) {
        memset(&u, 0, sizeof u);
        for (int j = 0; j < MLKEM_K; j++) {
            mlkemSampleNtt(&a, rho, j, i);
            mlkemMulAdd(&u, &a, &y[j]);
        }
        mlkemInvNtt(&u);
        mlkemSampleCbd(&e, r, (uint8_t)(MLKEM_K + i));
        mlkemAdd(&u, &e);
        mlkemCompress(&u, 10);
        mlkemEncode(c + 320 * i, &u, 10);
    }
    memset(&u, 0, sizeof u);
    for (int j = 0; j < MLKEM_K; j++) mlkemMulAdd(&u, &t[j], &y[j]);
    mlkemInvNtt(&u);
    mlkemSampleCbd(&e, r, 2 * MLKEM_K);
    mlkemAdd(&u, &e);
    for (int i = 0; i < 256; i++) u.c[i] = modq(u.c[i] + (uint32_t)(m[i / 8] >> (i % 8) & 1) * 1665);
    mlkemCompress(&u, 4);
    mlkemEncode(c + 320 * MLKEM_K, &u, 4);
    iris_wipe(y, sizeof y);
    iris_wipe(&u, sizeof u);
    iris_wipe(&e, sizeof e);
}

/* The encapsulation key of the 64-byte seed d || z, the form a decapsulation
   key is kept in. */
void iris_mlkem768_public(const char *seed, IrisString *out) {
    MlkemPoly s[MLKEM_K];
    iris_string_reserve(out, MLKEM_PUBLIC);
    mlkemKeyGen((const uint8_t *)seed, (uint8_t *)out->ptr + out->len, s);
    out->len += MLKEM_PUBLIC;
    iris_wipe(s, sizeof s);
}

/* Encapsulation: the ciphertext goes into out and the secret into shared.
   0 when ek isn't a key, its numbers not all under q. */
int iris_mlkem768_encapsulate(const char *ek, IrisString *out, char *shared) {
    pthread_once(&mlkemOnce, mlkemTables);
    const uint8_t *key = (const uint8_t *)ek;
    MlkemPoly t;
    for (int i = 0; i < MLKEM_K; i++) {
        mlkemDecode(&t, key + 384 * i, 12);
        for (int j = 0; j < 256; j++) {
            if (t.c[j] >= MLKEM_Q) return 0;
        }
    }
    uint8_t h[32], kr[64], m[32];
    iris_random_os((char *)m, sizeof m);
    SHA3_256(key, MLKEM_PUBLIC, NULL, 0, h);
    SHA3_512(m, 32, h, 32, kr);
    iris_string_reserve(out, MLKEM_CIPHERTEXT);
    mlkemEncrypt(key, m, kr + 32, (uint8_t *)out->ptr + out->len);
    out->len += MLKEM_CIPHERTEXT;
    memcpy(shared, kr, 32);
    iris_wipe(kr, sizeof kr);
    iris_wipe(m, sizeof m);
    return 1;
}

/* Decapsulation with the key made from seed. A ciphertext that wasn't made
   for the key gives the implicit rejection value SHAKE256(z || c), so a
   wrong one can't be told from the outside. */
void iris_mlkem768_decapsulate(const char *seed, const char *ct, char *shared) {
    const uint8_t *c = (const uint8_t *)ct, *z = (const uint8_t *)seed + 32;
    uint8_t ek[MLKEM_PUBLIC], h[32], m[32], kr[64], other[32], again[MLKEM_CIPHERTEXT];
    MlkemPoly s[MLKEM_K], u, w, v;
    mlkemKeyGen((const uint8_t *)seed, ek, s);
    memset(&w, 0, sizeof w);
    for (int i = 0; i < MLKEM_K; i++) {
        mlkemDecode(&u, c + 320 * i, 10);
        mlkemDecompress(&u, 10);
        mlkemNtt(&u);
        mlkemMulAdd(&w, &s[i], &u);
    }
    mlkemInvNtt(&w);
    mlkemDecode(&v, c + 320 * MLKEM_K, 4);
    mlkemDecompress(&v, 4);
    for (int i = 0; i < 256; i++) v.c[i] = modq((uint32_t)v.c[i] + MLKEM_Q - w.c[i]);
    mlkemCompress(&v, 1);
    memset(m, 0, sizeof m);
    for (int i = 0; i < 256; i++) m[i / 8] |= (uint8_t)(v.c[i] << (i % 8));
    SHA3_256(ek, sizeof ek, NULL, 0, h);
    SHA3_512(m, 32, h, 32, kr);
    keccakHash(136, 0x1f, z, 32, c, MLKEM_CIPHERTEXT, other, 32);
    mlkemEncrypt(ek, m, kr + 32, again);
    uint8_t diff = 0;
    for (int i = 0; i < MLKEM_CIPHERTEXT; i++) diff |= again[i] ^ c[i];
    /* all ones when they differ, without a branch */
    uint8_t mask = (uint8_t)-(uint8_t)((diff | (uint8_t)-diff) >> 7);
    for (int i = 0; i < 32; i++) shared[i] = (char)((kr[i] & ~mask) | (other[i] & mask));
    iris_wipe(s, sizeof s);
    iris_wipe(&w, sizeof w);
    iris_wipe(&v, sizeof v);
    iris_wipe(m, sizeof m);
    iris_wipe(kr, sizeof kr);
}

/* bcrypt_pbkdf, as OpenSSH encrypts private keys with a passphrase: PBKDF2's
   shape over SHA-512, with a Blowfish key schedule run 64 times as each
   round's hash. Blowfish's starting P-array and S-boxes are the hex digits
   of pi after the point. */
static const uint32_t blowfishPi[1042] = {
    0x243f6a88, 0x85a308d3, 0x13198a2e, 0x03707344, 0xa4093822, 0x299f31d0,
    0x082efa98, 0xec4e6c89, 0x452821e6, 0x38d01377, 0xbe5466cf, 0x34e90c6c,
    0xc0ac29b7, 0xc97c50dd, 0x3f84d5b5, 0xb5470917, 0x9216d5d9, 0x8979fb1b,
    0xd1310ba6, 0x98dfb5ac, 0x2ffd72db, 0xd01adfb7, 0xb8e1afed, 0x6a267e96,
    0xba7c9045, 0xf12c7f99, 0x24a19947, 0xb3916cf7, 0x0801f2e2, 0x858efc16,
    0x636920d8, 0x71574e69, 0xa458fea3, 0xf4933d7e, 0x0d95748f, 0x728eb658,
    0x718bcd58, 0x82154aee, 0x7b54a41d, 0xc25a59b5, 0x9c30d539, 0x2af26013,
    0xc5d1b023, 0x286085f0, 0xca417918, 0xb8db38ef, 0x8e79dcb0, 0x603a180e,
    0x6c9e0e8b, 0xb01e8a3e, 0xd71577c1, 0xbd314b27, 0x78af2fda, 0x55605c60,
    0xe65525f3, 0xaa55ab94, 0x57489862, 0x63e81440, 0x55ca396a, 0x2aab10b6,
    0xb4cc5c34, 0x1141e8ce, 0xa15486af, 0x7c72e993, 0xb3ee1411, 0x636fbc2a,
    0x2ba9c55d, 0x741831f6, 0xce5c3e16, 0x9b87931e, 0xafd6ba33, 0x6c24cf5c,
    0x7a325381, 0x28958677, 0x3b8f4898, 0x6b4bb9af, 0xc4bfe81b, 0x66282193,
    0x61d809cc, 0xfb21a991, 0x487cac60, 0x5dec8032, 0xef845d5d, 0xe98575b1,
    0xdc262302, 0xeb651b88, 0x23893e81, 0xd396acc5, 0x0f6d6ff3, 0x83f44239,
    0x2e0b4482, 0xa4842004, 0x69c8f04a, 0x9e1f9b5e, 0x21c66842, 0xf6e96c9a,
    0x670c9c61, 0xabd388f0, 0x6a51a0d2, 0xd8542f68, 0x960fa728, 0xab5133a3,
    0x6eef0b6c, 0x137a3be4, 0xba3bf050, 0x7efb2a98, 0xa1f1651d, 0x39af0176,
    0x66ca593e, 0x82430e88, 0x8cee8619, 0x456f9fb4, 0x7d84a5c3, 0x3b8b5ebe,
    0xe06f75d8, 0x85c12073, 0x401a449f, 0x56c16aa6, 0x4ed3aa62, 0x363f7706,
    0x1bfedf72, 0x429b023d, 0x37d0d724, 0xd00a1248, 0xdb0fead3, 0x49f1c09b,
    0x075372c9, 0x80991b7b, 0x25d479d8, 0xf6e8def7, 0xe3fe501a, 0xb6794c3b,
    0x976ce0bd, 0x04c006ba, 0xc1a94fb6, 0x409f60c4, 0x5e5c9ec2, 0x196a2463,
    0x68fb6faf, 0x3e6c53b5, 0x1339b2eb, 0x3b52ec6f, 0x6dfc511f, 0x9b30952c,
    0xcc814544, 0xaf5ebd09, 0xbee3d004, 0xde334afd, 0x660f2807, 0x192e4bb3,
    0xc0cba857, 0x45c8740f, 0xd20b5f39, 0xb9d3fbdb, 0x5579c0bd, 0x1a60320a,
    0xd6a100c6, 0x402c7279, 0x679f25fe, 0xfb1fa3cc, 0x8ea5e9f8, 0xdb3222f8,
    0x3c7516df, 0xfd616b15, 0x2f501ec8, 0xad0552ab, 0x323db5fa, 0xfd238760,
    0x53317b48, 0x3e00df82, 0x9e5c57bb, 0xca6f8ca0, 0x1a87562e, 0xdf1769db,
    0xd542a8f6, 0x287effc3, 0xac6732c6, 0x8c4f5573, 0x695b27b0, 0xbbca58c8,
    0xe1ffa35d, 0xb8f011a0, 0x10fa3d98, 0xfd2183b8, 0x4afcb56c, 0x2dd1d35b,
    0x9a53e479, 0xb6f84565, 0xd28e49bc, 0x4bfb9790, 0xe1ddf2da, 0xa4cb7e33,
    0x62fb1341, 0xcee4c6e8, 0xef20cada, 0x36774c01, 0xd07e9efe, 0x2bf11fb4,
    0x95dbda4d, 0xae909198, 0xeaad8e71, 0x6b93d5a0, 0xd08ed1d0, 0xafc725e0,
    0x8e3c5b2f, 0x8e7594b7, 0x8ff6e2fb, 0xf2122b64, 0x8888b812, 0x900df01c,
    0x4fad5ea0, 0x688fc31c, 0xd1cff191, 0xb3a8c1ad, 0x2f2f2218, 0xbe0e1777,
    0xea752dfe, 0x8b021fa1, 0xe5a0cc0f, 0xb56f74e8, 0x18acf3d6, 0xce89e299,
    0xb4a84fe0, 0xfd13e0b7, 0x7cc43b81, 0xd2ada8d9, 0x165fa266, 0x80957705,
    0x93cc7314, 0x211a1477, 0xe6ad2065, 0x77b5fa86, 0xc75442f5, 0xfb9d35cf,
    0xebcdaf0c, 0x7b3e89a0, 0xd6411bd3, 0xae1e7e49, 0x00250e2d, 0x2071b35e,
    0x226800bb, 0x57b8e0af, 0x2464369b, 0xf009b91e, 0x5563911d, 0x59dfa6aa,
    0x78c14389, 0xd95a537f, 0x207d5ba2, 0x02e5b9c5, 0x83260376, 0x6295cfa9,
    0x11c81968, 0x4e734a41, 0xb3472dca, 0x7b14a94a, 0x1b510052, 0x9a532915,
    0xd60f573f, 0xbc9bc6e4, 0x2b60a476, 0x81e67400, 0x08ba6fb5, 0x571be91f,
    0xf296ec6b, 0x2a0dd915, 0xb6636521, 0xe7b9f9b6, 0xff34052e, 0xc5855664,
    0x53b02d5d, 0xa99f8fa1, 0x08ba4799, 0x6e85076a, 0x4b7a70e9, 0xb5b32944,
    0xdb75092e, 0xc4192623, 0xad6ea6b0, 0x49a7df7d, 0x9cee60b8, 0x8fedb266,
    0xecaa8c71, 0x699a17ff, 0x5664526c, 0xc2b19ee1, 0x193602a5, 0x75094c29,
    0xa0591340, 0xe4183a3e, 0x3f54989a, 0x5b429d65, 0x6b8fe4d6, 0x99f73fd6,
    0xa1d29c07, 0xefe830f5, 0x4d2d38e6, 0xf0255dc1, 0x4cdd2086, 0x8470eb26,
    0x6382e9c6, 0x021ecc5e, 0x09686b3f, 0x3ebaefc9, 0x3c971814, 0x6b6a70a1,
    0x687f3584, 0x52a0e286, 0xb79c5305, 0xaa500737, 0x3e07841c, 0x7fdeae5c,
    0x8e7d44ec, 0x5716f2b8, 0xb03ada37, 0xf0500c0d, 0xf01c1f04, 0x0200b3ff,
    0xae0cf51a, 0x3cb574b2, 0x25837a58, 0xdc0921bd, 0xd19113f9, 0x7ca92ff6,
    0x94324773, 0x22f54701, 0x3ae5e581, 0x37c2dadc, 0xc8b57634, 0x9af3dda7,
    0xa9446146, 0x0fd0030e, 0xecc8c73e, 0xa4751e41, 0xe238cd99, 0x3bea0e2f,
    0x3280bba1, 0x183eb331, 0x4e548b38, 0x4f6db908, 0x6f420d03, 0xf60a04bf,
    0x2cb81290, 0x24977c79, 0x5679b072, 0xbcaf89af, 0xde9a771f, 0xd9930810,
    0xb38bae12, 0xdccf3f2e, 0x5512721f, 0x2e6b7124, 0x501adde6, 0x9f84cd87,
    0x7a584718, 0x7408da17, 0xbc9f9abc, 0xe94b7d8c, 0xec7aec3a, 0xdb851dfa,
    0x63094366, 0xc464c3d2, 0xef1c1847, 0x3215d908, 0xdd433b37, 0x24c2ba16,
    0x12a14d43, 0x2a65c451, 0x50940002, 0x133ae4dd, 0x71dff89e, 0x10314e55,
    0x81ac77d6, 0x5f11199b, 0x043556f1, 0xd7a3c76b, 0x3c11183b, 0x5924a509,
    0xf28fe6ed, 0x97f1fbfa, 0x9ebabf2c, 0x1e153c6e, 0x86e34570, 0xeae96fb1,
    0x860e5e0a, 0x5a3e2ab3, 0x771fe71c, 0x4e3d06fa, 0x2965dcb9, 0x99e71d0f,
    0x803e89d6, 0x5266c825, 0x2e4cc978, 0x9c10b36a, 0xc6150eba, 0x94e2ea78,
    0xa5fc3c53, 0x1e0a2df4, 0xf2f74ea7, 0x361d2b3d, 0x1939260f, 0x19c27960,
    0x5223a708, 0xf71312b6, 0xebadfe6e, 0xeac31f66, 0xe3bc4595, 0xa67bc883,
    0xb17f37d1, 0x018cff28, 0xc332ddef, 0xbe6c5aa5, 0x65582185, 0x68ab9802,
    0xeecea50f, 0xdb2f953b, 0x2aef7dad, 0x5b6e2f84, 0x1521b628, 0x29076170,
    0xecdd4775, 0x619f1510, 0x13cca830, 0xeb61bd96, 0x0334fe1e, 0xaa0363cf,
    0xb5735c90, 0x4c70a239, 0xd59e9e0b, 0xcbaade14, 0xeecc86bc, 0x60622ca7,
    0x9cab5cab, 0xb2f3846e, 0x648b1eaf, 0x19bdf0ca, 0xa02369b9, 0x655abb50,
    0x40685a32, 0x3c2ab4b3, 0x319ee9d5, 0xc021b8f7, 0x9b540b19, 0x875fa099,
    0x95f7997e, 0x623d7da8, 0xf837889a, 0x97e32d77, 0x11ed935f, 0x16681281,
    0x0e358829, 0xc7e61fd6, 0x96dedfa1, 0x7858ba99, 0x57f584a5, 0x1b227263,
    0x9b83c3ff, 0x1ac24696, 0xcdb30aeb, 0x532e3054, 0x8fd948e4, 0x6dbc3128,
    0x58ebf2ef, 0x34c6ffea, 0xfe28ed61, 0xee7c3c73, 0x5d4a14d9, 0xe864b7e3,
    0x42105d14, 0x203e13e0, 0x45eee2b6, 0xa3aaabea, 0xdb6c4f15, 0xfacb4fd0,
    0xc742f442, 0xef6abbb5, 0x654f3b1d, 0x41cd2105, 0xd81e799e, 0x86854dc7,
    0xe44b476a, 0x3d816250, 0xcf62a1f2, 0x5b8d2646, 0xfc8883a0, 0xc1c7b6a3,
    0x7f1524c3, 0x69cb7492, 0x47848a0b, 0x5692b285, 0x095bbf00, 0xad19489d,
    0x1462b174, 0x23820e00, 0x58428d2a, 0x0c55f5ea, 0x1dadf43e, 0x233f7061,
    0x3372f092, 0x8d937e41, 0xd65fecf1, 0x6c223bdb, 0x7cde3759, 0xcbee7460,
    0x4085f2a7, 0xce77326e, 0xa6078084, 0x19f8509e, 0xe8efd855, 0x61d99735,
    0xa969a7aa, 0xc50c06c2, 0x5a04abfc, 0x800bcadc, 0x9e447a2e, 0xc3453484,
    0xfdd56705, 0x0e1e9ec9, 0xdb73dbd3, 0x105588cd, 0x675fda79, 0xe3674340,
    0xc5c43465, 0x713e38d8, 0x3d28f89e, 0xf16dff20, 0x153e21e7, 0x8fb03d4a,
    0xe6e39f2b, 0xdb83adf7, 0xe93d5a68, 0x948140f7, 0xf64c261c, 0x94692934,
    0x411520f7, 0x7602d4f7, 0xbcf46b2e, 0xd4a20068, 0xd4082471, 0x3320f46a,
    0x43b7d4b7, 0x500061af, 0x1e39f62e, 0x97244546, 0x14214f74, 0xbf8b8840,
    0x4d95fc1d, 0x96b591af, 0x70f4ddd3, 0x66a02f45, 0xbfbc09ec, 0x03bd9785,
    0x7fac6dd0, 0x31cb8504, 0x96eb27b3, 0x55fd3941, 0xda2547e6, 0xabca0a9a,
    0x28507825, 0x530429f4, 0x0a2c86da, 0xe9b66dfb, 0x68dc1462, 0xd7486900,
    0x680ec0a4, 0x27a18dee, 0x4f3ffea2, 0xe887ad8c, 0xb58ce006, 0x7af4d6b6,
    0xaace1e7c, 0xd3375fec, 0xce78a399, 0x406b2a42, 0x20fe9e35, 0xd9f385b9,
    0xee39d7ab, 0x3b124e8b, 0x1dc9faf7, 0x4b6d1856, 0x26a36631, 0xeae397b2,
    0x3a6efa74, 0xdd5b4332, 0x6841e7f7, 0xca7820fb, 0xfb0af54e, 0xd8feb397,
    0x454056ac, 0xba489527, 0x55533a3a, 0x20838d87, 0xfe6ba9b7, 0xd096954b,
    0x55a867bc, 0xa1159a58, 0xcca92963, 0x99e1db33, 0xa62a4a56, 0x3f3125f9,
    0x5ef47e1c, 0x9029317c, 0xfdf8e802, 0x04272f70, 0x80bb155c, 0x05282ce3,
    0x95c11548, 0xe4c66d22, 0x48c1133f, 0xc70f86dc, 0x07f9c9ee, 0x41041f0f,
    0x404779a4, 0x5d886e17, 0x325f51eb, 0xd59bc0d1, 0xf2bcc18f, 0x41113564,
    0x257b7834, 0x602a9c60, 0xdff8e8a3, 0x1f636c1b, 0x0e12b4c2, 0x02e1329e,
    0xaf664fd1, 0xcad18115, 0x6b2395e0, 0x333e92e1, 0x3b240b62, 0xeebeb922,
    0x85b2a20e, 0xe6ba0d99, 0xde720c8c, 0x2da2f728, 0xd0127845, 0x95b794fd,
    0x647d0862, 0xe7ccf5f0, 0x5449a36f, 0x877d48fa, 0xc39dfd27, 0xf33e8d1e,
    0x0a476341, 0x992eff74, 0x3a6f6eab, 0xf4f8fd37, 0xa812dc60, 0xa1ebddf8,
    0x991be14c, 0xdb6e6b0d, 0xc67b5510, 0x6d672c37, 0x2765d43b, 0xdcd0e804,
    0xf1290dc7, 0xcc00ffa3, 0xb5390f92, 0x690fed0b, 0x667b9ffb, 0xcedb7d9c,
    0xa091cf0b, 0xd9155ea3, 0xbb132f88, 0x515bad24, 0x7b9479bf, 0x763bd6eb,
    0x37392eb3, 0xcc115979, 0x8026e297, 0xf42e312d, 0x6842ada7, 0xc66a2b3b,
    0x12754ccc, 0x782ef11c, 0x6a124237, 0xb79251e7, 0x06a1bbe6, 0x4bfb6350,
    0x1a6b1018, 0x11caedfa, 0x3d25bdd8, 0xe2e1c3c9, 0x44421659, 0x0a121386,
    0xd90cec6e, 0xd5abea2a, 0x64af674e, 0xda86a85f, 0xbebfe988, 0x64e4c3fe,
    0x9dbc8057, 0xf0f7c086, 0x60787bf8, 0x6003604d, 0xd1fd8346, 0xf6381fb0,
    0x7745ae04, 0xd736fccc, 0x83426b33, 0xf01eab71, 0xb0804187, 0x3c005e5f,
    0x77a057be, 0xbde8ae24, 0x55464299, 0xbf582e61, 0x4e58f48f, 0xf2ddfda2,
    0xf474ef38, 0x8789bdc2, 0x5366f9c3, 0xc8b38e74, 0xb475f255, 0x46fcd9b9,
    0x7aeb2661, 0x8b1ddf84, 0x846a0e79, 0x915f95e2, 0x466e598e, 0x20b45770,
    0x8cd55591, 0xc902de4c, 0xb90bace1, 0xbb8205d0, 0x11a86248, 0x7574a99e,
    0xb77f19b6, 0xe0a9dc09, 0x662d09a1, 0xc4324633, 0xe85a1f02, 0x09f0be8c,
    0x4a99a025, 0x1d6efe10, 0x1ab93d1d, 0x0ba5a4df, 0xa186f20f, 0x2868f169,
    0xdcb7da83, 0x573906fe, 0xa1e2ce9b, 0x4fcd7f52, 0x50115e01, 0xa70683fa,
    0xa002b5c4, 0x0de6d027, 0x9af88c27, 0x773f8641, 0xc3604c06, 0x61a806b5,
    0xf0177a28, 0xc0f586e0, 0x006058aa, 0x30dc7d62, 0x11e69ed7, 0x2338ea63,
    0x53c2dd94, 0xc2c21634, 0xbbcbee56, 0x90bcb6de, 0xebfc7da1, 0xce591d76,
    0x6f05e409, 0x4b7c0188, 0x39720a3d, 0x7c927c24, 0x86e3725f, 0x724d9db9,
    0x1ac15bb4, 0xd39eb8fc, 0xed545578, 0x08fca5b5, 0xd83d7cd3, 0x4dad0fc4,
    0x1e50ef5e, 0xb161e6f8, 0xa28514d9, 0x6c51133c, 0x6fd5c7e7, 0x56e14ec4,
    0x362abfce, 0xddc6c837, 0xd79a3234, 0x92638212, 0x670efa8e, 0x406000e0,
    0x3a39ce37, 0xd3faf5cf, 0xabc27737, 0x5ac52d1b, 0x5cb0679e, 0x4fa33742,
    0xd3822740, 0x99bc9bbe, 0xd5118e9d, 0xbf0f7315, 0xd62d1c7e, 0xc700c47b,
    0xb78c1b6b, 0x21a19045, 0xb26eb1be, 0x6a366eb4, 0x5748ab2f, 0xbc946e79,
    0xc6a376d2, 0x6549c2c8, 0x530ff8ee, 0x468dde7d, 0xd5730a1d, 0x4cd04dc6,
    0x2939bbdb, 0xa9ba4650, 0xac9526e8, 0xbe5ee304, 0xa1fad5f0, 0x6a2d519a,
    0x63ef8ce2, 0x9a86ee22, 0xc089c2b8, 0x43242ef6, 0xa51e03aa, 0x9cf2d0a4,
    0x83c061ba, 0x9be96a4d, 0x8fe51550, 0xba645bd6, 0x2826a2f9, 0xa73a3ae1,
    0x4ba99586, 0xef5562e9, 0xc72fefd3, 0xf752f7da, 0x3f046f69, 0x77fa0a59,
    0x80e4a915, 0x87b08601, 0x9b09e6ad, 0x3b3ee593, 0xe990fd5a, 0x9e34d797,
    0x2cf0b7d9, 0x022b8b51, 0x96d5ac3a, 0x017da67d, 0xd1cf3ed6, 0x7c7d2d28,
    0x1f9f25cf, 0xadf2b89b, 0x5ad6b472, 0x5a88f54c, 0xe029ac71, 0xe019a5e6,
    0x47b0acfd, 0xed93fa9b, 0xe8d3c48d, 0x283b57cc, 0xf8d56629, 0x79132e28,
    0x785f0191, 0xed756055, 0xf7960e44, 0xe3d35e8c, 0x15056dd4, 0x88f46dba,
    0x03a16125, 0x0564f0bd, 0xc3eb9e15, 0x3c9057a2, 0x97271aec, 0xa93a072a,
    0x1b3f6d9b, 0x1e6321f5, 0xf59c66fb, 0x26dcf319, 0x7533d928, 0xb155fdf5,
    0x03563482, 0x8aba3cbb, 0x28517711, 0xc20ad9f8, 0xabcc5167, 0xccad925f,
    0x4de81751, 0x3830dc8e, 0x379d5862, 0x9320f991, 0xea7a90c2, 0xfb3e7bce,
    0x5121ce64, 0x774fbe32, 0xa8b6e37e, 0xc3293d46, 0x48de5369, 0x6413e680,
    0xa2ae0810, 0xdd6db224, 0x69852dfd, 0x09072166, 0xb39a460a, 0x6445c0dd,
    0x586cdecf, 0x1c20c8ae, 0x5bbef7dd, 0x1b588d40, 0xccd2017f, 0x6bb4e3bb,
    0xdda26a7e, 0x3a59ff45, 0x3e350a44, 0xbcb4cdd5, 0x72eacea8, 0xfa6484bb,
    0x8d6612ae, 0xbf3c6f47, 0xd29be463, 0x542f5d9e, 0xaec2771b, 0xf64e6370,
    0x740e0d8d, 0xe75b1357, 0xf8721671, 0xaf537d5d, 0x4040cb08, 0x4eb4e2cc,
    0x34d2466a, 0x0115af84, 0xe1b00428, 0x95983a1d, 0x06b89fb4, 0xce6ea048,
    0x6f3f3b82, 0x3520ab82, 0x011a1d4b, 0x277227f8, 0x611560b1, 0xe7933fdc,
    0xbb3a792b, 0x344525bd, 0xa08839e1, 0x51ce794b, 0x2f32c9b7, 0xa01fbac9,
    0xe01cc87e, 0xbcc7d1f6, 0xcf0111c3, 0xa1e8aac7, 0x1a908749, 0xd44fbd9a,
    0xd0dadecb, 0xd50ada38, 0x0339c32a, 0xc6913667, 0x8df9317c, 0xe0b12b4f,
    0xf79e59b7, 0x43f5bb3a, 0xf2d519ff, 0x27d9459c, 0xbf97222c, 0x15e6fc2a,
    0x0f91fc71, 0x9b941525, 0xfae59361, 0xceb69ceb, 0xc2a86459, 0x12baa8d1,
    0xb6c1075e, 0xe3056a0c, 0x10d25065, 0xcb03a442, 0xe0ec6e0e, 0x1698db3b,
    0x4c98a0be, 0x3278e964, 0x9f1f9532, 0xe0d392df, 0xd3a0342b, 0x8971f21e,
    0x1b0a7441, 0x4ba3348c, 0xc5be7120, 0xc37632d8, 0xdf359f8d, 0x9b992f2e,
    0xe60b6f47, 0x0fe3f11d, 0xe54cda54, 0x1edad891, 0xce6279cf, 0xcd3e7e6f,
    0x1618b166, 0xfd2c1d05, 0x848fd2c5, 0xf6fb2299, 0xf523f357, 0xa6327623,
    0x93a83531, 0x56cccd02, 0xacf08162, 0x5a75ebb5, 0x6e163697, 0x88d273cc,
    0xde966292, 0x81b949d0, 0x4c50901b, 0x71c65614, 0xe6c6c7bd, 0x327a140a,
    0x45e1d006, 0xc3f27b9a, 0xc9aa53fd, 0x62a80f00, 0xbb25bfe2, 0x35bdd2f6,
    0x71126905, 0xb2040222, 0xb6cbcf7c, 0xcd769c2b, 0x53113ec0, 0x1640e3d3,
    0x38abbd60, 0x2547adf0, 0xba38209c, 0xf746ce76, 0x77afa1c5, 0x20756060,
    0x85cbfe4e, 0x8ae88dd8, 0x7aaaf9b0, 0x4cf9aa7e, 0x1948c25c, 0x02fb8a8c,
    0x01c36ae4, 0xd6ebe1f9, 0x90d4f869, 0xa65cdea0, 0x3f09252d, 0xc208e69f,
    0xb74e6132, 0xce77e25b, 0x578fdfe3, 0x3ac372e6
};

typedef struct {
    uint32_t p[18];
    uint32_t s[4][256];
} Blowfish;

static uint32_t blowfishF(const Blowfish *b, uint32_t x) {
    return ((b->s[0][x >> 24] + b->s[1][x >> 16 & 0xff]) ^ b->s[2][x >> 8 & 0xff]) + b->s[3][x & 0xff];
}

static void blowfishEncrypt(const Blowfish *b, uint32_t *l, uint32_t *r) {
    uint32_t xl = *l, xr = *r;
    xl ^= b->p[0];
    for (int i = 1; i <= 16; i += 2) {
        xr ^= blowfishF(b, xl) ^ b->p[i];
        xl ^= blowfishF(b, xr) ^ b->p[i + 1];
    }
    *l = xr ^ b->p[17];
    *r = xl;
}

/* the next 4 bytes of data, big-endian, going round to the start */
static uint32_t blowfishWord(const uint8_t *data, size_t n, size_t *at) {
    uint32_t w = 0;
    for (int i = 0; i < 4; i++) {
        w = w << 8 | data[*at];
        *at = (*at + 1) % n;
    }
    return w;
}

/* the expensive key schedule's step: the key into the P-array, then
   encryptions that mix in data (none when data is NULL) fill both */
static void blowfishExpand(Blowfish *b, const uint8_t *data, size_t dn, const uint8_t *key, size_t kn) {
    size_t at = 0;
    for (int i = 0; i < 18; i++) b->p[i] ^= blowfishWord(key, kn, &at);
    uint32_t l = 0, r = 0;
    at = 0;
    for (int i = 0; i < 18; i += 2) {
        if (data) {
            l ^= blowfishWord(data, dn, &at);
            r ^= blowfishWord(data, dn, &at);
        }
        blowfishEncrypt(b, &l, &r);
        b->p[i] = l;
        b->p[i + 1] = r;
    }
    for (int i = 0; i < 4; i++) {
        for (int k = 0; k < 256; k += 2) {
            if (data) {
                l ^= blowfishWord(data, dn, &at);
                r ^= blowfishWord(data, dn, &at);
            }
            blowfishEncrypt(b, &l, &r);
            b->s[i][k] = l;
            b->s[i][k + 1] = r;
        }
    }
}

static void bcryptHash(const uint8_t pass[64], const uint8_t salt[64], uint8_t out[32]) {
    static const char text[] = "OxychromaticBlowfishSwatDynamite";
    Blowfish b;
    memcpy(b.p, blowfishPi, sizeof b.p);
    memcpy(b.s, blowfishPi + 18, sizeof b.s);
    blowfishExpand(&b, salt, 64, pass, 64);
    for (int i = 0; i < 64; i++) {
        blowfishExpand(&b, NULL, 0, salt, 64);
        blowfishExpand(&b, NULL, 0, pass, 64);
    }
    uint32_t c[8];
    size_t at = 0;
    for (int i = 0; i < 8; i++) c[i] = blowfishWord((const uint8_t *)text, 32, &at);
    for (int i = 0; i < 64; i++) {
        for (int j = 0; j < 8; j += 2) blowfishEncrypt(&b, &c[j], &c[j + 1]);
    }
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)c[i];
        out[4 * i + 1] = (uint8_t)(c[i] >> 8);
        out[4 * i + 2] = (uint8_t)(c[i] >> 16);
        out[4 * i + 3] = (uint8_t)(c[i] >> 24);
    }
    iris_wipe(&b, sizeof b);
    iris_wipe(c, sizeof c);
}

/* size bytes of key from a password and a salt with rounds; the bytes of
   each block are spread over the key with a stride, as OpenBSD's does */
void iris_bcrypt_pbkdf(const char *pass, int64_t passLen, const char *salt, int64_t saltLen, int64_t rounds, char *key,
                       int64_t size) {
    size_t stride = ((size_t)size + 31) / 32;
    size_t amt = ((size_t)size + stride - 1) / stride;
    uint8_t sha2pass[64], sha2salt[64], out[32], tmp[32];
    iris_sha512(pass, passLen, sha2pass);
    uint8_t *countsalt = malloc((size_t)saltLen + 4);
    if (!countsalt) outOfMemory();
    memcpy(countsalt, salt, (size_t)saltLen);
    size_t left = (size_t)size;
    for (uint32_t count = 1; left > 0; count++) {
        countsalt[saltLen] = (uint8_t)(count >> 24);
        countsalt[saltLen + 1] = (uint8_t)(count >> 16);
        countsalt[saltLen + 2] = (uint8_t)(count >> 8);
        countsalt[saltLen + 3] = (uint8_t)count;
        iris_sha512((const char *)countsalt, saltLen + 4, sha2salt);
        bcryptHash(sha2pass, sha2salt, tmp);
        memcpy(out, tmp, sizeof out);
        for (int64_t r = 1; r < rounds; r++) {
            iris_sha512((const char *)tmp, sizeof tmp, sha2salt);
            bcryptHash(sha2pass, sha2salt, tmp);
            for (int i = 0; i < 32; i++) out[i] ^= tmp[i];
        }
        if (amt > left) amt = left;
        size_t i;
        for (i = 0; i < amt; i++) {
            size_t dest = i * stride + (count - 1);
            if (dest >= (size_t)size) break;
            key[dest] = (char)out[i];
        }
        left -= i;
    }
    iris_wipe(sha2pass, sizeof sha2pass);
    iris_wipe(sha2salt, sizeof sha2salt);
    iris_wipe(out, sizeof out);
    iris_wipe(tmp, sizeof tmp);
    free(countsalt);
}

/* chacha20-poly1305@openssh.com, SSH's packets: a 64-byte key whose
   first half encrypts the payload and makes the Poly1305 key, and whose
   second half encrypts the 4-byte length. The nonce is the packet's
   sequence number, and the tag covers the encrypted length and payload. */
static void sshChachaNonce(int64_t seq, unsigned char nonce[12]) {
    memset(nonce, 0, 4);
    for (int i = 0; i < 8; i++) nonce[4 + i] = (unsigned char)((uint64_t)seq >> (56 - 8 * i));
}

static void sshChachaTag(const unsigned char *key, const unsigned char nonce[12], const unsigned char *m, size_t n,
                         unsigned char tag[16]) {
    unsigned char block[64];
    chachaBlock(key, 0, nonce, block);
    Poly1305 p;
    polyInit(&p, block);
    while (n >= 16) {
        polyBlock(&p, m, 1ULL << 40);
        m += 16;
        n -= 16;
    }
    if (n) {
        unsigned char last[16] = {0};
        memcpy(last, m, n);
        last[n] = 1;
        polyBlock(&p, last, 0);
    }
    polyFinish(&p, tag);
    iris_wipe(block, sizeof block);
}

/* Adds the packet for plain (padding length, payload, padding) to out:
   its encrypted length and text, and the tag. */
void iris_ssh_chacha_seal(const unsigned char *key, int64_t seq, const unsigned char *plain, int64_t len,
                          IrisString *out) {
    unsigned char nonce[12];
    sshChachaNonce(seq, nonce);
    const unsigned char *none = NULL;
    unsigned char *o = aeadRoom(out, 4 + len + 16, &plain, &none);
    unsigned char size[4] = {(unsigned char)(len >> 24), (unsigned char)(len >> 16), (unsigned char)(len >> 8),
                             (unsigned char)len};
    chachaXor(key + 32, 0, nonce, size, o, 4);
    chachaXor(key, 1, nonce, plain, o + 4, (size_t)len);
    sshChachaTag(key, nonce, o, 4 + (size_t)len, o + 4 + len);
    out->len += 4 + len + 16;
}

/* The length of the packet whose first 4 bytes are enc. */
int64_t iris_ssh_chacha_length(const unsigned char *key, int64_t seq, const unsigned char *enc) {
    unsigned char nonce[12], size[4];
    sshChachaNonce(seq, nonce);
    chachaXor(key + 32, 0, nonce, enc, size, 4);
    return (int64_t)size[0] << 24 | (int64_t)size[1] << 16 | (int64_t)size[2] << 8 | (int64_t)size[3];
}

/* Checks the tag of a packet of 4 + len + 16 bytes and adds its decrypted
   text to out: 1, or 0 when the tag doesn't match and out gets nothing. */
int iris_ssh_chacha_open(const unsigned char *key, int64_t seq, const unsigned char *packet, int64_t len,
                         IrisString *out) {
    unsigned char nonce[12], tag[16];
    sshChachaNonce(seq, nonce);
    sshChachaTag(key, nonce, packet, 4 + (size_t)len, tag);
    if (!iris_const_equal((const char *)tag, 16, (const char *)packet + 4 + len, 16)) return 0;
    const unsigned char *none = NULL;
    const unsigned char *text = packet + 4;
    unsigned char *pt = aeadRoom(out, len, &text, &none);
    chachaXor(key, 1, nonce, text, pt, (size_t)len);
    out->len += len;
    return 1;
}

/* AES-GCM (NIST SP 800-38D) with a 12-byte nonce and a 16-byte tag, for
   AES-128, AES-192 and AES-256. Where the CPU has AES and carry-less
   multiply instructions (x86_64 AES-NI and PCLMULQDQ, ARMv8 AES and PMULL)
   they do the work. Elsewhere plain C does, in constant time: each S-box
   lookup reads the whole table. */

#if defined(__x86_64__)
#include <wmmintrin.h>
#include <emmintrin.h>
#define AES_HW_TARGET __attribute__((target("aes,pclmul,sse2")))
#elif defined(__aarch64__)
#include <arm_neon.h>
#ifdef __linux__
#include <sys/auxv.h>
#endif
#define AES_HW_TARGET __attribute__((target("aes")))
#endif

static const uint8_t aesSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

/* sbox[x], reading the whole table, 8 entries at a time, so the time
   doesn't depend on x */
static uint8_t aesSub(uint8_t x) {
    uint64_t word = 0;
    unsigned want = x >> 3;
    for (unsigned i = 0; i < 32; i++) {
        uint64_t w;
        memcpy(&w, aesSbox + 8 * i, 8);
        uint64_t hit = 0 - (uint64_t)((((i ^ want) - 1u) >> 8) & 1u);
        word |= w & hit;
    }
    uint8_t bytes[8];
    memcpy(bytes, &word, 8);
    uint8_t r = 0;
    for (unsigned i = 0; i < 8; i++) {
        uint8_t hit = (uint8_t)(0u - ((((i ^ (x & 7u)) - 1u) >> 8) & 1u));
        r |= bytes[i] & hit;
    }
    return r;
}

typedef struct {
    uint8_t rk[15][16];         /* round keys, as FIPS 197 lays them out */
    int rounds;
    int hw;
    uint64_t h1, h0;            /* GHASH's H, high and low 64 bits */
    /* with the CPU's carry-less multiply: H^(i+1) as low, high 64 bits
       and the two xored, so 8 blocks share one reduction */
    uint64_t hp[16][2], hk[16][2];
    int wide;                   /* x86_64 with VAES and VPCLMULQDQ: 2 blocks an instruction */
    /* for 2 blocks a register: H^(16 - 2j) and H^(15 - 2j) side by side */
    uint64_t hpw[8][4], hkw[8][4];
} AesGcm;

static int aesHw = -1;

static int aesHasHw(void) {
    if (aesHw < 0) {
        int hw = 0;
        if (!getenv("IRIS_AES_SOFT")) {
#if defined(__x86_64__)
            __builtin_cpu_init();
            hw = __builtin_cpu_supports("aes") && __builtin_cpu_supports("pclmul");
#elif defined(__aarch64__) && defined(__APPLE__)
            hw = 1;
#elif defined(__aarch64__) && defined(__linux__)
            unsigned long caps = getauxval(AT_HWCAP);
            hw = (caps & (1UL << 3)) && (caps & (1UL << 4));       /* HWCAP_AES, HWCAP_PMULL */
#endif
        }
        aesHw = hw;
    }
    return aesHw;
}

#ifdef AES_HW_TARGET
/* S-box of each byte of w with the AES instructions: w in all 4 columns,
   so ShiftRows changes nothing, and a zero round key */
AES_HW_TARGET static void aesSubWordHw(uint8_t w[4]) {
    uint32_t v;
    memcpy(&v, w, 4);
#if defined(__x86_64__)
    __m128i x = _mm_aesenclast_si128(_mm_set1_epi32((int)v), _mm_setzero_si128());
    v = (uint32_t)_mm_cvtsi128_si32(x);
#else
    uint8x16_t x = vaeseq_u8(vreinterpretq_u8_u32(vdupq_n_u32(v)), vdupq_n_u8(0));
    v = vgetq_lane_u32(vreinterpretq_u32_u8(x), 0);
#endif
    memcpy(w, &v, 4);
}
#endif

static void aesSubWord(uint8_t w[4]) {
#ifdef AES_HW_TARGET
    if (aesHasHw()) {
        aesSubWordHw(w);
        return;
    }
#endif
    for (int j = 0; j < 4; j++) w[j] = aesSub(w[j]);
}

#if defined(__x86_64__)
/* The key schedule with AESKEYGENASSIST, for 16 and 32-byte keys */
AES_HW_TARGET static inline __m128i aesKeyStep(__m128i k, __m128i assist) {
    k = _mm_xor_si128(k, _mm_slli_si128(k, 4));
    k = _mm_xor_si128(k, _mm_slli_si128(k, 4));
    k = _mm_xor_si128(k, _mm_slli_si128(k, 4));
    return _mm_xor_si128(k, assist);
}

#define AES128_ROUND(i, rcon)                                                                      \
    k = aesKeyStep(k, _mm_shuffle_epi32(_mm_aeskeygenassist_si128(k, rcon), 0xff));                \
    _mm_storeu_si128((__m128i *)a->rk[i], k)

#define AES256_ROUNDS(i, rcon)                                                                     \
    k1 = aesKeyStep(k1, _mm_shuffle_epi32(_mm_aeskeygenassist_si128(k2, rcon), 0xff));             \
    _mm_storeu_si128((__m128i *)a->rk[i], k1);                                                     \
    k2 = aesKeyStep(k2, _mm_shuffle_epi32(_mm_aeskeygenassist_si128(k1, 0), 0xaa));                \
    _mm_storeu_si128((__m128i *)a->rk[i + 1], k2)

AES_HW_TARGET static void aesExpandHw(AesGcm *a, const uint8_t *key, int keyLen) {
    if (keyLen == 16) {
        a->rounds = 10;
        __m128i k = _mm_loadu_si128((const __m128i *)key);
        _mm_storeu_si128((__m128i *)a->rk[0], k);
        AES128_ROUND(1, 0x01);
        AES128_ROUND(2, 0x02);
        AES128_ROUND(3, 0x04);
        AES128_ROUND(4, 0x08);
        AES128_ROUND(5, 0x10);
        AES128_ROUND(6, 0x20);
        AES128_ROUND(7, 0x40);
        AES128_ROUND(8, 0x80);
        AES128_ROUND(9, 0x1b);
        AES128_ROUND(10, 0x36);
        return;
    }
    a->rounds = 14;
    __m128i k1 = _mm_loadu_si128((const __m128i *)key), k2 = _mm_loadu_si128((const __m128i *)(key + 16));
    _mm_storeu_si128((__m128i *)a->rk[0], k1);
    _mm_storeu_si128((__m128i *)a->rk[1], k2);
    AES256_ROUNDS(2, 0x01);
    AES256_ROUNDS(4, 0x02);
    AES256_ROUNDS(6, 0x04);
    AES256_ROUNDS(8, 0x08);
    AES256_ROUNDS(10, 0x10);
    AES256_ROUNDS(12, 0x20);
    k1 = aesKeyStep(k1, _mm_shuffle_epi32(_mm_aeskeygenassist_si128(k2, 0x40), 0xff));
    _mm_storeu_si128((__m128i *)a->rk[14], k1);
}
#endif

static void aesExpand(AesGcm *a, const uint8_t *key, int keyLen) {
#if defined(__x86_64__)
    if (keyLen != 24 && aesHasHw()) {
        aesExpandHw(a, key, keyLen);
        return;
    }
#endif
    int nk = keyLen / 4;
    a->rounds = nk + 6;
    uint8_t w[60][4];
    for (int i = 0; i < nk; i++) memcpy(w[i], key + 4 * i, 4);
    uint8_t rcon = 1;
    for (int i = nk; i < 4 * (a->rounds + 1); i++) {
        uint8_t t[4];
        memcpy(t, w[i - 1], 4);
        if (i % nk == 0) {
            uint8_t first = t[0];
            t[0] = t[1];
            t[1] = t[2];
            t[2] = t[3];
            t[3] = first;
            aesSubWord(t);
            t[0] ^= rcon;
            rcon = (uint8_t)((rcon << 1) ^ ((rcon >> 7) * 0x1b));
        } else if (nk > 6 && i % nk == 4) {
            aesSubWord(t);
        }
        for (int j = 0; j < 4; j++) w[i][j] = w[i - nk][j] ^ t[j];
    }
    for (int r = 0; r <= a->rounds; r++) memcpy(a->rk[r], w[4 * r], 16);
}

static uint8_t aesTimes2(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }

static void aesBlockSoft(const AesGcm *a, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ a->rk[0][i];
    for (int r = 1; r <= a->rounds; r++) {
        uint8_t t[16];
        /* SubBytes and ShiftRows: byte c*4+row moves left by row columns */
        for (int c = 0; c < 4; c++) {
            for (int row = 0; row < 4; row++) t[c * 4 + row] = aesSub(s[((c + row) % 4) * 4 + row]);
        }
        if (r < a->rounds) {
            for (int c = 0; c < 4; c++) {
                uint8_t *x = t + 4 * c;
                uint8_t all = x[0] ^ x[1] ^ x[2] ^ x[3], first = x[0];
                x[0] ^= all ^ aesTimes2(x[0] ^ x[1]);
                x[1] ^= all ^ aesTimes2(x[1] ^ x[2]);
                x[2] ^= all ^ aesTimes2(x[2] ^ x[3]);
                x[3] ^= all ^ aesTimes2(x[3] ^ first);
            }
        }
        for (int i = 0; i < 16; i++) s[i] = t[i] ^ a->rk[r][i];
    }
    memcpy(out, s, 16);
}

/* The counter block hi:lo plus one: the low 32 bits only for GCM. */
static inline void aesCtrStep(uint64_t *hi, uint64_t *lo, int gcm) {
    if (gcm) {
        *lo = (*lo & 0xffffffff00000000ULL) | (uint32_t)(*lo + 1);
    } else if (++*lo == 0) {
        ++*hi;
    }
}

#ifdef AES_HW_TARGET
#if defined(__x86_64__)
AES_HW_TARGET static void aesBlockHw(const AesGcm *a, const uint8_t in[16], uint8_t out[16]) {
    __m128i s = _mm_xor_si128(_mm_loadu_si128((const __m128i *)in), _mm_loadu_si128((const __m128i *)a->rk[0]));
    for (int r = 1; r < a->rounds; r++) s = _mm_aesenc_si128(s, _mm_loadu_si128((const __m128i *)a->rk[r]));
    s = _mm_aesenclast_si128(s, _mm_loadu_si128((const __m128i *)a->rk[a->rounds]));
    _mm_storeu_si128((__m128i *)out, s);
}

/* 8 counter blocks from hi:lo encrypted side by side and xored into 128
   bytes of in. The blocks are made in registers: bytes stored one by one
   and read back as 16 would stall the loads. */
AES_HW_TARGET static void aesCtr8Hw(const AesGcm *a, uint64_t *hi, uint64_t *lo, int gcm, const uint8_t *in,
                                    uint8_t *out) {
    __m128i s[8];
    __m128i k = _mm_loadu_si128((const __m128i *)a->rk[0]);
    for (int i = 0; i < 8; i++) {
        s[i] = _mm_xor_si128(_mm_set_epi64x((long long)__builtin_bswap64(*lo), (long long)__builtin_bswap64(*hi)), k);
        aesCtrStep(hi, lo, gcm);
    }
    for (int r = 1; r < a->rounds; r++) {
        k = _mm_loadu_si128((const __m128i *)a->rk[r]);
        for (int i = 0; i < 8; i++) s[i] = _mm_aesenc_si128(s[i], k);
    }
    k = _mm_loadu_si128((const __m128i *)a->rk[a->rounds]);
    for (int i = 0; i < 8; i++) {
        s[i] = _mm_aesenclast_si128(s[i], k);
        __m128i d = _mm_loadu_si128((const __m128i *)(in + 16 * i));
        _mm_storeu_si128((__m128i *)(out + 16 * i), _mm_xor_si128(s[i], d));
    }
}

AES_HW_TARGET static void clmulHw(uint64_t x, uint64_t y, uint64_t *hi, uint64_t *lo) {
    __m128i p = _mm_clmulepi64_si128(_mm_cvtsi64_si128((long long)x), _mm_cvtsi64_si128((long long)y), 0);
    *lo = (uint64_t)_mm_cvtsi128_si64(p);
    *hi = (uint64_t)_mm_cvtsi128_si64(_mm_unpackhi_epi64(p, p));
}
#else
AES_HW_TARGET static void aesBlockHw(const AesGcm *a, const uint8_t in[16], uint8_t out[16]) {
    uint8x16_t s = vld1q_u8(in);
    for (int r = 0; r < a->rounds - 1; r++) s = vaesmcq_u8(vaeseq_u8(s, vld1q_u8(a->rk[r])));
    s = vaeseq_u8(s, vld1q_u8(a->rk[a->rounds - 1]));
    s = veorq_u8(s, vld1q_u8(a->rk[a->rounds]));
    vst1q_u8(out, s);
}

AES_HW_TARGET static void aesCtr8Hw(const AesGcm *a, uint64_t *hi, uint64_t *lo, int gcm, const uint8_t *in,
                                    uint8_t *out) {
    uint8x16_t s[8];
    for (int i = 0; i < 8; i++) {
        s[i] = vreinterpretq_u8_u64(vcombine_u64(vcreate_u64(__builtin_bswap64(*hi)), vcreate_u64(__builtin_bswap64(*lo))));
        aesCtrStep(hi, lo, gcm);
    }
    for (int r = 0; r < a->rounds - 1; r++) {
        uint8x16_t k = vld1q_u8(a->rk[r]);
        for (int i = 0; i < 8; i++) s[i] = vaesmcq_u8(vaeseq_u8(s[i], k));
    }
    uint8x16_t k = vld1q_u8(a->rk[a->rounds - 1]), last = vld1q_u8(a->rk[a->rounds]);
    for (int i = 0; i < 8; i++) {
        s[i] = veorq_u8(vaeseq_u8(s[i], k), last);
        vst1q_u8(out + 16 * i, veorq_u8(s[i], vld1q_u8(in + 16 * i)));
    }
}

AES_HW_TARGET static void clmulHw(uint64_t x, uint64_t y, uint64_t *hi, uint64_t *lo) {
    poly128_t p = vmull_p64((poly64_t)x, (poly64_t)y);
    uint64x2_t v = vreinterpretq_u64_p128(p);
    *lo = vgetq_lane_u64(v, 0);
    *hi = vgetq_lane_u64(v, 1);
}
#endif
#endif

static void aesBlock(const AesGcm *a, const uint8_t in[16], uint8_t out[16]) {
#ifdef AES_HW_TARGET
    if (a->hw) {
        aesBlockHw(a, in, out);
        return;
    }
#endif
    aesBlockSoft(a, in, out);
}

/* The low 64 bits of the carry-less product, with integer multiplies
   whose carries fall in gaps (BearSSL's bmul64): constant time. */
static uint64_t clmulLow(uint64_t x, uint64_t y) {
    const uint64_t m0 = 0x1111111111111111ULL, m1 = m0 << 1, m2 = m0 << 2, m3 = m0 << 3;
    uint64_t x0 = x & m0, x1 = x & m1, x2 = x & m2, x3 = x & m3;
    uint64_t y0 = y & m0, y1 = y & m1, y2 = y & m2, y3 = y & m3;
    uint64_t z0 = (x0 * y0) ^ (x1 * y3) ^ (x2 * y2) ^ (x3 * y1);
    uint64_t z1 = (x0 * y1) ^ (x1 * y0) ^ (x2 * y3) ^ (x3 * y2);
    uint64_t z2 = (x0 * y2) ^ (x1 * y1) ^ (x2 * y0) ^ (x3 * y3);
    uint64_t z3 = (x0 * y3) ^ (x1 * y2) ^ (x2 * y1) ^ (x3 * y0);
    return (z0 & m0) | (z1 & m1) | (z2 & m2) | (z3 & m3);
}

static uint64_t rev64(uint64_t x) {
    x = ((x >> 1) & 0x5555555555555555ULL) | ((x & 0x5555555555555555ULL) << 1);
    x = ((x >> 2) & 0x3333333333333333ULL) | ((x & 0x3333333333333333ULL) << 2);
    x = ((x >> 4) & 0x0f0f0f0f0f0f0f0fULL) | ((x & 0x0f0f0f0f0f0f0f0fULL) << 4);
    x = ((x >> 8) & 0x00ff00ff00ff00ffULL) | ((x & 0x00ff00ff00ff00ffULL) << 8);
    x = ((x >> 16) & 0x0000ffff0000ffffULL) | ((x & 0x0000ffff0000ffffULL) << 16);
    return (x >> 32) | (x << 32);
}

/* The 128-bit carry-less product of x and y. */
static void clmul(const AesGcm *a, uint64_t x, uint64_t y, uint64_t *hi, uint64_t *lo) {
#ifdef AES_HW_TARGET
    if (a->hw) {
        clmulHw(x, y, hi, lo);
        return;
    }
#endif
    *lo = clmulLow(x, y);
    *hi = rev64(clmulLow(rev64(x), rev64(y))) >> 1;
}

static uint64_t load64be(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

static void store64be(uint8_t *p, uint64_t v) {
    for (int i = 7; i >= 0; i--) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}

/* The 256-bit product v3:v2:v1:v0 of two GCM field elements reduced to
   y1:y0. The bits are reflected: shift left once, then reduce by
   x^128 + x^7 + x^2 + x + 1. */
static void ghashReduce(uint64_t v3, uint64_t v2, uint64_t v1, uint64_t v0, uint64_t *y1, uint64_t *y0) {
    v3 = (v3 << 1) | (v2 >> 63);
    v2 = (v2 << 1) | (v1 >> 63);
    v1 = (v1 << 1) | (v0 >> 63);
    v0 <<= 1;
    v2 ^= v0 ^ (v0 >> 1) ^ (v0 >> 2) ^ (v0 >> 7);
    v1 ^= (v0 << 63) ^ (v0 << 62) ^ (v0 << 57);
    v3 ^= v1 ^ (v1 >> 1) ^ (v1 >> 2) ^ (v1 >> 7);
    v2 ^= (v1 << 63) ^ (v1 << 62) ^ (v1 << 57);
    *y1 = v3;
    *y0 = v2;
}

/* y = x * h in GF(2^128), as GCM defines it */
static void ghashMul(const AesGcm *a, uint64_t x1, uint64_t x0, uint64_t h1, uint64_t h0, uint64_t *y1,
                     uint64_t *y0) {
    uint64_t z0h, z0l, z1h, z1l, z2h, z2l;
    clmul(a, x0, h0, &z0h, &z0l);
    clmul(a, x1, h1, &z1h, &z1l);
    clmul(a, x0 ^ x1, h0 ^ h1, &z2h, &z2l);
    z2h ^= z0h ^ z1h;
    z2l ^= z0l ^ z1l;
    ghashReduce(z1h, z1l ^ z2h, z0h ^ z2l, z0l, y1, y0);
}

/* y = (y ^ block) * H */
static void ghashBlock(const AesGcm *a, uint64_t *y1, uint64_t *y0, const uint8_t block[16]) {
    ghashMul(a, *y1 ^ load64be(block), *y0 ^ load64be(block + 8), a->h1, a->h0, y1, y0);
}

#ifdef AES_HW_TARGET
/* 8 blocks into y at once: y = (((y ^ b0) * H ^ b1) * H ...) * H is
   (y ^ b0) * H^8 ^ b1 * H^7 ^ ... ^ b7 * H, so the products are added
   up unreduced and reduced once. */
#if defined(__x86_64__)
__attribute__((target("aes,pclmul,ssse3"))) static void ghash8Hw(const AesGcm *a, uint64_t *y1, uint64_t *y0,
                                                                  const uint8_t *p) {
    const __m128i rev = _mm_set_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    __m128i lo = _mm_setzero_si128(), hi = lo, mid = lo;
    for (int i = 0; i < 8; i++) {
        __m128i x = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(p + 16 * i)), rev);
        if (i == 0) x = _mm_xor_si128(x, _mm_set_epi64x((long long)*y1, (long long)*y0));
        __m128i h = _mm_loadu_si128((const __m128i *)a->hp[7 - i]);
        __m128i hk = _mm_loadu_si128((const __m128i *)a->hk[7 - i]);
        lo = _mm_xor_si128(lo, _mm_clmulepi64_si128(x, h, 0x00));
        hi = _mm_xor_si128(hi, _mm_clmulepi64_si128(x, h, 0x11));
        __m128i xk = _mm_xor_si128(x, _mm_shuffle_epi32(x, 0x4e));
        mid = _mm_xor_si128(mid, _mm_clmulepi64_si128(xk, hk, 0x00));
    }
    mid = _mm_xor_si128(mid, _mm_xor_si128(lo, hi));
    uint64_t l0 = (uint64_t)_mm_cvtsi128_si64(lo), l1 = (uint64_t)_mm_cvtsi128_si64(_mm_unpackhi_epi64(lo, lo));
    uint64_t h0 = (uint64_t)_mm_cvtsi128_si64(hi), h1 = (uint64_t)_mm_cvtsi128_si64(_mm_unpackhi_epi64(hi, hi));
    uint64_t m0 = (uint64_t)_mm_cvtsi128_si64(mid), m1 = (uint64_t)_mm_cvtsi128_si64(_mm_unpackhi_epi64(mid, mid));
    ghashReduce(h1, h0 ^ m1, l1 ^ m0, l0, y1, y0);
}
#else
AES_HW_TARGET static void ghash8Hw(const AesGcm *a, uint64_t *y1, uint64_t *y0, const uint8_t *p) {
    poly128_t lo = 0, hi = 0, mid = 0;
    for (int i = 0; i < 8; i++) {
        uint64x2_t x = vreinterpretq_u64_u8(vrev64q_u8(vld1q_u8(p + 16 * i)));
        x = vextq_u64(x, x, 1);
        if (i == 0) x = veorq_u64(x, vcombine_u64(vcreate_u64(*y0), vcreate_u64(*y1)));
        uint64x2_t h = vld1q_u64(a->hp[7 - i]);
        lo ^= vmull_p64((poly64_t)vgetq_lane_u64(x, 0), (poly64_t)vgetq_lane_u64(h, 0));
        hi ^= vmull_high_p64(vreinterpretq_p64_u64(x), vreinterpretq_p64_u64(h));
        mid ^= vmull_p64((poly64_t)(vgetq_lane_u64(x, 0) ^ vgetq_lane_u64(x, 1)), (poly64_t)a->hk[7 - i][0]);
    }
    mid ^= lo ^ hi;
    uint64_t l0 = (uint64_t)lo, l1 = (uint64_t)(lo >> 64), h0 = (uint64_t)hi, h1 = (uint64_t)(hi >> 64);
    uint64_t m0 = (uint64_t)mid, m1 = (uint64_t)(mid >> 64);
    ghashReduce(h1, h0 ^ m1, l1 ^ m0, l0, y1, y0);
}
#endif
#endif

static void ghashPadded(const AesGcm *a, uint64_t *y1, uint64_t *y0, const uint8_t *p, size_t n) {
#ifdef AES_HW_TARGET
    if (a->hw) {
        for (; n >= 128; p += 128, n -= 128) ghash8Hw(a, y1, y0, p);
    }
#endif
    while (n >= 16) {
        ghashBlock(a, y1, y0, p);
        p += 16;
        n -= 16;
    }
    if (n > 0) {
        uint8_t last[16] = {0};
        memcpy(last, p, n);
        ghashBlock(a, y1, y0, last);
    }
}

static int gcmHasWide(void);

static void aesGcmInit(AesGcm *a, const uint8_t *key, int keyLen) {
    aesExpand(a, key, keyLen);
    a->hw = aesHasHw();
    uint8_t zero[16] = {0}, h[16];
    aesBlock(a, zero, h);
    a->h1 = load64be(h);
    a->h0 = load64be(h + 8);
    a->wide = a->hw && gcmHasWide();
    if (a->hw) {
        uint64_t p1 = a->h1, p0 = a->h0;
        for (int i = 0; i < (a->wide ? 16 : 8); i++) {
            if (i > 0) ghashMul(a, p1, p0, a->h1, a->h0, &p1, &p0);
            a->hp[i][0] = p0;
            a->hp[i][1] = p1;
            a->hk[i][0] = a->hk[i][1] = p0 ^ p1;
        }
        if (a->wide) {
            /* H^i * x^-1, so the products need no shift: shifted left
               once, with x^-1 = x^127 + x^6 + x + 1 for the bit that falls out */
            for (int j = 0; j < 8; j++) {
                for (int k = 0; k < 2; k++) {
                    const uint64_t *h = a->hp[15 - 2 * j - k];
                    uint64_t carry = h[1] >> 63;
                    uint64_t lo = (h[0] << 1) ^ carry;
                    uint64_t hi = ((h[1] << 1) | (h[0] >> 63)) ^ (carry ? 0xc200000000000000ULL : 0);
                    a->hpw[j][2 * k] = lo;
                    a->hpw[j][2 * k + 1] = hi;
                    a->hkw[j][2 * k] = a->hkw[j][2 * k + 1] = lo ^ hi;
                }
            }
        }
    }
}

/* in xored with AES of the counter block ctr, ctr + 1, ...: counted over
   the last 4 bytes for GCM, over all 16 for plain CTR. With the CPU's AES
   instructions 8 blocks go at a time, so their rounds overlap. */
static void aesCtrXor(const AesGcm *a, const uint8_t ctr[16], int gcm, const uint8_t *in, uint8_t *out, size_t n) {
    uint64_t hi = load64be(ctr), lo = load64be(ctr + 8);
#ifdef AES_HW_TARGET
    if (a->hw) {
        for (; n >= 128; in += 128, out += 128, n -= 128) aesCtr8Hw(a, &hi, &lo, gcm, in, out);
    }
#endif
    uint8_t block[16], ks[16];
    while (n > 0) {
        store64be(block, hi);
        store64be(block + 8, lo);
        aesBlock(a, block, ks);
        size_t k = n < 16 ? n : 16;
        for (size_t i = 0; i < k; i++) out[i] = in[i] ^ ks[i];
        in += k;
        out += k;
        n -= k;
        aesCtrStep(&hi, &lo, gcm);
    }
    iris_wipe(ks, sizeof ks);
}

#if defined(__x86_64__)
/* 16 blocks at a time in 256-bit registers: the counters encrypted, then
   the 16 blocks of ciphertext hashed with one reduction while they are
   still in the cache. */
#define GCM_WIDE_TARGET __attribute__((target("aes,pclmul,avx2,vaes,vpclmulqdq")))

static int gcmWideSeen = -1;

static int gcmHasWide(void) {
    if (gcmWideSeen < 0) {
        __builtin_cpu_init();
        gcmWideSeen = !getenv("IRIS_AES_NARROW") && __builtin_cpu_supports("avx2") &&
                      __builtin_cpu_supports("vaes") && __builtin_cpu_supports("vpclmulqdq");
    }
    return gcmWideSeen;
}

/* 16 blocks into y: (y ^ b0) * H^16 ^ b1 * H^15 ^ ... ^ b15 * H, two blocks
   a register, added up unreduced in lo, hi and mid one register at a
   time (step j takes blocks 2j and 2j + 1, the first step sets them),
   then reduced once. */
typedef struct {
    __m256i lo, hi, mid;
} GhashSum;

GCM_WIDE_TARGET static inline void ghashStep(const AesGcm *a, GhashSum *g, const uint8_t *p, int j, bool first,
                                             __m128i y) {
    const __m256i rev = _mm256_broadcastsi128_si256(_mm_set_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15));
    __m256i x = _mm256_shuffle_epi8(_mm256_loadu_si256((const __m256i *)(p + 32 * j)), rev);
    if (j == 0) x = _mm256_xor_si256(x, _mm256_zextsi128_si256(y));
    __m256i h = _mm256_loadu_si256((const __m256i *)a->hpw[j]);
    __m256i hk = _mm256_loadu_si256((const __m256i *)a->hkw[j]);
    __m256i lo = _mm256_clmulepi64_epi128(x, h, 0x00), hi = _mm256_clmulepi64_epi128(x, h, 0x11);
    __m256i mid = _mm256_clmulepi64_epi128(_mm256_xor_si256(x, _mm256_shuffle_epi32(x, 0x4e)), hk, 0x00);
    if (first) {
        g->lo = lo;
        g->hi = hi;
        g->mid = mid;
    } else {
        g->lo = _mm256_xor_si256(g->lo, lo);
        g->hi = _mm256_xor_si256(g->hi, hi);
        g->mid = _mm256_xor_si256(g->mid, mid);
    }
}

/* The sum reduced: with H * x^-1 the 256-bit product needs no shift, and
   two multiplies by x^127 + x^126 + x^121 + 1 fold the low half into the
   high one. y1:y0 comes back as one register. */
GCM_WIDE_TARGET static inline __m128i ghashSumReduce(const GhashSum *g) {
    const __m128i poly = _mm_set_epi64x((long long)0xc200000000000000ULL, 1);
    __m128i l = _mm_xor_si128(_mm256_castsi256_si128(g->lo), _mm256_extracti128_si256(g->lo, 1));
    __m128i h = _mm_xor_si128(_mm256_castsi256_si128(g->hi), _mm256_extracti128_si256(g->hi, 1));
    __m128i m = _mm_xor_si128(_mm256_castsi256_si128(g->mid), _mm256_extracti128_si256(g->mid, 1));
    m = _mm_xor_si128(m, _mm_xor_si128(l, h));
    m = _mm_xor_si128(m, _mm_xor_si128(_mm_shuffle_epi32(l, 0x4e), _mm_clmulepi64_si128(l, poly, 0x10)));
    return _mm_xor_si128(h, _mm_xor_si128(_mm_shuffle_epi32(m, 0x4e), _mm_clmulepi64_si128(m, poly, 0x10)));
}

/* GCM's counter blocks from 2 on, 256 bytes at a time, xored into in,
   with the ciphertext hashed into y. The hashing of 16 blocks is spread
   over the AES rounds of the next 16, so the two kinds of instruction
   overlap: the ones just read when opening, the ones just written, one
   round of 256 bytes behind, when sealing. The bytes done. */
GCM_WIDE_TARGET static size_t aesGcmWide(const AesGcm *a, const uint8_t nonce[12], int seal, const uint8_t *in,
                                         uint8_t *out, size_t n, uint64_t *y1, uint64_t *y0) {
    uint8_t nb[16] = {0};
    memcpy(nb, nonce, 12);
    const __m256i base = _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i *)nb));
    /* the counter is the last 4 bytes, big-endian: kept as a number in
       each lane's top 32 bits and turned around when a block is made */
    const __m256i turn = _mm256_broadcastsi128_si256(
        _mm_set_epi8(12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1));
    const __m256i two = _mm256_set_epi32(2, 0, 0, 0, 2, 0, 0, 0);
    __m256i ctr = _mm256_set_epi32(3, 0, 0, 0, 2, 0, 0, 0);
    __m256i rk[15];
    for (int r = 0; r <= a->rounds; r++) rk[r] = _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i *)a->rk[r]));
    size_t done = 0;
    __m128i y = _mm_set_epi64x((long long)*y1, (long long)*y0);
    for (; n - done >= 256; done += 256) {
        /* what is hashed while these 16 blocks are encrypted */
        const uint8_t *g = seal ? (done > 0 ? out + done - 256 : NULL) : in + done;
        GhashSum sum;
        __m256i s[8];
        for (int j = 0; j < 8; j++) {
            s[j] = _mm256_xor_si256(_mm256_or_si256(base, _mm256_shuffle_epi8(ctr, turn)), rk[0]);
            ctr = _mm256_add_epi32(ctr, two);
        }
        for (int r = 1; r < a->rounds; r++) {
            for (int j = 0; j < 8; j++) s[j] = _mm256_aesenc_epi128(s[j], rk[r]);
            if (g && r <= 8) ghashStep(a, &sum, g, r - 1, r == 1, y);
        }
        for (int j = 0; j < 8; j++) {
            s[j] = _mm256_aesenclast_epi128(s[j], rk[a->rounds]);
            __m256i d = _mm256_loadu_si256((const __m256i *)(in + done + 32 * j));
            _mm256_storeu_si256((__m256i *)(out + done + 32 * j), _mm256_xor_si256(s[j], d));
        }
        if (g) y = ghashSumReduce(&sum);
    }
    if (seal && done > 0) {
        GhashSum sum;
        for (int j = 0; j < 8; j++) ghashStep(a, &sum, out + done - 256, j, j == 0, y);
        y = ghashSumReduce(&sum);
    }
    *y0 = (uint64_t)_mm_cvtsi128_si64(y);
    *y1 = (uint64_t)_mm_cvtsi128_si64(_mm_unpackhi_epi64(y, y));
    memset(rk, 0, sizeof rk);
    return done;
}
#else
static int gcmHasWide(void) { return 0; }
#endif

/* AES-GCM over n bytes of in into out, sealing when seal is set, else
   opening, and the tag of aad and the ciphertext. */
static void aesGcmRun(const AesGcm *a, const uint8_t nonce[12], int seal, const uint8_t *aad, size_t aadLen,
                      const uint8_t *in, uint8_t *out, size_t n, uint8_t tag[16]) {
    uint64_t y1 = 0, y0 = 0;
    ghashPadded(a, &y1, &y0, aad, aadLen);
    size_t done = 0;
#ifdef GCM_WIDE_TARGET
    if (a->wide) done = aesGcmWide(a, nonce, seal, in, out, n, &y1, &y0);
#endif
    if (!seal) ghashPadded(a, &y1, &y0, in + done, n - done);
    uint8_t ctr[16];
    memcpy(ctr, nonce, 12);
    uint32_t next = (uint32_t)(2 + done / 16);
    ctr[12] = (uint8_t)(next >> 24);
    ctr[13] = (uint8_t)(next >> 16);
    ctr[14] = (uint8_t)(next >> 8);
    ctr[15] = (uint8_t)next;
    aesCtrXor(a, ctr, 1, in + done, out + done, n - done);
    if (seal) ghashPadded(a, &y1, &y0, out + done, n - done);
    uint8_t lens[16];
    store64be(lens, (uint64_t)aadLen * 8);
    store64be(lens + 8, (uint64_t)n * 8);
    ghashBlock(a, &y1, &y0, lens);
    uint8_t j0[16], ek[16], s[16];
    memcpy(j0, nonce, 12);
    j0[12] = j0[13] = j0[14] = 0;
    j0[15] = 1;
    aesBlock(a, j0, ek);
    store64be(s, y1);
    store64be(s + 8, y0);
    for (int i = 0; i < 16; i++) tag[i] = ek[i] ^ s[i];
}

/* AES-GCM: the data sealed, then its 16-byte tag, added to out */
void iris_aes_seal(const uint8_t *key, int64_t keyLen, const uint8_t *nonce, const uint8_t *data, int64_t len,
                   const uint8_t *aad, int64_t aadLen, IrisString *out) {
    AesGcm a;
    aesGcmInit(&a, key, (int)keyLen);
    iris_string_reserve(out, len + 16);
    uint8_t *ct = (uint8_t *)out->ptr + out->len;
    aesGcmRun(&a, nonce, 1, aad, (size_t)aadLen, data, ct, (size_t)len, ct + len);
    out->len += len + 16;
    iris_wipe(&a, sizeof a);
}

/* Opens what iris_aes_seal made: 1 and the data added to out, or 0 when
   the tag doesn't match, and then out gets nothing. */
int iris_aes_open(const uint8_t *key, int64_t keyLen, const uint8_t *nonce, const uint8_t *sealed, int64_t len,
                  const uint8_t *aad, int64_t aadLen, IrisString *out) {
    if (len < 16) {
        return 0;
    }
    AesGcm a;
    aesGcmInit(&a, key, (int)keyLen);
    size_t n = (size_t)len - 16;
    uint8_t tag[16];
    iris_string_reserve(out, (int64_t)n);
    uint8_t *pt = (uint8_t *)out->ptr + out->len;
    aesGcmRun(&a, nonce, 0, aad, (size_t)aadLen, sealed, pt, n, tag);
    iris_wipe(&a, sizeof a);
    if (!iris_const_equal((const char *)tag, 16, (const char *)sealed + n, 16)) {
        /* nothing of a forged message is kept */
        iris_wipe(pt, n);
        return 0;
    }
    out->len += (int64_t)n;
    return 1;
}

/* SHA-384 (FIPS 180-4): SHA-512 with its own start and 48 bytes kept. */
static void sha384Init(Sha512 *s) {
    static const uint64_t start[8] = {0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL, 0x9159015a3070dd17ULL,
                                      0x152fecd8f70e5939ULL, 0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL,
                                      0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL};
    memcpy(s->h, start, sizeof start);
    s->len = 0;
    s->used = 0;
}

void iris_sha384(const char *data, int64_t len, unsigned char *out) {
    Sha512 s;
    unsigned char full[64];
    sha384Init(&s);
    sha512Add(&s, (const unsigned char *)data, (size_t)len);
    sha512End(&s, full);
    memcpy(out, full, 48);
}

/* HMAC-SHA384 (RFC 2104, 4868), with SHA-384's 128-byte blocks. */
void iris_hmac_sha384(const char *key, int64_t keyLen, const char *data, int64_t len, unsigned char *out) {
    unsigned char k[128] = {0};
    if (keyLen > 128) {
        iris_sha384(key, keyLen, k);
    } else {
        memcpy(k, key, (size_t)keyLen);
    }
    unsigned char ipad[128], opad[128], inner[64], outer[64];
    for (int i = 0; i < 128; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    Sha512 s;
    sha384Init(&s);
    sha512Add(&s, ipad, 128);
    sha512Add(&s, (const unsigned char *)data, (size_t)len);
    sha512End(&s, inner);
    sha384Init(&s);
    sha512Add(&s, opad, 128);
    sha512Add(&s, inner, 48);
    sha512End(&s, outer);
    memcpy(out, outer, 48);
    iris_wipe(k, sizeof k);
}

void iris_sha1(const char *data, int64_t len, unsigned char *out);

/* HMAC-SHA1 (RFC 2104), for older protocols such as TOTP (RFC 6238). */
void iris_hmac_sha1(const char *key, int64_t keyLen, const char *data, int64_t len, unsigned char *out) {
    unsigned char k[64] = {0};
    if (keyLen > 64) {
        iris_sha1(key, keyLen, k);
    } else {
        memcpy(k, key, (size_t)keyLen);
    }
    unsigned char *msg = malloc((size_t)len + 64);
    if (!msg) outOfMemory();
    for (int i = 0; i < 64; i++) msg[i] = k[i] ^ 0x36;
    memcpy(msg + 64, data, (size_t)len);
    unsigned char inner[20], outerMsg[84];
    iris_sha1((const char *)msg, len + 64, inner);
    free(msg);
    for (int i = 0; i < 64; i++) outerMsg[i] = k[i] ^ 0x5c;
    memcpy(outerMsg + 64, inner, 20);
    iris_sha1((const char *)outerMsg, 84, out);
    iris_wipe(k, sizeof k);
}

/* One AES block encrypted with key (16, 24 or 32 bytes): ECB, as QUIC's
   header protection uses it. */
void iris_aes_block(const uint8_t *key, int64_t keyLen, const uint8_t *in, uint8_t *out) {
    AesGcm a;
    aesExpand(&a, key, (int)keyLen);
    a.hw = aesHasHw();
    aesBlock(&a, in, out);
    iris_wipe(&a, sizeof a);
}

/* AES in counter mode (NIST SP 800-38A): the 16-byte block iv is the first
   counter, counted up as one big-endian number. Encrypting and decrypting
   are the same; the result is added to out. */
void iris_aes_ctr(const uint8_t *key, int64_t keyLen, const uint8_t *iv, const uint8_t *data, int64_t len,
                  IrisString *out) {
    AesGcm a;
    aesExpand(&a, key, (int)keyLen);
    a.hw = aesHasHw();
    uint8_t ctr[16];
    memcpy(ctr, iv, 16);
    if (len > 0) {
        iris_string_reserve(out, len);
        aesCtrXor(&a, ctr, 0, data, (uint8_t *)out->ptr + out->len, (size_t)len);
        out->len += len;
    }
    iris_wipe(&a, sizeof a);
}

/* The ChaCha20 stream (RFC 8439) from block counter on, xored with data:
   encrypting and decrypting are the same; added to out. */
void iris_chacha20(const uint8_t *key, const uint8_t *nonce, uint32_t counter, const uint8_t *data, int64_t len,
                   IrisString *out) {
    uint8_t *buf = malloc(len ? (size_t)len : 1);
    if (!buf) outOfMemory();
    chachaXor(key, counter, nonce, data, buf, (size_t)len);
    iris_string_append(out, (const char *)buf, len);
    free(buf);
}

/* CRC32C (Castagnoli), as iSCSI, ext4, BEP 42 and WAL formats use: the
   CPU's crc32 instruction where it has one, else 8 table lookups a word. */
static uint32_t crc32cTable[8][256];
static pthread_once_t crc32cOnce = PTHREAD_ONCE_INIT;
static int crc32cHw;

static void crc32cShiftInit(void);

static void crc32cInit(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0x82F63B78u : c >> 1;
        crc32cTable[0][i] = c;
    }
    for (int t = 1; t < 8; t++) {
        for (int i = 0; i < 256; i++) {
            uint32_t c = crc32cTable[t - 1][i];
            crc32cTable[t][i] = (c >> 8) ^ crc32cTable[0][c & 0xff];
        }
    }
    crc32cShiftInit();
    if (getenv("IRIS_CRC_SOFT")) return;
#if defined(__x86_64__)
    __builtin_cpu_init();
    crc32cHw = __builtin_cpu_supports("sse4.2");
#elif defined(__aarch64__) && defined(__APPLE__)
    crc32cHw = 1;
#elif defined(__aarch64__) && defined(__linux__)
    crc32cHw = (getauxval(AT_HWCAP) & (1UL << 7)) != 0;    /* HWCAP_CRC32 */
#endif
}

/* Longer runs go as 3 blocks of one size side by side, as the crc32
   instruction can start a new one each cycle but takes 3 to finish. The
   CRC of the first block is then moved past the others: crc32cShift[s]
   is what a CRC becomes after crc32cBlock[s] zero bytes, a byte of it
   at a time. */
static const size_t crc32cBlock[2] = {4096, 512};
static uint32_t crc32cShift[2][4][256];

static uint32_t crc32cMove(int s, uint32_t c) {
    return crc32cShift[s][0][c & 0xff] ^ crc32cShift[s][1][(c >> 8) & 0xff] ^ crc32cShift[s][2][(c >> 16) & 0xff] ^
           crc32cShift[s][3][c >> 24];
}

static void crc32cShiftInit(void) {
    for (int s = 0; s < 2; s++) {
        uint32_t bit[32];
        for (int i = 0; i < 32; i++) {
            uint32_t c = 1u << i;
            for (size_t k = 0; k < crc32cBlock[s]; k++) c = (c >> 8) ^ crc32cTable[0][c & 0xff];
            bit[i] = c;
        }
        for (int k = 0; k < 4; k++) {
            for (int v = 0; v < 256; v++) {
                uint32_t c = 0;
                for (int i = 0; i < 8; i++) {
                    if (v >> i & 1) c ^= bit[8 * k + i];
                }
                crc32cShift[s][k][v] = c;
            }
        }
    }
}

#if defined(__x86_64__)
#define CRC32C_TARGET __attribute__((target("sse4.2")))
CRC32C_TARGET static inline uint32_t crc32cWord(uint32_t c, uint64_t v) {
    return (uint32_t)__builtin_ia32_crc32di(c, v);
}
CRC32C_TARGET static inline uint32_t crc32cByte(uint32_t c, uint8_t v) { return __builtin_ia32_crc32qi(c, v); }
#elif defined(__aarch64__)
#define CRC32C_TARGET __attribute__((target("crc")))
CRC32C_TARGET static inline uint32_t crc32cWord(uint32_t c, uint64_t v) { return __builtin_arm_crc32cd(c, v); }
CRC32C_TARGET static inline uint32_t crc32cByte(uint32_t c, uint8_t v) { return __builtin_arm_crc32cb(c, v); }
#endif

#ifdef CRC32C_TARGET
CRC32C_TARGET static uint32_t crc32cHwRun(uint32_t c, const uint8_t *p, size_t n) {
    for (int s = 0; s < 2; s++) {
        size_t b = crc32cBlock[s];
        for (; n >= 3 * b; p += 3 * b, n -= 3 * b) {
            uint32_t x = c, y = 0, z = 0;
            for (size_t i = 0; i < b; i += 8) {
                uint64_t u, v, w;
                memcpy(&u, p + i, 8);
                memcpy(&v, p + b + i, 8);
                memcpy(&w, p + 2 * b + i, 8);
                x = crc32cWord(x, u);
                y = crc32cWord(y, v);
                z = crc32cWord(z, w);
            }
            c = crc32cMove(s, crc32cMove(s, x) ^ y) ^ z;
        }
    }
    for (; n >= 8; p += 8, n -= 8) {
        uint64_t v;
        memcpy(&v, p, 8);
        c = crc32cWord(c, v);
    }
    for (; n > 0; p++, n--) c = crc32cByte(c, *p);
    return c;
}
#endif

uint32_t iris_crc32c(uint32_t before, const uint8_t *p, int64_t len) {
    pthread_once(&crc32cOnce, crc32cInit);
    uint32_t c = ~before;
    size_t n = (size_t)len;
#ifdef CRC32C_TARGET
    if (crc32cHw) return ~crc32cHwRun(c, p, n);
#endif
    for (; n >= 8; p += 8, n -= 8) {
        uint32_t lo, hi;
        memcpy(&lo, p, 4);
        memcpy(&hi, p + 4, 4);
        lo ^= c;
        c = crc32cTable[7][lo & 0xff] ^ crc32cTable[6][(lo >> 8) & 0xff] ^ crc32cTable[5][(lo >> 16) & 0xff] ^
            crc32cTable[4][lo >> 24] ^ crc32cTable[3][hi & 0xff] ^ crc32cTable[2][(hi >> 8) & 0xff] ^
            crc32cTable[1][(hi >> 16) & 0xff] ^ crc32cTable[0][hi >> 24];
    }
    for (; n > 0; p++, n--) c = (c >> 8) ^ crc32cTable[0][(c ^ *p) & 0xff];
    return ~c;
}

static uint8_t base32Tables[2][256];    /* 0-31, 0xfe to skip, 0xff wrong */
/* two characters for each 10 bits, so 4 lookups and one store make 8 */
static uint16_t base32Pairs[2][1024];
static pthread_once_t base32Once = PTHREAD_ONCE_INIT;

static void base32Init(void) {
    for (int h = 0; h < 2; h++) {
        memset(base32Tables[h], 0xff, 256);
        const char *al = h ? "0123456789ABCDEFGHIJKLMNOPQRSTUV" : "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
        for (int k = 0; k < 32; k++) {
            base32Tables[h][(uint8_t)al[k]] = (uint8_t)k;
            if (al[k] >= 'A' && al[k] <= 'Z') base32Tables[h][(uint8_t)(al[k] + 32)] = (uint8_t)k;
        }
        base32Tables[h]['\n'] = base32Tables[h]['\r'] = base32Tables[h][' '] = 0xfe;
        for (int k = 0; k < 1024; k++) {
            base32Pairs[h][k] = (uint16_t)((uint8_t)al[k >> 5] | (uint8_t)al[k & 31] << 8);
        }
    }
}

/* Base32 (RFC 4648): 5 bytes to 8 characters, of base32hex's alphabet
   when hex is set, with = to a multiple of 8 when pad is. Added to out. */
void iris_base32_encode(const uint8_t *p, int64_t len, int hex, int pad, IrisString *out) {
    int64_t size = (len + 4) / 5 * 8;
    if (size == 0) return;
    pthread_once(&base32Once, base32Init);
    const uint16_t *pairs = base32Pairs[hex ? 1 : 0];
    const char *alphabet = hex ? "0123456789ABCDEFGHIJKLMNOPQRSTUV" : "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    iris_string_reserve(out, size);
    char *o = out->ptr + out->len;
    int64_t i = 0;
    for (; i + 5 <= len; i += 5, o += 8) {
        uint64_t v = (uint64_t)p[i] << 32 | (uint64_t)p[i + 1] << 24 | (uint64_t)p[i + 2] << 16 |
                     (uint64_t)p[i + 3] << 8 | p[i + 4];
        uint64_t chars = (uint64_t)pairs[v >> 30] | (uint64_t)pairs[(v >> 20) & 1023] << 16 |
                         (uint64_t)pairs[(v >> 10) & 1023] << 32 | (uint64_t)pairs[v & 1023] << 48;
        memcpy(o, &chars, 8);
    }
    int64_t rest = len - i;
    if (rest > 0) {
        uint64_t v = 0;
        for (int64_t k = 0; k < rest; k++) v |= (uint64_t)p[i + k] << (32 - 8 * k);
        int chars = (int)((rest * 8 + 4) / 5);
        for (int k = 0; k < chars; k++) o[k] = alphabet[(v >> (35 - 5 * k)) & 31];
        if (pad) {
            for (int k = chars; k < 8; k++) o[k] = '=';
            o += 8;
        } else {
            o += chars;
        }
    }
    out->len = o - out->ptr;
}

/* Base32 text to bytes, added to out, in either case; line breaks and
   spaces are skipped. 0, or what is wrong: -1 for a = followed by more
   text, -2 for text that ends inside a byte, i + 1 for a bad character
   at i. */
int64_t iris_base32_decode(const uint8_t *t, int64_t len, int hex, IrisString *out) {
    pthread_once(&base32Once, base32Init);
    const uint8_t *table = base32Tables[hex ? 1 : 0];
    int64_t end = len;
    const uint8_t *eq = len > 0 ? memchr(t, '=', (size_t)len) : NULL;
    if (eq) {
        end = eq - t;
        for (int64_t k = end; k < len; k++) {
            if (t[k] != '=') return -1;
        }
    }
    /* room for every byte: 5 for each 8 characters, and the rest */
    iris_string_reserve(out, end / 8 * 5 + 5);
    uint8_t *o = (uint8_t *)out->ptr + out->len;
    uint64_t acc = 0;
    int bits = 0;
    int64_t n = 0, i = 0;
    /* 8 characters at a time while none is skipped or wrong */
    for (; i + 8 <= end; i += 8) {
        uint64_t v = 0;
        uint8_t bad = 0;
        for (int k = 0; k < 8; k++) {
            uint8_t d = table[t[i + k]];
            bad |= d;
            v = v << 5 | (d & 31);
        }
        if (bad & 0xe0) break;
        o[0] = (uint8_t)(v >> 32);
        o[1] = (uint8_t)(v >> 24);
        o[2] = (uint8_t)(v >> 16);
        o[3] = (uint8_t)(v >> 8);
        o[4] = (uint8_t)v;
        o += 5;
        n += 8;
    }
    for (; i < end; i++) {
        uint8_t d = table[t[i]];
        if (d == 0xfe) continue;
        if (d == 0xff) {
            out->len = o - (uint8_t *)out->ptr;
            return i + 1;
        }
        acc = (acc << 5 | d) & 0xffff;
        bits += 5;
        n++;
        if (bits >= 8) {
            bits -= 8;
            *o++ = (uint8_t)(acc >> bits);
        }
    }
    out->len = o - (uint8_t *)out->ptr;
    int r = (int)(n % 8);
    return r == 1 || r == 3 || r == 6 ? -2 : 0;
}

static const char base64Alphabets[2][65] = {
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/",
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_",
};
static uint8_t base64Tables[2][256];    /* 0-63, 0xfe to skip, 0xff wrong */
/* two characters for each 12 bits, so 2 lookups make 4 */
static uint16_t base64Pairs[2][4096];
static pthread_once_t base64Once = PTHREAD_ONCE_INIT;

static void base64Init(void) {
    for (int u = 0; u < 2; u++) {
        const char *al = base64Alphabets[u];
        memset(base64Tables[u], 0xff, 256);
        for (int k = 0; k < 64; k++) base64Tables[u][(uint8_t)al[k]] = (uint8_t)k;
        base64Tables[u]['\n'] = base64Tables[u]['\r'] = 0xfe;
        for (int k = 0; k < 4096; k++) base64Pairs[u][k] = (uint16_t)((uint8_t)al[k >> 6] | (uint8_t)al[k & 63] << 8);
    }
}

/* Base64 (RFC 4648): 3 bytes to 4 characters, of the URL-safe alphabet
   when url is set, with = to a multiple of 4 when pad is. Added to out. */
void iris_base64_encode(const uint8_t *p, int64_t len, int url, int pad, IrisString *out) {
    if (len == 0) return;
    pthread_once(&base64Once, base64Init);
    const uint16_t *pairs = base64Pairs[url ? 1 : 0];
    const char *alphabet = base64Alphabets[url ? 1 : 0];
    iris_string_reserve(out, (len + 2) / 3 * 4);
    char *o = out->ptr + out->len;
    int64_t i = 0;
    /* 6 bytes to 8 characters, read as 8 while 8 are there */
    for (; i + 8 <= len; i += 6, o += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        v = __builtin_bswap64(v) >> 16;
        uint64_t chars = (uint64_t)pairs[v >> 36] | (uint64_t)pairs[(v >> 24) & 4095] << 16 |
                         (uint64_t)pairs[(v >> 12) & 4095] << 32 | (uint64_t)pairs[v & 4095] << 48;
        memcpy(o, &chars, 8);
    }
    for (; i + 3 <= len; i += 3, o += 4) {
        uint32_t v = (uint32_t)p[i] << 16 | (uint32_t)p[i + 1] << 8 | p[i + 2];
        uint32_t chars = (uint32_t)pairs[v >> 12] | (uint32_t)pairs[v & 4095] << 16;
        memcpy(o, &chars, 4);
    }
    int64_t rest = len - i;
    if (rest > 0) {
        uint32_t v = (uint32_t)p[i] << 16 | (rest == 2 ? (uint32_t)p[i + 1] << 8 : 0);
        *o++ = alphabet[v >> 18];
        *o++ = alphabet[(v >> 12) & 63];
        if (rest == 2) {
            *o++ = alphabet[(v >> 6) & 63];
        } else if (pad) {
            *o++ = '=';
        }
        if (pad) *o++ = '=';
    }
    out->len = o - out->ptr;
}

/* Base64 text to bytes, added to out; line breaks are skipped. 0, or what
   is wrong: -1 for a = followed by more text, -2 for text that ends
   inside a byte, i + 1 for a bad character at i. */
int64_t iris_base64_decode(const uint8_t *t, int64_t len, int url, IrisString *out) {
    pthread_once(&base64Once, base64Init);
    const uint8_t *table = base64Tables[url ? 1 : 0];
    int64_t end = len;
    const uint8_t *eq = len > 0 ? memchr(t, '=', (size_t)len) : NULL;
    if (eq) {
        end = eq - t;
        for (int64_t k = end; k < len; k++) {
            if (t[k] != '=') return -1;
        }
    }
    /* room for every byte: 3 for each 4 characters, and the rest */
    iris_string_reserve(out, end / 4 * 3 + 3);
    uint8_t *o = (uint8_t *)out->ptr + out->len;
    int64_t n = 0, i = 0;
    /* 8 characters at a time while none is skipped or wrong */
    for (; i + 8 <= end; i += 8) {
        uint64_t v = 0;
        uint8_t bad = 0;
        for (int k = 0; k < 8; k++) {
            uint8_t d = table[t[i + k]];
            bad |= d;
            v = v << 6 | (d & 63);
        }
        if (bad & 0xc0) break;
        o[0] = (uint8_t)(v >> 40);
        o[1] = (uint8_t)(v >> 32);
        o[2] = (uint8_t)(v >> 24);
        o[3] = (uint8_t)(v >> 16);
        o[4] = (uint8_t)(v >> 8);
        o[5] = (uint8_t)v;
        o += 6;
        n += 8;
    }
    uint32_t acc = 0;
    int bits = 0;
    for (; i < end; i++) {
        uint8_t d = table[t[i]];
        if (d == 0xfe) continue;
        if (d == 0xff) {
            out->len = o - (uint8_t *)out->ptr;
            return i + 1;
        }
        acc = (acc << 6 | d) & 0xffffff;
        bits += 6;
        n++;
        if (bits >= 8) {
            bits -= 8;
            *o++ = (uint8_t)(acc >> bits);
        }
    }
    out->len = o - (uint8_t *)out->ptr;
    return n % 4 == 1 ? -2 : 0;
}

static uint16_t hexPairs[256];
static uint8_t hexValues[256];          /* 0-15, 0xff wrong */
static pthread_once_t hexOnce = PTHREAD_ONCE_INIT;

static void hexInit(void) {
    const char *digits = "0123456789abcdef";
    for (int k = 0; k < 256; k++) hexPairs[k] = (uint16_t)((uint8_t)digits[k >> 4] | (uint8_t)digits[k & 15] << 8);
    memset(hexValues, 0xff, 256);
    for (int k = 0; k < 16; k++) hexValues[(uint8_t)digits[k]] = (uint8_t)k;
    for (int k = 10; k < 16; k++) hexValues['A' + k - 10] = (uint8_t)k;
}

/* Each byte as two hex digits, 0-9 and a-f, added to out. */
void iris_hex_encode(const uint8_t *p, int64_t len, IrisString *out) {
    if (len == 0) return;
    pthread_once(&hexOnce, hexInit);
    iris_string_reserve(out, len * 2);
    char *o = out->ptr + out->len;
    int64_t i = 0;
    for (; i + 4 <= len; i += 4, o += 8) {
        uint64_t chars = (uint64_t)hexPairs[p[i]] | (uint64_t)hexPairs[p[i + 1]] << 16 |
                         (uint64_t)hexPairs[p[i + 2]] << 32 | (uint64_t)hexPairs[p[i + 3]] << 48;
        memcpy(o, &chars, 8);
    }
    for (; i < len; i++, o += 2) memcpy(o, &hexPairs[p[i]], 2);
    out->len = o - out->ptr;
}

/* Hex digits in pairs, in either case, to bytes added to out. 0, or i + 1
   for a pair at i that isn't hex. The length is even. */
int64_t iris_hex_decode(const uint8_t *t, int64_t len, IrisString *out) {
    pthread_once(&hexOnce, hexInit);
    iris_string_reserve(out, len / 2);
    uint8_t *o = (uint8_t *)out->ptr + out->len;
    for (int64_t i = 0; i + 2 <= len; i += 2) {
        uint8_t hi = hexValues[t[i]], lo = hexValues[t[i + 1]];
        if ((hi | lo) == 0xff) {
            out->len = o - (uint8_t *)out->ptr;
            return i + 1;
        }
        *o++ = (uint8_t)(hi << 4 | lo);
    }
    out->len = o - (uint8_t *)out->ptr;
    return 0;
}

/* xoshiro256** (Blackman and Vigna): fast, not for secrets. The thread's
   own generator is seeded from the system on first use. */
static uint64_t rotl64(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

uint64_t iris_rng_next(uint64_t *s) {
    uint64_t r = rotl64(s[1] * 5, 7) * 9, t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl64(s[3], 45);
    return r;
}

/* A state from a seed, through splitmix64, so that close seeds differ. */
void iris_rng_seed(uint64_t *s, uint64_t seed) {
    for (int i = 0; i < 4; i++) {
        uint64_t z = (seed += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        s[i] = z ^ (z >> 31);
    }
}

static _Thread_local uint64_t threadRng[4];
static _Thread_local int threadRngReady;

uint64_t iris_random_u64(void) {
    if (!threadRngReady) {
        uint64_t seed;
        iris_random_os((char *)&seed, sizeof seed);
        iris_rng_seed(threadRng, seed);
        threadRngReady = 1;
    }
    return iris_rng_next(threadRng);
}

/* A number in [0, n), without the bias of a plain modulo. */
uint64_t iris_rng_below(uint64_t *s, uint64_t n) {
    if (n == 0) return 0;
    uint64_t limit = UINT64_MAX - UINT64_MAX % n;
    for (;;) {
        uint64_t x = s ? iris_rng_next(s) : iris_random_u64();
        if (x < limit) return x % n;
    }
}

/* ---------------------------------------------------------------- SQLite

   The system's SQLite, loaded with dlopen when a program first opens a
   database. Calls wait for the database on the calling thread. */

static struct {
    int loaded;
    int (*openV2)(const char *, void **, int, const char *);
    int (*closeV2)(void *);
    int (*prepareV2)(void *, const char *, int, void **, const char **);
    int (*bindInt64)(void *, int, int64_t);
    int (*bindDouble)(void *, int, double);
    int (*bindText)(void *, int, const char *, int, void (*)(void *));
    int (*bindBlob)(void *, int, const void *, int, void (*)(void *));
    int (*bindNull)(void *, int);
    int (*bindCount)(void *);
    int (*step)(void *);
    int (*columnCount)(void *);
    int (*columnType)(void *, int);
    int64_t (*columnInt64)(void *, int);
    double (*columnDouble)(void *, int);
    const unsigned char *(*columnText)(void *, int);
    const void *(*columnBlob)(void *, int);
    int (*columnBytes)(void *, int);
    const char *(*columnName)(void *, int);
    int (*finalize)(void *);
    const char *(*errmsg)(void *);
    int (*errcode)(void *);
    int64_t (*lastRowid)(void *);
    int (*changes)(void *);
    int (*busyTimeout)(void *, int);
    void *(*dbHandle)(void *);
} sq;

static pthread_once_t sqOnce = PTHREAD_ONCE_INIT;
static _Thread_local char sqlMsg[512];
static _Thread_local int64_t sqlMsgLen;

const char *iris_sql_message_ptr(void) { return sqlMsg; }
int64_t iris_sql_message_len(void) { return sqlMsgLen; }

static void sqlSay(const char *what, const char *detail) {
    sqlMsgLen = snprintf(sqlMsg, sizeof sqlMsg, "%s%s%s", what, detail ? ": " : "", detail ? detail : "");
    if (sqlMsgLen >= (int64_t)sizeof sqlMsg) sqlMsgLen = sizeof sqlMsg - 1;
}

static void sqLoad(void) {
#ifdef __APPLE__
    static const char *const names[] = {"/usr/lib/libsqlite3.dylib", "/opt/homebrew/opt/sqlite/lib/libsqlite3.dylib",
                                        "/usr/local/opt/sqlite/lib/libsqlite3.dylib", NULL};
#else
    static const char *const names[] = {"libsqlite3.so.0", "libsqlite3.so", NULL};
#endif
    const char *env = getenv("IRIS_SQLITE");
    void *h = env && *env ? dlopen(env, RTLD_NOW) : NULL;
    for (int i = 0; !h && names[i]; i++) h = dlopen(names[i], RTLD_NOW);
    if (!h) {
        sq.loaded = -1;
        return;
    }
#define SQ(field, name)                                 \
    do {                                                \
        *(void **)&sq.field = dlsym(h, name);           \
        if (!sq.field) {                                \
            sq.loaded = -1;                             \
            return;                                     \
        }                                               \
    } while (0)
    SQ(openV2, "sqlite3_open_v2");
    SQ(closeV2, "sqlite3_close_v2");
    SQ(prepareV2, "sqlite3_prepare_v2");
    SQ(bindInt64, "sqlite3_bind_int64");
    SQ(bindDouble, "sqlite3_bind_double");
    SQ(bindText, "sqlite3_bind_text");
    SQ(bindBlob, "sqlite3_bind_blob");
    SQ(bindNull, "sqlite3_bind_null");
    SQ(bindCount, "sqlite3_bind_parameter_count");
    SQ(step, "sqlite3_step");
    SQ(columnCount, "sqlite3_column_count");
    SQ(columnType, "sqlite3_column_type");
    SQ(columnInt64, "sqlite3_column_int64");
    SQ(columnDouble, "sqlite3_column_double");
    SQ(columnText, "sqlite3_column_text");
    SQ(columnBlob, "sqlite3_column_blob");
    SQ(columnBytes, "sqlite3_column_bytes");
    SQ(columnName, "sqlite3_column_name");
    SQ(finalize, "sqlite3_finalize");
    SQ(errmsg, "sqlite3_errmsg");
    SQ(errcode, "sqlite3_extended_errcode");
    SQ(lastRowid, "sqlite3_last_insert_rowid");
    SQ(changes, "sqlite3_changes");
    SQ(busyTimeout, "sqlite3_busy_timeout");
    SQ(dbHandle, "sqlite3_db_handle");
#undef SQ
    sq.loaded = 1;
}

/* The error kinds of sqlite's error module: 1 failed, 2 busy, 3 constraint. */
static int sqlKind(int code) {
    switch (code & 0xff) {
    case 5: case 6: return 2;       /* SQLITE_BUSY, SQLITE_LOCKED */
    case 19: return 3;              /* SQLITE_CONSTRAINT */
    default: return 1;
    }
}

int iris_sql_open(const char *path, int64_t n, void **db) {
    pthread_once(&sqOnce, sqLoad);
    if (sq.loaded != 1) {
        sqlSay("SQLite isn't installed (libsqlite3); install it, or set IRIS_SQLITE to its library", NULL);
        return 1;
    }
    char *p = cpath(path, n);
    void *h = NULL;
    /* read, write and create; serialized, so threads can share a Db */
    int rc = sq.openV2(p, &h, 0x2 | 0x4 | 0x10000 | 0x40, NULL);
    if (rc != 0) {
        char what[300];
        snprintf(what, sizeof what, "opening %s", p);
        sqlSay(what, h ? sq.errmsg(h) : "out of memory");
        if (h) sq.closeV2(h);
        free(p);
        return sqlKind(rc);
    }
    free(p);
    sq.busyTimeout(h, 5000);        /* wait up to 5 s for a lock held elsewhere */
    *db = h;
    return 0;
}

void iris_sql_close(void *db) {
    if (db && sq.loaded == 1) sq.closeV2(db);
}

/* One statement of sql, from *at; *at moves past it. NULL stmt with 0 when
   nothing is left. */
int iris_sql_prepare(void *db, const char *sql, int64_t n, int64_t *at, void **stmt) {
    const char *start = sql + *at, *tail = NULL;
    *stmt = NULL;
    int rc = sq.prepareV2(db, start, (int)(n - *at), stmt, &tail);
    if (rc != 0) {
        sqlSay("in the SQL", sq.errmsg(db));
        return sqlKind(rc);
    }
    *at = tail ? tail - sql : n;
    return 0;
}

int iris_sql_bind(void *stmt, int64_t i, int kind, int64_t iv, double fv, const char *text, int64_t len) {
    int rc;
    switch (kind) {
    case 0: rc = sq.bindInt64(stmt, (int)i, iv); break;
    case 1: rc = sq.bindDouble(stmt, (int)i, fv); break;
    case 2: rc = sq.bindText(stmt, (int)i, text ? text : "", (int)len, (void (*)(void *))-1); break;
    case 4: rc = sq.bindBlob(stmt, (int)i, text ? text : "", (int)len, (void (*)(void *))-1); break;
    default: rc = sq.bindNull(stmt, (int)i); break;
    }
    if (rc != 0) {
        sqlSay("binding a value", sq.errmsg(sq.dbHandle(stmt)));
        return sqlKind(rc);
    }
    return 0;
}

int64_t iris_sql_params(void *stmt) { return sq.bindCount(stmt); }

/* 1 a row, 0 done, or minus the error kind. */
int iris_sql_step(void *stmt) {
    int rc = sq.step(stmt);
    if (rc == 100) return 1;
    if (rc == 101) return 0;
    void *db = sq.dbHandle(stmt);
    sqlSay("running the SQL", sq.errmsg(db));
    return -sqlKind(sq.errcode(db));
}

void iris_sql_finalize(void *stmt) {
    if (stmt) sq.finalize(stmt);
}

int64_t iris_sql_columns(void *stmt) { return sq.columnCount(stmt); }
int64_t iris_sql_type(void *stmt, int64_t i) { return sq.columnType(stmt, (int)i); }
int64_t iris_sql_int(void *stmt, int64_t i) { return sq.columnInt64(stmt, (int)i); }
double iris_sql_float(void *stmt, int64_t i) { return sq.columnDouble(stmt, (int)i); }
const char *iris_sql_text(void *stmt, int64_t i, int64_t *len) {
    const char *t = (const char *)sq.columnText(stmt, (int)i);
    *len = t ? sq.columnBytes(stmt, (int)i) : 0;
    return t ? t : "";
}
/* A column's bytes as they are, for a BLOB. */
const char *iris_sql_blob(void *stmt, int64_t i, int64_t *len) {
    const char *t = (const char *)sq.columnBlob(stmt, (int)i);
    *len = t ? sq.columnBytes(stmt, (int)i) : 0;
    return t ? t : "";
}
const char *iris_sql_name(void *stmt, int64_t i, int64_t *len) {
    const char *t = sq.columnName(stmt, (int)i);
    *len = t ? (int64_t)strlen(t) : 0;
    return t ? t : "";
}
int64_t iris_sql_last_id(void *db) { return sq.lastRowid(db); }
int64_t iris_sql_changes(void *db) { return sq.changes(db); }

/* Fills the fields of a @json struct from the row's columns of the same
   names. NULL leaves a field as it is; other columns are skipped. */
int iris_sql_scan(void *stmt, const JsonType *t, char *out) {
    const JsonField *fs = t->items;
    int cols = sq.columnCount(stmt);
    for (int c = 0; c < cols; c++) {
        const char *name = sq.columnName(stmt, c);
        size_t nl = name ? strlen(name) : 0;
        const JsonField *f = NULL;
        for (int64_t i = 0; i < t->count && !f; i++) {
            if ((size_t)fs[i].nameLen == nl && !memcmp(fs[i].name, name, nl)) f = &fs[i];
        }
        if (!f || sq.columnType(stmt, c) == 5) continue;
        char *p = out + f->offset;
        const JsonType *ft = f->type;
        switch (ft->kind) {
        case J_INT: case J_UINT: {
            int64_t v = sq.columnInt64(stmt, c);
            int bits = (int)ft->size * 8;
            bool fits = ft->kind == J_INT ? bits == 64 || (v >= -(1LL << (bits - 1)) && v < (1LL << (bits - 1)))
                                          : v >= 0 && (bits == 64 || (uint64_t)v < (1ULL << bits));
            if (!fits) {
                char what[200];
                snprintf(what, sizeof what, "the column %s has %lld, which doesn't fit in %c%d", name, (long long)v,
                         ft->kind == J_INT ? 'i' : 'u', bits);
                sqlSay(what, NULL);
                return 1;
            }
            writeInt(p, ft->size, v);
            break;
        }
        case J_FLOAT:
            if (ft->size == 4) *(float *)p = (float)sq.columnDouble(stmt, c);
            else *(double *)p = sq.columnDouble(stmt, c);
            break;
        case J_BOOL: *(uint8_t *)p = sq.columnInt64(stmt, c) != 0; break;
        case J_STRING: {
            IrisString *s = (IrisString *)p;
            iris_string_free(s);
            const char *text = (const char *)sq.columnText(stmt, c);
            iris_string_append(s, text ? text : "", sq.columnBytes(stmt, c));
            break;
        }
        case J_ENUM: {
            const JsonName *names = ft->items;
            const char *text = (const char *)sq.columnText(stmt, c);
            int64_t len = sq.columnBytes(stmt, c);
            int64_t k = 0;
            while (k < ft->count && !(names[k].len == len && !memcmp(names[k].name, text, (size_t)len))) k++;
            if (k == ft->count) {
                char what[200];
                snprintf(what, sizeof what, "the column %s has '%.*s', which isn't one of the enum's values", name,
                         (int)(len > 60 ? 60 : len), text ? text : "");
                sqlSay(what, NULL);
                return 1;
            }
            *(int32_t *)p = (int32_t)names[k].value;
            break;
        }
        default: {
            char what[200];
            snprintf(what, sizeof what, "the field %s can't be filled from a column", name);
            sqlSay(what, NULL);
            return 1;
        }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- env.load */

typedef struct {
    const char *name;
    int64_t nameLen, offset;
    const JsonType *type;
    int64_t required, duration;
} EnvField;

typedef struct {
    int64_t count;
    const EnvField *fields;
} EnvTable;

static _Thread_local char envMsg[400];
static _Thread_local int64_t envMsgLen;

const char *iris_env_message_ptr(void) { return envMsg; }
int64_t iris_env_message_len(void) { return envMsgLen; }

static int envFail(int code, const char *name, const char *value, const char *why) {
    if (value) {
        envMsgLen = snprintf(envMsg, sizeof envMsg, "%s: '%.80s' %s", name, value, why);
    } else {
        envMsgLen = snprintf(envMsg, sizeof envMsg, "%s %s", name, why);
    }
    if (envMsgLen >= (int64_t)sizeof envMsg) envMsgLen = sizeof envMsg - 1;
    return code;
}

/* 1500ms, 2s, 1.5h, 1h30m; a plain number is seconds. -1 when it isn't one. */
static bool parseDuration(const char *s, int64_t *out) {
    double total = 0;
    bool any = false;
    while (*s) {
        char *end;
        double v = irisStrtod(s, &end);
        if (end == s) return false;
        s = end;
        double unit = 1e9;
        if (!strncmp(s, "ns", 2)) { unit = 1; s += 2; }
        else if (!strncmp(s, "us", 2)) { unit = 1e3; s += 2; }
        else if (!strncmp(s, "ms", 2)) { unit = 1e6; s += 2; }
        else if (*s == 's') { unit = 1e9; s++; }
        else if (*s == 'm') { unit = 60e9; s++; }
        else if (*s == 'h') { unit = 3600e9; s++; }
        else if (*s || any) return false; /* a number alone is seconds, but only on its own: not 1m30 */
        total += v * unit;
        any = true;
    }
    if (!any) return false;
    *out = (int64_t)total;
    return true;
}

/* time.parseDuration: 1 when the text is a duration like 1m30s. */
int iris_time_parse_duration(const char *p, int64_t n, int64_t *out) {
    char *s = cpath(p, n);
    if (!s) {
        return 0;
    }
    bool ok = n > 0 && parseDuration(s, out);
    free(s);
    return ok;
}

static bool sameWord(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        if ((*a | 32) != (*b | 32)) return false;
    }
    return !*a && !*b;
}

/* Fills an @env struct from the environment. 0, 1 a required variable is
   missing, 2 a value doesn't fit its field. */
/* Puts the text v into the field at p, as env.load and flags.parse read
 * it. NULL when it worked, otherwise why it didn't. */
static const char *setFromText(const JsonType *ft, bool duration, char *p, const char *v) {
    switch (ft->kind) {
    case J_INT: case J_UINT: {
        if (duration) {
            int64_t d;
            if (!parseDuration(v, &d)) return "isn't a duration like 30s or 1m30s";
            *(int64_t *)p = d;
            return NULL;
        }
        char *end;
        errno = 0;
        long long n = strtoll(v, &end, 10);
        int bits = ft->size * 8;
        bool fits = ft->kind == J_INT ? (bits == 64 || (n >= -(1LL << (bits - 1)) && n < (1LL << (bits - 1))))
                                      : (n >= 0 && (bits == 64 || n < (1LL << bits)));
        if (*end || end == v || errno) return "isn't a whole number";
        if (!fits) return "doesn't fit";
        writeInt(p, ft->size, n);
        return NULL;
    }
    case J_FLOAT: {
        char *end;
        double d = irisStrtod(v, &end);
        if (*end || end == v) return "isn't a number";
        if (ft->size == 4) *(float *)p = (float)d;
        else *(double *)p = d;
        return NULL;
    }
    case J_BOOL:
        if (sameWord(v, "true") || sameWord(v, "1") || sameWord(v, "yes") || sameWord(v, "on")) {
            *(uint8_t *)p = 1;
        } else if (sameWord(v, "false") || sameWord(v, "0") || sameWord(v, "no") || sameWord(v, "off")) {
            *(uint8_t *)p = 0;
        } else {
            return "isn't true or false";
        }
        return NULL;
    case J_STR: {
        Slice *s = (Slice *)p;
        s->ptr = (char *)v;         /* the environment and the arguments stay for the program's life */
        s->len = (int64_t)strlen(v);
        return NULL;
    }
    case J_STRING: {
        IrisString *s = (IrisString *)p;
        iris_string_free(s);
        iris_string_append(s, v, (int64_t)strlen(v));
        return NULL;
    }
    case J_ENUM: {
        const JsonName *names = ft->items;
        int64_t k = 0;
        while (k < ft->count && !((size_t)names[k].len == strlen(v) && !memcmp(names[k].name, v, strlen(v)))) k++;
        if (k == ft->count) return "isn't one of the enum's values";
        *(int32_t *)p = (int32_t)names[k].value;
        return NULL;
    }
    default:
        return "can't be read from text";
    }
}

/* pg: a row's text values into a @json struct, by column name. NULL
   leaves a field as it is. 0, or 1 with the message set. */
static _Thread_local char pgMsg[300];
static _Thread_local int64_t pgMsgLen;

const char *iris_pg_message_ptr(void) { return pgMsg; }
int64_t iris_pg_message_len(void) { return pgMsgLen; }

int iris_pg_scan(const JsonType *t, char *out, const Slice *names, const Slice *values, const uint8_t *nulls,
                 int64_t n) {
    const JsonField *fs = t->items;
    for (int64_t c = 0; c < n; c++) {
        const JsonField *f = NULL;
        for (int64_t i = 0; i < t->count && !f; i++) {
            if (fs[i].nameLen == names[c].len && !memcmp(fs[i].name, names[c].ptr, (size_t)names[c].len)) f = &fs[i];
        }
        if (!f || nulls[c]) continue;
        char *p = out + f->offset;
        const Slice *v = &values[c];
        if (f->type->kind == J_STR) {
            pgMsgLen = snprintf(pgMsg, sizeof pgMsg, "the field %.*s is a str, which can't keep a column; make it a String",
                                (int)f->nameLen, f->name);
            return 1;
        }
        if (f->type->kind == J_BOOL && v->len == 1 && (v->ptr[0] == 't' || v->ptr[0] == 'f')) {
            *(uint8_t *)p = v->ptr[0] == 't';
            continue;
        }
        char *text = malloc((size_t)v->len + 1);
        memcpy(text, v->ptr, (size_t)v->len);
        text[v->len] = 0;
        const char *why = setFromText(f->type, false, p, text);
        if (why) {
            pgMsgLen = snprintf(pgMsg, sizeof pgMsg, "the column %.*s has '%.60s', which %s", (int)names[c].len,
                                names[c].ptr, text, why);
            free(text);
            return 1;
        }
        free(text);
    }
    return 0;
}

int iris_env_load(const EnvTable *t, char *out) {
    for (int64_t i = 0; i < t->count; i++) {
        const EnvField *f = &t->fields[i];
        char name[256];
        snprintf(name, sizeof name, "%.*s", (int)f->nameLen, f->name);
        const char *v = getenv(name);
        if (!v || !*v) {
            if (f->required) return envFail(1, name, NULL, "is needed but isn't set");
            continue;
        }
        const char *why = setFromText(f->type, f->duration, out + f->offset, v);
        if (why) return envFail(2, name, v, why);
    }
    return 0;
}

/* ---------------------------------------------------------------- flags.parse */

typedef struct {
    const char *name;           /* the long name, without the -- */
    int64_t nameLen, offset;
    const JsonType *type;
    int64_t required, duration, letter;
    const char *help;
    int64_t helpLen, args;      /* args: the str[] that gets the other arguments */
} FlagField;

typedef struct {
    int64_t count;
    const FlagField *fields;
    const char *about;
    int64_t aboutLen;
} FlagTable;

/* the help text, or what was wrong */
static _Thread_local IrisString flagMsg;

const char *iris_flags_message_ptr(void) { return flagMsg.ptr; }
int64_t iris_flags_message_len(void) { return flagMsg.len; }

static void msgf(const char *f, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    int n = vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
    iris_string_append(&flagMsg, buf, n);
}

/* What a flag's value looks like in the help: <n>, <text>, <a|b|c>. */
static void flagValueHint(const FlagField *f, IrisString *s) {
    const JsonType *t = f->type;
    const char *hint = NULL;
    switch (t->kind) {
    case J_INT: case J_UINT: hint = f->duration ? "<duration>" : "<n>"; break;
    case J_FLOAT: hint = "<number>"; break;
    case J_STR: case J_STRING: hint = "<text>"; break;
    case J_ENUM: {
        const JsonName *names = t->items;
        iris_string_append(s, "<", 1);
        for (int64_t k = 0; k < t->count; k++) {
            if (k) iris_string_append(s, "|", 1);
            iris_string_append(s, names[k].name, names[k].len);
        }
        iris_string_append(s, ">", 1);
        return;
    }
    default: return;
    }
    iris_string_append(s, hint, (int64_t)strlen(hint));
}

/* The field's value as the help shows a default; nothing for zero values. */
static void flagDefault(const FlagField *f, const char *p, IrisString *s) {
    const JsonType *t = f->type;
    char buf[64];
    int n = 0;
    switch (t->kind) {
    case J_INT: case J_UINT: {
        int64_t v = t->size == 1 ? (t->kind == J_INT ? *(int8_t *)p : *(uint8_t *)p)
                  : t->size == 2 ? (t->kind == J_INT ? *(int16_t *)p : *(uint16_t *)p)
                  : t->size == 4 ? (t->kind == J_INT ? *(int32_t *)p : (int64_t)*(uint32_t *)p)
                  : *(int64_t *)p;
        if (v == 0) return;
        if (f->duration) {
            iris_string_append(s, " (default ", 10);
            iris_write_duration(3, s, v);
            iris_string_append(s, ")", 1);
            return;
        }
        n = snprintf(buf, sizeof buf, " (default %lld)", (long long)v);
        break;
    }
    case J_BOOL:
        if (!*(const uint8_t *)p) return;
        n = snprintf(buf, sizeof buf, " (default true)");
        break;
    case J_FLOAT: {
        double v = t->size == 4 ? *(float *)p : *(double *)p;
        if (v == 0) return;
        n = snprintf(buf, sizeof buf, " (default %g)", v);
        break;
    }
    case J_STR: case J_STRING: {
        const Slice *v = (const Slice *)p;
        if (v->len == 0) return;
        iris_string_append(s, " (default \"", 11);
        iris_string_append(s, v->ptr, v->len);
        iris_string_append(s, "\")", 2);
        return;
    }
    case J_ENUM: {
        const JsonName *names = t->items;
        int32_t v = *(const int32_t *)p;
        for (int64_t k = 0; k < t->count; k++) {
            if (names[k].value == v) {
                iris_string_append(s, " (default ", 10);
                iris_string_append(s, names[k].name, names[k].len);
                iris_string_append(s, ")", 1);
            }
        }
        return;
    }
    default: return;
    }
    iris_string_append(s, buf, n);
}

static void flagUsageLine(const FlagTable *t, const char *prog, int64_t progLen) {
    msgf("usage: %.*s", (int)progLen, prog);
    msgf(" [flags]");
    for (int64_t i = 0; i < t->count; i++) {
        const FlagField *f = &t->fields[i];
        if (f->args) msgf(f->required ? " %.*s..." : " [%.*s...]", (int)f->nameLen, f->name);
    }
    msgf("\n");
}

static void flagHelp(const FlagTable *t, const char *out, const char *prog, int64_t progLen) {
    if (t->aboutLen) msgf("%.*s\n\n", (int)t->aboutLen, t->about);
    flagUsageLine(t, prog, progLen);
    msgf("\nflags:\n");
    /* the left column: "  -p, --port <n>" */
    IrisString left[256] = {0};
    int64_t rows = 0, width = 0;
    const FlagField *shown[256];
    for (int64_t i = 0; i < t->count && rows < 255; i++) {
        const FlagField *f = &t->fields[i];
        if (f->args) continue;
        IrisString *l = &left[rows];
        char head[16];
        int n = f->letter ? snprintf(head, sizeof head, "  -%c, --", (char)f->letter) : snprintf(head, sizeof head, "      --");
        iris_string_append(l, head, n);
        iris_string_append(l, f->name, f->nameLen);
        if (f->type->kind != J_BOOL) {
            iris_string_append(l, " ", 1);
            flagValueHint(f, l);
        }
        if (l->len > width) width = l->len;
        shown[rows++] = f;
    }
    if (width < 16) width = 16;
    for (int64_t r = 0; r < rows; r++) {
        const FlagField *f = shown[r];
        iris_string_append(&flagMsg, left[r].ptr, left[r].len);
        int64_t bare = flagMsg.len;
        for (int64_t k = left[r].len; k < width + 2; k++) iris_string_append(&flagMsg, " ", 1);
        int64_t padded = flagMsg.len;
        iris_string_append(&flagMsg, f->help, f->helpLen);
        if (f->required) {
            iris_string_append(&flagMsg, f->helpLen ? " (required)" : "(required)", f->helpLen ? 11 : 10);
        } else {
            flagDefault(f, out + f->offset, &flagMsg);
        }
        if (flagMsg.len == padded) {
            flagMsg.len = bare;         /* no spaces at the end */
        } else if (!f->helpLen && flagMsg.ptr[padded] == ' ') {
            memmove(flagMsg.ptr + padded, flagMsg.ptr + padded + 1, (size_t)(flagMsg.len - padded - 1));
            flagMsg.len--;              /* "(default 30s)" right after the padding */
        }
        iris_string_append(&flagMsg, "\n", 1);
        iris_string_free(&left[r]);
    }
    msgf("  -h, --help");
    for (int64_t k = 12; k < width + 2; k++) msgf(" ");
    msgf("show this help\n");
}

/* Fills out from the arguments. 0 when it worked, 1 for --help (the
 * message is the help), 2 when something is wrong (the message says what).
 * n < 0 reads the program's own arguments. */
int iris_flags_parse(const FlagTable *t, char *out, const Slice *args, int64_t n, const char *prog,
                     int64_t progLen) {
    iris_string_free(&flagMsg);
    const char *argv0 = NULL;
    if (n < 0) {
        argv0 = gArgc > 0 ? gArgv[0] : "program";
        const char *slash = strrchr(argv0, '/');
        if (slash) argv0 = slash + 1;
    }
    if (progLen == 0) {
        prog = argv0 ? argv0 : "program";
        progLen = (int64_t)strlen(prog);
    }
    int64_t count = n < 0 ? (gArgc > 0 ? gArgc - 1 : 0) : n;
    /* C strings for each argument */
    char **av = calloc((size_t)count + 1, sizeof *av);
    for (int64_t i = 0; i < count; i++) {
        if (n < 0) {
            av[i] = gArgv[i + 1];
        } else {
            av[i] = malloc((size_t)args[i].len + 1);
            memcpy(av[i], args[i].ptr, (size_t)args[i].len);
            av[i][args[i].len] = 0;
        }
    }
    const FlagField *rest = NULL;
    for (int64_t i = 0; i < t->count; i++) {
        if (t->fields[i].args) rest = &t->fields[i];
    }
    bool *seen = calloc((size_t)t->count + 1, 1);
    Slice *positional = calloc((size_t)count + 1, sizeof *positional);
    int64_t npos = 0;
    int code = 0;
    bool flagsDone = false;
    for (int64_t i = 0; i < count && code == 0; i++) {
        char *a = av[i];
        if (flagsDone || a[0] != '-' || a[1] == 0) {
            /* the caller's own text, or the program's argument */
            positional[npos].ptr = n < 0 ? a : args[i].ptr;
            positional[npos].len = (int64_t)strlen(a);
            npos++;
            continue;
        }
        if (strcmp(a, "--") == 0) {
            flagsDone = true;
            continue;
        }
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            flagHelp(t, out, prog, progLen);
            code = 1;
            break;
        }
        bool isLong = a[1] == '-';
        const char *name = a + (isLong ? 2 : 1);
        const char *eq = strchr(name, '=');
        size_t nameLen = eq ? (size_t)(eq - name) : strlen(name);
        /* -abc: several one-letter bool flags at once */
        if (!isLong && nameLen > 1) {
            for (size_t k = 0; k < nameLen && code == 0; k++) {
                const FlagField *f = NULL;
                for (int64_t j = 0; j < t->count; j++) {
                    if (t->fields[j].letter == name[k] && !t->fields[j].args) f = &t->fields[j];
                }
                if (!f) {
                    msgf("-%c isn't a flag", name[k]);
                    code = 2;
                } else if (f->type->kind != J_BOOL) {
                    msgf("-%c needs a value, so it can't be joined with other flags", name[k]);
                    code = 2;
                } else {
                    *(uint8_t *)(out + f->offset) = 1;
                    seen[f - t->fields] = true;
                }
            }
            continue;
        }
        const FlagField *f = NULL;
        for (int64_t j = 0; j < t->count; j++) {
            const FlagField *g = &t->fields[j];
            if (g->args) continue;
            if (isLong ? ((size_t)g->nameLen == nameLen && !memcmp(g->name, name, nameLen))
                       : (nameLen == 1 && g->letter == name[0])) {
                f = g;
            }
        }
        if (!f) {
            msgf("%.*s isn't a flag", (int)(nameLen + (isLong ? 2 : 1)), a);
            code = 2;
            break;
        }
        char shownName[300];
        snprintf(shownName, sizeof shownName, "%.*s", (int)(nameLen + (isLong ? 2 : 1)), a);
        const char *value = eq ? eq + 1 : NULL;
        int64_t from = i;
        if (!value) {
            if (f->type->kind == J_BOOL) {
                value = "true";
            } else if (i + 1 < count) {
                from = ++i;
                value = av[i];
            } else {
                msgf("%s needs a value", shownName);
                code = 2;
                break;
            }
        }
        const char *why = setFromText(f->type, f->duration, out + f->offset, value);
        if (why) {
            msgf("%s: '%.80s' %s", shownName, value, why);
            code = 2;
            break;
        }
        if (n >= 0 && f->type->kind == J_STR) {
            /* a str points into the caller's text, not the copy */
            Slice *v = (Slice *)(out + f->offset);
            v->ptr = args[from].ptr + (value - av[from]);
        }
        seen[f - t->fields] = true;
    }
    for (int64_t j = 0; j < t->count && code == 0; j++) {
        const FlagField *f = &t->fields[j];
        if (f->args) {
            if (f->required && npos == 0) {
                msgf("%.*s are needed", (int)f->nameLen, f->name);
                code = 2;
            }
        } else if (f->required && !seen[j]) {
            msgf("--%.*s is needed", (int)f->nameLen, f->name);
            code = 2;
        }
    }
    if (code == 0 && npos > 0 && !rest) {
        msgf("'%.80s' isn't a flag, and %.*s takes no other arguments", (char *)positional[0].ptr, (int)progLen, prog);
        code = 2;
    }
    if (code == 2) {
        iris_string_append(&flagMsg, "\n", 1);
        flagUsageLine(t, prog, progLen);
        msgf("run %.*s --help to see the flags\n", (int)progLen, prog);
    }
    if (code == 0 && rest && npos > 0) {
        /* the slice belongs to the struct, and its free gives it back */
        Slice *dst = (Slice *)(out + rest->offset);
        dst->ptr = malloc((size_t)npos * sizeof(Slice));
        dst->len = npos;
        memcpy(dst->ptr, positional, (size_t)npos * sizeof(Slice));
    }
    if (n >= 0) {
        for (int64_t i = 0; i < count; i++) free(av[i]);
    }
    free(seen);
    free(positional);
    free(av);
    return code;
}

void iris_json_quote(const char *text, int64_t len, IrisString *out) {
    jsonQuote(out, text, len);
}

/* ---------------------------------------------------------------- signals

   A signal the program watches is written, as one byte, into a pipe; a
   task that waits for one waits on the pipe, so the other tasks run. */

#include <signal.h>

static int sigPipe[2] = {-1, -1};
static int sigLock;

static void onSignal(int sig) {
    unsigned char b = (unsigned char)sig;
    ssize_t r = write(sigPipe[1], &b, 1);
    (void)r;
}

/* signal.Signal's cases, in order, as the system numbers them */
static const int sigNumbers[] = {SIGINT, SIGTERM, SIGHUP, SIGUSR1, SIGUSR2, SIGCHLD, SIGWINCH, SIGQUIT,
                                  SIGKILL, SIGSTOP, SIGCONT};

void iris_signal_watch(int which) {
    if (which < 0 || which >= (int)(sizeof sigNumbers / sizeof sigNumbers[0])) return;
    int sig = sigNumbers[which];
    spinLock(&sigLock);
    if (sigPipe[0] < 0) {
        if (pipe(sigPipe) != 0) {
            spinUnlock(&sigLock);
            return;
        }
        for (int i = 0; i < 2; i++) {
            fcntl(sigPipe[i], F_SETFL, fcntl(sigPipe[i], F_GETFL, 0) | O_NONBLOCK);
            fcntl(sigPipe[i], F_SETFD, FD_CLOEXEC);
        }
    }
    spinUnlock(&sigLock);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(sig, &sa, NULL);
}

/* Sends signal.Signal which to process pid, or to every process of the
   group pid when group: 0, or errno. */
int iris_signal_send(int64_t pid, int which, int group) {
    if (which < 0 || which >= (int)(sizeof sigNumbers / sizeof sigNumbers[0]) || pid <= 0) return EINVAL;
    return kill(group ? -(pid_t)pid : (pid_t)pid, sigNumbers[which]) == 0 ? 0 : errno;
}

/* The next signal that came, as its case of signal.Signal. */
int iris_signal_wait(void) {
    for (;;) {
        unsigned char b;
        ssize_t n = read(sigPipe[0], &b, 1);
        if (n == 1) {
            for (int i = 0; i < (int)(sizeof sigNumbers / sizeof sigNumbers[0]); i++) {
                if (sigNumbers[i] == b) return i;
            }
            continue;
        }
        waitFd(sigPipe[0], 0);
    }
}

/* Ends a connection's reads and writes from another task: whatever waits
   on it wakes up with the connection closed. */
void iris_net_shutdown(int fd) {
    if (fd >= 0) shutdown(fd, SHUT_RDWR);
}

/* MD5 (RFC 1321), for protocols that still use it, like PostgreSQL's older
   password login; it isn't safe for signing. */
void iris_md5(const char *data, int64_t len, unsigned char *out) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                              5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                              4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    uint64_t bits = (uint64_t)len * 8;
    int64_t total = ((len + 8) / 64 + 1) * 64;
    unsigned char *msg = calloc((size_t)total, 1);
    if (!msg) outOfMemory();
    memcpy(msg, data, (size_t)len);
    msg[len] = 0x80;
    for (int i = 0; i < 8; i++) msg[total - 8 + i] = (unsigned char)(bits >> (8 * i));
    for (int64_t off = 0; off < total; off += 64) {
        uint32_t w[16];
        for (int i = 0; i < 16; i++) {
            const unsigned char *b = msg + off + i * 4;
            w[i] = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; i++) {
            uint32_t f;
            int g;
            if (i < 16) {
                f = (b & c) | (~b & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | (~d & c);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
            } else {
                f = c ^ (b | ~d);
                g = (7 * i) % 16;
            }
            uint32_t t = d;
            d = c;
            c = b;
            uint32_t x = a + f + K[i] + w[g];
            b = b + (x << R[i] | x >> (32 - R[i]));
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
    }
    free(msg);
    for (int i = 0; i < 4; i++) {
        for (int k = 0; k < 4; k++) out[i * 4 + k] = (unsigned char)(h[i] >> (8 * k));
    }
}

/* SHA-1, for the WebSocket handshake only: it isn't safe for signing. */
void iris_sha1(const char *data, int64_t len, unsigned char *out) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint64_t bits = (uint64_t)len * 8;
    int64_t total = ((len + 8) / 64 + 1) * 64;
    unsigned char *msg = calloc((size_t)total, 1);
    if (!msg) outOfMemory();
    memcpy(msg, data, (size_t)len);
    msg[len] = 0x80;
    for (int i = 0; i < 8; i++) msg[total - 1 - i] = (unsigned char)(bits >> (8 * i));
    for (int64_t off = 0; off < total; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            const unsigned char *p = msg + off + i * 4;
            w[i] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
        }
        for (int i = 16; i < 80; i++) {
            uint32_t x = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = (x << 1) | (x >> 31);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d;
            d = c;
            c = (b << 30) | (b >> 2);
            b = a;
            a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    free(msg);
    for (int i = 0; i < 5; i++) {
        out[i * 4] = (unsigned char)(h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)h[i];
    }
}

/* ---------------------------------------------------------------- compress

   zlib, loaded on first use. Its stream struct is declared here, as it is
   in zlib.h, so no headers are needed to build programs. */

typedef struct {
    const unsigned char *next_in;
    unsigned avail_in;
    unsigned long total_in;
    unsigned char *next_out;
    unsigned avail_out;
    unsigned long total_out;
    const char *msg;
    void *state;
    void *zalloc, *zfree, *opaque;
    int data_type;
    unsigned long adler, reserved;
} ZStream;

static struct {
    int loaded;
    const char *(*version)(void);
    int (*deflateInit2)(ZStream *, int, int, int, int, int, const char *, int);
    int (*deflate)(ZStream *, int);
    int (*deflateEnd)(ZStream *);
    int (*inflateInit2)(ZStream *, int, const char *, int);
    int (*inflate)(ZStream *, int);
    int (*inflateEnd)(ZStream *);
    unsigned long (*crc32)(unsigned long, const unsigned char *, unsigned);
    unsigned long (*adler32)(unsigned long, const unsigned char *, unsigned);
} zl;

static pthread_once_t zOnce = PTHREAD_ONCE_INIT;
static _Thread_local char zMsg[300];
static _Thread_local int64_t zMsgLen;

const char *iris_z_message_ptr(void) { return zMsg; }
int64_t iris_z_message_len(void) { return zMsgLen; }

static void zSay(const char *what, const char *detail) {
    zMsgLen = snprintf(zMsg, sizeof zMsg, "%s%s%s", what, detail ? ": " : "", detail ? detail : "");
    if (zMsgLen >= (int64_t)sizeof zMsg) zMsgLen = sizeof zMsg - 1;
}

static void zLoad(void) {
#ifdef __APPLE__
    static const char *const names[] = {"/usr/lib/libz.1.dylib", "/usr/lib/libz.dylib",
                                        "/opt/homebrew/opt/zlib/lib/libz.1.dylib", NULL};
#else
    static const char *const names[] = {"libz.so.1", "libz.so", NULL};
#endif
    const char *env = getenv("IRIS_ZLIB");
    void *h = env && *env ? dlopen(env, RTLD_NOW) : NULL;
    for (int i = 0; !h && names[i]; i++) h = dlopen(names[i], RTLD_NOW);
    zl.loaded = -1;
    if (!h) return;
#define ZS(field, name)                         \
    do {                                        \
        *(void **)&zl.field = dlsym(h, name);   \
        if (!zl.field) return;                  \
    } while (0)
    ZS(version, "zlibVersion");
    ZS(deflateInit2, "deflateInit2_");
    ZS(deflate, "deflate");
    ZS(deflateEnd, "deflateEnd");
    ZS(inflateInit2, "inflateInit2_");
    ZS(inflate, "inflate");
    ZS(inflateEnd, "inflateEnd");
    ZS(crc32, "crc32");
    ZS(adler32, "adler32");
#undef ZS
    zl.loaded = 1;
}

static bool zReady(void) {
    pthread_once(&zOnce, zLoad);
    if (zl.loaded != 1) {
        zSay("compression needs zlib (libz): install it, or set IRIS_ZLIB to its library", NULL);
        return false;
    }
    return true;
}

typedef struct {
    ZStream s;
    int compress;
    int done;
} ZState;

/* A stream that compresses (compress 1) or decompresses; format 0 gzip,
   1 zlib, 2 raw deflate; decompressing gzip also takes zlib. */
void *iris_z_new(int compress, int format, int level) {
    if (!zReady()) return NULL;
    ZState *z = calloc(1, sizeof *z);
    z->compress = compress;
    int bits = format == 0 ? 31 : format == 1 ? 15 : -15;
    int rc;
    if (compress) {
        rc = zl.deflateInit2(&z->s, level < 0 ? 5 : level > 9 ? 9 : level, 8, bits, 8, 0, zl.version(),
                             (int)sizeof(ZStream));
    } else {
        rc = zl.inflateInit2(&z->s, format == 0 ? 47 : bits, zl.version(), (int)sizeof(ZStream));
    }
    if (rc != 0) {
        zSay("starting zlib", z->s.msg);
        free(z);
        return NULL;
    }
    return z;
}

/* Feeds n bytes and appends what comes out to out; finish ends a
   compressed stream. 0 when more can come, 1 at the end of a decompressed
   stream, -1 when the data is broken, -2 when out would pass limit. */
int iris_z_push(void *handle, const char *in, int64_t n, int finish, IrisString *out, int64_t limit) {
    ZState *z = handle;
    if (z->done) return n > 0 && !z->compress ? -1 : 1;
    z->s.next_in = (const unsigned char *)in;
    z->s.avail_in = (unsigned)n;
    char buf[65536];
    for (;;) {
        z->s.next_out = (unsigned char *)buf;
        z->s.avail_out = sizeof buf;
        int rc = z->compress ? zl.deflate(&z->s, finish ? 4 : 0) : zl.inflate(&z->s, 0);
        int64_t got = (int64_t)(sizeof buf - z->s.avail_out);
        if (got > 0) {
            if (limit > 0 && out->len + got > limit) {
                zSay("the data comes out larger than the limit", NULL);
                return -2;
            }
            iris_string_append(out, buf, got);
        }
        if (rc == 1) {                          /* Z_STREAM_END */
            z->done = 1;
            return 1;
        }
        if (rc == -5) return 0;                 /* Z_BUF_ERROR: it needs more input */
        if (rc != 0) {
            zSay(z->compress ? "compressing" : "the compressed data is broken", z->s.msg);
            return -1;
        }
        if (z->s.avail_in == 0 && z->s.avail_out != 0 && !finish) return 0;
    }
}

void iris_z_free(void *handle) {
    ZState *z = handle;
    if (!z) return;
    if (z->compress) zl.deflateEnd(&z->s);
    else zl.inflateEnd(&z->s);
    free(z);
}

#ifdef __x86_64__
/* CRC-32 (the zip polynomial) one byte at a time, and the table for it */
static uint32_t crcTable[8][256];
static pthread_once_t crcOnce = PTHREAD_ONCE_INIT;
static void crcTables(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crcTable[0][i] = c;
    }
    for (int t = 1; t < 8; t++)
        for (int i = 0; i < 256; i++) crcTable[t][i] = (crcTable[t - 1][i] >> 8) ^ crcTable[0][crcTable[t - 1][i] & 0xff];
}

/* slicing by 8: c is the running value, already inverted */
static uint32_t crcSlice(uint32_t c, const uint8_t *p, int64_t n) {
    while (n >= 8) {
        uint32_t a, b;
        memcpy(&a, p, 4);
        memcpy(&b, p + 4, 4);
        a ^= c;
        c = crcTable[7][a & 0xff] ^ crcTable[6][(a >> 8) & 0xff] ^ crcTable[5][(a >> 16) & 0xff] ^ crcTable[4][a >> 24] ^
            crcTable[3][b & 0xff] ^ crcTable[2][(b >> 8) & 0xff] ^ crcTable[1][(b >> 16) & 0xff] ^ crcTable[0][b >> 24];
        p += 8;
        n -= 8;
    }
    while (n-- > 0) c = crcTable[0][(c ^ *p++) & 0xff] ^ (c >> 8);
    return c;
}

#define CRC_HW __attribute__((target("pclmul,sse4.1")))

CRC_HW static inline __m128i crcFold(__m128i a, __m128i b, __m128i k) {
    return _mm_xor_si128(_mm_xor_si128(b, _mm_clmulepi64_si128(a, k, 0x00)), _mm_clmulepi64_si128(a, k, 0x11));
}

/* folds 64 bytes at a time with carry-less multiplies, then reduces to 32
   bits (Intel's "Fast CRC Computation Using PCLMULQDQ"); n is at least 64.
   c is the running value, already inverted. */
CRC_HW static uint32_t crcClmul(uint32_t c, const uint8_t *p, int64_t n) {
    __m128i x3 = _mm_loadu_si128((const __m128i *)p), x2 = _mm_loadu_si128((const __m128i *)(p + 16)),
            x1 = _mm_loadu_si128((const __m128i *)(p + 32)), x0 = _mm_loadu_si128((const __m128i *)(p + 48));
    x3 = _mm_xor_si128(x3, _mm_cvtsi32_si128((int)c));
    p += 64;
    n -= 64;
    __m128i k12 = _mm_set_epi64x(0x1c6e41596, 0x154442bd4);
    while (n >= 64) {
        x3 = crcFold(x3, _mm_loadu_si128((const __m128i *)p), k12);
        x2 = crcFold(x2, _mm_loadu_si128((const __m128i *)(p + 16)), k12);
        x1 = crcFold(x1, _mm_loadu_si128((const __m128i *)(p + 32)), k12);
        x0 = crcFold(x0, _mm_loadu_si128((const __m128i *)(p + 48)), k12);
        p += 64;
        n -= 64;
    }
    __m128i k34 = _mm_set_epi64x(0x0ccaa009e, 0x1751997d0);
    __m128i x = crcFold(x3, x2, k34);
    x = crcFold(x, x1, k34);
    x = crcFold(x, x0, k34);
    while (n >= 16) {
        x = crcFold(x, _mm_loadu_si128((const __m128i *)p), k34);
        p += 16;
        n -= 16;
    }
    __m128i low32 = _mm_set_epi32(0, 0, 0, -1);
    x = _mm_xor_si128(_mm_clmulepi64_si128(x, k34, 0x10), _mm_srli_si128(x, 8));
    x = _mm_xor_si128(_mm_clmulepi64_si128(_mm_and_si128(x, low32), _mm_set_epi64x(0, 0x163cd6124), 0x00),
                      _mm_srli_si128(x, 4));
    __m128i pu = _mm_set_epi64x(0x1f7011641, 0x1db710641);   /* Barrett: u', then P(x) */
    __m128i t1 = _mm_clmulepi64_si128(_mm_and_si128(x, low32), pu, 0x10);
    __m128i t2 = _mm_clmulepi64_si128(_mm_and_si128(t1, low32), pu, 0x00);
    c = (uint32_t)_mm_extract_epi32(_mm_xor_si128(x, t2), 1);
    return crcSlice(c, p, n);
}

#endif

int64_t iris_crc32(int64_t crc, const char *p, int64_t n) {
#ifdef __x86_64__
    pthread_once(&crcOnce, crcTables);
    uint32_t c = ~(uint32_t)crc;
    if (n >= 64 && __builtin_cpu_supports("pclmul") && __builtin_cpu_supports("sse4.1"))
        c = crcClmul(c, (const uint8_t *)p, n);
    else
        c = crcSlice(c, (const uint8_t *)p, n);
    return (int64_t)(uint32_t)~c;
#else
    if (!zReady()) return 0;
    unsigned long c = (unsigned long)crc;
    while (n > 0) {
        unsigned part = n > 0x40000000 ? 0x40000000 : (unsigned)n;
        c = zl.crc32(c, (const unsigned char *)p, part);
        p += part;
        n -= part;
    }
    return (int64_t)c;
#endif
}

int64_t iris_adler32(int64_t a, const char *p, int64_t n) {
    if (!zReady()) return 0;
    unsigned long c = (unsigned long)a;
    while (n > 0) {
        unsigned part = n > 0x40000000 ? 0x40000000 : (unsigned)n;
        c = zl.adler32(c, (const unsigned char *)p, part);
        p += part;
        n -= part;
    }
    return (int64_t)c;
}

/* ---------------------------------------------------------------- archives (0.66)

   zip's deflate and inflate: one raw stream for each thread, reset for each
   entry, so an archive of many small files sets zlib up once, and the bytes
   go straight into the String. */

static struct {
    int (*deflateReset)(ZStream *);
    int (*inflateReset)(ZStream *);
    unsigned long (*deflateBound)(ZStream *, unsigned long);
} zr;
static pthread_once_t zrOnce = PTHREAD_ONCE_INIT;

static void zrLoad(void) {
#ifdef __APPLE__
    static const char *const names[] = {"/usr/lib/libz.1.dylib", "/usr/lib/libz.dylib",
                                        "/opt/homebrew/opt/zlib/lib/libz.1.dylib", NULL};
#else
    static const char *const names[] = {"libz.so.1", "libz.so", NULL};
#endif
    const char *env = getenv("IRIS_ZLIB");
    void *h = env && *env ? dlopen(env, RTLD_NOW) : NULL;
    for (int i = 0; !h && names[i]; i++) h = dlopen(names[i], RTLD_NOW);
    if (!h) return;
    *(void **)&zr.deflateReset = dlsym(h, "deflateReset");
    *(void **)&zr.inflateReset = dlsym(h, "inflateReset");
    *(void **)&zr.deflateBound = dlsym(h, "deflateBound");
}

static bool zrReady(void) {
    if (!zReady()) return false;
    pthread_once(&zrOnce, zrLoad);
    if (!zr.deflateReset || !zr.inflateReset || !zr.deflateBound) {
        zSay("this zlib has no deflateReset, inflateReset or deflateBound", NULL);
        return false;
    }
    return true;
}

/* Raw deflate (RFC 1951) of a whole buffer: LZ77 with hash chains and
   lazy matching, as zlib's levels 4 to 9 do, with matches compared 8 bytes
   at a time and a long match only partly put in the hash table, as
   zlib-ng's medium strategy does. Each block is written with dynamic
   Huffman codes, the fixed ones or stored, whichever is smallest. */

#define DF_WSIZE 32768
#define DF_WMASK (DF_WSIZE - 1)
#define DF_HBITS 15
#define DF_HSIZE (1 << DF_HBITS)
#define DF_MINMATCH 3
#define DF_MAXMATCH 258
#define DF_BLOCK 16384                  /* symbols a block holds */

typedef struct {
    uint8_t *out;
    int64_t len, cap;
    uint64_t bits;
    int nbits;
} DfOut;

static void dfReserve(DfOut *o, int64_t more) {
    if (o->len + more > o->cap) {
        int64_t cap = o->cap ? o->cap * 2 : 4096;
        while (cap < o->len + more) cap *= 2;
        uint8_t *q = realloc(o->out, (size_t)cap);
        if (!q) outOfMemory();
        o->out = q;
        o->cap = cap;
    }
}

/* the low n bits of v, the lowest first */
static inline void dfPut(DfOut *o, uint64_t v, int n) {
    o->bits |= v << o->nbits;
    o->nbits += n;
    if (o->nbits >= 32) {
        dfReserve(o, 8);
        uint32_t w = (uint32_t)o->bits;
        memcpy(o->out + o->len, &w, 4);
        o->len += 4;
        o->bits >>= 32;
        o->nbits -= 32;
    }
}

static void dfAlign(DfOut *o) {
    dfReserve(o, 8);
    while (o->nbits > 0) {
        o->out[o->len++] = (uint8_t)o->bits;
        o->bits >>= 8;
        o->nbits = o->nbits > 8 ? o->nbits - 8 : 0;
    }
    o->bits = 0;
    o->nbits = 0;
}

static const uint16_t dfLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                       35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t dfLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dfDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                        1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dfDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
static const uint8_t dfClOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

static uint8_t dfLenCode[259];          /* match length -> length code - 257 */
static uint8_t dfDistCodeLo[512];       /* distance - 1 below 512 -> code; above, (d - 1) >> 7 */
static pthread_once_t dfOnce = PTHREAD_ONCE_INIT;

static void dfTables(void) {
    for (int c = 0; c < 29; c++) {
        int top = c == 28 ? 258 : dfLenBase[c] + (1 << dfLenExtra[c]) - 1;
        for (int l = dfLenBase[c]; l <= top && l <= 258; l++) dfLenCode[l] = (uint8_t)c;
    }
    dfLenCode[258] = 28;
    for (int c = 0; c < 30; c++) {
        int top = dfDistBase[c] + (1 << dfDistExtra[c]) - 1;
        for (int d = dfDistBase[c]; d <= top && d <= 32768; d++) {
            if (d - 1 < 256) dfDistCodeLo[d - 1] = (uint8_t)c;
            else dfDistCodeLo[256 + ((d - 1) >> 7)] = (uint8_t)c;
        }
    }
}

static inline int dfDistCode(int d) {
    return d - 1 < 256 ? dfDistCodeLo[d - 1] : dfDistCodeLo[256 + ((d - 1) >> 7)];
}

/* Code lengths of at most maxBits for the symbols' frequencies: Moffat
   and Katajainen's in-place minimum redundancy on the sorted frequencies,
   then the longest codes shortened as miniz does, keeping the code
   complete. Symbols with no uses get 0. */
static void dfLengths(const uint32_t *freq, int n, int maxBits, uint8_t *lens) {
    int idx[288], used = 0;
    uint32_t a[288];
    for (int i = 0; i < n; i++) {
        lens[i] = 0;
        if (freq[i]) idx[used++] = i;
    }
    if (used == 0) return;
    if (used == 1) {
        lens[idx[0]] = 1;
        return;
    }
    /* by frequency, the rarest first (insertion sort: n is at most 288) */
    for (int i = 1; i < used; i++) {
        int s = idx[i], j = i - 1;
        while (j >= 0 && freq[idx[j]] > freq[s]) {
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = s;
    }
    for (int i = 0; i < used; i++) a[i] = freq[idx[i]];
    /* Moffat-Katajainen */
    int root = 0, leaf = 2, next;
    a[0] += a[1];
    for (next = 1; next < used - 1; next++) {
        if (leaf >= used || a[root] < a[leaf]) {
            a[next] = a[root];
            a[root++] = (uint32_t)next;
        } else {
            a[next] = a[leaf++];
        }
        if (leaf >= used || (root < next && a[root] < a[leaf])) {
            a[next] += a[root];
            a[root++] = (uint32_t)next;
        } else {
            a[next] += a[leaf++];
        }
    }
    a[used - 2] = 0;
    for (next = used - 3; next >= 0; next--) a[next] = a[a[next]] + 1;
    int avbl = 1, usedNodes = 0, depth = 0;
    root = used - 2;
    next = used - 1;
    while (avbl > 0) {
        while (root >= 0 && (int)a[root] == depth) {
            usedNodes++;
            root--;
        }
        while (avbl > usedNodes) {
            a[next--] = (uint32_t)depth;
            avbl--;
        }
        avbl = 2 * usedNodes;
        depth++;
        usedNodes = 0;
    }
    /* a[i] is now the length of the i-th rarest; count them by length */
    int count[33] = {0};
    for (int i = 0; i < used; i++) count[a[i] > 32 ? 32 : a[i]]++;
    for (int i = maxBits + 1; i <= 32; i++) {
        count[maxBits] += count[i];
        count[i] = 0;
    }
    uint32_t total = 0;
    for (int i = maxBits; i > 0; i--) total += (uint32_t)count[i] << (maxBits - i);
    while (total != (1u << maxBits)) {
        count[maxBits]--;
        for (int i = maxBits - 1; i > 0; i--) {
            if (count[i]) {
                count[i]--;
                count[i + 1] += 2;
                break;
            }
        }
        total--;
    }
    /* the longest lengths to the rarest symbols */
    int k = 0;
    for (int len = maxBits; len > 0; len--) {
        for (int c = 0; c < count[len]; c++) lens[idx[k++]] = (uint8_t)len;
    }
}

/* Canonical codes for the lengths, bit-reversed as deflate writes them. */
static void dfCodes(const uint8_t *lens, int n, uint16_t *codes) {
    int count[16] = {0}, next[16];
    for (int i = 0; i < n; i++) count[lens[i]]++;
    count[0] = 0;
    int code = 0;
    for (int b = 1; b < 16; b++) {
        code = (code + count[b - 1]) << 1;
        next[b] = code;
    }
    for (int i = 0; i < n; i++) {
        int l = lens[i];
        if (!l) {
            codes[i] = 0;
            continue;
        }
        unsigned c = (unsigned)next[l]++, r = 0;
        for (int b = 0; b < l; b++) r |= ((c >> b) & 1) << (l - 1 - b);
        codes[i] = (uint16_t)r;
    }
}

/* A block's symbols: a literal, or a length (3..258) and a distance. */
typedef struct {
    uint16_t len;                       /* 0 for a literal */
    uint16_t val;                       /* the byte, or the distance */
} DfSym;

typedef struct {
    uint32_t lfreq[286], dfreq[30];
    DfSym sym[DF_BLOCK];
    int n;
    int64_t start;                      /* where the block's bytes start in the input */
} DfBlock;

static void dfWriteSyms(DfOut *o, const DfBlock *b, const uint16_t *lc, const uint8_t *ll, const uint16_t *dc,
                        const uint8_t *dl) {
    for (int i = 0; i < b->n; i++) {
        DfSym s = b->sym[i];
        if (!s.len) {
            dfPut(o, lc[s.val], ll[s.val]);
            continue;
        }
        int c = dfLenCode[s.len];
        dfPut(o, lc[257 + c], ll[257 + c]);
        if (dfLenExtra[c]) dfPut(o, (uint64_t)(s.len - dfLenBase[c]), dfLenExtra[c]);
        int d = dfDistCode(s.val);
        dfPut(o, dc[d], dl[d]);
        if (dfDistExtra[d]) dfPut(o, (uint64_t)(s.val - dfDistBase[d]), dfDistExtra[d]);
    }
    dfPut(o, lc[256], ll[256]);
}

/* bits of the block's symbols with these lengths, the end-of-block too */
static uint64_t dfCost(const DfBlock *b, const uint8_t *ll, const uint8_t *dl) {
    uint64_t bits = ll[256];
    for (int i = 0; i < 256; i++) bits += (uint64_t)b->lfreq[i] * ll[i];
    for (int c = 0; c < 29; c++) bits += (uint64_t)b->lfreq[257 + c] * (ll[257 + c] + dfLenExtra[c]);
    for (int c = 0; c < 30; c++) bits += (uint64_t)b->dfreq[c] * (dl[c] + dfDistExtra[c]);
    return bits;
}

static void dfFlush(DfOut *o, DfBlock *b, const uint8_t *in, int64_t end, bool last) {
    b->lfreq[256] = 1;
    /* dynamic codes */
    uint8_t ll[286], dl[30], cll[19];
    uint32_t dfreq[30];
    memcpy(dfreq, b->dfreq, sizeof dfreq);
    int dused = 0;
    for (int i = 0; i < 30; i++) dused += dfreq[i] != 0;
    if (dused == 0) dfreq[0] = 1;       /* one code of one bit when no distance is used */
    dfLengths(b->lfreq, 286, 15, ll);
    dfLengths(dfreq, 30, 15, dl);
    int hlit = 286, hdist = 30;
    while (hlit > 257 && !ll[hlit - 1]) hlit--;
    while (hdist > 1 && !dl[hdist - 1]) hdist--;
    /* the lengths, run-length coded with 16, 17 and 18 */
    uint8_t all[316], rle[316], rleExtra[316];
    int nall = 0, nrle = 0;
    for (int i = 0; i < hlit; i++) all[nall++] = ll[i];
    for (int i = 0; i < hdist; i++) all[nall++] = dl[i];
    uint32_t clfreq[19] = {0};
    for (int i = 0; i < nall;) {
        int v = all[i], run = 1;
        while (i + run < nall && all[i + run] == v) run++;
        int left = run;
        if (v == 0) {
            while (left >= 11) {
                int k = left > 138 ? 138 : left;
                rle[nrle] = 18;
                rleExtra[nrle++] = (uint8_t)(k - 11);
                clfreq[18]++;
                left -= k;
            }
            if (left >= 3) {
                rle[nrle] = 17;
                rleExtra[nrle++] = (uint8_t)(left - 3);
                clfreq[17]++;
                left = 0;
            }
        } else {
            rle[nrle] = (uint8_t)v;
            rleExtra[nrle++] = 0;
            clfreq[v]++;
            left--;
            while (left >= 3) {
                int k = left > 6 ? 6 : left;
                rle[nrle] = 16;
                rleExtra[nrle++] = (uint8_t)(k - 3);
                clfreq[16]++;
                left -= k;
            }
        }
        while (left-- > 0) {
            rle[nrle] = (uint8_t)v;
            rleExtra[nrle++] = 0;
            clfreq[v]++;
        }
        i += run;
    }
    dfLengths(clfreq, 19, 7, cll);
    int hclen = 19;
    while (hclen > 4 && !cll[dfClOrder[hclen - 1]]) hclen--;
    uint64_t dynBits = 3 + 5 + 5 + 4 + 3 * (uint64_t)hclen + dfCost(b, ll, dl);
    for (int i = 0; i < nrle; i++) {
        dynBits += cll[rle[i]] + (rle[i] == 16 ? 2 : rle[i] == 17 ? 3 : rle[i] == 18 ? 7 : 0);
    }
    /* fixed codes */
    uint8_t fll[288], fdl[30];
    for (int i = 0; i < 288; i++) fll[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
    for (int i = 0; i < 30; i++) fdl[i] = 5;
    uint64_t fixBits = 3 + dfCost(b, fll, fdl);
    /* stored: whole bytes, at most 65535 to a block */
    int64_t raw = end - b->start;
    uint64_t storedBits = (uint64_t)(((raw + 65534) / 65535 + (raw == 0)) * 5 + raw) * 8 + 7;
    if (storedBits <= dynBits && storedBits <= fixBits) {
        int64_t at = b->start;
        do {
            int64_t k = end - at > 65535 ? 65535 : end - at;
            bool fin = last && at + k == end;
            dfPut(o, fin ? 1 : 0, 3);
            dfAlign(o);
            dfReserve(o, 4 + k);
            o->out[o->len++] = (uint8_t)k;
            o->out[o->len++] = (uint8_t)(k >> 8);
            o->out[o->len++] = (uint8_t)~k;
            o->out[o->len++] = (uint8_t)(~k >> 8);
            memcpy(o->out + o->len, in + at, (size_t)k);
            o->len += k;
            at += k;
        } while (at < end);
    } else if (fixBits <= dynBits) {
        uint16_t flc[288], fdc[30];
        dfCodes(fll, 288, flc);
        dfCodes(fdl, 30, fdc);
        dfPut(o, (last ? 1 : 0) | 1 << 1, 3);
        dfWriteSyms(o, b, flc, fll, fdc, fdl);
    } else {
        uint16_t lc[286], dc[30], clc[19];
        dfCodes(ll, 286, lc);
        dfCodes(dl, 30, dc);
        dfCodes(cll, 19, clc);
        dfPut(o, (last ? 1 : 0) | 2 << 1, 3);
        dfPut(o, (uint64_t)(hlit - 257), 5);
        dfPut(o, (uint64_t)(hdist - 1), 5);
        dfPut(o, (uint64_t)(hclen - 4), 4);
        for (int i = 0; i < hclen; i++) dfPut(o, cll[dfClOrder[i]], 3);
        for (int i = 0; i < nrle; i++) {
            int s = rle[i];
            dfPut(o, clc[s], cll[s]);
            if (s == 16) dfPut(o, rleExtra[i], 2);
            else if (s == 17) dfPut(o, rleExtra[i], 3);
            else if (s == 18) dfPut(o, rleExtra[i], 7);
        }
        dfWriteSyms(o, b, lc, ll, dc, dl);
    }
    memset(b->lfreq, 0, sizeof b->lfreq);
    memset(b->dfreq, 0, sizeof b->dfreq);
    b->n = 0;
    b->start = end;
}

/* zlib's level settings: a match this long or more searches less; lazy
   matching only below lazy; a match this long ends the search; the most
   chain links followed */
static const struct {
    int good, lazy, nice, chain;
} dfLevels[10] = {{0, 0, 0, 0},     {4, 4, 8, 4},       {4, 5, 16, 8},      {4, 6, 32, 32},
                  {4, 4, 16, 16},   {8, 16, 32, 32},    {8, 12, 128, 128},  {8, 32, 128, 256},
                  {32, 128, 258, 1024}, {32, 258, 258, 4096}};

/* 4 bytes hashed: a chain holds positions that agree in their first 4,
   which makes the chains much shorter than 3 would */
static inline uint32_t dfHash(const uint8_t *p, int shift) {
    uint32_t v;
    memcpy(&v, p, 4);
    return (v * 2654435761u) >> shift;
}

/* how many bytes at a and b are the same, up to max */
static inline int dfMatchLen(const uint8_t *a, const uint8_t *b, int max) {
    int n = 0;
    while (n + 8 <= max) {
        uint64_t x, y;
        memcpy(&x, a + n, 8);
        memcpy(&y, b + n, 8);
        uint64_t d = x ^ y;
        if (d) return n + (__builtin_ctzll(d) >> 3);
        n += 8;
    }
    while (n < max && a[n] == b[n]) n++;
    return n;
}

typedef struct {
    int32_t head[DF_HSIZE];
    int32_t prev[DF_WSIZE];
    int shift;      /* 32 less the hash's bits: fewer for a small input */
    DfBlock block;
} DfState;

/* Empties the hash table, using as much of it as the input needs: a small
   input clears a small table. */
static void dfReset(DfState *st, int64_t n) {
    int bits = 10;
    while (bits < DF_HBITS && ((int64_t)1 << bits) < n) bits++;
    st->shift = 32 - bits;
    memset(st->head, 0xff, sizeof st->head[0] << bits);   /* -1: no position yet */
}

static inline void dfInsert(DfState *st, const uint8_t *in, int64_t i) {
    uint32_t h = dfHash(in + i, st->shift);
    st->prev[i & DF_WMASK] = st->head[h];
    st->head[h] = (int32_t)i;
}

static int dfLongest(DfState *st, const uint8_t *in, int64_t n, int64_t i, int prevLen, int good, int nice,
                     int chain, int *dist) {
    int64_t cur = st->head[dfHash(in + i, st->shift)];
    int max = n - i > DF_MAXMATCH ? DF_MAXMATCH : (int)(n - i);
    if (nice > max) nice = max;
    int best = prevLen;
    if (best >= max) return best;       /* nothing longer fits */
    if (prevLen >= good) chain >>= 2;
    int64_t limit = i > DF_WSIZE ? i - DF_WSIZE : -1;
    /* the 4 bytes that end the best match so far: a candidate must have
       them to be longer, so most are passed over on one compare */
    uint32_t endWant, startWant;
    int endAt = best >= 3 ? best - 3 : 0;
    memcpy(&endWant, in + i + endAt, 4);
    memcpy(&startWant, in + i, 4);
    /* every link goes to an earlier position in the window, so the walk
       ends at the window's start, at -1, or when chain runs out */
    while (cur > limit) {
        int64_t next = st->prev[cur & DF_WMASK];
        __builtin_prefetch(in + (next > 0 ? next : 0) + endAt);
        uint32_t endHave, startHave;
        memcpy(&endHave, in + cur + endAt, 4);
        memcpy(&startHave, in + cur, 4);
        if (endHave == endWant && startHave == startWant) {
            int l = dfMatchLen(in + cur, in + i, max);
            if (l > best) {
                best = l;
                *dist = (int)(i - cur);
                if (l >= nice) break;
                endAt = best - 3;
                memcpy(&endWant, in + i + endAt, 4);
            }
        }
        if (--chain <= 0) break;
        cur = next;
    }
    return best;
}

static inline void dfLiteral(DfBlock *b, uint8_t c) {
    b->sym[b->n].len = 0;
    b->sym[b->n++].val = c;
    b->lfreq[c]++;
}

static inline void dfMatch(DfBlock *b, int len, int dist) {
    b->sym[b->n].len = (uint16_t)len;
    b->sym[b->n++].val = (uint16_t)dist;
    b->lfreq[257 + dfLenCode[len]]++;
    b->dfreq[dfDistCode(dist)]++;
}

/* The raw deflate of in[0..n] at a level from 1 to 9, appended to o. */
static void dfDeflate(const uint8_t *in, int64_t n, int level, DfOut *o) {
    pthread_once(&dfOnce, dfTables);
    if (level < 1) level = 1;
    if (level > 9) level = 9;
    int good = dfLevels[level].good, lazy = dfLevels[level].lazy, nice = dfLevels[level].nice,
        chain = dfLevels[level].chain;
    DfState *st = malloc(sizeof *st);
    if (!st) outOfMemory();
    dfReset(st, n);
    DfBlock *b = &st->block;
    memset(b->lfreq, 0, sizeof b->lfreq);
    memset(b->dfreq, 0, sizeof b->dfreq);
    b->n = 0;
    b->start = 0;
    int prevLen = DF_MINMATCH - 1, prevDist = 0;
    bool pending = false;               /* a literal at i - 1 still to place */
    int64_t i = 0;
    while (i < n) {
        int curLen = DF_MINMATCH - 1, curDist = 0;
        if (i + 4 <= n) {
            if (prevLen < lazy) curLen = dfLongest(st, in, n, i, prevLen, good, nice, chain, &curDist);
            if (curLen <= prevLen) curLen = DF_MINMATCH - 1;
            if (curLen == DF_MINMATCH && curDist > 4096) curLen = DF_MINMATCH - 1;    /* too far to pay */
            dfInsert(st, in, i);
        }
        if (pending && prevLen >= DF_MINMATCH && curLen <= prevLen) {
            /* the match at i - 1 is as good as any later one */
            dfMatch(b, prevLen, prevDist);
            int64_t end = i - 1 + prevLen;
            /* the positions the match covers go into the hash table, but
               only the first few of a long one, as zlib-ng does */
            int64_t stop = prevLen > 32 ? i + 4 : end;
            for (int64_t k = i + 1; k < stop && k + 4 <= n; k++) dfInsert(st, in, k);
            if (prevLen > 32 && end - 1 > i + 4 && end - 1 + 4 <= n) dfInsert(st, in, end - 1);
            i = end;
            pending = false;
            prevLen = DF_MINMATCH - 1;
        } else {
            if (pending) dfLiteral(b, in[i - 1]);
            pending = true;
            prevLen = curLen;
            prevDist = curDist;
            i++;
        }
        if (b->n >= DF_BLOCK - 2) dfFlush(o, b, in, i - (pending ? 1 : 0), false);
    }
    if (pending) dfLiteral(b, in[n - 1]);
    dfFlush(o, b, in, n, true);
    dfAlign(o);
    free(st);
}

static _Thread_local ZStream *zipDef, *zipInf;
static _Thread_local int zipLevel;

/* Deflates n bytes (raw deflate, at level) onto the end of out: 0, or -1
   with the message set. */
int iris_zip_deflate(const char *in, int64_t n, int level, IrisString *out) {
    if (n > 0xffffffffLL) {
        zSay("an entry over 4 GiB", NULL);
        return -1;
    }
    if (level != 0) {
        /* the runtime's own deflate (above): zip writes whole entries */
        DfOut o = {0};
        dfDeflate((const uint8_t *)in, n, level < 0 ? 5 : level, &o);
        iris_string_append(out, (const char *)o.out, o.len);
        free(o.out);
        return 0;
    }
    if (!zrReady()) return -1;
    if (n > 0xffffffffLL) {
        zSay("an entry over 4 GiB", NULL);
        return -1;
    }
    if (zipDef && zipLevel != level) {
        zl.deflateEnd(zipDef);
        free(zipDef);
        zipDef = NULL;
    }
    if (!zipDef) {
        zipDef = calloc(1, sizeof *zipDef);
        if (zl.deflateInit2(zipDef, level, 8, -15, 8, 0, zl.version(), (int)sizeof(ZStream)) != 0) {
            zSay("starting zlib", zipDef->msg);
            free(zipDef);
            zipDef = NULL;
            return -1;
        }
        zipLevel = level;
    } else {
        zr.deflateReset(zipDef);
    }
    int64_t bound = (int64_t)zr.deflateBound(zipDef, (unsigned long)n);
    if (out->len + bound > out->cap) {
        const char *none = NULL;
        stringGrow(out, &none, bound);
    }
    zipDef->next_in = (const unsigned char *)in;
    zipDef->avail_in = (unsigned)n;
    zipDef->next_out = (unsigned char *)out->ptr + out->len;
    zipDef->avail_out = (unsigned)bound;
    int rc = zl.deflate(zipDef, 4);              /* Z_FINISH */
    if (rc != 1) {
        zSay("compressing", zipDef->msg);
        return -1;
    }
    out->len += bound - zipDef->avail_out;
    return 0;
}

/* Inflates raw deflate data that should come out as size bytes onto the
   end of out: 0, or -1 when it is broken or comes out another size. */
int iris_zip_inflate(const char *in, int64_t n, int64_t size, IrisString *out) {
    if (!zrReady()) return -1;
    if (n > 0xffffffffLL || size > 0xffffffffLL) {
        zSay("an entry over 4 GiB", NULL);
        return -1;
    }
    if (!zipInf) {
        zipInf = calloc(1, sizeof *zipInf);
        if (zl.inflateInit2(zipInf, -15, zl.version(), (int)sizeof(ZStream)) != 0) {
            zSay("starting zlib", zipInf->msg);
            free(zipInf);
            zipInf = NULL;
            return -1;
        }
    } else {
        zr.inflateReset(zipInf);
    }
    /* one byte more than size, so data that comes out larger is seen */
    if (out->len + size + 1 > out->cap) {
        const char *none = NULL;
        stringGrow(out, &none, size + 1);
    }
    zipInf->next_in = (const unsigned char *)in;
    zipInf->avail_in = (unsigned)n;
    zipInf->next_out = (unsigned char *)out->ptr + out->len;
    zipInf->avail_out = (unsigned)(size + 1);
    int rc = zl.inflate(zipInf, 4);              /* Z_FINISH */
    int64_t got = size + 1 - (int64_t)zipInf->avail_out;
    if (rc != 1 || got != size) {
        zSay(rc == 1 || rc == -5 ? "it comes out another size than the archive says" : "the compressed data is broken",
             rc == 1 || rc == -5 ? NULL : zipInf->msg);
        return -1;
    }
    out->len += got;
    return 0;
}

/* Cuts a String back to len bytes. */
void iris_zip_truncate(IrisString *s, int64_t len) {
    if (len >= 0 && len < s->len) s->len = len;
}

/* ---------------------------------------------------------------- msgpack, cbor and protobuf

   msgpack.encode/decode, cbor.encode/decode and proto.encode/decode work
   from the same tables as JSON (ir.c, jsonType). A field of a @proto
   struct keeps its number and setting in JsonField.required: bit 0 is
   @json(required), bits 8 and 9 zigzag and fixed, the number from bit 16.
   Errors use JSON's codes and message (iris_json_message_ptr/len). */

enum { BIN_MSGPACK = 1, BIN_CBOR = 2 };

static bool binFail(JsonIn *in, int code, const char *what) {
    if (in->code) return false;
    in->code = code;
    if (in->pathLen) {
        jsonMsgLen = snprintf(jsonMsg, sizeof jsonMsg, "%s: %s", in->path, what);
    } else if (code == 1) {
        jsonMsgLen = snprintf(jsonMsg, sizeof jsonMsg, "byte %lld: %s", (long long)(in->p - in->s), what);
    } else {
        jsonMsgLen = snprintf(jsonMsg, sizeof jsonMsg, "%s", what);
    }
    if (jsonMsgLen >= (int64_t)sizeof jsonMsg) jsonMsgLen = sizeof jsonMsg - 1;
    return false;
}

static inline char *binRoom(IrisString *o, int64_t n) {
    if (__builtin_expect(o->len + n > o->cap, 0)) {
        const char *none = NULL;
        stringGrow(o, &none, n);
    }
    return o->ptr + o->len;
}

static inline void putBe(char *d, uint64_t v, int n) {
    for (int i = n - 1; i >= 0; i--) {
        d[i] = (char)v;
        v >>= 8;
    }
}

static inline uint64_t getBe(const char *s, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v = v << 8 | (unsigned char)s[i];
    return v;
}

/* ------------------------------------------------ writing MessagePack and CBOR */

static void mpUint(IrisString *o, uint64_t v) {
    char *d = binRoom(o, 9);
    if (v < 128) {
        d[0] = (char)v;
        o->len += 1;
    } else if (v <= 0xff) {
        d[0] = (char)0xcc;
        d[1] = (char)v;
        o->len += 2;
    } else if (v <= 0xffff) {
        d[0] = (char)0xcd;
        putBe(d + 1, v, 2);
        o->len += 3;
    } else if (v <= 0xffffffffu) {
        d[0] = (char)0xce;
        putBe(d + 1, v, 4);
        o->len += 5;
    } else {
        d[0] = (char)0xcf;
        putBe(d + 1, v, 8);
        o->len += 9;
    }
}

static void mpInt(IrisString *o, int64_t v) {
    if (v >= 0) {
        mpUint(o, (uint64_t)v);
        return;
    }
    char *d = binRoom(o, 9);
    if (v >= -32) {
        d[0] = (char)v;
        o->len += 1;
    } else if (v >= -128) {
        d[0] = (char)0xd0;
        d[1] = (char)v;
        o->len += 2;
    } else if (v >= -32768) {
        d[0] = (char)0xd1;
        putBe(d + 1, (uint64_t)v, 2);
        o->len += 3;
    } else if (v >= INT32_MIN) {
        d[0] = (char)0xd2;
        putBe(d + 1, (uint64_t)v, 4);
        o->len += 5;
    } else {
        d[0] = (char)0xd3;
        putBe(d + 1, (uint64_t)v, 8);
        o->len += 9;
    }
}

/* A header with a length: str, array or map (fix, 8 and 16 and 32 bits). */
static void mpHead(IrisString *o, int fix, int fixMax, int b8, int b16, uint64_t n) {
    char *d = binRoom(o, 5);
    if (n < (uint64_t)fixMax) {
        d[0] = (char)(fix | (int)n);
        o->len += 1;
    } else if (b8 && n <= 0xff) {
        d[0] = (char)b8;
        d[1] = (char)n;
        o->len += 2;
    } else if (n <= 0xffff) {
        d[0] = (char)b16;
        putBe(d + 1, n, 2);
        o->len += 3;
    } else {
        d[0] = (char)(b16 + 1);
        putBe(d + 1, n, 4);
        o->len += 5;
    }
}

/* CBOR's header: the major type and its argument in the fewest bytes. */
static void cbHead(IrisString *o, int major, uint64_t v) {
    char *d = binRoom(o, 9);
    int m = major << 5;
    if (v < 24) {
        d[0] = (char)(m | (int)v);
        o->len += 1;
    } else if (v <= 0xff) {
        d[0] = (char)(m | 24);
        d[1] = (char)v;
        o->len += 2;
    } else if (v <= 0xffff) {
        d[0] = (char)(m | 25);
        putBe(d + 1, v, 2);
        o->len += 3;
    } else if (v <= 0xffffffffu) {
        d[0] = (char)(m | 26);
        putBe(d + 1, v, 4);
        o->len += 5;
    } else {
        d[0] = (char)(m | 27);
        putBe(d + 1, v, 8);
        o->len += 9;
    }
}

/* A float in CBOR's preferred form: the shortest of half, single and
   double that keeps its value (RFC 8949, 4.1). */
static void cbFloat(IrisString *o, double v) {
    char *d = binRoom(o, 9);
    if (v != v) {
        d[0] = (char)0xf9;
        d[1] = 0x7e;
        d[2] = 0;
        o->len += 3;
        return;
    }
    float f = (float)v;
    if ((double)f == v) {
        uint32_t b;
        memcpy(&b, &f, 4);
        uint32_t sign = (b >> 16) & 0x8000, mant = b & 0x7fffff;
        int e = (int)((b >> 23) & 0xff) - 127;
        int h = -1;
        if ((b & 0x7fffffff) == 0) h = (int)sign;
        else if (e == 128) h = (int)(sign | 0x7c00);   /* infinity */
        else if (e >= -14 && e <= 15 && !(mant & 0x1fff)) h = (int)(sign | (uint32_t)(e + 15) << 10 | mant >> 13);
        else if (e >= -24 && e < -14) {
            int shift = 13 + (-14 - e);
            uint32_t full = 0x800000 | mant;
            if (!(full & ((1u << shift) - 1))) h = (int)(sign | full >> shift);
        }
        if (h >= 0) {
            d[0] = (char)0xf9;
            putBe(d + 1, (uint64_t)h, 2);
            o->len += 3;
        } else {
            d[0] = (char)0xfa;
            putBe(d + 1, b, 4);
            o->len += 5;
        }
        return;
    }
    uint64_t bits;
    memcpy(&bits, &v, 8);
    d[0] = (char)0xfb;
    putBe(d + 1, bits, 8);
    o->len += 9;
}

static void binText(IrisString *o, int fmt, const char *s, int64_t n) {
    if (fmt == BIN_MSGPACK) {
        mpHead(o, 0xa0, 32, 0xd9, 0xda, (uint64_t)n);
    } else {
        cbHead(o, 3, (uint64_t)n);
    }
    if (n > 0) {
        char *d = binRoom(o, n);
        if (n <= 16) {
            for (int64_t i = 0; i < n; i++) d[i] = s[i];
        } else {
            memcpy(d, s, (size_t)n);
        }
        o->len += n;
    }
}

static void binCount(IrisString *o, int fmt, bool map, uint64_t n) {
    if (fmt == BIN_MSGPACK) {
        if (map) mpHead(o, 0x80, 16, 0, 0xde, n);
        else mpHead(o, 0x90, 16, 0, 0xdc, n);
    } else {
        cbHead(o, map ? 5 : 4, n);
    }
}

static void binEncode(IrisString *o, int fmt, const JsonType *t, const char *p) {
    switch (t->kind) {
    case J_INT: case J_UINT: {
        int64_t v = readInt(p, t->size, t->kind == J_INT);
        bool isSigned = t->kind == J_INT || t->size < 8;
        if (fmt == BIN_MSGPACK) {
            if (isSigned) mpInt(o, v);
            else mpUint(o, (uint64_t)v);
        } else if (isSigned && v < 0) {
            cbHead(o, 1, (uint64_t)(-1 - v));
        } else {
            cbHead(o, 0, (uint64_t)v);
        }
        break;
    }
    case J_FLOAT: {
        if (fmt == BIN_CBOR) {
            cbFloat(o, t->size == 4 ? (double)*(const float *)p : *(const double *)p);
            break;
        }
        char *d = binRoom(o, 9);
        if (t->size == 4) {
            uint32_t bits;
            memcpy(&bits, p, 4);
            d[0] = (char)(fmt == BIN_MSGPACK ? 0xca : 0xfa);
            putBe(d + 1, bits, 4);
            o->len += 5;
        } else {
            uint64_t bits;
            memcpy(&bits, p, 8);
            d[0] = (char)(fmt == BIN_MSGPACK ? 0xcb : 0xfb);
            putBe(d + 1, bits, 8);
            o->len += 9;
        }
        break;
    }
    case J_BOOL: {
        bool b = *(const uint8_t *)p;
        *binRoom(o, 1) = (char)(fmt == BIN_MSGPACK ? (b ? 0xc3 : 0xc2) : (b ? 0xf5 : 0xf4));
        o->len++;
        break;
    }
    case J_STR: {
        const Slice *s = (const Slice *)p;
        binText(o, fmt, s->ptr, s->len);
        break;
    }
    case J_STRING: {
        const IrisString *s = (const IrisString *)p;
        binText(o, fmt, s->ptr, s->len);
        break;
    }
    case J_ENUM: {
        int64_t v = *(const int32_t *)p;
        const JsonName *names = t->items;
        for (int64_t i = 0; i < t->count; i++) {
            if (names[i].value == v) {
                binText(o, fmt, names[i].name, names[i].len);
                return;
            }
        }
        if (fmt == BIN_MSGPACK) mpInt(o, v);
        else if (v < 0) cbHead(o, 1, (uint64_t)(-1 - v));
        else cbHead(o, 0, (uint64_t)v);
        break;
    }
    case J_STRUCT: {
        const JsonField *fs = t->items;
        binCount(o, fmt, true, (uint64_t)t->count);
        for (int64_t i = 0; i < t->count; i++) {
            binText(o, fmt, fs[i].name, fs[i].nameLen);
            binEncode(o, fmt, fs[i].type, p + fs[i].offset);
        }
        break;
    }
    case J_MAP: {
        const JsonMapInfo *mi = t->items;
        const JsonMap *m = (const JsonMap *)p;
        binCount(o, fmt, true, (uint64_t)m->len);
        for (int64_t i = 0; i < m->state.len; i++) {
            if ((unsigned char)m->state.ptr[i] < 0x80) continue;
            const char *e = m->slots.ptr + i * mi->entrySize;
            binText(o, fmt, ((const Slice *)e)->ptr, ((const Slice *)e)->len);
            binEncode(o, fmt, mi->val, e + mi->valOffset);
        }
        break;
    }
    case J_SLICE: case J_ARRAY: case J_LIST: {
        const JsonType *et = t->items;
        const char *base = p;
        int64_t n = t->count;
        if (t->kind == J_SLICE) {
            base = ((const Slice *)p)->ptr;
            n = ((const Slice *)p)->len;
        } else if (t->kind == J_LIST) {
            base = ((const JsonList *)p)->buf.ptr;
            n = ((const JsonList *)p)->len;
        }
        binCount(o, fmt, false, (uint64_t)n);
        for (int64_t i = 0; i < n; i++) binEncode(o, fmt, et, base + i * et->size);
        break;
    }
    }
}

void iris_msgpack_encode(const JsonType *t, const char *value, IrisString *out) {
    binRoom(out, 128);
    binEncode(out, BIN_MSGPACK, t, value);
}

void iris_cbor_encode(const JsonType *t, const char *value, IrisString *out) {
    binRoom(out, 128);
    binEncode(out, BIN_CBOR, t, value);
}

/* ------------------------------------------------ reading MessagePack and CBOR

   Both are read one item at a time into a BinTok; binValue fills a typed
   value from the items, as jsonValue does from JSON text. */

enum { BT_NIL, BT_BOOL, BT_UINT, BT_INT, BT_FLOAT, BT_STR, BT_BIN, BT_ARRAY, BT_MAP, BT_BREAK };

typedef struct {
    int kind;
    bool huge;                  /* a CBOR negative integer below the smallest i64 */
    uint64_t u;                 /* BT_UINT, BT_BOOL; BT_INT keeps its i64 here */
    int64_t i;                  /* the same bits as an i64, for msgpack.Value and cbor.Value */
    double f;
    const char *s;              /* BT_STR, BT_BIN */
    int64_t n;                  /* their length, or the items of BT_ARRAY and BT_MAP; -1 until a break */
} BinTok;

static _Thread_local IrisString binChunks;  /* CBOR text and bytes given in pieces, put together */

static inline __attribute__((always_inline)) bool binNeed(JsonIn *in, int64_t n) {
    if (in->end - in->p < n) return binFail(in, 1, "the data ends in the middle of a value");
    return true;
}

static inline __attribute__((always_inline)) bool mpTok(JsonIn *in, BinTok *k) {
    if (!binNeed(in, 1)) return false;
    unsigned char b = (unsigned char)*in->p++;
    if (b < 0x80) {
        k->kind = BT_UINT;
        k->u = b;
        return true;
    }
    if (b >= 0xe0) {
        k->kind = BT_INT;
        k->u = (uint64_t)(int64_t)(int8_t)b;
        return true;
    }
    if (b < 0x90) {
        k->kind = BT_MAP;
        k->n = b & 15;
        return true;
    }
    if (b < 0xa0) {
        k->kind = BT_ARRAY;
        k->n = b & 15;
        return true;
    }
    if (b < 0xc0) {
        k->kind = BT_STR;
        k->n = b & 31;
        goto bytes;
    }
    int w;
    switch (b) {
    case 0xc0: k->kind = BT_NIL; return true;
    case 0xc2: case 0xc3: k->kind = BT_BOOL; k->u = b & 1; return true;
    case 0xc4: case 0xc5: case 0xc6:
    case 0xd9: case 0xda: case 0xdb:
        k->kind = b <= 0xc6 ? BT_BIN : BT_STR;
        w = 1 << (b <= 0xc6 ? b - 0xc4 : b - 0xd9);
        if (!binNeed(in, w)) return false;
        k->n = (int64_t)getBe(in->p, w);
        in->p += w;
        goto bytes;
    case 0xca: case 0xcb:
        w = b == 0xca ? 4 : 8;
        if (!binNeed(in, w)) return false;
        k->kind = BT_FLOAT;
        if (w == 4) {
            uint32_t bits = (uint32_t)getBe(in->p, 4);
            float f;
            memcpy(&f, &bits, 4);
            k->f = f;
        } else {
            uint64_t bits = getBe(in->p, 8);
            memcpy(&k->f, &bits, 8);
        }
        in->p += w;
        return true;
    case 0xcc: case 0xcd: case 0xce: case 0xcf:
        w = 1 << (b - 0xcc);
        if (!binNeed(in, w)) return false;
        k->kind = BT_UINT;
        k->u = getBe(in->p, w);
        in->p += w;
        return true;
    case 0xd0: case 0xd1: case 0xd2: case 0xd3: {
        w = 1 << (b - 0xd0);
        if (!binNeed(in, w)) return false;
        uint64_t v = getBe(in->p, w);
        int sh = 64 - 8 * w;
        k->u = sh ? (uint64_t)((int64_t)(v << sh) >> sh) : v;
        k->kind = (int64_t)k->u < 0 ? BT_INT : BT_UINT;
        in->p += w;
        return true;
    }
    case 0xdc: case 0xdd: case 0xde: case 0xdf:
        w = b & 1 ? 4 : 2;
        if (!binNeed(in, w)) return false;
        k->kind = b < 0xde ? BT_ARRAY : BT_MAP;
        k->n = (int64_t)getBe(in->p, w);
        in->p += w;
        return true;
    case 0xd4: case 0xd5: case 0xd6: case 0xd7: case 0xd8:
    case 0xc7: case 0xc8: case 0xc9: {
        /* an extension: its bytes, with the type byte in front */
        int64_t n;
        if (b >= 0xd4) {
            n = 1 << (b - 0xd4);
        } else {
            w = 1 << (b - 0xc7);
            if (!binNeed(in, w)) return false;
            n = (int64_t)getBe(in->p, w);
            in->p += w;
        }
        k->kind = BT_BIN;
        k->n = n + 1;
        goto bytes;
    }
    default:
        in->p--;
        return binFail(in, 1, "0xc1 isn't a MessagePack value");
    }
bytes:
    if (!binNeed(in, k->n)) return false;
    k->s = in->p;
    in->p += k->n;
    return true;
}

static bool cbArg(JsonIn *in, int ai, uint64_t *v) {
    if (ai < 24) {
        *v = (uint64_t)ai;
        return true;
    }
    if (ai > 27) return binFail(in, 1, "a CBOR item with a reserved length");
    int w = 1 << (ai - 24);
    if (!binNeed(in, w)) return false;
    *v = getBe(in->p, w);
    in->p += w;
    return true;
}

static double halfFloat(uint16_t h) {
    int e = (h >> 10) & 31, m = h & 1023;
    double v = e == 0 ? ldexp(m, -24) : e == 31 ? (m ? NAN : INFINITY) : ldexp(m + 1024, e - 25);
    return h & 0x8000 ? -v : v;
}

static bool cbTok(JsonIn *in, BinTok *k) {
    for (;;) {
        if (!binNeed(in, 1)) return false;
        unsigned char b = (unsigned char)*in->p++;
        int major = b >> 5, ai = b & 31;
        uint64_t v = 0;
        if (ai == 31) {
            switch (major) {
            case 2: case 3: {
                /* text or bytes in pieces of the same major type, until a break */
                int64_t at = in->p - in->s - 1;
                binChunks.len = 0;
                for (;;) {
                    if (!binNeed(in, 1)) return false;
                    unsigned char c = (unsigned char)*in->p;
                    if (c == 0xff) {
                        in->p++;
                        break;
                    }
                    in->p++;
                    if ((c >> 5) != major || (c & 31) == 31 || !cbArg(in, c & 31, &v)) {
                        return in->code ? false : binFail(in, 1, "a piece of CBOR text or bytes of another type");
                    }
                    if (!binNeed(in, (int64_t)v)) return false;
                    iris_string_append(&binChunks, in->p, (int64_t)v);
                    in->p += v;
                }
                (void)at;
                k->kind = major == 2 ? BT_BIN : BT_STR;
                k->s = binChunks.ptr ? binChunks.ptr : "";
                k->n = binChunks.len;
                return true;
            }
            case 4: case 5:
                k->kind = major == 4 ? BT_ARRAY : BT_MAP;
                k->n = -1;
                return true;
            case 7:
                k->kind = BT_BREAK;
                return true;
            default:
                in->p--;
                return binFail(in, 1, "an integer or tag with no length");
            }
        }
        if (major != 7 && !cbArg(in, ai, &v)) return false;
        switch (major) {
        case 0:
            k->kind = BT_UINT;
            k->u = v;
            return true;
        case 1:
            k->kind = BT_INT;
            k->huge = v > (uint64_t)INT64_MAX;
            k->u = (uint64_t)(-1 - (int64_t)v);
            return true;
        case 2: case 3:
            if (!binNeed(in, (int64_t)v)) return false;
            k->kind = major == 2 ? BT_BIN : BT_STR;
            k->s = in->p;
            k->n = (int64_t)v;
            in->p += v;
            return true;
        case 4: case 5:
            k->kind = major == 4 ? BT_ARRAY : BT_MAP;
            if (v > (uint64_t)(in->end - in->p)) return binFail(in, 1, "more items than there are bytes left");
            k->n = (int64_t)v;
            return true;
        case 6:
            continue;           /* a tag: the value it marks is read as it is */
        default:
            switch (ai) {
            case 20: case 21: k->kind = BT_BOOL; k->u = ai == 21; return true;
            case 22: case 23: k->kind = BT_NIL; return true;
            case 25: case 26: case 27: {
                int w = 1 << (ai - 24);
                if (!binNeed(in, w)) return false;
                uint64_t bits = getBe(in->p, w);
                in->p += w;
                k->kind = BT_FLOAT;
                if (w == 2) {
                    k->f = halfFloat((uint16_t)bits);
                } else if (w == 4) {
                    uint32_t b32 = (uint32_t)bits;
                    float f;
                    memcpy(&f, &b32, 4);
                    k->f = f;
                } else {
                    memcpy(&k->f, &bits, 8);
                }
                return true;
            }
            default:
                if (ai < 24 || ai == 24) {
                    /* a simple value without a meaning here */
                    if (ai == 24 && !binNeed(in, 1)) return false;
                    if (ai == 24) in->p++;
                    k->kind = BT_NIL;
                    return true;
                }
                in->p--;
                return binFail(in, 1, "a CBOR simple value with a reserved length");
            }
        }
    }
}

static inline __attribute__((always_inline)) bool binTok(JsonIn *in, int fmt, BinTok *k) {
    k->huge = false;
    if (fmt == BIN_MSGPACK) return mpTok(in, k);
    /* CBOR's short items in line: small integers, short text, arrays and maps */
    if (in->p < in->end) {
        unsigned char b = (unsigned char)*in->p;
        int major = b >> 5, ai = b & 31;
        if (ai < 24 && major != 6 && major != 7) {
            in->p++;
            switch (major) {
            case 0: k->kind = BT_UINT; k->u = (uint64_t)ai; return true;
            case 1: k->kind = BT_INT; k->u = (uint64_t)(-1 - ai); return true;
            case 4: case 5:
                if (in->end - in->p < ai) {
                    in->p--;
                    break;
                }
                k->kind = major == 4 ? BT_ARRAY : BT_MAP;
                k->n = ai;
                return true;
            default:
                if (in->end - in->p < ai) {
                    in->p--;
                    break;
                }
                k->kind = major == 2 ? BT_BIN : BT_STR;
                k->s = in->p;
                k->n = ai;
                in->p += ai;
                return true;
            }
        }
    }
    return cbTok(in, k);
}

/* Whether a key's n bytes at a are those at b, without a call for short ones. */
static inline bool binSame(const char *a, const char *b, int64_t n) {
    if (n > 16) return !memcmp(a, b, (size_t)n);
    for (int64_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

/* Whether the items of an array or map go on: n of them, or until a break. */
static bool binMore(JsonIn *in, int fmt, int64_t n, int64_t i, bool *ok) {
    if (n >= 0) return i < n;
    if (fmt == BIN_CBOR && in->p < in->end && (unsigned char)*in->p == 0xff) {
        in->p++;
        return false;
    }
    if (in->p >= in->end) *ok = binFail(in, 1, "the data ends before the break of an array or map");
    return in->p < in->end;
}

static bool binSkipTok(JsonIn *in, int fmt, BinTok *k) {
    if (k->kind != BT_ARRAY && k->kind != BT_MAP) {
        if (k->kind == BT_BREAK) return binFail(in, 1, "a break outside an array or map");
        return true;
    }
    if (++in->depth > 512) return binFail(in, 1, "the data is nested too deeply");
    int64_t n = k->n, per = k->kind == BT_MAP ? 2 : 1;
    bool ok = true;
    for (int64_t i = 0; ok && binMore(in, fmt, n, i, &ok); i++) {
        for (int64_t j = 0; j < per; j++) {
            BinTok e;
            if (!binTok(in, fmt, &e) || !binSkipTok(in, fmt, &e)) return false;
        }
    }
    in->depth--;
    return ok;
}

static const char *binKindName(int kind) {
    switch (kind) {
    case BT_NIL: return "nil";
    case BT_BOOL: return "a bool";
    case BT_UINT: case BT_INT: return "an integer";
    case BT_FLOAT: return "a float";
    case BT_STR: return "text";
    case BT_BIN: return "bytes";
    case BT_ARRAY: return "an array";
    case BT_MAP: return "a map";
    default: return "a break";
    }
}

static bool binExpected(JsonIn *in, const char *what, const BinTok *k) {
    char msg[96];
    snprintf(msg, sizeof msg, "expected %s, found %s", what, binKindName(k->kind));
    return binFail(in, 2, msg);
}

static bool binValue(JsonIn *in, int fmt, const JsonType *t, char *p, BinTok *k);

static bool binList(JsonIn *in, int fmt, const JsonType *t, char *p, BinTok *k) {
    const JsonType *et = t->items;
    bool slice = t->kind == J_SLICE || t->kind == J_LIST;
    if (slice) jsonFree(t, p);
    int64_t n = 0, cap = slice ? (k->n > 0 ? (k->n < 4096 ? k->n : 4096) : 0) : t->count;
    char *buf = slice ? (cap ? calloc((size_t)cap, (size_t)et->size) : NULL) : p;
    if (slice && cap && !buf) outOfMemory();
    if (++in->depth > 512) return binFail(in, 1, "the data is nested too deeply");
    bool ok = true;
    for (int64_t i = 0; ok && binMore(in, fmt, k->n, i, &ok); i++) {
        if (n == cap) {
            if (!slice) {
                char msg[96];
                snprintf(msg, sizeof msg, "more than %lld items", (long long)t->count);
                ok = binFail(in, 2, msg);
                break;
            }
            int64_t ncap = cap ? cap * 2 : 8;
            char *nb = realloc(buf, (size_t)(ncap * et->size));
            if (!nb) outOfMemory();
            memset(nb + cap * et->size, 0, (size_t)((ncap - cap) * et->size));
            buf = nb;
            cap = ncap;
        }
        size_t saved = in->pathLen;
        pathIndex(in, n);
        if (slice && et->init) memcpy(buf + n * et->size, et->init, (size_t)et->size);
        BinTok e;
        ok = binTok(in, fmt, &e) && binValue(in, fmt, et, buf + n * et->size, &e);
        in->pathLen = saved;
        in->path[saved] = 0;
        n++;
    }
    in->depth--;
    if (t->kind == J_LIST) {
        JsonList *l = (JsonList *)p;
        l->buf.ptr = buf;
        l->buf.len = cap;
        l->len = n;
    } else if (slice) {
        Slice *s = (Slice *)p;
        s->ptr = buf;
        s->len = n;
        if (!n) {
            free(buf);
            s->ptr = NULL;
        }
    }
    return ok;
}

static bool binStruct(JsonIn *in, int fmt, const JsonType *t, char *p, BinTok *k) {
    const JsonField *fs = t->items;
    uint64_t seenBits = 0;
    unsigned char *seen = t->count <= 64 ? NULL : calloc((size_t)t->count, 1);
    if (t->count > 64 && !seen) outOfMemory();
    if (++in->depth > 512) return binFail(in, 1, "the data is nested too deeply");
    bool ok = true;
    int64_t next = 0;
    for (int64_t i = 0; ok && binMore(in, fmt, k->n, i, &ok); i++) {
        BinTok key, v;
        if (!binTok(in, fmt, &key)) {
            ok = false;
            break;
        }
        if (key.kind != BT_STR) {
            /* a key that isn't text names no field */
            ok = binSkipTok(in, fmt, &key) && binTok(in, fmt, &v) && binSkipTok(in, fmt, &v);
            continue;
        }
        int64_t f = -1;
        for (int64_t j = 0; j < t->count; j++) {
            int64_t x = next + j < t->count ? next + j : next + j - t->count;
            if (fs[x].nameLen == key.n && binSame(fs[x].name, key.s, key.n)) {
                f = x;
                next = x + 1;
                break;
            }
        }
        if (!binTok(in, fmt, &v)) {
            ok = false;
            break;
        }
        if (f < 0) {
            ok = binSkipTok(in, fmt, &v);
            continue;
        }
        size_t saved = in->pathLen;
        pathField(in, fs[f].name, fs[f].nameLen);
        ok = binValue(in, fmt, fs[f].type, p + fs[f].offset, &v);
        in->pathLen = saved;
        in->path[saved] = 0;
        if (seen) seen[f] = 1;
        else seenBits |= 1ull << f;
    }
    in->depth--;
    for (int64_t i = 0; ok && i < t->count; i++) {
        if ((fs[i].required & 1) && !(seen ? seen[i] : (seenBits >> i) & 1)) {
            char msg[160];
            snprintf(msg, sizeof msg, "the required field \"%.*s\" is missing", (int)fs[i].nameLen, fs[i].name);
            ok = binFail(in, 3, msg);
        }
    }
    free(seen);
    return ok;
}

static bool binMap(JsonIn *in, int fmt, const JsonType *t, char *p, BinTok *k) {
    const JsonMapInfo *mi = t->items;
    JsonMap *m = (JsonMap *)p;
    jsonMapFree(t, p);
    if (++in->depth > 512) return binFail(in, 1, "the data is nested too deeply");
    bool ok = true;
    for (int64_t i = 0; ok && binMore(in, fmt, k->n, i, &ok); i++) {
        BinTok key, v;
        if (!binTok(in, fmt, &key)) {
            ok = false;
            break;
        }
        if (key.kind != BT_STR) {
            ok = binExpected(in, "a text key", &key);
            break;
        }
        if ((m->len + m->removed + 1) * 4 > m->state.len * 3) jsonMapGrow(m, mi);
        bool found;
        unsigned char tag;
        int64_t at = jsonMapSlot(m, mi, key.s, key.n, &found, &tag);
        char *e = m->slots.ptr + at * mi->entrySize;
        char *val = e + mi->valOffset;
        if (found) {
            jsonFree(mi->val, val);
            memset(val, 0, (size_t)mi->val->size);
        } else {
            if (m->state.ptr[at] == 1) m->removed--;
            Slice *ks = (Slice *)e;
            ks->ptr = malloc(key.n ? (size_t)key.n : 1);
            if (!ks->ptr) outOfMemory();
            memcpy(ks->ptr, key.s, (size_t)key.n);
            ks->len = key.n;
            if (mi->keySize == 24) ((IrisString *)e)->cap = key.n;
            m->state.ptr[at] = (char)tag;
            m->len++;
        }
        if (mi->val->init) memcpy(val, mi->val->init, (size_t)mi->val->size);
        size_t saved = in->pathLen;
        pathField(in, key.s, key.n);
        ok = binTok(in, fmt, &v) && binValue(in, fmt, mi->val, val, &v);
        in->pathLen = saved;
        in->path[saved] = 0;
    }
    in->depth--;
    return ok;
}

static bool binValue(JsonIn *in, int fmt, const JsonType *t, char *p, BinTok *k) {
    if (k->kind == BT_NIL) return true;   /* nil leaves the value as it is */
    if (k->kind == BT_BREAK) return binFail(in, 1, "a break outside an array or map");
    switch (t->kind) {
    case J_INT: case J_UINT: {
        if (k->kind != BT_UINT && k->kind != BT_INT) return binExpected(in, "an integer", k);
        int bits = t->size * 8;
        char msg[128];
        if (k->kind == BT_INT) {
            int64_t v = (int64_t)k->u;
            if (k->huge || t->kind == J_UINT || (bits < 64 && v < -(1LL << (bits - 1)))) {
                if (k->huge) snprintf(msg, sizeof msg, "a negative integer doesn't fit in i%d", bits);
                else snprintf(msg, sizeof msg, "%lld doesn't fit in %c%d", (long long)v, t->kind == J_UINT ? 'u' : 'i', bits);
                return binFail(in, 4, msg);
            }
            writeInt(p, t->size, v);
        } else {
            uint64_t v = k->u;
            uint64_t max = t->kind == J_INT ? (bits == 64 ? (uint64_t)INT64_MAX : (1ULL << (bits - 1)) - 1)
                                            : (bits == 64 ? UINT64_MAX : (1ULL << bits) - 1);
            if (v > max) {
                snprintf(msg, sizeof msg, "%llu doesn't fit in %c%d", (unsigned long long)v, t->kind == J_UINT ? 'u' : 'i', bits);
                return binFail(in, 4, msg);
            }
            writeInt(p, t->size, (int64_t)v);
        }
        return true;
    }
    case J_FLOAT: {
        double v = k->kind == BT_FLOAT ? k->f : k->kind == BT_UINT ? (double)k->u : k->kind == BT_INT ? (double)(int64_t)k->u : 0;
        if (k->kind != BT_FLOAT && k->kind != BT_UINT && k->kind != BT_INT) return binExpected(in, "a number", k);
        if (t->size == 4) *(float *)p = (float)v;
        else *(double *)p = v;
        return true;
    }
    case J_BOOL:
        if (k->kind != BT_BOOL) return binExpected(in, "a bool", k);
        *(uint8_t *)p = (uint8_t)k->u;
        return true;
    case J_STRING: {
        if (k->kind != BT_STR && k->kind != BT_BIN) return binExpected(in, "text", k);
        IrisString *s = (IrisString *)p;
        iris_string_free(s);
        s->ptr = malloc(k->n ? (size_t)k->n : 1);
        if (!s->ptr) outOfMemory();
        memcpy(s->ptr, k->s, (size_t)k->n);
        s->len = s->cap = k->n;
        if (!s->cap) s->cap = 1;
        return true;
    }
    case J_ENUM: {
        const JsonName *names = t->items;
        if (k->kind == BT_UINT || k->kind == BT_INT) {
            for (int64_t i = 0; i < t->count; i++) {
                if (names[i].value == (int64_t)k->u) {
                    *(int32_t *)p = (int32_t)names[i].value;
                    return true;
                }
            }
            char msg[96];
            snprintf(msg, sizeof msg, "%lld isn't one of the enum's values", (long long)(int64_t)k->u);
            return binFail(in, 2, msg);
        }
        if (k->kind != BT_STR) return binExpected(in, "text", k);
        for (int64_t i = 0; i < t->count; i++) {
            if (names[i].len == k->n && !memcmp(names[i].name, k->s, (size_t)k->n)) {
                *(int32_t *)p = (int32_t)names[i].value;
                return true;
            }
        }
        char msg[160];
        snprintf(msg, sizeof msg, "\"%.*s\" isn't one of the enum's values", (int)(k->n > 60 ? 60 : k->n), k->s);
        return binFail(in, 2, msg);
    }
    case J_STRUCT:
        if (k->kind != BT_MAP) return binExpected(in, "a map", k);
        return binStruct(in, fmt, t, p, k);
    case J_MAP:
        if (k->kind != BT_MAP) return binExpected(in, "a map", k);
        return binMap(in, fmt, t, p, k);
    case J_SLICE: case J_ARRAY: case J_LIST:
        if (k->kind != BT_ARRAY) return binExpected(in, "an array", k);
        return binList(in, fmt, t, p, k);
    default:
        return binFail(in, 2, "this kind of value can't be read");
    }
}

static int binDecode(int fmt, const JsonType *t, const char *data, int64_t len, char *out) {
    JsonIn in = {.s = data, .p = data, .end = data + len};
    BinTok k;
    bool ok = binTok(&in, fmt, &k) && binValue(&in, fmt, t, out, &k);
    if (ok && in.p < in.end) ok = binFail(&in, 1, "more bytes after the value");
    if (!ok) {
        jsonFree(t, out);
        return in.code ? in.code : 1;
    }
    return 0;
}

int iris_msgpack_decode(const JsonType *t, const char *data, int64_t len, char *out) {
    return binDecode(BIN_MSGPACK, t, data, len, out);
}

int iris_cbor_decode(const JsonType *t, const char *data, int64_t len, char *out) {
    return binDecode(BIN_CBOR, t, data, len, out);
}

/* For msgpack.Value and cbor.Value: one item read at *at, 0 or the error's
   code; and one item written, an array's or map's header without its items. */
int iris_bin_token(int fmt, const char *data, int64_t len, int64_t *at, BinTok *k) {
    JsonIn in = {.s = data, .p = data + *at, .end = data + len};
    if (!binTok(&in, fmt, k)) return in.code ? in.code : 1;
    k->i = (int64_t)k->u;
    *at = in.p - data;
    return 0;
}

void iris_bin_put(int fmt, IrisString *o, const BinTok *k) {
    switch (k->kind) {
    case BT_NIL:
        *binRoom(o, 1) = (char)(fmt == BIN_MSGPACK ? 0xc0 : 0xf6);
        o->len++;
        break;
    case BT_BOOL:
        *binRoom(o, 1) = (char)(fmt == BIN_MSGPACK ? (k->u ? 0xc3 : 0xc2) : (k->u ? 0xf5 : 0xf4));
        o->len++;
        break;
    case BT_UINT:
        if (fmt == BIN_MSGPACK) mpUint(o, k->u);
        else cbHead(o, 0, k->u);
        break;
    case BT_INT:
        if (fmt == BIN_MSGPACK) mpInt(o, k->i);
        else if (k->i < 0) cbHead(o, 1, (uint64_t)(-1 - k->i));
        else cbHead(o, 0, (uint64_t)k->i);
        break;
    case BT_FLOAT: {
        if (fmt == BIN_CBOR) {
            cbFloat(o, k->f);
            break;
        }
        char *d = binRoom(o, 9);
        uint64_t bits;
        memcpy(&bits, &k->f, 8);
        d[0] = (char)(fmt == BIN_MSGPACK ? 0xcb : 0xfb);
        putBe(d + 1, bits, 8);
        o->len += 9;
        break;
    }
    case BT_STR:
        binText(o, fmt, k->s, k->n);
        break;
    case BT_BIN:
        if (fmt == BIN_MSGPACK) {
            char *d = binRoom(o, 5);
            if (k->n <= 0xff) {
                d[0] = (char)0xc4;
                d[1] = (char)k->n;
                o->len += 2;
            } else if (k->n <= 0xffff) {
                d[0] = (char)0xc5;
                putBe(d + 1, (uint64_t)k->n, 2);
                o->len += 3;
            } else {
                d[0] = (char)0xc6;
                putBe(d + 1, (uint64_t)k->n, 4);
                o->len += 5;
            }
        } else {
            cbHead(o, 2, (uint64_t)k->n);
        }
        if (k->n > 0) {
            memcpy(binRoom(o, k->n), k->s, (size_t)k->n);
            o->len += k->n;
        }
        break;
    case BT_ARRAY: case BT_MAP:
        binCount(o, fmt, k->kind == BT_MAP, (uint64_t)k->n);
        break;
    }
}

/* ------------------------------------------------ protobuf

   A @proto struct is a message. Fields at their zero value are left out
   (proto3), except items of a list and a map's entries. A list of numbers
   is packed; a list of text or messages is a field per item. A nested
   message is written behind a one-byte length, moved along when it turns
   out longer. Reading takes packed and unpacked lists and skips fields the
   struct doesn't have.

   Each struct's table is turned once into a plan (PbPlan): one step per
   field with its tag already encoded, and the steps by field number for
   reading. Plans are kept for the life of the program. */

enum { PB_ZIGZAG = 1, PB_FIXED = 2 };

/* What a step does with one value. */
enum { PO_INT, PO_UINT, PO_ZIGZAG, PO_FIXED, PO_BOOL, PO_STRING, PO_MSG };
/* and with the field: one value, a list of them, or a map to them */
enum { PF_ONE, PF_LIST, PF_SLICE, PF_MAP };

typedef struct PbPlan PbPlan;

typedef struct {
    uint8_t op, shape, size, wt;        /* size: of one value, in bytes */
    uint8_t tagLen, packedTagLen;
    uint8_t tag[5], packedTag[5];       /* the tag with wt, and with wire type 2 */
    int32_t num;
    int64_t offset, esize;              /* esize: of an item, or of a map's entry */
    const JsonType *type;               /* the field's */
    const JsonType *elem;               /* the item's or the map value's */
    const PbPlan *sub;                  /* the plan of a message value */
    const JsonMapInfo *map;
} PbStep;

struct PbPlan {
    const JsonType *t;
    _Atomic int64_t sizeHint;           /* the room the last encode needed, given at once to the next */
    int n;
    PbStep *steps;
    int8_t byNum[64];                   /* the step of numbers below 64, -1 for none */
};

static int pbVarintPut(uint8_t *d, uint64_t v) {
    int n = 0;
    while (v >= 0x80) {
        d[n++] = (uint8_t)(v | 0x80);
        v >>= 7;
    }
    d[n++] = (uint8_t)v;
    return n;
}

static inline int pbVarintLen(uint64_t v) {
    return 1 + (63 - __builtin_clzll(v | 1)) / 7;
}

/* The plans made so far, found by their table. */
#define PB_PLANS 4096
static const JsonType *_Atomic pbKeys[PB_PLANS];
static PbPlan *_Atomic pbVals[PB_PLANS];
static pthread_mutex_t pbLock = PTHREAD_MUTEX_INITIALIZER;

static inline size_t pbSlot(const JsonType *t) {
    return (size_t)(((uintptr_t)t >> 3) * 0x9E3779B97F4A7C15ull >> 52) & (PB_PLANS - 1);
}

static PbPlan *pbFind(const JsonType *t) {
    for (size_t i = pbSlot(t), k = 0; k < PB_PLANS; i = (i + 1) & (PB_PLANS - 1), k++) {
        const JsonType *key = atomic_load_explicit(&pbKeys[i], memory_order_acquire);
        if (key == t) return atomic_load_explicit(&pbVals[i], memory_order_acquire);
        if (!key) return NULL;
    }
    return NULL;
}

typedef struct {
    const JsonType **types;
    PbPlan **plans;
    int n, cap;
} PbBuild;

static PbPlan *pbBuild(PbBuild *b, const JsonType *t);

static void pbStepValue(PbBuild *b, PbStep *s, const JsonType *t, int how) {
    s->size = (uint8_t)(t->kind == J_ENUM ? 4 : t->kind == J_BOOL ? 1 : t->size);
    switch (t->kind) {
    case J_INT:
        s->op = how == PB_ZIGZAG ? PO_ZIGZAG : how == PB_FIXED ? PO_FIXED : PO_INT;
        break;
    case J_ENUM:
        s->op = PO_INT;
        break;
    case J_UINT:
        s->op = how == PB_FIXED ? PO_FIXED : PO_UINT;
        break;
    case J_FLOAT:
        s->op = PO_FIXED;
        break;
    case J_BOOL:
        s->op = PO_BOOL;
        break;
    case J_STRING: case J_STR:
        s->op = PO_STRING;
        break;
    default:
        s->op = PO_MSG;
        s->sub = pbBuild(b, t);
        break;
    }
    s->wt = s->op == PO_STRING || s->op == PO_MSG ? 2 : s->op == PO_FIXED ? (s->size == 4 ? 5 : 1) : 0;
}

static PbPlan *pbBuild(PbBuild *b, const JsonType *t) {
    PbPlan *found = pbFind(t);
    if (found) return found;
    for (int i = 0; i < b->n; i++) {
        if (b->types[i] == t) return b->plans[i];
    }
    PbPlan *pl = calloc(1, sizeof *pl);
    if (!pl) outOfMemory();
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 8;
        b->types = realloc(b->types, sizeof *b->types * (size_t)b->cap);
        b->plans = realloc(b->plans, sizeof *b->plans * (size_t)b->cap);
        if (!b->types || !b->plans) outOfMemory();
    }
    b->types[b->n] = t;
    b->plans[b->n++] = pl;
    pl->t = t;
    pl->n = (int)t->count;
    pl->steps = calloc((size_t)(t->count ? t->count : 1), sizeof *pl->steps);
    if (!pl->steps) outOfMemory();
    memset(pl->byNum, -1, sizeof pl->byNum);
    const JsonField *fs = t->items;
    for (int i = 0; i < pl->n; i++) {
        PbStep *s = &pl->steps[i];
        const JsonType *ft = fs[i].type;
        int64_t info = fs[i].required >> 8;
        int how = (int)(info & 3);
        s->num = (int32_t)(info >> 8);
        s->offset = fs[i].offset;
        s->type = ft;
        if (s->num < 64 && i < 127) pl->byNum[s->num] = (int8_t)i;
        if (ft->kind == J_SLICE || ft->kind == J_LIST) {
            s->shape = ft->kind == J_SLICE ? PF_SLICE : PF_LIST;
            s->elem = ft->items;
            s->esize = s->elem->size;
            pbStepValue(b, s, s->elem, how);
        } else if (ft->kind == J_MAP) {
            s->shape = PF_MAP;
            s->map = ft->items;
            s->elem = s->map->val;
            s->esize = s->map->entrySize;
            pbStepValue(b, s, s->elem, how);
        } else {
            s->shape = PF_ONE;
            pbStepValue(b, s, ft, how);
        }
        bool packed = s->shape != PF_ONE && s->shape != PF_MAP && s->op != PO_STRING && s->op != PO_MSG;
        int wt = s->shape == PF_MAP || packed ? 2 : s->wt;
        s->tagLen = (uint8_t)pbVarintPut(s->tag, (uint64_t)s->num << 3 | (uint64_t)wt);
        s->packedTagLen = (uint8_t)pbVarintPut(s->packedTag, (uint64_t)s->num << 3 | 2);
    }
    return pl;
}

static const PbPlan *pbPlan(const JsonType *t) {
    PbPlan *pl = pbFind(t);
    if (__builtin_expect(pl != NULL, 1)) return pl;
    pthread_mutex_lock(&pbLock);
    PbBuild b = {0};
    pl = pbBuild(&b, t);
    /* every plan is whole before another thread can see one */
    for (int i = 0; i < b.n; i++) {
        size_t at = pbSlot(b.types[i]);
        for (size_t k = 0; k < PB_PLANS; k++, at = (at + 1) & (PB_PLANS - 1)) {
            if (!atomic_load_explicit(&pbKeys[at], memory_order_relaxed)) break;
        }
        atomic_store_explicit(&pbVals[at], b.plans[i], memory_order_release);
        atomic_store_explicit(&pbKeys[at], b.types[i], memory_order_release);
    }
    free(b.types);
    free(b.plans);
    pthread_mutex_unlock(&pbLock);
    return pl;
}

/* ---------------- writing */

static void pbWrite(IrisString *o, const PbPlan *pl, const char *p);

static inline uint64_t pbLoad(const PbStep *s, const char *p) {
    switch (s->size) {
    case 1: return s->op == PO_INT ? (uint64_t)(int64_t)*(const int8_t *)p : *(const uint8_t *)p;
    case 2: return s->op == PO_INT ? (uint64_t)(int64_t)*(const int16_t *)p : *(const uint16_t *)p;
    case 4: {
        uint32_t v;
        memcpy(&v, p, 4);
        if (s->op == PO_INT) return (uint64_t)(int64_t)(int32_t)v;
        if (s->op == PO_ZIGZAG) return (uint32_t)((v << 1) ^ (uint32_t)((int32_t)v >> 31));
        return v;
    }
    default: {
        uint64_t v;
        memcpy(&v, p, 8);
        if (s->op == PO_ZIGZAG) return (v << 1) ^ (uint64_t)((int64_t)v >> 63);
        return v;
    }
    }
}

/* One number at d, as the step's wire type says; the bytes written. */
static inline int pbPutNumber(uint8_t *d, const PbStep *s, uint64_t v) {
    if (s->wt == 0) return pbVarintPut(d, v);
    int w = s->wt == 5 ? 4 : 8;
    for (int i = 0; i < w; i++) d[i] = (uint8_t)(v >> (8 * i));
    return w;
}

/* Fills in the length of what was written after at, where one byte was
   kept for it. */
static void pbEndLength(IrisString *o, int64_t at) {
    int64_t n = o->len - at - 1;
    if (__builtin_expect(n < 128, 1)) {
        o->ptr[at] = (char)n;
        return;
    }
    int w = pbVarintLen((uint64_t)n);
    binRoom(o, w - 1);
    memmove(o->ptr + at + w, o->ptr + at + 1, (size_t)n);
    o->len += w - 1;
    pbVarintPut((uint8_t *)o->ptr + at, (uint64_t)n);
}

static inline void pbPutText(IrisString *o, const uint8_t *tag, int tagLen, const Slice *sv) {
    uint8_t *d = (uint8_t *)binRoom(o, 16 + sv->len);
    memcpy(d, tag, 5);
    int n = tagLen;
    n += pbVarintPut(d + n, (uint64_t)sv->len);
    memcpy(d + n, sv->ptr, (size_t)sv->len);
    o->len += n + sv->len;
}

static inline void pbPutMessage(IrisString *o, const uint8_t *tag, int tagLen, const PbPlan *sub, const char *p,
                                bool keepEmpty) {
    int64_t start = o->len;
    uint8_t *d = (uint8_t *)binRoom(o, 8);
    memcpy(d, tag, 5);
    o->len += tagLen + 1;
    int64_t at = o->len - 1;
    pbWrite(o, sub, p);
    if (!keepEmpty && o->len == at + 1) {
        o->len = start;         /* a message with nothing set is left out */
        return;
    }
    pbEndLength(o, at);
}

static void pbWrite(IrisString *o, const PbPlan *pl, const char *base) {
    const PbStep *s = pl->steps, *end = s + pl->n;
    for (; s < end; s++) {
        const char *p = base + s->offset;
        if (s->shape == PF_ONE) {
            switch (s->op) {
            case PO_STRING: {
                const Slice *sv = (const Slice *)p;
                if (sv->len) pbPutText(o, s->tag, s->tagLen, sv);
                break;
            }
            case PO_MSG:
                pbPutMessage(o, s->tag, s->tagLen, s->sub, p, false);
                break;
            default: {
                uint64_t v = pbLoad(s, p);
                if (!v) break;
                uint8_t *d = (uint8_t *)binRoom(o, 16);
                memcpy(d, s->tag, 5);
                o->len += s->tagLen + pbPutNumber(d + s->tagLen, s, v);
            }
            }
            continue;
        }
        if (s->shape == PF_MAP) {
            const JsonMap *m = (const JsonMap *)p;
            if (!m->len) continue;
            uint8_t vtag[5] = {0};
            int vtagLen = pbVarintPut(vtag, (uint64_t)(2 << 3 | s->wt));
            static const uint8_t ktag[5] = {1 << 3 | 2};
            for (int64_t i = 0; i < m->state.len; i++) {
                if ((unsigned char)m->state.ptr[i] < 0x80) continue;
                const char *e = m->slots.ptr + i * s->esize;
                uint8_t *d = (uint8_t *)binRoom(o, 8);
                memcpy(d, s->tag, 5);
                o->len += s->tagLen + 1;
                int64_t at = o->len - 1;
                pbPutText(o, ktag, 1, (const Slice *)e);
                const char *v = e + s->map->valOffset;
                if (s->op == PO_STRING) {
                    pbPutText(o, vtag, vtagLen, (const Slice *)v);
                } else if (s->op == PO_MSG) {
                    pbPutMessage(o, vtag, vtagLen, s->sub, v, true);
                } else {
                    d = (uint8_t *)binRoom(o, 16);
                    memcpy(d, vtag, 5);
                    o->len += vtagLen + pbPutNumber(d + vtagLen, s, pbLoad(s, v));
                }
                pbEndLength(o, at);
            }
            continue;
        }
        const char *items;
        int64_t n;
        if (s->shape == PF_SLICE) {
            items = ((const Slice *)p)->ptr;
            n = ((const Slice *)p)->len;
        } else {
            items = ((const JsonList *)p)->buf.ptr;
            n = ((const JsonList *)p)->len;
        }
        if (!n) continue;
        if (s->op == PO_STRING) {
            for (int64_t i = 0; i < n; i++) pbPutText(o, s->tag, s->tagLen, (const Slice *)(items + i * s->esize));
            continue;
        }
        if (s->op == PO_MSG) {
            for (int64_t i = 0; i < n; i++) pbPutMessage(o, s->tag, s->tagLen, s->sub, items + i * s->esize, true);
            continue;
        }
        /* packed: the tag, the length, the numbers */
        int64_t most = n * (s->wt == 0 ? 10 : s->wt == 5 ? 4 : 8);
        uint8_t *d = (uint8_t *)binRoom(o, 16 + most);
        memcpy(d, s->tag, 5);
        int64_t at = s->tagLen, lenAt = at;
        int64_t body = s->wt == 5 ? n * 4 : s->wt == 1 ? n * 8 : -1;
        if (body >= 0) {
            at += pbVarintPut(d + at, (uint64_t)body);
            for (int64_t i = 0; i < n; i++) at += pbPutNumber(d + at, s, pbLoad(s, items + i * s->esize));
        } else {
            at += 1;            /* a byte for the length, moved along when longer */
            int64_t first = at;
            for (int64_t i = 0; i < n; i++) at += pbVarintPut(d + at, pbLoad(s, items + i * s->esize));
            int64_t len = at - first;
            int w = pbVarintLen((uint64_t)len);
            if (w > 1) {
                memmove(d + first + w - 1, d + first, (size_t)len);
                at += w - 1;
            }
            pbVarintPut(d + lenAt, (uint64_t)len);
        }
        o->len += at;
    }
}

void iris_proto_encode(const JsonType *t, const char *value, IrisString *out) {
    const PbPlan *pl = pbPlan(t);
    int64_t hint = atomic_load_explicit(&((PbPlan *)pl)->sizeHint, memory_order_relaxed);
    binRoom(out, hint > 64 ? (hint < 4096 ? hint : 4096) : 64);
    pbWrite(out, pl, value);
    if (out->len > hint) atomic_store_explicit(&((PbPlan *)pl)->sizeHint, out->len + out->len / 4, memory_order_relaxed);
}

/* ---------------- reading */

typedef struct {
    JsonIn in;
    int nest;
} PbIn;

/* The message of an error; the path of fields to it is put in front as
   the reader comes back out of them (pbPathOut). */
static bool pbFail(PbIn *r, const uint8_t *at, int code, const char *what) {
    if (r->in.code) return false;
    r->in.code = code;
    jsonMsgLen = snprintf(jsonMsg, sizeof jsonMsg, "%s", what);
    if (jsonMsgLen >= (int64_t)sizeof jsonMsg) jsonMsgLen = sizeof jsonMsg - 1;
    r->in.pathLen = 0;
    r->in.p = (const char *)at;
    return false;
}

static void pbPathOut(PbIn *r, const char *name, int64_t n) {
    char *path = r->in.path;
    size_t have = r->in.pathLen, add = (size_t)n + (have ? 1 : 0);
    if (have + add >= sizeof r->in.path) return;
    memmove(path + add, path, have + 1);
    memcpy(path, name, (size_t)n);
    if (have) path[n] = '.';
    r->in.pathLen = have + add;
}

/* A varint at *pp, which moves past it; NULL when it is cut short or too long. */
static inline const uint8_t *pbGetVarint(const uint8_t *p, const uint8_t *end, uint64_t *v) {
    if (__builtin_expect(p < end && *p < 0x80, 1)) {
        *v = *p;
        return p + 1;
    }
    uint64_t x = 0;
    for (int sh = 0; sh < 70; sh += 7) {
        if (p >= end) return NULL;
        uint8_t b = *p++;
        x |= (uint64_t)(b & 0x7f) << sh;
        if (b < 0x80) {
            *v = x;
            return p;
        }
    }
    return NULL;
}

static const uint8_t *pbBadVarint(PbIn *r, const uint8_t *p, const uint8_t *end) {
    const uint8_t *q = p;
    while (q < end && *q >= 0x80 && q - p < 10) q++;
    pbFail(r, p, 1, q >= end ? "the data ends in the middle of a varint" : "a varint longer than 10 bytes");
    return NULL;
}

static const uint8_t *pbSkip(PbIn *r, const uint8_t *p, const uint8_t *end, int wt) {
    uint64_t n;
    const uint8_t *q;
    switch (wt) {
    case 0:
        q = pbGetVarint(p, end, &n);
        return q ? q : pbBadVarint(r, p, end);
    case 1: n = 8; break;
    case 5: n = 4; break;
    case 2:
        q = pbGetVarint(p, end, &n);
        if (!q) return pbBadVarint(r, p, end);
        p = q;
        break;
    default:
        pbFail(r, p, 1, "a field of wire type 3 or 4 (groups), which this reader doesn't take");
        return NULL;
    }
    if (n > (uint64_t)(end - p)) {
        pbFail(r, p, 1, "a field longer than the data left");
        return NULL;
    }
    return p + n;
}

/* Stores a number into a value of the step's type. */
static inline __attribute__((always_inline)) bool pbStore(PbIn *r, const uint8_t *at, const PbStep *s, uint64_t v, char *p) {
    switch (s->op) {
    case PO_ZIGZAG:
        v = (v >> 1) ^ (uint64_t)-(int64_t)(v & 1);
        break;
    case PO_BOOL:
        *(uint8_t *)p = v != 0;
        return true;
    case PO_INT:
        if (__builtin_expect(s->size < 4, 0)) {
            int64_t x = (int64_t)v;
            int bits = s->size * 8;
            if (x < -(1LL << (bits - 1)) || x >= (1LL << (bits - 1))) {
                char msg[96];
                snprintf(msg, sizeof msg, "%lld doesn't fit in i%d", (long long)x, bits);
                return pbFail(r, at, 4, msg);
            }
        }
        break;
    case PO_UINT:
        if (__builtin_expect(s->size < 4, 0) && v >= (1ULL << (s->size * 8))) {
            char msg[96];
            snprintf(msg, sizeof msg, "%llu doesn't fit in u%d", (unsigned long long)v, s->size * 8);
            return pbFail(r, at, 4, msg);
        }
        break;
    default:
        break;
    }
    switch (s->size) {     /* an int32 keeps the low 32 bits, as protobuf says */
    case 1: *(uint8_t *)p = (uint8_t)v; break;
    case 2: {
        uint16_t x = (uint16_t)v;
        memcpy(p, &x, 2);
        break;
    }
    case 4: {
        uint32_t x = (uint32_t)v;
        memcpy(p, &x, 4);
        break;
    }
    default: memcpy(p, &v, 8); break;
    }
    return true;
}

/* One number with the step's wire type. */
static inline const uint8_t *pbGetNumber(PbIn *r, const uint8_t *p, const uint8_t *end, const PbStep *s, uint64_t *v) {
    if (s->wt == 0) {
        const uint8_t *q = pbGetVarint(p, end, v);
        return q ? q : pbBadVarint(r, p, end);
    }
    int w = s->wt == 5 ? 4 : 8;
    if (end - p < w) {
        pbFail(r, p, 1, "the data ends in the middle of a number");
        return NULL;
    }
    if (w == 4) {
        uint32_t x;
        memcpy(&x, p, 4);
        *v = x;
    } else {
        memcpy(v, p, 8);
    }
    return p + w;
}

static const uint8_t *pbRead(PbIn *r, const PbPlan *pl, char *base, const uint8_t *p, const uint8_t *end);

/* A length and the bytes after it. */
static inline const uint8_t *pbGetLength(PbIn *r, const uint8_t *p, const uint8_t *end, uint64_t *n) {
    const uint8_t *q = pbGetVarint(p, end, n);
    if (!q) return pbBadVarint(r, p, end);
    if (*n > (uint64_t)(end - q)) {
        pbFail(r, p, 1, "a field longer than the data left");
        return NULL;
    }
    return q;
}

/* One value of the step's type, its wire type checked, into v. */
static const uint8_t *pbGetValue(PbIn *r, const uint8_t *p, const uint8_t *end, int wt, const PbStep *s, char *v) {
    if (wt != s->wt) {
        char msg[96];
        snprintf(msg, sizeof msg, "wire type %d where %s was expected", wt,
                 s->op == PO_STRING ? "text" : s->op == PO_MSG ? "a message" : "a number");
        pbFail(r, p, 2, msg);
        return NULL;
    }
    uint64_t n;
    if (s->op == PO_STRING) {
        const uint8_t *q = pbGetLength(r, p, end, &n);
        if (!q) return NULL;
        IrisString *str = (IrisString *)v;
        if (str->cap) free(str->ptr);
        str->ptr = malloc(n ? (size_t)n : 1);
        if (!str->ptr) outOfMemory();
        memcpy(str->ptr, q, (size_t)n);
        str->len = (int64_t)n;
        str->cap = n ? (int64_t)n : 1;
        return q + n;
    }
    if (s->op == PO_MSG) {
        const uint8_t *q = pbGetLength(r, p, end, &n);
        if (!q) return NULL;
        if (++r->nest > 100) {
            pbFail(r, p, 1, "messages nested too deeply");
            return NULL;
        }
        q = pbRead(r, s->sub, v, q, q + n);
        r->nest--;
        return q;
    }
    const uint8_t *q = pbGetNumber(r, p, end, s, &n);
    if (!q || !pbStore(r, p, s, n, v)) return NULL;
    return q;
}

/* Room for one more item at the end of a slice or List, set to the item's
   default. A slice's room is its length rounded up to a power of two. */
static inline __attribute__((always_inline)) char *pbPush(const PbStep *s, char *p, bool init) {
    char **buf;
    int64_t *len;
    if (s->shape == PF_LIST) {
        JsonList *l = (JsonList *)p;
        buf = &l->buf.ptr;
        len = &l->len;
        if (__builtin_expect(*len == l->buf.len, 0)) {
            int64_t cap = l->buf.len ? l->buf.len * 2 : 4;
            char *nb = realloc(*buf, (size_t)(cap * s->esize));
            if (!nb) outOfMemory();
            *buf = nb;
            l->buf.len = cap;
        }
    } else {
        Slice *sl = (Slice *)p;
        buf = &sl->ptr;
        len = &sl->len;
        if (*len == 0 || (*len >= 4 && !(*len & (*len - 1)))) {
            int64_t cap = *len ? *len * 2 : 4;
            char *nb = realloc(*buf, (size_t)(cap * s->esize));
            if (!nb) outOfMemory();
            *buf = nb;
        }
    }
    char *item = *buf + *len * s->esize;
    if (!init) {
        /* a number, stored whole next */
    } else if (s->elem->init) {
        memcpy(item, s->elem->init, (size_t)s->esize);
    } else {
        memset(item, 0, (size_t)s->esize);
    }
    (*len)++;
    return item;
}

static const uint8_t *pbGetEntry(PbIn *r, const PbStep *s, char *p, const uint8_t *q, const uint8_t *end) {
    const JsonMapInfo *mi = s->map;
    JsonMap *m = (JsonMap *)p;
    const uint8_t *start = q, *key = (const uint8_t *)"";
    uint64_t keyLen = 0, tag;
    /* the key first, wherever it is */
    while (q < end) {
        const uint8_t *t = pbGetVarint(q, end, &tag);
        if (!t) return pbBadVarint(r, q, end);
        if (tag == (1 << 3 | 2)) {
            q = pbGetLength(r, t, end, &keyLen);
            if (!q) return NULL;
            key = q;
            q += keyLen;
        } else if (!(q = pbSkip(r, t, end, (int)(tag & 7)))) {
            return NULL;
        }
    }
    if ((m->len + m->removed + 1) * 4 > m->state.len * 3) jsonMapGrow(m, mi);
    bool found;
    unsigned char tg;
    int64_t at = jsonMapSlot(m, mi, (const char *)key, (int64_t)keyLen, &found, &tg);
    char *e = m->slots.ptr + at * mi->entrySize;
    char *val = e + mi->valOffset;
    if (found) {
        jsonFree(mi->val, val);
    } else {
        if (m->state.ptr[at] == 1) m->removed--;
        Slice *ks = (Slice *)e;
        ks->ptr = malloc(keyLen ? (size_t)keyLen : 1);
        if (!ks->ptr) outOfMemory();
        memcpy(ks->ptr, key, (size_t)keyLen);
        ks->len = (int64_t)keyLen;
        if (mi->keySize == 24) ((IrisString *)e)->cap = (int64_t)keyLen;
        m->state.ptr[at] = (char)tg;
        m->len++;
    }
    if (mi->val->init) memcpy(val, mi->val->init, (size_t)mi->val->size);
    else memset(val, 0, (size_t)mi->val->size);
    q = start;
    while (q < end) {
        const uint8_t *t = pbGetVarint(q, end, &tag);
        if (!t) return pbBadVarint(r, q, end);
        q = tag >> 3 == 2 ? pbGetValue(r, t, end, (int)(tag & 7), s, val) : pbSkip(r, t, end, (int)(tag & 7));
        if (!q) return NULL;
    }
    return q;
}

static const uint8_t *pbField(PbIn *r, const PbStep *s, char *p, const uint8_t *q, const uint8_t *end, int wt) {
    if (s->shape == PF_ONE) return pbGetValue(r, q, end, wt, s, p);
    if (s->shape == PF_MAP) {
        uint64_t n;
        if (wt != 2) return pbGetValue(r, q, end, wt, &(PbStep){.op = PO_MSG, .wt = 2}, p);
        const uint8_t *b = pbGetLength(r, q, end, &n);
        if (!b) return NULL;
        return pbGetEntry(r, s, p, b, b + n) ? b + n : NULL;
    }
    if (wt == 2 && s->wt != 2) {
        /* packed numbers */
        uint64_t n;
        const uint8_t *b = pbGetLength(r, q, end, &n);
        if (!b) return NULL;
        const uint8_t *pend = b + n;
        while (b < pend) {
            uint64_t v;
            const uint8_t *at = b;
            b = pbGetNumber(r, b, pend, s, &v);
            if (!b || !pbStore(r, at, s, v, pbPush(s, p, false))) return NULL;
        }
        return pend;
    }
    return pbGetValue(r, q, end, wt, s, pbPush(s, p, true));
}

static const uint8_t *pbRead(PbIn *r, const PbPlan *pl, char *base, const uint8_t *p, const uint8_t *end) {
    while (p < end) {
        uint64_t tag;
        const uint8_t *q = pbGetVarint(p, end, &tag);
        if (!q) return pbBadVarint(r, p, end);
        uint64_t num = tag >> 3;
        int wt = (int)(tag & 7);
        int i = -1;
        if (num < 64) {
            i = pl->byNum[num];
        } else {
            for (int k = 0; k < pl->n; k++) {
                if ((uint64_t)pl->steps[k].num == num) i = k;
            }
        }
        if (i < 0) {
            if (num == 0) {
                pbFail(r, p, 1, "a field numbered 0");
                return NULL;
            }
            if (!(p = pbSkip(r, q, end, wt))) return NULL;
            continue;
        }
        const PbStep *s = &pl->steps[i];
        char *v = base + s->offset;
        if (s->shape == PF_ONE && wt == s->wt) {
            /* the usual cases in line: text, and a varint of one byte */
            if (s->op == PO_STRING && q < end && *q < 0x80 && *q <= end - q - 1) {
                int64_t n = *q++;
                IrisString *str = (IrisString *)v;
                if (str->cap) free(str->ptr);
                str->ptr = malloc(n ? (size_t)n : 1);
                if (!str->ptr) outOfMemory();
                memcpy(str->ptr, q, (size_t)n);
                str->len = n;
                str->cap = n ? n : 1;
                p = q + n;
                continue;
            }
            if (wt == 0 && q < end && *q < 0x80 && (s->op == PO_INT || s->op == PO_UINT || s->op == PO_BOOL) &&
                (s->size >= 4 || s->op == PO_BOOL)) {
                uint64_t x = *q;
                if (s->op == PO_BOOL) *(uint8_t *)v = x != 0;
                else if (s->size == 4) memcpy(v, &(uint32_t){(uint32_t)x}, 4);
                else memcpy(v, &x, 8);
                p = q + 1;
                continue;
            }
        }
        p = pbField(r, s, v, q, end, wt);
        if (!p) {
            const JsonField *f = &((const JsonField *)pl->t->items)[i];
            pbPathOut(r, f->name, f->nameLen);
            return NULL;
        }
    }
    return p;
}

int iris_proto_decode(const JsonType *t, const char *data, int64_t len, char *out) {
    PbIn r = {.in = {.s = data, .p = data, .end = data + len}};
    const uint8_t *p = (const uint8_t *)data;
    if (!pbRead(&r, pbPlan(t), out, p, p + len)) {
        jsonFree(t, out);
        char what[512];
        snprintf(what, sizeof what, "%.*s", (int)jsonMsgLen, jsonMsg);
        if (r.in.pathLen) {
            jsonMsgLen = snprintf(jsonMsg, sizeof jsonMsg, "%s: %s", r.in.path, what);
        } else {
            jsonMsgLen = snprintf(jsonMsg, sizeof jsonMsg, "byte %lld: %s", (long long)(r.in.p - data), what);
        }
        if (jsonMsgLen >= (int64_t)sizeof jsonMsg) jsonMsgLen = sizeof jsonMsg - 1;
        return r.in.code ? r.in.code : 1;
    }
    return 0;
}

/* proto.encodeTo: the same, for a String that may hold bytes already. */
void iris_proto_encode_to(const JsonType *t, const char *value, IrisString *out) {
    pbWrite(out, pbPlan(t), value);
}

/* ---------------------------------------------------------------- SHA-256 instructions (0.82)
   SHA-256 blocks with the CPU's own instructions: SHA-NI on x86_64, the
   SHA2 extension on arm64. shaHwBlocks gives 0 when there are none, and
   shaAdd then uses shaBlock. IRIS_SHA_SOFT=1 turns them off, for tests. */

static int shaHw = -1;

static int shaHasHw(void) {
    if (shaHw < 0) {
        int hw = 0;
        if (!getenv("IRIS_SHA_SOFT")) {
#if defined(__x86_64__)
            unsigned a, b, c, d;
            __asm__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
            int sha = (b >> 29) & 1;
            __asm__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
            hw = sha && ((c >> 19) & 1) && ((c >> 9) & 1);    /* SHA, SSE4.1, SSSE3 */
#elif defined(__aarch64__) && defined(__APPLE__)
            hw = 1;
#elif defined(__aarch64__) && defined(__linux__)
            hw = (getauxval(AT_HWCAP) & (1UL << 6)) != 0;         /* HWCAP_SHA2 */
#endif
        }
        shaHw = hw;
    }
    return shaHw;
}

#if defined(__x86_64__)
#include <immintrin.h>

__attribute__((target("sha,sse4.1,ssse3"))) static void shaNiBlocks(uint32_t h[8], const unsigned char *p, size_t blocks) {
    const __m128i swap = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);
    __m128i tmp = _mm_loadu_si128((const __m128i *)&h[0]);
    __m128i st1 = _mm_loadu_si128((const __m128i *)&h[4]);
    tmp = _mm_shuffle_epi32(tmp, 0xB1);              /* CDAB */
    st1 = _mm_shuffle_epi32(st1, 0x1B);              /* EFGH */
    __m128i st0 = _mm_alignr_epi8(tmp, st1, 8);      /* ABEF */
    st1 = _mm_blend_epi16(st1, tmp, 0xF0);           /* CDGH */
    for (; blocks > 0; blocks--, p += 64) {
        __m128i save0 = st0, save1 = st1;
        __m128i w[4];
        for (int g = 0; g < 16; g++) {
            __m128i m;
            if (g < 4) {
                m = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(p + 16 * g)), swap);
            } else {
                /* W[t-16] + s0(W[t-15]) + W[t-7], then the s1 terms */
                m = _mm_sha256msg1_epu32(w[g & 3], w[(g + 1) & 3]);
                m = _mm_add_epi32(m, _mm_alignr_epi8(w[(g + 3) & 3], w[(g + 2) & 3], 4));
                m = _mm_sha256msg2_epu32(m, w[(g + 3) & 3]);
            }
            w[g & 3] = m;
            __m128i k = _mm_add_epi32(m, _mm_loadu_si128((const __m128i *)&shaK[4 * g]));
            st1 = _mm_sha256rnds2_epu32(st1, st0, k);
            st0 = _mm_sha256rnds2_epu32(st0, st1, _mm_shuffle_epi32(k, 0x0E));
        }
        st0 = _mm_add_epi32(st0, save0);
        st1 = _mm_add_epi32(st1, save1);
    }
    tmp = _mm_shuffle_epi32(st0, 0x1B);              /* FEBA */
    st1 = _mm_shuffle_epi32(st1, 0xB1);              /* DCHG */
    st0 = _mm_blend_epi16(tmp, st1, 0xF0);           /* DCBA */
    st1 = _mm_alignr_epi8(st1, tmp, 8);              /* HGFE */
    _mm_storeu_si128((__m128i *)&h[0], st0);
    _mm_storeu_si128((__m128i *)&h[4], st1);
}
#elif defined(__aarch64__)
__attribute__((target("sha2"))) static void shaArmBlocks(uint32_t h[8], const unsigned char *p, size_t blocks) {
    uint32x4_t st0 = vld1q_u32(&h[0]), st1 = vld1q_u32(&h[4]);
    for (; blocks > 0; blocks--, p += 64) {
        uint32x4_t save0 = st0, save1 = st1;
        uint32x4_t w[4];
        for (int g = 0; g < 16; g++) {
            uint32x4_t m;
            if (g < 4) {
                m = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p + 16 * g)));
            } else {
                m = vsha256su1q_u32(vsha256su0q_u32(w[g & 3], w[(g + 1) & 3]), w[(g + 2) & 3], w[(g + 3) & 3]);
            }
            w[g & 3] = m;
            uint32x4_t k = vaddq_u32(m, vld1q_u32(&shaK[4 * g]));
            uint32x4_t prev = st0;
            st0 = vsha256hq_u32(st0, st1, k);
            st1 = vsha256h2q_u32(st1, prev, k);
        }
        st0 = vaddq_u32(st0, save0);
        st1 = vaddq_u32(st1, save1);
    }
    vst1q_u32(&h[0], st0);
    vst1q_u32(&h[4], st1);
}
#endif

static int shaHwBlocks(uint32_t h[8], const unsigned char *p, size_t blocks) {
    if (!shaHasHw()) return 0;
#if defined(__x86_64__)
    shaNiBlocks(h, p, blocks);
    return 1;
#elif defined(__aarch64__)
    shaArmBlocks(h, p, blocks);
    return 1;
#else
    (void)h; (void)p; (void)blocks;
    return 0;
#endif
}

/* ---------------------------------------------------------------- RSA-OAEP (0.82)
   RSA encryption with OAEP padding, for MySQL's caching_sha2_password over
   a connection without TLS. hash: 0 SHA-1 (what MySQL uses), 1 SHA-256. */

static struct {
    int loaded;
    void *(*ctxNew)(void *, void *);
    void (*ctxFree)(void *);
    int (*encryptInit)(void *);
    int (*encrypt)(void *, unsigned char *, size_t *, const unsigned char *, size_t);
    int (*setOaepMd)(void *, const void *);
} oaep;

static pthread_once_t oaepOnce = PTHREAD_ONCE_INIT;

static void oaepLoad(void) {
    if (!pkReady()) return;
    void *c = ossl.cryptoLib;
    *(void **)&oaep.ctxNew = dlsym(c, "EVP_PKEY_CTX_new");
    *(void **)&oaep.ctxFree = dlsym(c, "EVP_PKEY_CTX_free");
    *(void **)&oaep.encryptInit = dlsym(c, "EVP_PKEY_encrypt_init");
    *(void **)&oaep.encrypt = dlsym(c, "EVP_PKEY_encrypt");
    *(void **)&oaep.setOaepMd = dlsym(c, "EVP_PKEY_CTX_set_rsa_oaep_md");     /* OpenSSL 3 */
    oaep.loaded = oaep.ctxNew && oaep.ctxFree && oaep.encryptInit && oaep.encrypt;
}

int iris_pk_encrypt_oaep(void *key, const char *data, int64_t len, int hash, IrisString *out) {
    pthread_once(&oaepOnce, oaepLoad);
    if (!oaep.loaded) {
        tlsSay("RSA encryption needs OpenSSL's libcrypto");
        return 0;
    }
    void *ctx = oaep.ctxNew(key, NULL);
    int ok = ctx && oaep.encryptInit(ctx) == 1;
    /* RSA_PKCS1_OAEP_PADDING */
    ok = ok && (pk.setPadding ? pk.setPadding(ctx, 4) == 1 : pk.pkeyCtrl(ctx, 6, -1, 0x1001, 4, NULL) == 1);
    if (ok && hash == 1) {
        const void *md = pk.md[1]();
        ok = oaep.setOaepMd ? oaep.setOaepMd(ctx, md) == 1 : pk.pkeyCtrl(ctx, 6, 0x300, 0x1009, 0, (void *)md) == 1;
    }
    size_t n = 0;
    ok = ok && oaep.encrypt(ctx, NULL, &n, (const unsigned char *)data, (size_t)len) == 1;
    unsigned char *buf = ok ? malloc(n ? n : 1) : NULL;
    ok = ok && buf && oaep.encrypt(ctx, buf, &n, (const unsigned char *)data, (size_t)len) == 1;
    if (ok) iris_string_append(out, (const char *)buf, (int64_t)n);
    else tlsSayOpenssl("encrypting with RSA");
    free(buf);
    if (ctx) oaep.ctxFree(ctx);
    return ok;
}

/* ======================================================================
 * metrics and tracing
 *
 * Floats that every thread adds to: a counter, a gauge, a histogram's sum.
 * There is no atomic double, so the bits go through a compare-and-swap.
 * Trace and span ids come from a thread's own xoshiro, seeded from the OS.
 * ====================================================================== */

static inline uint64_t metricBits(double v) { uint64_t u; memcpy(&u, &v, 8); return u; }
static inline double metricFloat(uint64_t u) { double v; memcpy(&v, &u, 8); return v; }

void iris_metric_add(double *p, double v) {
    _Atomic uint64_t *a = (_Atomic uint64_t *)p;
    uint64_t old = atomic_load_explicit(a, memory_order_relaxed);
    while (!atomic_compare_exchange_weak_explicit(a, &old, metricBits(metricFloat(old) + v),
                                                  memory_order_relaxed, memory_order_relaxed)) {
    }
}

void iris_metric_set(double *p, double v) {
    atomic_store_explicit((_Atomic uint64_t *)p, metricBits(v), memory_order_relaxed);
}

double iris_metric_get(const double *p) {
    return metricFloat(atomic_load_explicit((_Atomic uint64_t *)p, memory_order_relaxed));
}

void iris_metric_count(int64_t *p, int64_t n) {
    atomic_fetch_add_explicit((_Atomic int64_t *)p, n, memory_order_relaxed);
}

int64_t iris_metric_load(const int64_t *p) {
    return atomic_load_explicit((_Atomic int64_t *)p, memory_order_relaxed);
}

/* A histogram's observation: the first bucket whose bound is at least v,
 * or the last slot (+Inf), then the sum. counts has n + 1 slots. */
void iris_metric_observe(const double *bounds, int64_t n, int64_t *counts, double *sum, double v) {
    int64_t i = 0;
    while (i < n && v > bounds[i]) i++;
    atomic_fetch_add_explicit((_Atomic int64_t *)&counts[i], 1, memory_order_relaxed);
    iris_metric_add(sum, v);
}

static __thread uint64_t traceRng[4];

static inline uint64_t traceRot(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

/* A random id that isn't 0, which W3C says is no id. */
uint64_t iris_trace_random(void) {
    uint64_t *s = traceRng;
    if (!(s[0] | s[1] | s[2] | s[3])) {
        iris_random_os((char *)s, sizeof traceRng);
        if (!(s[0] | s[1] | s[2] | s[3])) s[0] = 1;
    }
    for (;;) {
        uint64_t r = traceRot(s[1] * 5, 7) * 9, t = s[1] << 17;
        s[2] ^= s[0];
        s[3] ^= s[1];
        s[1] ^= s[2];
        s[0] ^= s[3];
        s[2] ^= t;
        s[3] = traceRot(s[3], 45);
        if (r) return r;
    }
}

/* A metric family's table of series: a slot is read without a lock, and
 * written once, after its hash, under the family's lock. */
void *iris_metric_slot(void **p) {
    return atomic_load_explicit((_Atomic(void *) *)p, memory_order_acquire);
}

void iris_metric_put_slot(void **p, void *v) {
    atomic_store_explicit((_Atomic(void *) *)p, v, memory_order_release);
}
