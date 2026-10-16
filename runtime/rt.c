/* The Iris runtime: what every program links with. Printing goes through
 * stdio's stdout buffer, which C flushes when the program ends. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE        /* MAP_ANON and SO_NOSIGPIPE on macOS */
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    char *ptr;
    int64_t len, cap;
} IrisString;

/* Debug builds use AddressSanitizer. Leaks are not reported (yet). */
const char *__asan_default_options(void) {
    return "detect_leaks=0:exitcode=2:abort_on_error=0:detect_stack_use_after_return=0";
}

static char panicBuf[4096];
static size_t panicLen;

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
#ifdef __APPLE__
#include <sys/event.h>
#else
#include <sys/epoll.h>
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
    Task *nextSleeper;
    void *asanFake;             /* AddressSanitizer's bookkeeping while switched out */
    const void *asanBottom;     /* the main task's stack, learned on the first switch */
    size_t asanSize;
};

static Task mainTask;
static Task *current = &mainTask;
static Task *readyHead, *readyTail;
static Task *sleepers;          /* sorted by wakeAt */
static Task *deadTask;          /* to free once off its stack */
static int waitingIo;           /* tasks waiting for a file descriptor */
static int poller = -1;
static Task *switchedFrom;      /* the task the last switch left */

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

static void ensurePoller(void) {
    if (poller >= 0) {
        return;
    }
#ifdef __APPLE__
    poller = kqueue();
#else
    poller = epoll_create1(0);
#endif
    if (poller < 0) {
        fflush(stdout);
        fputs("panic: can't start the event loop\n", stderr);
        exit(2);
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
    if (waitingIo == 0 && timeout < 0) {
        fflush(stdout);
        fputs("panic: every task is waiting and nothing can wake them: a deadlock\n", stderr);
        exit(2);
    }
    if (waitingIo > 0) {
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
            Task *t = evs[i].udata;
            waitingIo--;
            makeReady(t);
        }
#else
        struct epoll_event evs[64];
        int ms = timeout < 0 ? -1 : (int)((timeout + 999999) / 1000000);
        int n = epoll_wait(poller, evs, 64, ms);
        for (int i = 0; i < n; i++) {
            Task *t = evs[i].data.ptr;
            waitingIo--;
            makeReady(t);
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
        makeReady(t);
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

static void switchTo(Task *next) {
    Task *prev = current;
    if (next == prev) {
        return;
    }
    current = next;
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
        munmap(deadTask->stack - 4096, deadTask->stackSize + 4096);
        free(deadTask);
        deadTask = NULL;
    }
}

/* Runs the next ready task; the current one must have arranged to be
 * woken (or be ready already). */
static void schedule(void) {
    Task *next;
    while (!(next = takeReady())) {
        pollEvents();
    }
    switchTo(next);
}

static void taskEntry(void) {
    arrived(NULL);
    if (deadTask && deadTask != current) {
        munmap(deadTask->stack - 4096, deadTask->stackSize + 4096);
        free(deadTask);
        deadTask = NULL;
    }
    current->fn(current->arg);
    current->dead = 1;
    deadTask = current;
    schedule();
    abort();                    /* never comes back */
}

/* spawn: a new task that will run fn(arg) */
void iris_spawn(void (*fn)(void *), void *arg) {
    Task *t = calloc(1, sizeof(Task));
    if (!t) {
        outOfMemory();
    }
    char *mem = mmap(NULL, STACK_SIZE + 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mem == MAP_FAILED) {
        outOfMemory();
    }
    mprotect(mem, 4096, PROT_NONE);     /* a guard page: overflowing the stack crashes cleanly */
    t->stack = mem + 4096;
    t->stackSize = STACK_SIZE;
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
static void waitFd(int fd, int write) {
    ensurePoller();
#ifdef __APPLE__
    struct kevent ev;
    EV_SET(&ev, fd, write ? EVFILT_WRITE : EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, current);
    kevent(poller, &ev, 1, NULL, 0, NULL);
#else
    struct epoll_event ev;
    ev.events = (write ? EPOLLOUT : EPOLLIN) | EPOLLONESHOT;
    ev.data.ptr = current;
    if (epoll_ctl(poller, EPOLL_CTL_MOD, fd, &ev) != 0) {
        epoll_ctl(poller, EPOLL_CTL_ADD, fd, &ev);
    }
#endif
    waitingIo++;
    schedule();
}

/* ---------------------------------------------------------------- channels */

typedef struct Waiter Waiter;
struct Waiter {
    Task *task;
    Waiter *next;
};

typedef struct {
    int64_t elemSize, cap, len, head;
    char *buf;
    Waiter *recvWaiters, *sendWaiters;
} Chan;

void *iris_chan_new(int64_t elemSize, int64_t cap, const char *file, int line, int col, const char *fn) {
    if (cap < 1) {
        stop("a channel needs room for at least one value", 43, file, line, col, fn);
    }
    Chan *c = calloc(1, sizeof(Chan));
    c->buf = calloc((size_t)cap, (size_t)(elemSize ? elemSize : 1));
    if (!c || !c->buf) {
        outOfMemory();
    }
    c->elemSize = elemSize;
    c->cap = cap;
    return c;
}

static void wakeOne(Waiter **list) {
    Waiter *w = *list;
    if (w) {
        *list = w->next;
        makeReady(w->task);
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

void iris_chan_send(Chan *c, const void *value) {
    while (c->len == c->cap) {
        Waiter w = {current, c->sendWaiters};
        c->sendWaiters = &w;
        schedule();
        unlinkWaiter(&c->sendWaiters, &w);
    }
    memcpy(c->buf + ((c->head + c->len) % c->cap) * c->elemSize, value, (size_t)c->elemSize);
    c->len++;
    wakeOne(&c->recvWaiters);
}

static void chanTake(Chan *c, void *out) {
    memcpy(out, c->buf + c->head * c->elemSize, (size_t)c->elemSize);
    c->head = (c->head + 1) % c->cap;
    c->len--;
    wakeOne(&c->sendWaiters);
}

void iris_chan_recv(Chan *c, void *out) {
    while (c->len == 0) {
        Waiter w = {current, c->recvWaiters};
        c->recvWaiters = &w;
        schedule();
        unlinkWaiter(&c->recvWaiters, &w);
    }
    chanTake(c, out);
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
            if (chans[i]->len > 0) {
                chanTake(chans[i], outs[i]);
                return i;
            }
        }
        if (deadline >= 0 && nowNs() >= deadline) {
            return n;
        }
        for (int64_t i = 0; i < n; i++) {
            ws[i].task = current;
            ws[i].next = chans[i]->recvWaiters;
            chans[i]->recvWaiters = &ws[i];
        }
        if (deadline >= 0) {
            addSleeper(current, deadline);
        }
        schedule();
        for (int64_t i = 0; i < n; i++) {
            unlinkWaiter(&chans[i]->recvWaiters, &ws[i]);
        }
        if (current->wakeAt) {
            removeSleeper(current);
        }
    }
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

int iris_net_listen(const char *host, int64_t hostLen, int64_t port, int *err) {
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
    if (bind(fd, res->ai_addr, res->ai_addrlen) != 0 || listen(fd, 1024) != 0) {
        *err = netCode(errno);
        close(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    nonBlocking(fd);
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

int iris_net_accept(int fd, int *err) {
    for (;;) {
        int c = accept(fd, NULL, NULL);
        if (c >= 0) {
            nonBlocking(c);
            int one = 1;
            setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            *err = 0;
            return c;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            waitFd(fd, 0);
            continue;
        }
        *err = netCode(errno);
        return -1;
    }
}

int iris_net_dial(const char *host, int64_t hostLen, int64_t port, int *err) {
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
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        if (errno != EINPROGRESS) {
            *err = netCode(errno);
            close(fd);
            freeaddrinfo(res);
            return -1;
        }
        waitFd(fd, 1);
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

/* bytes read, 0 at the end, -code on an error */
int64_t iris_net_read(int fd, char *buf, int64_t len) {
    for (;;) {
        ssize_t n = read(fd, buf, (size_t)len);
        if (n >= 0) {
            return n;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            waitFd(fd, 0);
            continue;
        }
        return -netCode(errno);
    }
}

/* 0 when everything was written, a code on an error */
int iris_net_write(int fd, const char *buf, int64_t len) {
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
            waitFd(fd, 1);
            continue;
        }
        return netCode(errno);
    }
    return 0;
}

void iris_net_close(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}
