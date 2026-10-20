/* The Iris runtime: what every program links with. Printing goes through
 * stdio's stdout buffer, which C flushes when the program ends. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE        /* MAP_ANON and SO_NOSIGPIPE on macOS */
#define _GNU_SOURCE             /* sched_getaffinity on Linux */
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

/* A float in the fewest digits that read back as the same value, written
 * as a plain decimal unless it is very large or very small: 100, 0.25,
 * 1e+21, 1e-07. */
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
    /* the shortest digits: d.ddde+XX */
    int prec = 0;
    for (prec = 0; prec < 17; prec++) {
        snprintf(buf, sizeof buf, "%.*e", prec, v);
        double back = strtod(buf, NULL);
        if (bits == 32 ? (float)back == (float)v : back == v) {
            break;
        }
    }
    char *ep = strchr(buf, 'e');
    int exp10 = atoi(ep + 1);
    if (exp10 < -6 || exp10 >= 21) {
        /* the exponent form, without padding zeros: 1e+21, 1.5e-7 */
        n = snprintf(buf + (ep - buf), sizeof buf - (size_t)(ep - buf), "e%c%d", exp10 < 0 ? '-' : '+',
                     exp10 < 0 ? -exp10 : exp10);
        put(sink, dst, buf, strlen(buf));
        return;
    }
    /* the digits without the point and the exponent */
    char digits[32];
    int nd = 0;
    bool neg = buf[0] == '-';
    for (char *q = buf + (neg ? 1 : 0); q < ep; q++) {
        if (*q != '.') digits[nd++] = *q;
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

static void addFrame(const char *file, int line, int col, const char *fn) {
    if (traceLen < TRACE_MAX) {
        trace[traceLen++] = (Frame){file, fn, line, col};
    } else {
        traceDropped++;
    }
}

/* A new error: its message becomes the current one and its trace starts
 * where it was made. */
int64_t iris_error_finish(int64_t code, const char *file, int line, int col, const char *fn) {
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

int iris_fs_write(const char *path, int64_t n, const char *data, int64_t len, int append) {
    char *p = cpath(path, n);
    if (!p) {
        return 1;
    }
    FILE *f = fopen(p, append ? "ab" : "wb");
    int err = errno;
    free(p);
    if (!f) {
        return fsCode(err);
    }
    size_t put = len ? fwrite(data, 1, (size_t)len, f) : 0;
    int bad = fclose(f) != 0 || put != (size_t)len;
    return bad ? 5 : 0;
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
    if (parents) {
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

static int removeTree(const char *p) {
    struct stat st;
    if (lstat(p, &st) != 0) {
        return errno == ENOENT ? 0 : fsCode(errno);
    }
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(p);
        if (!d) return fsCode(errno);
        struct dirent *e;
        int code = 0;
        while (!code && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            size_t len = strlen(p) + strlen(e->d_name) + 2;
            char *sub = malloc(len);
            if (!sub) outOfMemory();
            snprintf(sub, len, "%s/%s", p, e->d_name);
            code = removeTree(sub);
            free(sub);
        }
        closedir(d);
        if (code) return code;
        return rmdir(p) == 0 ? 0 : fsCode(errno);
    }
    return unlink(p) == 0 ? 0 : fsCode(errno);
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
    int flags = mode == 0 ? O_RDONLY : mode == 1 ? (O_WRONLY | O_CREAT | O_TRUNC) : (O_WRONLY | O_CREAT | O_APPEND);
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
int64_t iris_str_find(const char *s, int64_t n, const char *sub, int64_t m) {
    const char *at = memmem(s, (size_t)n, sub, (size_t)m);
    return at ? (int64_t)(at - s) : -1;
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
    double v = strtod(buf, &end);
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
} Sched;

static _Thread_local Sched *mySched;
static int liveThreads = 1;

static void spinLock(int *l) {
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(l, __ATOMIC_RELAXED)) {
            sched_yield();
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
    RING_ENTRIES = 1024,
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
    int fd = (int)syscall(__NR_io_uring_setup, RING_ENTRIES, &p);
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

/* Waits for the operating system or the next timer, and makes the tasks
 * whose wait is over ready. */
static void pollEvents(void) {
    int64_t timeout = -1;
    if (sleepers) {
        int64_t d = sleepers->wakeAt - nowNs();
        timeout = d < 0 ? 0 : d;
    }
    /* another thread may still wake a task here: then wait for it */
    int threads = __atomic_load_n(&liveThreads, __ATOMIC_SEQ_CST);
    drainInbox();
    if (readyHead) {
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
    if (block && useRing()) {
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
#define TASK_CACHE_MAX 16
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

/* lets the other ready tasks run */
void iris_yield(void) {
    makeReady(current);
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
};

static bool claim(Waiter *w) {
    return !__atomic_exchange_n(w->claimed, 1, __ATOMIC_ACQ_REL);
}

/* Channels work between threads: each has a lock, held only briefly. */
typedef struct {
    int lock;
    int64_t elemSize, cap, len, head;
    char *buf;
    Waiter *recvWaiters, *sendWaiters;
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

/* Wakes the first waiter in list that can still be claimed. */
static void wakeOne(Waiter **list) {
    while (*list) {
        Waiter *w = *list;
        *list = w->next;
        if (claim(w)) {
            wakeTask(w->task);
            return;
        }
    }
}

static void unlinkWaiter(Waiter **list, Waiter *w) {
    for (Waiter **p = list; *p; p = &(*p)->next) {
        if (*p == w) {
            *p = w->next;
            return;
        }
    }
}

/* The first waiter in list that can still be claimed, taken out of it. */
static Waiter *takeWaiter(Waiter **list) {
    while (*list) {
        Waiter *w = *list;
        *list = w->next;
        if (claim(w)) return w;
    }
    return NULL;
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
        Waiter w = {current, &claimed, c->sendWaiters, (void *)value, &taken, 0};
        c->sendWaiters = &w;
        spinUnlock(&c->lock);
        schedule();
        spinLock(&c->lock);
        unlinkWaiter(&c->sendWaiters, &w);
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
    *s->got = 1;
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
        Waiter w = {current, &claimed, c->recvWaiters, out, &got, 0};
        c->recvWaiters = &w;
        spinUnlock(&c->lock);
        schedule();
        spinLock(&c->lock);
        unlinkWaiter(&c->recvWaiters, &w);
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
        Waiter w = {current, &claimed, c->sendWaiters};
        c->sendWaiters = &w;
        spinUnlock(&c->lock);
        schedule();
        spinLock(&c->lock);
        unlinkWaiter(&c->sendWaiters, &w);
    }
    memcpy(c->buf + ((c->head + c->len) % c->cap) * c->elemSize, value, (size_t)c->elemSize);
    c->len++;
    wakeOne(&c->recvWaiters);
    spinUnlock(&c->lock);
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
        Waiter w = {current, &claimed, c->recvWaiters};
        c->recvWaiters = &w;
        spinUnlock(&c->lock);
        schedule();
        spinLock(&c->lock);
        unlinkWaiter(&c->recvWaiters, &w);
    }
    chanTake(c, out);
    spinUnlock(&c->lock);
}

/* select: takes a value from the first of chans that has one, or returns
 * n when timeout (ns, -1 for none) runs out first. */
int64_t iris_select(int64_t n, Chan **chans, void **outs, int64_t timeout) {
    int64_t deadline = timeout >= 0 ? nowNs() + timeout : -1;
    Waiter ws[64];
    if (n > 64) {
        n = 64;
    }
    for (;;) {
        for (int64_t i = 0; i < n; i++) {
            spinLock(&chans[i]->lock);
            if (chans[i]->cap == 0 ? handTake(chans[i], outs[i]) : chans[i]->len > 0) {
                if (chans[i]->cap != 0) chanTake(chans[i], outs[i]);
                spinUnlock(&chans[i]->lock);
                return i;
            }
            spinUnlock(&chans[i]->lock);
        }
        if (deadline >= 0 && nowNs() >= deadline) {
            return n;
        }
        /* wait on every channel; one that got a value meanwhile means
           trying again, unless a waker already claimed this task */
        int claimed = 0;
        int64_t added = 0, got = 0;
        bool arrived = false;
        for (; added < n && !arrived; added++) {
            Chan *c = chans[added];
            spinLock(&c->lock);
            ws[added] = (Waiter){current, &claimed, c->recvWaiters, outs[added], &got, added};
            c->recvWaiters = &ws[added];
            arrived = c->cap == 0 ? c->sendWaiters != NULL : c->len > 0;
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
            unlinkWaiter(&chans[i]->recvWaiters, &ws[i]);
            spinUnlock(&chans[i]->lock);
        }
        if (got) {
            return got - 1;         /* a sender handed its value straight over */
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
} Mutex;

void iris_mutex_lock(Mutex *m) {
    spinLock(&m->lock);
    if (!m->locked) {
        m->locked = 1;
        spinUnlock(&m->lock);
        return;
    }
    int claimed = 0;
    Waiter w = {current, &claimed, NULL};
    if (m->tail) {
        m->tail->next = &w;
    } else {
        m->head = &w;
    }
    m->tail = &w;
    spinUnlock(&m->lock);
    schedule();                 /* unlock gave it the lock */
}

bool iris_mutex_try_lock(Mutex *m) {
    spinLock(&m->lock);
    bool got = !m->locked;
    m->locked = 1;
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
        spinUnlock(&m->lock);
        wakeTask(t);
        return;
    }
    m->locked = 0;
    spinUnlock(&m->lock);
}

/* ---------------------------------------------------------------- net */

/* errno as net's error codes: 1 refused, 2 closed, 3 timeout, 4 notFound, 5 inUse, 6 other */
static int netCode(int e) {
    switch (e) {
    case ECONNREFUSED: return 1;
    case ECONNRESET: case EPIPE: case ENOTCONN: return 2;
    case ETIMEDOUT: return 3;
    case EADDRINUSE: return 5;
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
    int rc = getaddrinfo(h && *h ? h : NULL, portStr, &hints, &res);
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
#ifdef __linux__
        int c;
        if (useRing()) {
            c = ringOpUntil(RING_OP_ACCEPT, fd, NULL, 0, SOCK_NONBLOCK | SOCK_CLOEXEC, deadline);
            if (c == -ECANCELED) {
                *err = 3;
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

/* Sends one datagram, to ip and port, or where the socket is connected
 * (ip NULL). 0, or a code. */
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
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            if (!waitFdUntil(fd, 1, deadline)) return 3;
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

void iris_net_close(int fd) {
    if (fd >= 0) {
        fdForget(fd);
        close(fd);
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

/* ---------------------------------------------------------------- json

   json.encode, json.decode and the free of @json structs work from a table
   the compiler writes for each type (ir.c, jsonType). */

enum { J_INT = 1, J_UINT, J_FLOAT, J_BOOL, J_STR, J_STRING, J_STRUCT, J_SLICE, J_ARRAY, J_ENUM, J_LIST };

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

static int64_t readInt(const char *p, int size, bool sign) {
    switch (size) {
    case 1: return sign ? *(int8_t *)p : *(uint8_t *)p;
    case 2: return sign ? *(int16_t *)p : *(uint16_t *)p;
    case 4: return sign ? *(int32_t *)p : *(uint32_t *)p;
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

static void jsonQuote(IrisString *out, const char *s, int64_t n) {
    iris_string_append(out, "\"", 1);
    int64_t start = 0;
    for (int64_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        const char *esc = NULL;
        char buf[8];
        switch (ch) {
        case '"': esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n"; break;
        case '\r': esc = "\\r"; break;
        case '\t': esc = "\\t"; break;
        case '\b': esc = "\\b"; break;
        case '\f': esc = "\\f"; break;
        default:
            if (ch < 0x20) {
                snprintf(buf, sizeof buf, "\\u%04x", ch);
                esc = buf;
            }
        }
        if (esc) {
            iris_string_append(out, s + start, i - start);
            iris_string_append(out, esc, (int64_t)strlen(esc));
            start = i + 1;
        }
    }
    iris_string_append(out, s + start, n - start);
    iris_string_append(out, "\"", 1);
}

static void jsonEncode(IrisString *out, const JsonType *t, const char *p) {
    char buf[32];
    switch (t->kind) {
    case J_INT: case J_UINT: {
        int64_t v = readInt(p, t->size, t->kind == J_INT);
        int n = t->kind == J_INT || t->size < 8 ? snprintf(buf, sizeof buf, "%lld", (long long)v)
                                                : snprintf(buf, sizeof buf, "%llu", (unsigned long long)v);
        iris_string_append(out, buf, n);
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
    jsonEncode(out, t, value);
}

static bool jsonOwns(const JsonType *t) {
    switch (t->kind) {
    case J_STRING: case J_SLICE: case J_LIST: return true;
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

/* Frees the Strings and slices inside the value at p, and empties them. */
static void jsonFree(const JsonType *t, char *p) {
    switch (t->kind) {
    case J_STRING:
        iris_string_free((IrisString *)p);
        break;
    case J_STRUCT: {
        const JsonField *fs = t->items;
        for (int64_t i = 0; i < t->count; i++) {
            if (jsonOwns(fs[i].type)) jsonFree(fs[i].type, p + fs[i].offset);
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
    case J_LIST: {
        const JsonType *et = t->items;
        JsonList *l = (JsonList *)p;
        if (jsonOwns(et)) {
            for (int64_t i = 0; i < l->len; i++) jsonFree(et, l->buf.ptr + i * et->size);
        }
        free(l->buf.ptr);
        memset(l, 0, sizeof *l);
        break;
    }
    }
}

void iris_json_free(const JsonType *t, char *value) {
    jsonFree(t, value);
}

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

static void pathPush(JsonIn *in, const char *fmtStr, const char *s, int64_t n) {
    size_t room = sizeof in->path - in->pathLen;
    int w = snprintf(in->path + in->pathLen, room, fmtStr, (int)n, s);
    if (w > 0) in->pathLen += (size_t)w < room ? (size_t)w : room - 1;
}

static bool jsonValue(JsonIn *in, const JsonType *t, char *p);

static bool jsonStruct(JsonIn *in, const JsonType *t, char *p) {
    const JsonField *fs = t->items;
    unsigned char seenSmall[64];
    unsigned char *seen = t->count <= 64 ? seenSmall : calloc((size_t)t->count, 1);
    if (!seen) outOfMemory();
    memset(seen, 0, t->count <= 64 ? sizeof seenSmall : 0);
    IrisString key = {0};
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
                int64_t f = -1;
                for (int64_t i = 0; i < t->count; i++) {
                    if (fs[i].nameLen == key.len && !memcmp(fs[i].name, key.ptr, (size_t)key.len)) {
                        f = i;
                        break;
                    }
                }
                if (f < 0) {
                    ok = jsonSkip(in);  /* a key the struct doesn't have */
                } else {
                    size_t saved = in->pathLen;
                    pathPush(in, in->pathLen ? ".%.*s" : "%.*s", fs[f].name, fs[f].nameLen);
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
        if (fs[i].required && !seen[i]) {
            char msg[160];
            snprintf(msg, sizeof msg, "the required field \"%.*s\" is missing", (int)fs[i].nameLen, fs[i].name);
            ok = jsonFail(in, 3, msg);
        }
    }
    iris_string_free(&key);
    if (seen != seenSmall) free(seen);
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
            pathPush(in, "[%d]%s", "", n);
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
        if (*in->p != '-' && (*in->p < '0' || *in->p > '9')) return jsonExpected(in, "a number");
        const char *s;
        size_t n;
        bool integer;
        if (!jsonNumber(in, &s, &n, &integer)) return false;
        char num[64];
        if (n >= sizeof num) n = sizeof num - 1;
        memcpy(num, s, n);
        num[n] = 0;
        if (t->kind == J_FLOAT) {
            double v = strtod(num, NULL);
            if (t->size == 4) *(float *)p = (float)v;
            else *(double *)p = v;
            return true;
        }
        if (!integer) return jsonFail(in, 2, "expected a whole number");
        errno = 0;
        char msg[128];
        if (t->kind == J_INT) {
            long long v = strtoll(num, NULL, 10);
            int bits = t->size * 8;
            bool fits = errno == 0 && (bits == 64 || (v >= -(1LL << (bits - 1)) && v < (1LL << (bits - 1))));
            if (!fits) {
                snprintf(msg, sizeof msg, "%s doesn't fit in i%d", num, bits);
                return jsonFail(in, 4, msg);
            }
            writeInt(p, t->size, v);
        } else {
            unsigned long long v = num[0] == '-' ? 0 : strtoull(num, NULL, 10);
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
#undef SYM
    *(void **)&ossl.ctxSetAlpnSelect = dlsym(ssl, "SSL_CTX_set_alpn_select_cb");
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

/* A context for clients: the system's certificates, or the ones in ca. */
static void *newClientCtx(const char *ca) {
    void *ctx = ossl.ctxNew(ossl.clientMethod());
    if (!ctx) return NULL;
    ossl.ctxCtrl(ctx, TLS_CTRL_MIN_VERSION, TLS_V12, NULL);
    ossl.ctxSetVerify(ctx, TLS_VERIFY_PEER, NULL);
    if (ca ? ossl.ctxLoadVerify(ctx, ca, NULL) != 1 : ossl.ctxDefaultPaths(ctx) != 1) {
        ossl.ctxFree(ctx);
        return NULL;
    }
    return ctx;
}

static int lockedClientCtx;
static void *sharedClientCtx(void) {
    spinLock(&lockedClientCtx);
    if (!clientCtx) clientCtx = newClientCtx(NULL);
    void *c = clientCtx;
    spinUnlock(&lockedClientCtx);
    return c;
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

static bool tlsWait(void *ssl, int r) {
    bool t = false;
    return tlsWaitUntil(ssl, r, 0, &t);
}

/* The client side of a handshake on a connected socket: the server must
   have a certificate for host. ca, when not empty, is a file of the
   certificates to trust instead of the system's. NULL on an error, with
   the message set. */
void *iris_tls_connect(int fd, const char *host, int64_t hostLen, const char *ca, int64_t caLen, const char *alpn,
                       int64_t alpnLen) {
    if (!tlsReady()) return NULL;
    char *h = cpath(host, hostLen), *caPath = caLen > 0 ? cpath(ca, caLen) : NULL;
    void *ctx = caPath ? newClientCtx(caPath) : sharedClientCtx();
    void *ssl = ctx ? ossl.sslNew(ctx) : NULL;
    if (!ssl) {
        tlsSayOpenssl(caPath ? "reading the certificates to trust" : "starting TLS");
        free(h);
        free(caPath);
        if (caPath && ctx) ossl.ctxFree(ctx);
        return NULL;
    }
    if (caPath) ossl.ctxFree(ctx);      /* the connection keeps its own reference */
    free(caPath);
    ossl.sslSetFd(ssl, fd);
    ossl.sslCtrl(ssl, TLS_CTRL_SNI, 0, h);
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

/* A server's certificate chain and key, from PEM files. */
void *iris_tls_server_ctx(const char *cert, int64_t certLen, const char *key, int64_t keyLen) {
    if (!tlsReady()) return NULL;
    char *c = cpath(cert, certLen), *k = cpath(key, keyLen);
    void *ctx = ossl.ctxNew(ossl.serverMethod());
    bool ok = ctx != NULL;
    if (ok) {
        ossl.ctxCtrl(ctx, TLS_CTRL_MIN_VERSION, TLS_V12, NULL);
        if (ossl.ctxUseChain(ctx, c) != 1) {
            char what[300];
            snprintf(what, sizeof what, "reading the certificate %s", c);
            tlsSayOpenssl(what);
            ok = false;
        } else if (ossl.ctxUseKey(ctx, k, 1) != 1 || ossl.ctxCheckKey(ctx) != 1) {
            char what[300];
            snprintf(what, sizeof what, "reading the key %s", k);
            tlsSayOpenssl(what);
            ok = false;
        }
    } else {
        tlsSayOpenssl("starting TLS");
    }
    free(c);
    free(k);
    if (!ok && ctx) {
        ossl.ctxFree(ctx);
        ctx = NULL;
    }
    return ctx;
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

/* Lets clients of a server context ask for HTTP/2. */
void iris_tls_ctx_http2(void *ctx) {
    static const unsigned char protos[] = "\x02h2\x08http/1.1";
    if (ctx && ossl.ctxSetAlpnSelect) ossl.ctxSetAlpnSelect(ctx, alpnSelect, (void *)protos);
}

/* The protocol the two sides picked with ALPN: "" for none. */
void iris_tls_alpn(void *ssl, const char **p, int64_t *n) {
    const unsigned char *data = NULL;
    unsigned len = 0;
    if (ssl && ossl.sslGetAlpn) ossl.sslGetAlpn(ssl, &data, &len);
    *p = data ? (const char *)data : "";
    *n = data ? len : 0;
}

void iris_tls_ctx_free(void *ctx) {
    if (ctx && ossl.loaded == 1) ossl.ctxFree(ctx);
}

/* The server side of a handshake on an accepted socket. */
void *iris_tls_accept(void *ctx, int fd, int64_t timeout) {
    int64_t deadline = deadlineOf(timeout);
    if (!tlsReady()) return NULL;
    void *ssl = ossl.sslNew(ctx);
    if (!ssl) {
        tlsSayOpenssl("starting TLS");
        return NULL;
    }
    ossl.sslSetFd(ssl, fd);
    for (;;) {
        PipeGuard g;
        pipeHold(&g);
        int r = ossl.sslAccept(ssl);
        pipeRelease(&g);
        if (r == 1) return ssl;
        bool timedOut = false;
        if (tlsWaitUntil(ssl, r, deadline, &timedOut)) continue;
        tlsSayOpenssl(timedOut ? "the TLS handshake timed out" : "the TLS handshake failed");
        ossl.sslFree(ssl);
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

static void shaAdd(Sha256 *s, const unsigned char *p, size_t n) {
    s->len += n;
    while (n > 0) {
        size_t take = 64 - s->used < n ? 64 - s->used : n;
        memcpy(s->block + s->used, p, take);
        s->used += take;
        p += take;
        n -= take;
        if (s->used == 64) {
            shaBlock(s, s->block);
            s->used = 0;
        }
    }
}

static void shaEnd(Sha256 *s, unsigned char out[32]) {
    uint64_t bits = s->len * 8;
    unsigned char pad = 0x80;
    shaAdd(s, &pad, 1);
    unsigned char zero = 0;
    while (s->used != 56) shaAdd(s, &zero, 1);
    unsigned char lenBytes[8];
    for (int i = 0; i < 8; i++) lenBytes[i] = (unsigned char)(bits >> (56 - 8 * i));
    shaAdd(s, lenBytes, 8);
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

static void sha512Block(Sha512 *s, const unsigned char *p) {
    uint64_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = 0;
        for (int k = 0; k < 8; k++) w[i] = (w[i] << 8) | p[i * 8 + k];
    }
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = ROR64(w[i - 15], 1) ^ ROR64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = ROR64(w[i - 2], 19) ^ ROR64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint64_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 80; i++) {
        uint64_t t1 = h + (ROR64(e, 14) ^ ROR64(e, 18) ^ ROR64(e, 41)) + ((e & f) ^ (~e & g)) + sha512K[i] + w[i];
        uint64_t t2 = (ROR64(a, 28) ^ ROR64(a, 34) ^ ROR64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
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
    while (n > 0) {
        size_t take = 128 - s->used < n ? 128 - s->used : n;
        memcpy(s->block + s->used, p, take);
        s->used += take;
        p += take;
        n -= take;
        if (s->used == 128) {
            sha512Block(s, s->block);
            s->used = 0;
        }
    }
}

static void sha512End(Sha512 *s, unsigned char out[64]) {
    uint64_t bits = s->len * 8;
    unsigned char pad = 0x80;
    sha512Add(s, &pad, 1);
    unsigned char zero = 0;
    while (s->used != 112) sha512Add(s, &zero, 1);
    unsigned char lenBytes[16] = {0};
    for (int i = 0; i < 8; i++) lenBytes[8 + i] = (unsigned char)(bits >> (56 - 8 * i));
    sha512Add(s, lenBytes, 16);
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

/* Ed25519 signatures (RFC 8032), after TweetNaCl (public domain). A field
   element is 16 limbs of 16 bits; a point is (X, Y, Z, T) in extended
   coordinates. Every step takes the same time whatever the secret is. */
typedef int64_t Fe[16];

static const Fe fe0 = {0}, fe1 = {1};
static const Fe edD = {0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
                       0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203};
static const Fe edD2 = {0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
                        0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406};
static const Fe edX = {0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
                       0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169};
static const Fe edY = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
                       0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666};
static const Fe edI = {0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
                       0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83};
/* the order of the group, little end first */
static const int64_t edL[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7,
                                0xa2, 0xde, 0xf9, 0xde, 0x14, 0, 0, 0, 0, 0, 0, 0, 0,
                                0, 0, 0, 0, 0, 0, 0, 0x10};

static void feCopy(Fe o, const Fe a) { memcpy(o, a, sizeof(Fe)); }

static void feCarry(Fe o) {
    for (int i = 0; i < 16; i++) {
        o[i] += 1 << 16;
        int64_t c = o[i] >> 16;
        if (i < 15) {
            o[i + 1] += c - 1;
        } else {
            o[0] += 38 * (c - 1);
        }
        o[i] -= c * 65536;
    }
}

/* swaps p and q when b is 1, in the same time either way */
static void feSwap(Fe p, Fe q, int b) {
    int64_t c = ~(int64_t)(b - 1);
    for (int i = 0; i < 16; i++) {
        int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void fePack(unsigned char *o, const Fe n) {
    Fe m, t;
    feCopy(t, n);
    feCarry(t);
    feCarry(t);
    feCarry(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        feSwap(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) {
        o[2 * i] = (unsigned char)(t[i] & 0xff);
        o[2 * i + 1] = (unsigned char)(t[i] >> 8);
    }
}

static void feUnpack(Fe o, const unsigned char *n) {
    for (int i = 0; i < 16; i++) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}

static int feDiffer(const Fe a, const Fe b) {
    unsigned char c[32], d[32];
    fePack(c, a);
    fePack(d, b);
    return !iris_const_equal((const char *)c, 32, (const char *)d, 32);
}

static int feParity(const Fe a) {
    unsigned char d[32];
    fePack(d, a);
    return d[0] & 1;
}

static void feAdd(Fe o, const Fe a, const Fe b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] + b[i];
}

static void feSub(Fe o, const Fe a, const Fe b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] - b[i];
}

static void feMul(Fe o, const Fe a, const Fe b) {
    int64_t t[31] = {0};
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    }
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    feCarry(o);
    feCarry(o);
}

/* a to the power 2^255 - 21: 1/a */
static void feInvert(Fe o, const Fe a) {
    Fe c;
    feCopy(c, a);
    for (int i = 253; i >= 0; i--) {
        feMul(c, c, c);
        if (i != 2 && i != 4) feMul(c, c, a);
    }
    feCopy(o, c);
}

/* a to the power (p - 5) / 8, for square roots */
static void fePow2523(Fe o, const Fe a) {
    Fe c;
    feCopy(c, a);
    for (int i = 250; i >= 0; i--) {
        feMul(c, c, c);
        if (i != 1) feMul(c, c, a);
    }
    feCopy(o, c);
}

static void edAdd(Fe p[4], Fe q[4]) {
    Fe a, b, c, d, t, e, f, g, h;
    feSub(a, p[1], p[0]);
    feSub(t, q[1], q[0]);
    feMul(a, a, t);
    feAdd(b, p[0], p[1]);
    feAdd(t, q[0], q[1]);
    feMul(b, b, t);
    feMul(c, p[3], q[3]);
    feMul(c, c, edD2);
    feMul(d, p[2], q[2]);
    feAdd(d, d, d);
    feSub(e, b, a);
    feSub(f, d, c);
    feAdd(g, d, c);
    feAdd(h, b, a);
    feMul(p[0], e, f);
    feMul(p[1], h, g);
    feMul(p[2], g, f);
    feMul(p[3], e, h);
}

static void edPack(unsigned char *r, Fe p[4]) {
    Fe tx, ty, zi;
    feInvert(zi, p[2]);
    feMul(tx, p[0], zi);
    feMul(ty, p[1], zi);
    fePack(r, ty);
    r[31] ^= (unsigned char)(feParity(tx) << 7);
}

/* p = s * q, one bit at a time with no branch on the bits */
static void edScalarMult(Fe p[4], Fe q[4], const unsigned char *s) {
    feCopy(p[0], fe0);
    feCopy(p[1], fe1);
    feCopy(p[2], fe1);
    feCopy(p[3], fe0);
    for (int i = 255; i >= 0; i--) {
        int b = (s[i / 8] >> (i & 7)) & 1;
        for (int k = 0; k < 4; k++) feSwap(p[k], q[k], b);
        edAdd(q, p);
        edAdd(p, p);
        for (int k = 0; k < 4; k++) feSwap(p[k], q[k], b);
    }
}

static void edScalarBase(Fe p[4], const unsigned char *s) {
    Fe q[4];
    feCopy(q[0], edX);
    feCopy(q[1], edY);
    feCopy(q[2], fe1);
    feMul(q[3], edX, edY);
    edScalarMult(p, q, s);
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
    Fe p[4];
    edExpand(seed, d);
    edScalarBase(p, d);
    edPack(pub, p);
}

void iris_ed25519_sign(const unsigned char *seed, const char *msg, int64_t len, unsigned char *sig) {
    unsigned char d[64], r[64], h[64], pub[32];
    Fe p[4];
    edExpand(seed, d);
    edScalarBase(p, d);
    edPack(pub, p);
    Sha512 s;
    sha512Init(&s);
    sha512Add(&s, d + 32, 32);
    sha512Add(&s, (const unsigned char *)msg, (size_t)len);
    sha512End(&s, r);
    edReduce(r);
    edScalarBase(p, r);
    edPack(sig, p);
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
}

/* -A from the public key, or -1 when it isn't a point on the curve */
static int edUnpackNeg(Fe r[4], const unsigned char *p) {
    Fe t, chk, num, den, den2, den4, den6;
    feCopy(r[2], fe1);
    feUnpack(r[1], p);
    feMul(num, r[1], r[1]);
    feMul(den, num, edD);
    feSub(num, num, r[2]);
    feAdd(den, r[2], den);
    feMul(den2, den, den);
    feMul(den4, den2, den2);
    feMul(den6, den4, den2);
    feMul(t, den6, num);
    feMul(t, t, den);
    fePow2523(t, t);
    feMul(t, t, num);
    feMul(t, t, den);
    feMul(t, t, den);
    feMul(r[0], t, den);
    feMul(chk, r[0], r[0]);
    feMul(chk, chk, den);
    if (feDiffer(chk, num)) feMul(r[0], r[0], edI);
    feMul(chk, r[0], r[0]);
    feMul(chk, chk, den);
    if (feDiffer(chk, num)) return -1;
    if (feParity(r[0]) == (p[31] >> 7)) feSub(r[0], fe0, r[0]);
    feMul(r[3], r[0], r[1]);
    return 0;
}

int iris_ed25519_verify(const unsigned char *pub, const char *msg, int64_t len, const unsigned char *sig) {
    /* S must be less than L, so a signature has one form */
    for (int i = 31; i >= 0; i--) {
        if (sig[32 + i] < edL[i]) break;
        if (sig[32 + i] > edL[i] || i == 0) return 0;
    }
    Fe p[4], q[4];
    if (edUnpackNeg(q, pub)) return 0;
    unsigned char h[64], t[32];
    Sha512 s;
    sha512Init(&s);
    sha512Add(&s, sig, 32);
    sha512Add(&s, pub, 32);
    sha512Add(&s, (const unsigned char *)msg, (size_t)len);
    sha512End(&s, h);
    edReduce(h);
    edScalarMult(p, q, h);
    edScalarBase(q, sig + 32);
    edAdd(p, q);
    edPack(t, p);
    return iris_const_equal((const char *)sig, 32, (const char *)t, 32);
}

/* X25519 key exchange (RFC 7748), with the field arithmetic of Ed25519
   above. Gives 0 when the result is all zeros: the peer's key was one of
   the few points that give away nothing secret. */
int iris_x25519(const unsigned char *scalar, const unsigned char *point, unsigned char *out) {
    static const Fe f121665 = {0xdb41, 1};
    unsigned char z[32];
    memcpy(z, scalar, 32);
    z[31] = (unsigned char)((z[31] & 127) | 64);
    z[0] &= 248;
    Fe x, a, b, c, d, e, f;
    feUnpack(x, point);
    feCopy(b, x);
    feCopy(a, fe1);
    feCopy(c, fe0);
    feCopy(d, fe1);
    for (int i = 254; i >= 0; i--) {
        int r = (z[i >> 3] >> (i & 7)) & 1;
        feSwap(a, b, r);
        feSwap(c, d, r);
        feAdd(e, a, c);
        feSub(a, a, c);
        feAdd(c, b, d);
        feSub(b, b, d);
        feMul(d, e, e);
        feMul(f, a, a);
        feMul(a, c, a);
        feMul(c, b, e);
        feAdd(e, a, c);
        feSub(a, a, c);
        feMul(b, a, a);
        feSub(c, d, f);
        feMul(a, c, f121665);
        feAdd(a, a, d);
        feMul(c, c, a);
        feMul(a, d, f);
        feMul(d, b, x);
        feMul(b, e, e);
        feSwap(a, b, r);
        feSwap(c, d, r);
    }
    feInvert(c, c);
    feMul(a, a, c);
    fePack(out, a);
    unsigned char any = 0;
    for (int i = 0; i < 32; i++) any |= out[i];
    return any != 0;
}

void iris_x25519_public(const unsigned char *scalar, unsigned char *out) {
    static const unsigned char base[32] = {9};
    iris_x25519(scalar, base, out);
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

/* data XOR the key stream from block counter on */
static void chachaXor(const unsigned char key[32], uint32_t counter, const unsigned char nonce[12],
                      const unsigned char *in, unsigned char *out, size_t n) {
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

/* ChaCha20-Poly1305: the data sealed, then its 16-byte tag, added to out */
void iris_chacha_seal(const unsigned char *key, const unsigned char *nonce, const unsigned char *data, int64_t len,
                      const unsigned char *aad, int64_t aadLen, IrisString *out) {
    unsigned char *ct = malloc((size_t)len + 16);
    if (!ct) {
        outOfMemory();
    }
    chachaXor(key, 1, nonce, data, ct, (size_t)len);
    aeadTag(key, nonce, aad, (size_t)aadLen, ct, (size_t)len, ct + len);
    iris_string_append(out, (const char *)ct, len + 16);
    free(ct);
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
    unsigned char *pt = malloc(n ? n : 1);
    if (!pt) {
        outOfMemory();
    }
    chachaXor(key, 1, nonce, sealed, pt, n);
    iris_string_append(out, (const char *)pt, (int64_t)n);
    free(pt);
    return 1;
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
        double v = strtod(s, &end);
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
        double d = strtod(v, &end);
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

void iris_signal_watch(int sig) {
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

/* The next signal that came, by its number. */
int iris_signal_wait(void) {
    for (;;) {
        unsigned char b;
        ssize_t n = read(sigPipe[0], &b, 1);
        if (n == 1) return b;
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
        rc = zl.deflateInit2(&z->s, level < 0 ? 6 : level > 9 ? 9 : level, 8, bits, 8, 0, zl.version(),
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

int64_t iris_crc32(int64_t crc, const char *p, int64_t n) {
    if (!zReady()) return 0;
    unsigned long c = (unsigned long)crc;
    while (n > 0) {
        unsigned part = n > 0x40000000 ? 0x40000000 : (unsigned)n;
        c = zl.crc32(c, (const unsigned char *)p, part);
        p += part;
        n -= part;
    }
    return (int64_t)c;
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
