/* The Iris runtime: what every program links with. Printing goes through
 * stdio's stdout buffer, which C flushes when the program ends. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char panicBuf[4096];
static size_t panicLen;

void iris_init(int argc, char **argv) {
    (void)argc;
    (void)argv;
}

static void put(int stream, const char *s, size_t n) {
    if (stream == 1) {
        fwrite(s, 1, n, stdout);
        return;
    }
    /* stream 2: the text of a panic, kept until iris_panic */
    if (n > sizeof panicBuf - panicLen) {
        n = sizeof panicBuf - panicLen;
    }
    memcpy(panicBuf + panicLen, s, n);
    panicLen += n;
}

void iris_write_str(int stream, const char *s, int64_t n) { put(stream, s, (size_t)n); }

void iris_write_i64(int stream, int64_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%lld", (long long)v);
    put(stream, buf, (size_t)n);
}

void iris_write_u64(int stream, int64_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%llu", (unsigned long long)(uint64_t)v);
    put(stream, buf, (size_t)n);
}

void iris_write_bool(int stream, int v) {
    if (v) {
        put(stream, "true", 4);
    } else {
        put(stream, "false", 5);
    }
}

void iris_print_end(void) { fputc('\n', stdout); }

int iris_str_eq(const char *a, int64_t na, const char *b, int64_t nb) {
    return na == nb && (na == 0 || memcmp(a, b, (size_t)na) == 0);
}

static void stop(const char *msg, size_t n, const char *file, int line, int col, const char *fn) {
    fflush(stdout);
    fprintf(stderr, "panic: %.*s\n  at %s:%d:%d in %s\n", (int)n, msg, file, line, col, fn);
    exit(2);
}

void iris_panic(const char *file, int line, int col, const char *fn) {
    stop(panicBuf, panicLen, file, line, col, fn);
}

void iris_check_failed(const char *msg, const char *file, int line, int col, const char *fn) {
    stop(msg, strlen(msg), file, line, col, fn);
}
