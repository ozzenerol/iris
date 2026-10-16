/* The Iris runtime: what every program links with. Printing goes through
 * stdio's stdout buffer, which C flushes when the program ends. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *ptr;
    int64_t len, cap;
} IrisString;

/* Debug builds use AddressSanitizer. Leaks are not reported (yet). */
const char *__asan_default_options(void) { return "detect_leaks=0:exitcode=2:abort_on_error=0"; }

static char panicBuf[4096];
static size_t panicLen;

void iris_init(int argc, char **argv) {
    (void)argc;
    (void)argv;
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

void iris_string_append(IrisString *s, const char *p, int64_t n) {
    if (n <= 0) {
        return;
    }
    if (s->len + n > s->cap) {
        int64_t cap = s->cap ? s->cap * 2 : 16;
        while (cap < s->len + n) {
            cap *= 2;
        }
        char *q = realloc(s->ptr, (size_t)cap);
        if (!q) {
            outOfMemory();
        }
        s->ptr = q;
        s->cap = cap;
    }
    memcpy(s->ptr + s->len, p, (size_t)n);
    s->len += n;
}

void iris_string_free(IrisString *s) {
    free(s->ptr);
    s->ptr = NULL;
    s->len = s->cap = 0;
}

/* sink 1: stdout; 2: the text of a panic; 3: the String at dst */
static void put(int sink, IrisString *dst, const char *s, size_t n) {
    if (sink == 1) {
        fwrite(s, 1, n, stdout);
    } else if (sink == 3) {
        iris_string_append(dst, s, (int64_t)n);
    } else {
        if (n > sizeof panicBuf - panicLen) {
            n = sizeof panicBuf - panicLen;
        }
        memcpy(panicBuf + panicLen, s, n);
        panicLen += n;
    }
}

void iris_write_str(int sink, IrisString *dst, const char *s, int64_t n) { put(sink, dst, s, (size_t)n); }

void iris_write_i64(int sink, IrisString *dst, int64_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%lld", (long long)v);
    put(sink, dst, buf, (size_t)n);
}

void iris_write_u64(int sink, IrisString *dst, int64_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%llu", (unsigned long long)(uint64_t)v);
    put(sink, dst, buf, (size_t)n);
}

void iris_write_bool(int sink, IrisString *dst, int v) {
    if (v) {
        put(sink, dst, "true", 4);
    } else {
        put(sink, dst, "false", 5);
    }
}

void iris_print_end(void) { fputc('\n', stdout); }

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
