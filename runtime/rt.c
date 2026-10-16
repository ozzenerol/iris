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
static void ringPush(uint8_t opcode, int fd, void *buf, uint32_t len, uint32_t opFlags, uint64_t userData) {
    uint32_t tail = *ring.sqTail;
    while (tail - __atomic_load_n(ring.sqHead, __ATOMIC_ACQUIRE) >= ring.sqCount) {
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
    ring.sqArray[i] = i;
    __atomic_store_n(ring.sqTail, tail + 1, __ATOMIC_RELEASE);
    ring.pending++;
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
static void waitFd(int fd, int write) {
#ifdef __linux__
    if (useRing()) {
        ringOp(RING_OP_POLL_ADD, fd, NULL, 0, write ? POLLOUT : POLLIN);
        return;
    }
#endif
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
    schedule();
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
    if (cap < 1) {
        stop("a channel needs room for at least one value", 43, file, line, col, fn);
    }
    Chan *c = calloc(1, sizeof(Chan));
    if (!c) {
        outOfMemory();
    }
    c->buf = calloc((size_t)cap, (size_t)(elemSize ? elemSize : 1));
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

void iris_chan_send(Chan *c, const void *value) {
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
            if (chans[i]->len > 0) {
                chanTake(chans[i], outs[i]);
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
        int64_t added = 0;
        bool arrived = false;
        for (; added < n && !arrived; added++) {
            Chan *c = chans[added];
            spinLock(&c->lock);
            ws[added] = (Waiter){current, &claimed, c->recvWaiters};
            c->recvWaiters = &ws[added];
            arrived = c->len > 0;
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

int iris_net_accept(int fd, int *err) {
    for (;;) {
#ifdef __linux__
        int c;
        if (useRing()) {
            c = ringOp(RING_OP_ACCEPT, fd, NULL, 0, SOCK_NONBLOCK | SOCK_CLOEXEC);
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
    fdForget(fd);
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
#ifdef __linux__
    if (useRing()) {
        for (;;) {
            int n = ringOp(RING_OP_RECV, fd, buf, len > 0x7fffffff ? 0x7fffffff : (uint32_t)len, 0);
            if (n >= 0) {
                return n;
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

enum { J_INT = 1, J_UINT, J_FLOAT, J_BOOL, J_STR, J_STRING, J_STRUCT, J_SLICE, J_ARRAY, J_ENUM };

typedef struct JsonType {
    int32_t kind, size;
    int64_t count;              /* fields, array items or enum values */
    const void *items;          /* JsonField[], the element's JsonType, or JsonName[] */
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
    case J_SLICE: case J_ARRAY: {
        const JsonType *et = t->items;
        const char *base = p;
        int64_t n = t->count;
        if (t->kind == J_SLICE) {
            base = ((const Slice *)p)->ptr;
            n = ((const Slice *)p)->len;
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
    case J_STRING: case J_SLICE: return true;
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
    bool slice = t->kind == J_SLICE;
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
    if (slice) {
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
    case J_SLICE: case J_ARRAY:
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
