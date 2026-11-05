/* iris lsp: a language server for editors, over standard input and output.
 *
 * It answers diagnostics, hover, go to definition, document symbols,
 * completion (of names, after a dot and after @; a module that isn't
 * imported yet completes and adds its import), quick fixes that add a
 * missing import, and formatting. The editor sends whole documents
 * (full sync); open documents are read instead of the files on disk.
 *
 * Each request that needs the compiler runs in a child process (fork): the
 * child loads and checks the program with the documents as they are in the
 * editor, writes the replies to a pipe and exits. A compiler crash or a
 * hang then costs one reply, never the server. */

#define _XOPEN_SOURCE 700         /* realpath, strndup */
#include "iris.h"

#include <ctype.h>
#include <dirent.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---------------------------------------------------------------- JSON */

typedef struct J J;
struct J {
    char type;          /* o object, a array, s string, n number, t true, f false, z null */
    char *key;          /* in an object */
    char *str;
    double num;
    J *child, *next;
};

static void ws(const char **p) {
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') (*p)++;
}

static char *jstring(const char **p) {
    StrBuf b = {0};
    (*p)++;
    while (**p && **p != '"') {
        char c = *(*p)++;
        if (c != '\\') {
            sbPutc(&b, c);
            continue;
        }
        char e = *(*p)++;
        switch (e) {
        case 'n': sbPutc(&b, '\n'); break;
        case 't': sbPutc(&b, '\t'); break;
        case 'r': sbPutc(&b, '\r'); break;
        case 'b': sbPutc(&b, '\b'); break;
        case 'f': sbPutc(&b, '\f'); break;
        case 'u': {
            unsigned v = 0;
            for (int i = 0; i < 4 && **p; i++) {
                char h = *(*p)++;
                v = v * 16 + (unsigned)(isdigit((unsigned char)h) ? h - '0' : (tolower((unsigned char)h) - 'a' + 10));
            }
            if (v >= 0xd800 && v < 0xdc00 && (*p)[0] == '\\' && (*p)[1] == 'u') {
                unsigned lo = 0;
                for (int i = 2; i < 6; i++) {
                    char h = (*p)[i];
                    lo = lo * 16 + (unsigned)(isdigit((unsigned char)h) ? h - '0' : (tolower((unsigned char)h) - 'a' + 10));
                }
                v = 0x10000 + ((v - 0xd800) << 10) + (lo - 0xdc00);
                *p += 6;
            }
            if (v < 0x80) {
                sbPutc(&b, (char)v);
            } else if (v < 0x800) {
                sbPutc(&b, (char)(0xc0 | (v >> 6)));
                sbPutc(&b, (char)(0x80 | (v & 0x3f)));
            } else if (v < 0x10000) {
                sbPutc(&b, (char)(0xe0 | (v >> 12)));
                sbPutc(&b, (char)(0x80 | ((v >> 6) & 0x3f)));
                sbPutc(&b, (char)(0x80 | (v & 0x3f)));
            } else {
                sbPutc(&b, (char)(0xf0 | (v >> 18)));
                sbPutc(&b, (char)(0x80 | ((v >> 12) & 0x3f)));
                sbPutc(&b, (char)(0x80 | ((v >> 6) & 0x3f)));
                sbPutc(&b, (char)(0x80 | (v & 0x3f)));
            }
            break;
        }
        default: sbPutc(&b, e); break;
        }
    }
    if (**p == '"') (*p)++;
    sbPutc(&b, 0);
    return b.buf;
}

static J *jparse(const char **p) {
    ws(p);
    J *j = calloc(1, sizeof(J));
    if (!j) exit(2);
    char c = **p;
    if (c == '{' || c == '[') {
        j->type = c == '{' ? 'o' : 'a';
        (*p)++;
        J **tail = &j->child;
        for (;;) {
            ws(p);
            if (**p == '}' || **p == ']') {
                (*p)++;
                break;
            }
            if (!**p) break;
            char *key = NULL;
            if (j->type == 'o') {
                key = jstring(p);
                ws(p);
                if (**p == ':') (*p)++;
            }
            J *v = jparse(p);
            v->key = key;
            *tail = v;
            tail = &v->next;
            ws(p);
            if (**p == ',') (*p)++;
        }
    } else if (c == '"') {
        j->type = 's';
        j->str = jstring(p);
    } else if (c == 't' || c == 'f' || c == 'n') {
        j->type = c == 'n' ? 'z' : c;
        while (isalpha((unsigned char)**p)) (*p)++;
    } else {
        j->type = 'n';
        char *end;
        j->num = strtod(*p, &end);
        *p = end > *p ? end : *p + 1;
    }
    return j;
}

static void jfree(J *j) {
    while (j) {
        J *n = j->next;
        jfree(j->child);
        free(j->key);
        free(j->str);
        free(j);
        j = n;
    }
}

static J *jget(J *o, const char *key) {
    for (J *c = o ? o->child : NULL; c; c = c->next) {
        if (c->key && strcmp(c->key, key) == 0) return c;
    }
    return NULL;
}

static const char *jstr(J *o, const char *key) {
    J *v = jget(o, key);
    return v && v->type == 's' ? v->str : NULL;
}

static void putJsonString(StrBuf *b, const char *s, size_t n) {
    sbPutc(b, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': sbPuts(b, "\\\""); break;
        case '\\': sbPuts(b, "\\\\"); break;
        case '\n': sbPuts(b, "\\n"); break;
        case '\r': sbPuts(b, "\\r"); break;
        case '\t': sbPuts(b, "\\t"); break;
        default:
            if (c < 0x20) {
                sbPrintf(b, "\\u%04x", c);
            } else {
                sbPutc(b, (char)c);
            }
        }
    }
    sbPutc(b, '"');
}

static void putStr(StrBuf *b, const char *s) { putJsonString(b, s, strlen(s)); }

/* The id of a request, as it came: a number or a string. */
static void putId(StrBuf *b, J *id) {
    if (!id) {
        sbPuts(b, "null");
    } else if (id->type == 's') {
        putStr(b, id->str);
    } else {
        sbPrintf(b, "%.0f", id->num);
    }
}

/* ---------------------------------------------------------------- transport */

static char *readMessage(void) {
    size_t len = 0;
    bool have = false;
    char line[1024];
    for (;;) {
        size_t n = 0;
        int c;
        while ((c = getchar()) != EOF && c != '\n') {
            if (n + 1 < sizeof line) line[n++] = (char)c;
        }
        if (c == EOF) return NULL;
        line[n] = 0;
        if (n && line[n - 1] == '\r') line[--n] = 0;
        if (n == 0) {
            if (have) break;
            continue;
        }
        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            len = (size_t)strtoul(line + 15, NULL, 10);
            have = true;
        }
    }
    char *body = malloc(len + 1);
    if (!body) exit(2);
    if (fread(body, 1, len, stdin) != len) {
        free(body);
        return NULL;
    }
    body[len] = 0;
    return body;
}

static void sendMsg(const char *body, size_t n) {
    printf("Content-Length: %zu\r\n\r\n", n);
    fwrite(body, 1, n, stdout);
    fflush(stdout);
}

/* ---------------------------------------------------------------- documents */

typedef struct {
    char *uri, *path, *text;
    size_t len;
} Doc;

static Doc docs[256];
static int ndocs;
static char *diagUris[512];     /* files that have diagnostics in the editor now */
static int ndiagUris;

static char *pathOf(const char *uri) {
    if (strncmp(uri, "file://", 7) != 0) return strdup(uri);
    StrBuf b = {0};
    for (const char *p = uri + 7; *p; p++) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char h[3] = {p[1], p[2], 0};
            sbPutc(&b, (char)strtol(h, NULL, 16));
            p += 2;
        } else {
            sbPutc(&b, *p);
        }
    }
    sbPutc(&b, 0);
    return b.buf;
}

static char *uriOf(const char *path) {
    StrBuf b = {0};
    sbPuts(&b, "file://");
    for (const char *p = path; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || strchr("/-_.~", c)) {
            sbPutc(&b, (char)c);
        } else {
            sbPrintf(&b, "%%%02X", c);
        }
    }
    sbPutc(&b, 0);
    return b.buf;
}

static Doc *findDoc(const char *uri) {
    for (int i = 0; i < ndocs; i++) {
        if (strcmp(docs[i].uri, uri) == 0) return &docs[i];
    }
    return NULL;
}

static void setDoc(const char *uri, const char *text) {
    Doc *d = findDoc(uri);
    if (!d) {
        if (ndocs == (int)(sizeof docs / sizeof docs[0])) return;
        d = &docs[ndocs++];
        d->uri = strdup(uri);
        d->path = pathOf(uri);
    } else {
        free(d->text);
    }
    d->text = strdup(text);
    d->len = strlen(text);
}

static void closeDoc(const char *uri) {
    Doc *d = findDoc(uri);
    if (!d) return;
    free(d->uri);
    free(d->path);
    free(d->text);
    *d = docs[--ndocs];
}

/* Paths are compared without "./" parts. */
static bool samePath(const char *a, const char *b) {
    char ra[4096], rb[4096];
    const char *x = realpath(a, ra) ? ra : a;
    const char *y = realpath(b, rb) ? rb : b;
    return strcmp(x, y) == 0;
}

/* A program made up to look at one std module: import json. */
static const char *viewPath = "/iris-lsp/std-view.iris";
static char viewText[256];

static const char *readDoc(const char *path, size_t *len) {
    if (viewText[0] && strcmp(path, viewPath) == 0) {
        *len = strlen(viewText);
        return viewText;
    }
    for (int i = 0; i < ndocs; i++) {
        if (samePath(docs[i].path, path)) {
            *len = docs[i].len;
            return docs[i].text;
        }
    }
    return NULL;
}

/* The .iris files open in a folder, saved or not. */
static int docsIn(const char *dir, const char **names, int max) {
    int n = 0;
    for (int i = 0; i < ndocs && n < max; i++) {
        const char *path = docs[i].path, *slash = strrchr(path, '/');
        size_t len = strlen(path);
        if (!slash || len < 5 || strcmp(path + len - 5, ".iris") != 0 || slash[1] == '.') continue;
        char d[4096];
        size_t dl = slash == path ? 1 : (size_t)(slash - path);
        if (dl >= sizeof d) continue;
        memcpy(d, path, dl);
        d[dl] = 0;
        if (samePath(d, dir)) names[n++] = slash + 1;
    }
    return n;
}

/* ---------------------------------------------------------------- positions */

/* LSP counts characters in UTF-16 units, Iris columns in bytes. */
static int utf16Col(const char *lineStart, const char *end, int col) {
    int u = 0;
    const char *p = lineStart;
    for (int i = 1; i < col && p < end && *p != '\n'; i++, p++) {
        unsigned char c = (unsigned char)*p;
        if ((c & 0xc0) == 0x80) continue;
        u += c >= 0xf0 ? 2 : 1;
    }
    return u;
}

static const char *lineStartOf(const char *text, size_t len, int line) {
    const char *p = text, *end = text + len;
    for (int l = 1; l < line && p < end; p++) {
        if (*p == '\n') l++;
    }
    return p;
}

/* 1-based byte column of a UTF-16 character position on a line. */
static int byteCol(const char *text, size_t len, int line, int character) {
    const char *p = lineStartOf(text, len, line), *end = text + len;
    int u = 0, col = 1;
    while (p < end && *p != '\n' && u < character) {
        unsigned char c = (unsigned char)*p;
        int n = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
        u += c >= 0xf0 ? 2 : 1;
        p += n;
        col += n;
    }
    return col;
}

static void putPos(StrBuf *b, Source *src, int line, int col) {
    const char *ls = src ? lineStartOf(src->text, src->len, line) : NULL;
    int c = src ? utf16Col(ls, src->text + src->len, col) : col - 1;
    sbPrintf(b, "{\"line\":%d,\"character\":%d}", line > 0 ? line - 1 : 0, c);
}

static int wordLen(Source *src, int line, int col) {
    const char *p = lineStartOf(src->text, src->len, line) + col - 1, *end = src->text + src->len;
    int n = 0;
    while (p + n < end && (isalnum((unsigned char)p[n]) || p[n] == '_')) n++;
    return n ? n : 1;
}

static void putRange(StrBuf *b, Source *src, int line, int col, int len) {
    sbPuts(b, "{\"start\":");
    putPos(b, src, line, col);
    sbPuts(b, ",\"end\":");
    putPos(b, src, line, col + len);
    sbPutc(b, '}');
}

/* ---------------------------------------------------------------- analysis (in the child) */

typedef struct {
    Source *src;
    int line, col;
    char *msg;
    bool warning;       /* a variable that is not used, from the server; else a compiler error */
} Diag;

static Diag diags[512];
static int ndiags;

static void collect(Source *src, int line, int col, const char *msg) {
    if (ndiags < (int)(sizeof diags / sizeof diags[0])) {
        diags[ndiags++] = (Diag){src, line, col, strdup(msg), false};
    }
}

static Arena gA;

static Program *analyze(const char *path) {
    gDiagHook = collect;
    gReadHook = readDoc;
    gFolderHook = docsIn;
    gTesting = gWithTests = true;     /* no main needed; tests are checked */
    gComptime = 1;                      /* comptime values stand in as zeros, as in the helper */
    gErrors = 0;
    Program *prog = loadProgram(&gA, path, NULL, 0);
    if (prog) {
        checkProgram(&gA, prog);
    }
    return prog;
}

/* The program's files' sources, by path. */
static Source *sourceOf(Program *prog, const char *path) {
    for (Module *m = prog ? prog->modules : NULL; m; m = m->next) {
        for (int f = 0; f < m->files.len; f++) {
            Node *file = m->files.items[f];
            if (file->src && samePath(file->src->path, path)) return file->src;
        }
    }
    return NULL;
}

/* Where a standard library file can be opened: a copy in the cache. */
static void stdCacheDir(char *dir, size_t size) {
    const char *base = getenv("XDG_CACHE_HOME");
    if (base && *base) {
        snprintf(dir, size, "%s/iris/std-%s", base, IRIS_VERSION);
    } else {
        snprintf(dir, size, "%s/.cache/iris/std-%s", getenv("HOME") ? getenv("HOME") : "/tmp", IRIS_VERSION);
    }
}

/* A copy in the cache as the compiler names it, std/json/json.iris, or NULL. */
static char *stdPathOf(const char *path) {
    char dir[4096], real[4096], rdir[4096];
    stdCacheDir(dir, sizeof dir);
    if (!realpath(dir, rdir) || !realpath(path, real)) return NULL;
    size_t n = strlen(rdir);
    if (strncmp(real, rdir, n) != 0 || real[n] != '/') return NULL;
    char std[4200];
    snprintf(std, sizeof std, "std/%s", real + n + 1);
    return stdSource(std) ? strdup(std) : NULL;
}

/* Whether the file at path holds exactly text. */
static bool sameFileText(const char *path, const char *text) {
    struct stat st;
    size_t n = strlen(text);
    if (stat(path, &st) != 0 || (size_t)st.st_size != n) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char *buf = malloc(n + 1);
    bool same = buf && fread(buf, 1, n, f) == n && memcmp(buf, text, n) == 0;
    free(buf);
    fclose(f);
    return same;
}

static char *stdCopy(const char *path) {
    const char *text = stdSource(path);
    if (!text) return NULL;
    char dir[4096];
    stdCacheDir(dir, sizeof dir);
    char full[4200];
    snprintf(full, sizeof full, "%s/%s", dir, path + 4);
    /* a build with other std files but the same version rewrites it */
    if (!sameFileText(full, text)) {
        /* make the folders, then the file */
        for (char *s = full + 1; *s; s++) {
            if (*s == '/') {
                *s = 0;
                mkdir(full, 0755);
                *s = '/';
            }
        }
        writeText(full, text, strlen(text));
    }
    return strdup(full);
}

static char *fileUri(const char *path) {
    for (int i = 0; i < ndocs; i++) {
        if (samePath(docs[i].path, path)) {
            return strdup(docs[i].uri);     /* as the editor named it, symlinks and all */
        }
    }
    if (strncmp(path, "std/", 4) == 0) {
        char *copy = stdCopy(path);
        if (!copy) return NULL;
        char *u = uriOf(copy);
        free(copy);
        return u;
    }
    char full[4096];
    return uriOf(realpath(path, full) ? full : path);
}

/* The names the compiler knows itself have no file. builtin.iris in the
 * cache shows them, so hover and go to definition have a place to go:
 * Error, e.code, error(...), print, Mutex.lock... */
typedef struct {
    const char *key;        /* Error, Error.code, print */
    const char *doc;        /* comment lines above the declaration, or a blank line */
    const char *decl;
} Builtin;

static const Builtin builtinDecls[] = {
    {NULL, "// The names Iris knows without an import. The compiler makes them; this\n"
           "// file only shows them.\n\n", NULL},
    {"Error", "// What a function that returns !T gives when it fails. catch e { } and\n"
              "// try pass it on; print(\"{e.message}\") shows it.\n", "struct Error {"},
    {"Error.code", "    // What went wrong: a value of the enum named in error(...),\n"
                   "    // compared and matched on as that enum's values.\n", "    error code code"},
    {"Error.message", "    // What happened, for people: \"no port given\". Empty when error(...)\n"
                      "    // had no message.\n", "    str message"},
    {NULL, NULL, "}\n"},
    {"error", "// Makes an Error from an enum value and, if wanted, a message, which can\n"
              "// have values in braces: return error(ParseError.empty, \"no port given\")\n",
     "Error error(enum code, str message)\n"},
    {"print", "// Writes the text and a new line to standard output. Values go in braces:\n"
              "// print(\"{n} items\")\n", "void print(str text)\n"},
    {"format", "// Makes a String from text with values in braces. Free it when done.\n",
     "String format(str text)\n"},
    {"panic", "// Stops the program with the message, the file and the line.\n",
     "@noreturn void panic(str message)\n"},
    {"alloc", "// n new items of type T, all at zero: u8[] buf = alloc(u8, 4096). Give the\n"
              "// memory back with free.\n", "T[] alloc(type T, i64 n)\n"},
    {"sizeof", "// The bytes a value of type T takes, padding included, known when compiling:\n"
               "// sizeof(Header).\n", "i64 sizeof(type T)\n"},
    {"free", "// Gives back the memory of a slice or pointer from alloc. A String is\n"
             "// freed with s.free().\n", "void free(T[] memory)\n"},
    {"sleep", "// Waits the time: sleep(200ms). Other tasks of the thread keep running.\n",
     "void sleep(Duration d)\n"},
    {"after", "// A select arm that runs when nothing came in the time: after(5s) => ...\n",
     "after(Duration d)\n"},
    {"expect", "// In a test block: checks the condition and, when it is false, reports the\n"
               "// file, the line and the message, then goes on.\n",
     "void expect(bool cond, str message)\n"},
    {"str", "// Text that is borrowed: a slice of bytes, UTF-8.\n", "type str = u8[]\n"},
    {"String", "// Text that is owned and can grow. Free it with s.free().\n", "struct String\n"},
    {"Duration", "// A time in nanoseconds, written 250ms, 2s, 10m, 1h. Prints as 1m30s.\n",
     "type Duration = i64\n"},
    {"Mutex", "// Guards data that takes more than one step to change. Can't be copied.\n",
     "struct Mutex {"},
    {"Mutex.lock", "    // Takes the lock, waiting while another task holds it.\n", "    void lock()"},
    {"Mutex.unlock", "    // Gives the lock back. Unlocking a Mutex that isn't locked stops the program.\n",
     "    void unlock()"},
    {"Mutex.tryLock", "    // Takes the lock if it is free, and says whether it did.\n", "    bool tryLock()"},
    {NULL, NULL, "}\n"},
    {"Thread", "// What thread f(x) gives.\n", "struct Thread {"},
    {"Thread.join", "    // Waits for the thread to end.\n", "    void join()"},
    {NULL, NULL, "}\n"},
    {"Atomic", "// An integer or a bool that threads change safely. Can't be copied.\n", "struct Atomic[T] {"},
    {"Atomic.load", "    // The value.\n", "    T load()"},
    {"Atomic.store", "    // Sets it.\n", "    void store(T v)"},
    {"Atomic.add", "    // Adds n and gives the new value. Not for Atomic[bool].\n", "    T add(T n)"},
    {"Atomic.sub", "    // Takes away n and gives the new value. Not for Atomic[bool].\n", "    T sub(T n)"},
    {"Atomic.swap", "    // Sets it and gives the old value.\n", "    T swap(T v)"},
    {"Atomic.cas", "    // Sets it to new if it is old, and says whether it did.\n", "    bool cas(T old, T new)"},
    {NULL, NULL, "}\n"},
};

static void builtinText(StrBuf *b) {
    for (size_t i = 0; i < sizeof builtinDecls / sizeof builtinDecls[0]; i++) {
        if (builtinDecls[i].doc) sbPuts(b, builtinDecls[i].doc);
        if (builtinDecls[i].decl) {
            sbPuts(b, builtinDecls[i].decl);
            sbPutc(b, '\n');
        }
    }
}

/* Where builtin.iris is, written there the first time. */
static void builtinPath(char *path, size_t size) {
    char dir[4096];
    stdCacheDir(dir, sizeof dir);
    snprintf(path, size, "%s/builtin.iris", dir);
    struct stat st;
    if (stat(path, &st) == 0) return;
    for (char *s = path + 1; *s; s++) {
        if (*s == '/') {
            *s = 0;
            mkdir(path, 0755);
            *s = '/';
        }
    }
    StrBuf b = {0};
    builtinText(&b);
    writeText(path, b.buf, b.len);
    sbFree(&b);
}

static bool isBuiltinFile(const char *path) {
    char bp[4200], real[4096], rbp[4096];
    builtinPath(bp, sizeof bp);
    return realpath(path, real) && realpath(bp, rbp) && strcmp(real, rbp) == 0;
}

/* The line of a built-in name in builtin.iris, from 1, and the column of its
 * name; 0 when there is none. */
static int builtinLine(const char *key, int *col) {
    int line = 1;
    for (size_t i = 0; i < sizeof builtinDecls / sizeof builtinDecls[0]; i++) {
        for (const char *p = builtinDecls[i].doc; p && *p; p++) line += *p == '\n';
        if (builtinDecls[i].key && strcmp(builtinDecls[i].key, key) == 0) {
            const char *dot = strchr(key, '.');
            const char *name = dot ? dot + 1 : key;
            const char *d = builtinDecls[i].decl;
            size_t n = strlen(name);
            *col = 1;
            /* the last one: in "error code code" the name is the second */
            for (const char *p = d; (p = strstr(p, name)); p++) {
                bool start = p == d || !(isalnum((unsigned char)p[-1]) || p[-1] == '_');
                bool end = !(isalnum((unsigned char)p[n]) || p[n] == '_');
                if (start && end) *col = (int)(p - d) + 1;
            }
            return line;
        }
        for (const char *p = builtinDecls[i].decl; p && *p; p++) line += *p == '\n';
        line += builtinDecls[i].decl != NULL;
    }
    return 0;
}

static const Builtin *builtinOf(const char *key) {
    for (size_t i = 0; i < sizeof builtinDecls / sizeof builtinDecls[0]; i++) {
        if (builtinDecls[i].key && strcmp(builtinDecls[i].key, key) == 0) return &builtinDecls[i];
    }
    return NULL;
}

/* publishDiagnostics for every file with errors, and empty ones for the
 * files that had errors before; then a record of the new list. */
static void putDiagnostics(StrBuf *out, Program *prog, const char *mainPath) {
    char *uris[512];
    int nuris = 0;
    for (int i = 0; i < ndiags; i++) {
        if (!diags[i].src || strncmp(diags[i].src->path, "std/", 4) == 0) continue;
        char *u = fileUri(diags[i].src->path);
        bool seen = false;
        for (int k = 0; k < nuris; k++) seen = seen || strcmp(uris[k], u) == 0;
        if (seen || nuris == 512) {
            free(u);
            continue;
        }
        uris[nuris++] = u;
    }
    for (int k = 0; k < nuris; k++) {
        StrBuf b = {0};
        sbPuts(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":");
        putStr(&b, uris[k]);
        sbPuts(&b, ",\"diagnostics\":[");
        bool first = true;
        for (int i = 0; i < ndiags; i++) {
            if (!diags[i].src || strncmp(diags[i].src->path, "std/", 4) == 0) continue;
            char *u = fileUri(diags[i].src->path);
            bool here = strcmp(u, uris[k]) == 0;
            free(u);
            if (!here) continue;
            if (!first) sbPutc(&b, ',');
            first = false;
            sbPuts(&b, "{\"range\":");
            putRange(&b, diags[i].src, diags[i].line, diags[i].col, wordLen(diags[i].src, diags[i].line, diags[i].col));
            /* a warning is "unnecessary": the editor shows the name faded */
            sbPuts(&b, diags[i].warning ? ",\"severity\":2,\"tags\":[1]" : ",\"severity\":1");
            sbPuts(&b, ",\"source\":\"iris\",\"message\":");
            putStr(&b, diags[i].msg);
            sbPutc(&b, '}');
        }
        sbPuts(&b, "]}}");
        sbPutn(out, b.buf, b.len);
        sbPutc(out, 0);
        sbFree(&b);
    }
    /* files that had errors and have none now */
    for (int i = 0; i < ndiagUris; i++) {
        bool still = false;
        for (int k = 0; k < nuris; k++) still = still || strcmp(diagUris[i], uris[k]) == 0;
        if (still) continue;
        StrBuf b = {0};
        sbPuts(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":");
        putStr(&b, diagUris[i]);
        sbPuts(&b, ",\"diagnostics\":[]}}");
        sbPutn(out, b.buf, b.len);
        sbPutc(out, 0);
        sbFree(&b);
    }
    sbPuts(out, "@uris");
    for (int k = 0; k < nuris; k++) {
        sbPutc(out, '\n');
        sbPuts(out, uris[k]);
        free(uris[k]);
    }
    sbPutc(out, 0);
    (void)prog;
    (void)mainPath;
}

/* ---------------------------------------------------------------- finding things */

typedef struct {
    Source *src;
    int line, col;
    Node *best;         /* the name under the cursor */
    Node *parent;
    Node *func;         /* the function it is in */
} Find;

/* The length of a name as written: a checked copy of a generic is named
 * biggest[i64], written biggest. */
static int writtenLen(const char *name) {
    const char *b = name ? strchr(name, '[') : NULL;
    return name ? (int)(b ? (size_t)(b - name) : strlen(name)) : 0;
}

static int nameLen(Node *n) {
    const char *dot = n->name ? strrchr(n->name, '.') : NULL;
    return n->name ? writtenLen(dot ? dot + 1 : n->name) : 0;
}

/* The column of a method's name in its declaration: area in Square.area. */
static int methodNameCol(Source *src, Node *fn) {
    if (!src || !src->text || !fn->name) return fn->col;
    const char *ls = lineStartOf(src->text, src->len, fn->line), *end = src->text + src->len;
    size_t n = (size_t)writtenLen(fn->name);
    int depth = 0;
    for (const char *p = ls + fn->col - 1; p < end && *p != '\n' && *p != '('; p++) {
        if (*p == '[') depth++;
        if (*p == ']') depth--;
        if (*p == '.' && depth == 0 && (size_t)(end - p - 1) >= n && strncmp(p + 1, fn->name, n) == 0) {
            return (int)(p + 1 - ls) + 1;
        }
    }
    return fn->col;
}

/* Where a name is written on its line, from col on: the e of catch e, whose
 * node sits at the catch. */
static int wordCol(Source *src, int line, int col, const char *name) {
    if (!src || !src->text || !name) return col;
    const char *ls = lineStartOf(src->text, src->len, line), *end = src->text + src->len;
    size_t n = (size_t)writtenLen(name);
    for (const char *p = ls + col - 1; p + n <= end && *p != '\n'; p++) {
        if (strncmp(p, name, n) != 0) continue;
        if (p > ls && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) continue;
        if (p + n < end && (isalnum((unsigned char)p[n]) || p[n] == '_')) continue;
        return (int)(p - ls) + 1;
    }
    return col;
}

/* Where the name of a node is written: its first column and its length.
 * False for a node that has no name in the text. */
static bool nameSpan(Source *src, Node *n, Node *parent, int *start, int *len) {
    /* self is written as the receiver's type, Service.add: the type is what's there */
    if (!n->name || (n->kind == N_PARAM && (n->flags & F_SELF))) return false;
    if (!(n->kind == N_IDENT || n->kind == N_FIELD_ACCESS || n->kind == N_FUNC || n->kind == N_STRUCT ||
          n->kind == N_ENUM || n->kind == N_CONST || n->kind == N_VAR || n->kind == N_PARAM || n->kind == N_FIELD ||
          n->kind == N_ENUM_MEMBER || n->kind == N_TYPEDEF || n->kind == N_INTERFACE || n->kind == N_T_NAME ||
          n->kind == N_FIELD_INIT || n->kind == N_ENUM_DOT || n->kind == N_TYPE_PARAM)) {
        return false;
    }
    *start = n->col;
    *len = nameLen(n);
    const char *dot = n->kind == N_T_NAME ? strrchr(n->name, '.') : NULL;
    if (dot) *start += (int)(dot + 1 - n->name);     /* sqlite.Db: the Db */
    if (n->kind == N_FUNC && (n->b || (n->flags & F_INSTANCE))) *start = methodNameCol(src, n);
    if (n->kind == N_ENUM_DOT) (*len)++;            /* .admin, from the dot */
    if (parent && parent->kind == N_CATCH && parent->d == n) {
        *start = wordCol(src, n->line, n->col + 5, n->name);
        if (parent->ival) *len = 0;                 /* catch return: the error has no name */
    }
    return true;
}

static void walk(Find *f, Node *n, Node *parent, Node *fn) {
    if (!n) return;
    int start, len;
    if (n->src == f->src && n->line == f->line && nameSpan(f->src, n, parent, &start, &len) && f->col >= start &&
        f->col < start + len) {
        f->best = n;
        f->parent = parent;
        f->func = fn;
    }
    if (n->kind == N_FUNC) fn = n;
    walk(f, n->a, n, fn);
    walk(f, n->b, n, fn);
    walk(f, n->c, n, fn);
    walk(f, n->d, n, fn);
    for (int i = 0; i < n->list.len; i++) walk(f, n->list.items[i], n, fn);
    for (int i = 0; i < n->list2.len; i++) walk(f, n->list2.items[i], n, fn);
}

static Node *nodeAt(Program *prog, Source *src, int line, int col, Node **parent, Node **func, bool copies) {
    Find f = {src, line, col, NULL, NULL, NULL};
    for (Module *m = prog->modules; m; m = m->next) {
        for (int i = 0; i < m->files.len; i++) {
            Node *file = m->files.items[i];
            if (file->src == src) walk(&f, file, NULL, NULL);
        }
    }
    for (int i = 0; copies && i < prog->instances.len; i++) walk(&f, prog->instances.items[i], NULL, NULL);
    if (parent) *parent = f.parent;
    if (func) *func = f.func;
    return f.best;
}

/* What a name under the cursor stands for: its declaration. */
static Node *declOf(Node *n, Node *parent) {
    switch (n->kind) {
    case N_IDENT: case N_FIELD_ACCESS:
        if (n->ref) return n->ref;
        if (parent && parent->kind == N_CALL && parent->a == n && parent->ref) return parent->ref;
        if (parent && parent->kind == N_STRUCT_LIT && parent->a == n && parent->type && parent->type->decl) {
            return parent->type->decl;      /* Point{...} */
        }
        if (parent && parent->kind == N_DEFINE && n->type) return n;    /* p := ... */
        if (parent && parent->kind == N_CATCH && parent->d == n) return n;  /* catch e */
        if (parent && parent->kind == N_FOR) {
            for (int i = 0; i < parent->list.len; i++) {
                if (parent->list.items[i] == n) return n;               /* for x in xs */
            }
        }
        return NULL;
    case N_ENUM_DOT:
        /* .admin, where the enum is known from the other side */
        if (n->type && n->type->kind == TY_ENUM && n->type->decl) {
            Node *e = n->type->decl;
            for (int i = 0; i < e->list.len; i++) {
                Node *m = e->list.items[i];
                if (m->name && strcmp(m->name, n->name) == 0) return m;
            }
        }
        return NULL;
    case N_T_NAME:
        return n->type && n->type->decl ? n->type->decl : NULL;
    case N_FIELD_INIT:
        /* Point{x: 1}: the field x */
        if (parent && parent->type && parent->type->kind == TY_STRUCT && parent->type->decl) {
            Node *s = parent->type->decl;
            for (int i = 0; i < s->list.len; i++) {
                Node *f = s->list.items[i];
                if (f->kind == N_FIELD && f->name && strcmp(f->name, n->name) == 0) return f;
            }
        }
        return NULL;
    default:
        return n;       /* the declaration itself */
    }
}

/* The type declared with this name in a module. */
static Node *typeIn(Module *m, const char *name, size_t len) {
    for (int f = 0; m && f < m->files.len; f++) {
        Node *file = m->files.items[f];
        for (int i = 0; i < file->list.len; i++) {
            Node *d = file->list.items[i];
            bool isType = d->kind == N_STRUCT || d->kind == N_ENUM || d->kind == N_INTERFACE || d->kind == N_TYPEDEF;
            if (isType && d->name && strlen(d->name) == len && strncmp(d->name, name, len) == 0) return d;
        }
    }
    return NULL;
}

/* A type by its name where the checker left no link: one passed as a value
 * (flags.parse(Options), json.decode(models.User, text)), the parts of
 * List[i64] and dyn Shape, and a function's type parameters. */
static Node *typeNamed(Program *prog, Source *src, Node *n, Node *fn) {
    if (!prog || !n->name) return NULL;
    if (n->kind != N_IDENT && n->kind != N_T_NAME && n->kind != N_FIELD_ACCESS) return NULL;
    char full[256];
    const char *name = n->name;
    if (n->kind == N_FIELD_ACCESS) {
        if (!n->a || n->a->kind != N_IDENT || !n->a->name) return NULL;
        snprintf(full, sizeof full, "%s.%s", n->a->name, n->name);
        name = full;
    }
    const char *dot = strrchr(name, '.');
    if (dot) {
        for (Module *m = prog->modules; m; m = m->next) {
            if (m != prog->main && strlen(m->name) == (size_t)(dot - name) && strncmp(m->name, name, dot - name) == 0) {
                return typeIn(m, dot + 1, strlen(dot + 1));
            }
        }
        return NULL;
    }
    if (fn) {
        for (int i = 0; i < fn->list2.len; i++) {
            Node *tp = fn->list2.items[i];
            if (tp->name && strcmp(tp->name, name) == 0) return tp;
        }
        if (fn->b && fn->b->kind == N_T_GENERIC) {
            /* List[T].push: the receiver's T */
            for (int i = 0; i < fn->b->list.len; i++) {
                Node *tp = fn->b->list.items[i];
                if (tp != n && tp->name && strcmp(tp->name, name) == 0) return tp;
            }
        }
    }
    for (Module *m = prog->modules; m; m = m->next) {
        for (int f = 0; f < m->files.len; f++) {
            if (m->files.items[f]->src != src) continue;
            Node *d = typeIn(m, name, strlen(name));
            if (d) return d;
        }
    }
    return typeIn(prog->core, name, strlen(name));
}

static void putTypeName(StrBuf *b, Type *t) {
    sbPuts(b, t && t->name ? t->name : "?");
}

/* How a declaration reads in a hover. */
static void signature(StrBuf *b, Node *d) {
    switch (d->kind) {
    case N_FUNC: {
        putTypeName(b, d->type);
        sbPutc(b, ' ');
        int first = 0;
        if (d->list.len && (d->list.items[0]->flags & F_SELF)) {
            Type *rt = d->list.items[0]->type;
            putTypeName(b, rt && rt->kind == TY_PTR ? rt->elem : rt);
            sbPutc(b, '.');
            first = 1;
        }
        sbPuts(b, d->flags & F_TESTFN ? "test" : d->name);
        sbPutc(b, '(');
        for (int i = first; i < d->list.len; i++) {
            Node *p = d->list.items[i];
            if (i > first) sbPuts(b, ", ");
            if ((p->flags & F_REST) && p->type && p->type->kind == TY_SLICE) {
                putTypeName(b, p->type->elem);
                sbPuts(b, "...");
            } else {
                putTypeName(b, p->type);
            }
            sbPrintf(b, " %s", p->name);
        }
        sbPutc(b, ')');
        break;
    }
    case N_STRUCT: {
        sbPrintf(b, "struct %s {\n", d->name);
        for (int i = 0; i < d->list.len; i++) {
            Node *f = d->list.items[i];
            if (f->kind == N_EMBED) {
                sbPrintf(b, "    %s\n", f->name);
            } else {
                sbPuts(b, "    ");
                putTypeName(b, f->type);
                sbPrintf(b, " %s\n", f->name);
            }
        }
        sbPutc(b, '}');
        break;
    }
    case N_ENUM: {
        sbPrintf(b, "enum %s%s%s%s { ", d->name, d->a ? " " : "", d->a ? d->a->name : "", (d->flags & F_FLAGS) ? " flags" : "");
        for (int i = 0; i < d->list.len; i++) {
            sbPrintf(b, "%s%s", i ? ", " : "", d->list.items[i]->name);
        }
        sbPuts(b, " }");
        break;
    }
    case N_ENUM_MEMBER:
        putTypeName(b, d->type);
        sbPrintf(b, ".%s", d->name);
        break;
    case N_CONST:
        sbPuts(b, "const ");
        putTypeName(b, d->type);
        sbPrintf(b, " %s", d->name);
        break;
    default:
        putTypeName(b, d->type);
        sbPrintf(b, " %s", d->name ? d->name : "");
        break;
    }
}

/* The comment lines right above a declaration. */
static void docComment(StrBuf *b, Node *d) {
    if (!d->src || d->line < 2) return;
    const char *text = d->src->text;
    size_t len = d->src->len;
    int first = d->line, l = d->line - 1;
    while (l >= 1) {
        const char *s = lineStartOf(text, len, l), *e = s;
        while (e < text + len && *e != '\n') e++;
        while (s < e && (*s == ' ' || *s == '\t')) s++;
        if (e - s >= 2 && s[0] == '/' && s[1] == '/') {
            first = l--;
        } else {
            break;
        }
    }
    for (l = first; l < d->line; l++) {
        const char *s = lineStartOf(text, len, l), *e = s;
        while (e < text + len && *e != '\n') e++;
        while (s < e && (*s == ' ' || *s == '\t')) s++;
        s += 2;
        if (s < e && *s == ' ') s++;
        sbPutn(b, s, (size_t)(e - s));
        sbPutc(b, '\n');
    }
}

/* ---------------------------------------------------------------- requests (in the child) */

/* The keywords of Iris, as in lexer.c. */
static const char *irisWords[] = {"if", "else", "for", "in", "match", "break", "continue", "return", "struct",
                                  "union", "enum", "interface", "type", "dyn", "pub", "const", "comptime", "when",
                                  "import", "try", "catch", "defer", "errdefer", "scoped", "spawn", "select", "chan",
                                  "thread", "threadlocal", "asm", "test", "not", "and", "or", "is", "true", "false", "null"};

static void reply(StrBuf *out, J *id, const char *result) {
    StrBuf b = {0};
    sbPuts(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    putId(&b, id);
    sbPuts(&b, ",\"result\":");
    sbPuts(&b, result ? result : "null");
    sbPutc(&b, '}');
    sbPutn(out, b.buf, b.len);
    sbPutc(out, 0);
    sbFree(&b);
}

/* The declaration of the name under the cursor. The checked copies of
 * generic functions come first, since names in their bodies are linked
 * there; then the code as written, for its type parameters. */
static Node *declAt(Program *prog, Source *src, int line, int col) {
    if (!prog || !src) return NULL;
    Node *d = NULL;
    for (int pass = 0; pass < 2 && !d; pass++) {
        Node *parent = NULL, *fn = NULL;
        Node *n = nodeAt(prog, src, line, col, &parent, &fn, pass == 0);
        d = n ? declOf(n, parent) : NULL;
        if (n && !d) d = typeNamed(prog, src, n, fn);
    }
    return d;
}

static size_t offsetOf(Source *src, int line, int col);

/* The built-in name under the cursor, as builtinDecls names it: Error, print,
 * e.code as Error.code, lock.lock() as Mutex.lock; NULL for other names. */
static const Builtin *builtinAt(Program *prog, Source *src, int line, int col) {
    if (!src || !src->text) return NULL;
    size_t at = offsetOf(src, line, col), s = at, e = at;
    const char *t = src->text;
    while (s > 0 && (isalnum((unsigned char)t[s - 1]) || t[s - 1] == '_')) s--;
    while (e < src->len && (isalnum((unsigned char)t[e]) || t[e] == '_')) e++;
    if (e == s || e - s > 64) return NULL;
    char word[80];
    memcpy(word, t + s, e - s);
    word[e - s] = 0;
    if (s > 0 && t[s - 1] == '.') {
        /* a field or method: the type of the value before the dot */
        Node *parent = NULL;
        Node *n = prog ? nodeAt(prog, src, line, col, &parent, NULL, true) : NULL;
        Type *bt = n && n->kind == N_FIELD_ACCESS && n->a ? n->a->type : NULL;
        if (bt && bt->kind == TY_PTR) bt = bt->elem;
        const char *tn = !bt ? NULL
                       : bt->kind == TY_ERRVAL ? "Error"
                       : bt->kind == TY_MUTEX ? "Mutex"
                       : bt->kind == TY_THREAD ? "Thread"
                       : bt->kind == TY_ATOMIC ? "Atomic"
                       : NULL;
        if (!tn) return NULL;
        char key[160];
        snprintf(key, sizeof key, "%s.%s", tn, word);
        return builtinOf(key);
    }
    return strchr(word, '.') ? NULL : builtinOf(word);
}

static void hover(StrBuf *out, J *id, Program *prog, Source *src, int line, int col) {
    Node *d = declAt(prog, src, line, col);
    const Builtin *bi = d ? NULL : builtinAt(prog, src, line, col);
    if (bi) {
        StrBuf v = {0};
        sbPuts(&v, "```iris\n");
        const char *decl = bi->decl;
        while (*decl == ' ') decl++;
        size_t dl = strcspn(decl, "{\n");
        while (dl && decl[dl - 1] == ' ') dl--;
        sbPutn(&v, decl, dl);
        sbPuts(&v, "\n```\n\n");
        for (const char *p = bi->doc; *p;) {
            while (*p == ' ') p++;
            if (p[0] == '/' && p[1] == '/') p += p[2] == ' ' ? 3 : 2;
            const char *nl = strchr(p, '\n');
            size_t n = nl ? (size_t)(nl - p) : strlen(p);
            sbPutn(&v, p, n);
            sbPutc(&v, '\n');
            p += n + (nl != NULL);
        }
        StrBuf r = {0};
        sbPuts(&r, "{\"contents\":{\"kind\":\"markdown\",\"value\":");
        putJsonString(&r, v.buf, v.len);
        sbPuts(&r, "}}");
        reply(out, id, r.buf);
        sbFree(&v);
        sbFree(&r);
        return;
    }
    if (!d) {
        reply(out, id, NULL);
        return;
    }
    StrBuf v = {0};
    sbPuts(&v, "```iris\n");
    signature(&v, d);
    sbPuts(&v, "\n```");
    StrBuf doc = {0};
    docComment(&doc, d);
    if (doc.len) {
        sbPuts(&v, "\n\n");
        sbPutn(&v, doc.buf, doc.len);
    }
    StrBuf r = {0};
    sbPuts(&r, "{\"contents\":{\"kind\":\"markdown\",\"value\":");
    putJsonString(&r, v.buf, v.len);
    sbPuts(&r, "}}");
    reply(out, id, r.buf);
    sbFree(&v);
    sbFree(&doc);
    sbFree(&r);
}

/* A module named by the word under the cursor, as in `import sqlite` or
 * sqlite.open: the file of the module with its name (sqlite/sqlite.iris), or
 * its first file. */
static Node *moduleFileAt(Program *prog, Source *src, int line, int col) {
    if (!prog || !src || !src->text) return NULL;
    size_t at = offsetOf(src, line, col), s = at, e = at;
    const char *t = src->text;
    while (s > 0 && (isalnum((unsigned char)t[s - 1]) || t[s - 1] == '_')) s--;
    while (e < src->len && (isalnum((unsigned char)t[e]) || t[e] == '_')) e++;
    if (e == s) return NULL;
    for (Module *m = prog->modules; m; m = m->next) {
        if (m == prog->main || m->isC || !m->files.len || strlen(m->name) != e - s ||
            strncmp(m->name, t + s, e - s) != 0) {
            continue;
        }
        Node *best = m->files.items[0];
        for (int i = 0; i < m->files.len; i++) {
            const char *path = m->files.items[i]->src ? m->files.items[i]->src->path : "";
            const char *base = strrchr(path, '/');
            base = base ? base + 1 : path;
            if (strncmp(base, m->name, strlen(m->name)) == 0 && strcmp(base + strlen(m->name), ".iris") == 0) {
                best = m->files.items[i];
            }
        }
        return best;
    }
    return NULL;
}

static void definition(StrBuf *out, J *id, Program *prog, Source *src, int line, int col) {
    Node *d = declAt(prog, src, line, col);
    if (!d || d->kind == N_IMPORT) {
        Node *file = moduleFileAt(prog, src, line, col);
        if (file && file->src) {
            char *mu = fileUri(file->src->path);
            if (mu) {
                StrBuf r = {0};
                sbPuts(&r, "{\"uri\":");
                putStr(&r, mu);
                sbPuts(&r, ",\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":0,\"character\":0}}}");
                reply(out, id, r.buf);
                free(mu);
                sbFree(&r);
                return;
            }
        }
    }
    const Builtin *bi = d ? NULL : builtinAt(prog, src, line, col);
    if (bi) {
        char path[4200];
        builtinPath(path, sizeof path);
        int bcol = 1, bline = builtinLine(bi->key, &bcol);
        char *bu = uriOf(path);
        const char *dot = strchr(bi->key, '.');
        StrBuf r = {0};
        sbPuts(&r, "{\"uri\":");
        putStr(&r, bu);
        sbPuts(&r, ",\"range\":");
        putRange(&r, NULL, bline, bcol, (int)strlen(dot ? dot + 1 : bi->key));
        sbPutc(&r, '}');
        reply(out, id, r.buf);
        free(bu);
        sbFree(&r);
        return;
    }
    char *u = d && d->src ? fileUri(d->src->path) : NULL;
    if (!u) {
        reply(out, id, NULL);
        return;
    }
    Source shown = *d->src;
    if (strncmp(d->src->path, "std/", 4) == 0) {
        shown.text = stdSource(d->src->path);
        shown.len = shown.text ? strlen(shown.text) : 0;
    }
    StrBuf r = {0};
    sbPuts(&r, "{\"uri\":");
    putStr(&r, u);
    sbPuts(&r, ",\"range\":");
    int dcol = d->kind == N_FUNC && (d->b || (d->flags & F_INSTANCE)) && shown.text ? methodNameCol(&shown, d)
             : d->kind == N_IDENT && shown.text ? wordCol(&shown, d->line, d->col, d->name)
             : d->col;
    putRange(&r, shown.text ? &shown : NULL, d->line, dcol, d->name ? writtenLen(d->name) : 1);
    sbPutc(&r, '}');
    reply(out, id, r.buf);
    free(u);
    sbFree(&r);
}

/* ---------------------------------------------------------------- references and rename */

typedef struct {
    Source *src;
    int line, col, len;
    bool write;     /* the declaration, or a place that gets a value */
} Ref;

typedef struct {
    Program *prog;
    Node *decl;
    Ref *items;
    int n, cap;
} Refs;

/* Two nodes for one declaration. A checked copy of a generic has copies of
 * its locals at the same place in the text. */
static bool sameDecl(Node *a, Node *b) {
    if (a == b) return true;
    if (!a || !b || a->src != b->src || a->line != b->line || a->col != b->col || a->kind != b->kind) return false;
    int la = writtenLen(a->name), lb = writtenLen(b->name);
    return la == lb && (la == 0 || strncmp(a->name, b->name, (size_t)la) == 0);
}

static bool isStdSource(Source *src) {
    return !src || !src->path || strncmp(src->path, "std/", 4) == 0;
}

static void addRef(Refs *r, Source *src, int line, int col, int len, bool write) {
    for (int i = 0; i < r->n; i++) {
        if (r->items[i].src == src && r->items[i].line == line && r->items[i].col == col) {
            r->items[i].write = r->items[i].write || write;
            return;
        }
    }
    if (r->n == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 32;
        r->items = realloc(r->items, (size_t)r->cap * sizeof(Ref));
    }
    r->items[r->n++] = (Ref){src, line, col, len, write};
}

/* Whether the name n gives its declaration d a value: the declaration
 * itself, the target of = or +=, x++ and a field in a struct literal. */
static bool isWrite(Node *n, Node *parent, Node *d) {
    if (n == d || sameDecl(n, d) || n->kind == N_FIELD_INIT) return true;
    if (!parent) return false;
    if (parent->kind == N_INCDEC) return parent->a == n;
    if (parent->kind == N_ASSIGN) {
        for (int i = 0; i < parent->list.len; i++) {
            if (parent->list.items[i] == n) return true;
        }
    }
    return false;
}

static void refWalk(Refs *r, Node *n, Node *parent, Node *fn) {
    if (!n) return;
    int start, len;
    if (!isStdSource(n->src) && nameSpan(n->src, n, parent, &start, &len) && len > 0) {
        Node *d = declOf(n, parent);
        if (!d) d = typeNamed(r->prog, n->src, n, fn);
        if (n->kind == N_ENUM_DOT) {
            start++;        /* .admin: the name after the dot */
            len--;
        }
        if (d && sameDecl(d, r->decl)) addRef(r, n->src, n->line, start, len, isWrite(n, parent, d));
    }
    if (n->kind == N_FUNC) fn = n;
    refWalk(r, n->a, n, fn);
    refWalk(r, n->b, n, fn);
    refWalk(r, n->c, n, fn);
    refWalk(r, n->d, n, fn);
    for (int i = 0; i < n->list.len; i++) refWalk(r, n->list.items[i], n, fn);
    for (int i = 0; i < n->list2.len; i++) refWalk(r, n->list2.items[i], n, fn);
}

/* Every place in the project's files that names the declaration d: the code
 * as written, then the checked copies of generics. */
static Refs findRefs(Program *prog, Node *d) {
    Refs r = {prog, d, NULL, 0, 0};
    for (Module *m = prog->modules; m; m = m->next) {
        if (m->isStd || m->isC) continue;
        for (int i = 0; i < m->files.len; i++) refWalk(&r, m->files.items[i], NULL, NULL);
    }
    for (int i = 0; i < prog->instances.len; i++) refWalk(&r, prog->instances.items[i], NULL, NULL);
    return r;
}

static void putLocation(StrBuf *b, Ref *ref) {
    char *u = fileUri(ref->src->path);
    sbPuts(b, "{\"uri\":");
    putStr(b, u ? u : "");
    sbPuts(b, ",\"range\":");
    putRange(b, ref->src, ref->line, ref->col, ref->len);
    sbPutc(b, '}');
    free(u);
}

static void references(StrBuf *out, J *id, Program *prog, Source *src, int line, int col, bool withDecl) {
    Node *d = declAt(prog, src, line, col);
    if (!d || d->kind == N_IMPORT) {
        reply(out, id, "[]");
        return;
    }
    Refs r = findRefs(prog, d);
    StrBuf b = {0};
    sbPutc(&b, '[');
    bool first = true;
    for (int i = 0; i < r.n; i++) {
        Ref *ref = &r.items[i];
        int start, len;
        if (!withDecl && ref->src == d->src && ref->line == d->line && nameSpan(d->src, d, NULL, &start, &len) &&
            start == ref->col) {
            continue;       /* the declaration itself */
        }
        if (!first) sbPutc(&b, ',');
        first = false;
        putLocation(&b, ref);
    }
    sbPutc(&b, ']');
    reply(out, id, b.buf);
    sbFree(&b);
    free(r.items);
}

static int refOrder(const void *a, const void *b) {
    const Ref *x = a, *y = b;
    return x->line != y->line ? x->line - y->line : x->col - y->col;
}

/* ---------------------------------------------------------------- variables that are not used */

typedef struct {
    Source *src;
    int line, col;
} Place;

/* The declarations that code uses: a hash set of their places. */
static Place *gUsed;
static size_t capUsed, nUsed;

static size_t placeHash(Source *src, int line, int col) {
    uint64_t h = (uint64_t)(uintptr_t)src * 0x9E3779B97F4A7C15ULL ^ (uint64_t)line * 0xC2B2AE3D27D4EB4FULL ^ (uint64_t)col;
    return (size_t)(h ^ (h >> 29));
}

static bool isUsed(Node *d) {
    if (!capUsed) return false;
    for (size_t i = placeHash(d->src, d->line, d->col) & (capUsed - 1);; i = (i + 1) & (capUsed - 1)) {
        if (!gUsed[i].src) return false;
        if (gUsed[i].src == d->src && gUsed[i].line == d->line && gUsed[i].col == d->col) return true;
    }
}

static void addUsed(Source *src, int line, int col) {
    if (!src) return;
    if ((nUsed + 1) * 2 > capUsed) {
        size_t old = capUsed;
        Place *was = gUsed;
        capUsed = capUsed ? capUsed * 2 : 1024;
        gUsed = calloc(capUsed, sizeof *gUsed);
        nUsed = 0;
        for (size_t i = 0; i < old; i++) {
            if (was[i].src) addUsed(was[i].src, was[i].line, was[i].col);
        }
        free(was);
    }
    for (size_t i = placeHash(src, line, col) & (capUsed - 1);; i = (i + 1) & (capUsed - 1)) {
        if (!gUsed[i].src) {
            gUsed[i] = (Place){src, line, col};
            nUsed++;
            return;
        }
        if (gUsed[i].src == src && gUsed[i].line == line && gUsed[i].col == col) return;
    }
}

/* Marks each declaration that a name in the code refers to. */
static void useWalk(Node *n, Node *parent) {
    if (!n) return;
    if (n->kind == N_IDENT || n->kind == N_FIELD_ACCESS) {
        Node *d = declOf(n, parent);
        if (d && d != n && !sameDecl(n, d)) addUsed(d->src, d->line, d->col);
    }
    useWalk(n->a, n);
    useWalk(n->b, n);
    useWalk(n->c, n);
    useWalk(n->d, n);
    for (int i = 0; i < n->list.len; i++) useWalk(n->list.items[i], n);
    for (int i = 0; i < n->list2.len; i++) useWalk(n->list2.items[i], n);
}

/* A scoped variable is not reported: its value is freed at the end of the
 * block, and a name is needed for that. */
static void unusedLocal(Node *v, Node *decl) {
    if ((decl->flags & F_SCOPED) || (v->flags & F_SCOPED)) return;
    if (!v->name || !strcmp(v->name, "_") || isUsed(v) || ndiags >= (int)(sizeof diags / sizeof diags[0])) return;
    char msg[200];
    snprintf(msg, sizeof msg, "the variable %s is not used", v->name);
    diags[ndiags++] = (Diag){v->src, v->line, wordCol(v->src, v->line, v->col, v->name), strdup(msg), true};
}

/* The local variables of a function body that no code uses. */
static void localWalk(Node *n) {
    if (!n) return;
    if (n->kind == N_DEFINE) {
        for (int i = 0; i < n->list.len; i++) unusedLocal(n->list.items[i], n);
    } else if (n->kind == N_VAR) {
        unusedLocal(n, n);
    }
    localWalk(n->a);
    localWalk(n->b);
    localWalk(n->c);
    localWalk(n->d);
    for (int i = 0; i < n->list.len; i++) localWalk(n->list.items[i]);
    for (int i = 0; i < n->list2.len; i++) localWalk(n->list2.items[i]);
}

/* Adds a warning for each local variable that no code uses, in the files of
 * the program that are not std. Only when the program has no errors: the
 * checker then linked every name. A generic function is checked as its
 * copies, so its own body is not looked at. */
static void addUnused(Program *prog) {
    if (!prog || ndiags) return;
    for (Module *m = prog->modules; m; m = m->next) {
        if (m->isStd || m->isC) continue;
        for (int i = 0; i < m->files.len; i++) useWalk(m->files.items[i], NULL);
    }
    for (int i = 0; i < prog->instances.len; i++) useWalk(prog->instances.items[i], NULL);
    for (Module *m = prog->modules; m; m = m->next) {
        if (m->isStd || m->isC) continue;
        for (int i = 0; i < m->files.len; i++) {
            Node *file = m->files.items[i];
            if (isStdSource(file->src)) continue;
            for (int k = 0; k < file->list.len; k++) {
                Node *d = file->list.items[k];
                if (d->kind != N_FUNC && d->kind != N_TEST) continue;
                if (d->list2.len || (d->b && d->b->kind == N_T_GENERIC)) continue;
                localWalk(d->c);
            }
        }
    }
}

/* textDocument/documentHighlight: the uses in this file of the name at the
 * cursor. Kind 3 (write) for the declaration and the places that get a
 * value, kind 2 (read) for the others. */
static void highlights(StrBuf *out, J *id, Program *prog, Source *src, int line, int col) {
    Node *d = declAt(prog, src, line, col);
    if (!d || d->kind == N_IMPORT) {
        reply(out, id, "[]");
        return;
    }
    Refs r = findRefs(prog, d);
    if (r.n) qsort(r.items, (size_t)r.n, sizeof *r.items, refOrder);
    StrBuf b = {0};
    sbPutc(&b, '[');
    bool first = true;
    for (int i = 0; i < r.n; i++) {
        Ref *ref = &r.items[i];
        if (ref->src != src) continue;
        if (!first) sbPutc(&b, ',');
        first = false;
        sbPuts(&b, "{\"range\":");
        putRange(&b, ref->src, ref->line, ref->col, ref->len);
        sbPrintf(&b, ",\"kind\":%d}", ref->write ? 3 : 2);
    }
    sbPutc(&b, ']');
    reply(out, id, b.buf);
    sbFree(&b);
    free(r.items);
}

static void replyError(StrBuf *out, J *id, const char *msg) {
    StrBuf b = {0};
    sbPuts(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    putId(&b, id);
    sbPuts(&b, ",\"error\":{\"code\":-32803,\"message\":");
    putStr(&b, msg);
    sbPuts(&b, "}}");
    sbPutn(out, b.buf, b.len);
    sbPutc(out, 0);
    sbFree(&b);
}

/* The declaration that a rename at the cursor changes, or NULL and why not. */
static Node *renameTarget(Program *prog, Source *src, int line, int col, const char **why) {
    Node *d = declAt(prog, src, line, col);
    if (!d && builtinAt(prog, src, line, col)) {
        *why = "a built-in name can't be renamed";
        return NULL;
    }
    if (!d || !d->name || d->kind == N_IMPORT) {
        *why = "there is no name to rename here";
        return NULL;
    }
    if (isStdSource(d->src)) {
        *why = "a name of the standard library can't be renamed";
        return NULL;
    }
    return d;
}

static bool validName(const char *s) {
    if (!s || !(isalpha((unsigned char)*s) || *s == '_')) return false;
    for (const char *p = s; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '_') return false;
    }
    if (strcmp(s, "_") == 0 || strcmp(s, "self") == 0) return false;
    for (size_t i = 0; i < sizeof irisWords / sizeof irisWords[0]; i++) {
        if (strcmp(s, irisWords[i]) == 0) return false;
    }
    return true;
}

static void prepareRename(StrBuf *out, J *id, Program *prog, Source *src, int line, int col) {
    const char *why = NULL;
    Node *d = renameTarget(prog, src, line, col, &why);
    Node *parent = NULL;
    Node *n = d ? nodeAt(prog, src, line, col, &parent, NULL, false) : NULL;
    if (!n) n = d ? nodeAt(prog, src, line, col, &parent, NULL, true) : NULL;
    int start, len;
    if (!d || !n || !nameSpan(src, n, parent, &start, &len) || len == 0) {
        replyError(out, id, why ? why : "there is no name to rename here");
        return;
    }
    if (n->kind == N_ENUM_DOT) {
        start++;        /* .admin: the name after the dot */
        len--;
    }
    StrBuf b = {0};
    sbPuts(&b, "{\"range\":");
    putRange(&b, src, n->line, start, len);
    sbPuts(&b, ",\"placeholder\":");
    putJsonString(&b, lineStartOf(src->text, src->len, n->line) + start - 1, (size_t)len);
    sbPutc(&b, '}');
    reply(out, id, b.buf);
    sbFree(&b);
}

static void renameAll(StrBuf *out, J *id, Program *prog, Source *src, int line, int col, const char *newName) {
    const char *why = NULL;
    Node *d = renameTarget(prog, src, line, col, &why);
    if (!d) {
        replyError(out, id, why);
        return;
    }
    if (!validName(newName)) {
        char msg[400];
        snprintf(msg, sizeof msg, "%s is not a valid name", newName ? newName : "the new name");
        replyError(out, id, msg);
        return;
    }
    Refs r = findRefs(prog, d);
    StrBuf b = {0};
    sbPuts(&b, "{\"changes\":{");
    bool firstFile = true;
    for (int i = 0; i < r.n; i++) {
        Source *s = r.items[i].src;
        bool seen = false;
        for (int k = 0; k < i && !seen; k++) seen = r.items[k].src == s;
        if (seen) continue;
        char *u = fileUri(s->path);
        if (!firstFile) sbPutc(&b, ',');
        firstFile = false;
        putStr(&b, u ? u : "");
        sbPuts(&b, ":[");
        bool first = true;
        for (int k = i; k < r.n; k++) {
            Ref *ref = &r.items[k];
            if (ref->src != s) continue;
            if (!first) sbPutc(&b, ',');
            first = false;
            sbPuts(&b, "{\"range\":");
            putRange(&b, s, ref->line, ref->col, ref->len);
            sbPuts(&b, ",\"newText\":");
            putStr(&b, newName);
            sbPutc(&b, '}');
        }
        sbPutc(&b, ']');
        free(u);
    }
    sbPuts(&b, "}}");
    reply(out, id, b.buf);
    sbFree(&b);
    free(r.items);
}

/* The folder with iris.mod above the file, or "" when there is none. */
static void projectRoot(const char *path, char *root, size_t size) {
    root[0] = 0;
    char cur[4096];
    snprintf(cur, sizeof cur, "%s", path);
    for (char *slash = strrchr(cur, '/'); slash && slash != cur; slash = strrchr(cur, '/')) {
        *slash = 0;
        char mod[4200];
        snprintf(mod, sizeof mod, "%s/iris.mod", cur);
        if (access(mod, F_OK) == 0) {
            snprintf(root, size, "%s", cur);
            return;
        }
    }
}

static int symbolKind(Node *d) {
    switch (d->kind) {
    case N_FUNC: return d->list.len && (d->list.items[0]->flags & F_SELF) ? 6 : 12;
    case N_STRUCT: return 23;
    case N_ENUM: return 10;
    case N_INTERFACE: return 11;
    case N_CONST: return 14;
    case N_TYPEDEF: return 26;
    default: return 13;
    }
}

static const char *statementEnd(const char *s, const char *end);

/* The struct, enum or interface that a method's receiver names, or NULL. */
static Node *receiverDecl(Node *fn) {
    if (fn->kind != N_FUNC || !fn->list.len || !(fn->list.items[0]->flags & F_SELF)) return NULL;
    Type *rt = fn->list.items[0]->type;
    if (rt && rt->kind == TY_PTR) rt = rt->elem;
    return rt ? rt->decl : NULL;
}

/* One DocumentSymbol: the range is the whole declaration, from its first
 * line to the end of its block, and selectionRange is the name. With last,
 * the range goes on to the end of last: a type's range holds its methods,
 * as a child's range must be in its parent's. */
static void putSymbol(StrBuf *r, Source *src, Node *named, const char *name, int kind, const char *detail, Node *last) {
    const char *t = src->text, *end = t + src->len;
    const char *start = lineStartOf(t, src->len, named->line);
    Node *to = last && last->line > named->line ? last : named;
    const char *stop = statementEnd(t + offsetOf(src, to->line, to->col), end);
    int endLine = named->line;
    for (const char *p = start; p < stop; p++) endLine += *p == '\n';
    const char *ls = lineStartOf(t, src->len, endLine);
    sbPuts(r, "{\"name\":");
    putStr(r, name);
    sbPrintf(r, ",\"kind\":%d", kind);
    if (detail && *detail) {
        sbPuts(r, ",\"detail\":");
        putStr(r, detail);
    }
    sbPuts(r, ",\"range\":{\"start\":");
    putPos(r, src, named->line, 1);
    sbPuts(r, ",\"end\":");
    putPos(r, src, endLine, (int)(stop - ls) + 1);
    sbPuts(r, "},\"selectionRange\":");
    int col = wordCol(src, named->line, named->col, named->name);
    putRange(r, src, named->line, col, writtenLen(named->name));
}

/* textDocument/documentSymbol: the file's declarations as a tree. A struct
 * has its fields and the methods of this file, an enum its members, an
 * interface its methods. */
static void symbols(StrBuf *out, J *id, Program *prog, Source *src) {
    StrBuf r = {0};
    sbPutc(&r, '[');
    bool first = true;
    for (Module *m = prog ? prog->modules : NULL; m; m = m->next) {
        for (int f = 0; f < m->files.len; f++) {
            Node *file = m->files.items[f];
            if (file->src != src) continue;
            for (int i = 0; i < file->list.len; i++) {
                Node *d = file->list.items[i];
                Node *named = d->kind == N_DEFINE && d->list.len == 1 ? d->list.items[0] : d;
                if (d->kind == N_IMPORT) continue;
                Node *owner = receiverDecl(d);
                if (owner && owner->src == src) continue;      /* under its type */
                StrBuf name = {0}, detail = {0};
                int kind = symbolKind(d);
                if (d->kind == N_TEST) {
                    sbPrintf(&name, "%s \"%s\"", (d->flags & F_BENCH) ? "bench" : "test", d->text ? d->text : "");
                    kind = 12;
                } else if (d->kind == N_FUNC && (d->flags & F_TESTFN)) {
                    sbPrintf(&name, "test \"%s\"", d->text);
                } else if (!named->name) {
                    continue;
                } else if (owner || (d->kind == N_FUNC && d->list.len && (d->list.items[0]->flags & F_SELF))) {
                    Type *rt = d->list.items[0]->type;
                    putTypeName(&name, rt && rt->kind == TY_PTR ? rt->elem : rt);
                    sbPrintf(&name, ".%s", d->name);
                } else {
                    sbPuts(&name, named->name);
                }
                if ((d->kind == N_FUNC && !(d->flags & F_TESTFN)) || d->kind == N_CONST) signature(&detail, d);
                if (!first) sbPutc(&r, ',');
                first = false;
                /* a test has no name node: its line is the place */
                Node at = *named;
                if (!at.name) at.name = d->kind == N_TEST ? ((d->flags & F_BENCH) ? "bench" : "test") : "";
                Node *last = NULL;
                for (int k = 0; k < file->list.len; k++) {
                    Node *fn = file->list.items[k];
                    if (receiverDecl(fn) == d && (!last || fn->line > last->line)) last = fn;
                }
                putSymbol(&r, src, &at, name.buf, kind, detail.buf, last);
                sbFree(&name);
                sbFree(&detail);
                if (d->kind == N_STRUCT || d->kind == N_ENUM || d->kind == N_INTERFACE) {
                    sbPuts(&r, ",\"children\":[");
                    bool firstKid = true;
                    for (int k = 0; k < d->list.len; k++) {
                        Node *c = d->list.items[k];
                        if (!c->name || c->src != src) continue;
                        StrBuf cd = {0};
                        int ck = c->kind == N_ENUM_MEMBER ? 22 : c->kind == N_FUNC ? 6 : 8;
                        if (c->kind == N_FUNC) signature(&cd, c);
                        else if (c->type && c->kind != N_ENUM_MEMBER) sbPuts(&cd, c->type->name);
                        if (!firstKid) sbPutc(&r, ',');
                        firstKid = false;
                        putSymbol(&r, src, c, c->name, ck, cd.buf, NULL);
                        sbPutc(&r, '}');
                        sbFree(&cd);
                    }
                    /* the methods of this file whose receiver is the type */
                    for (int k = 0; k < file->list.len; k++) {
                        Node *fn = file->list.items[k];
                        if (receiverDecl(fn) != d) continue;
                        StrBuf cd = {0};
                        signature(&cd, fn);
                        if (!firstKid) sbPutc(&r, ',');
                        firstKid = false;
                        putSymbol(&r, src, fn, fn->name, 6, cd.buf, NULL);
                        sbPutc(&r, '}');
                        sbFree(&cd);
                    }
                    sbPutc(&r, ']');
                }
                sbPutc(&r, '}');
            }
        }
    }
    sbPutc(&r, ']');
    reply(out, id, r.buf);
    sbFree(&r);
}

/* ---------------------------------------------------------------- workspace symbols */

static char gRoot[4096];        /* the folder of the workspace, from initialize */

/* Whether query matches name: a part of it, with no regard to case, or the
 * starts of its words in order (rA, RdA and reAl match readAll). */
static bool humpMatch(const char *q, const char *name, bool start) {
    if (!*q) return true;
    for (const char *p = name; *p; p++) {
        bool hump = p == name || isupper((unsigned char)*p) || p[-1] == '_' || p[-1] == '.' ||
                    (isdigit((unsigned char)*p) && !isdigit((unsigned char)p[-1]));
        if (start && p != name) return false;
        if (!start && !hump) continue;
        if (tolower((unsigned char)*p) != tolower((unsigned char)*q)) continue;
        /* the rest of the query: on in this word, or at a later word */
        const char *qq = q + 1, *pp = p + 1;
        while (*qq && *pp && tolower((unsigned char)*qq) == tolower((unsigned char)*pp)) {
            if (humpMatch(qq, pp, false)) return true;
            qq++;
            pp++;
        }
        if (humpMatch(qq, pp, false)) return true;
    }
    return false;
}

static bool symbolMatch(const char *query, const char *name) {
    size_t q = strlen(query), n = strlen(name);
    for (size_t i = 0; i + q <= n; i++) {
        if (!strncasecmp(name + i, query, q)) return true;
    }
    return humpMatch(query, name, false);
}

typedef struct {
    StrBuf r;
    int n;
    const char *query;
} WsSymbols;

/* The declarations at the start of the lines of text: what a line that
 * starts at column 0 declares (pub or not), and test "name" blocks. */
static void textSymbols(WsSymbols *w, const char *path, const char *text, const char *container) {
    int line = 1;
    char *uri = NULL;
    for (const char *ls = text; *ls && w->n < 500; line++) {
        const char *le = strchr(ls, '\n');
        if (!le) le = ls + strlen(ls);
        const char *s = ls, *name = NULL;
        size_t len = 0;
        int kind = 0;
        char buf[200];
        if (!strncmp(s, "pub ", 4)) s += 4;
        if (!strncmp(s, "test \"", 6) || !strncmp(s, "bench \"", 7)) {
            const char *q = strchr(s, '"'), *qe = q ? memchr(q + 1, '"', (size_t)(le - q - 1)) : NULL;
            if (qe) {
                snprintf(buf, sizeof buf, "%.*s", (int)(qe - s + 1), s);
                name = buf;
                len = strlen(buf);
                kind = 12;
            }
        } else if ((isalpha((unsigned char)*s) || *s == '!' || *s == '(' || *s == '?') && strncmp(s, "import ", 7) &&
                   strncmp(s, "module ", 7) && strncmp(s, "when ", 5)) {
            const char *nm, *from = s;
            if (*from == '(') {     /* a tuple result: (i64, str) split(...) */
                int depth = 0;
                for (; from < le; from++) {
                    if (*from == '(') depth++;
                    else if (*from == ')' && --depth == 0) break;
                }
                if (from < le) from++;
            }
            len = pubDeclName(from, le, &nm, &kind, true);
            if (len && len < sizeof buf) {
                snprintf(buf, sizeof buf, "%.*s", (int)len, nm);
                name = buf;
                s = nm;
            } else {
                len = 0;
            }
        }
        if (len && symbolMatch(w->query, name)) {
            if (!uri) uri = fileUri(path);
            int col = (int)(s - ls);
            int u0 = utf16Col(ls, le, col + 1), u1 = utf16Col(ls, le, col + 1 + (int)len);
            if (w->n++) sbPutc(&w->r, ',');
            sbPuts(&w->r, "{\"name\":");
            putStr(&w->r, name);
            sbPrintf(&w->r, ",\"kind\":%d,\"location\":{\"uri\":", kind);
            putStr(&w->r, uri ? uri : "");
            sbPrintf(&w->r, ",\"range\":{\"start\":{\"line\":%d,\"character\":%d},\"end\":{\"line\":%d,\"character\":%d}}}",
                     line - 1, u0, line - 1, u1);
            if (container) {
                sbPuts(&w->r, ",\"containerName\":");
                putStr(&w->r, container);
            }
            sbPutc(&w->r, '}');
        }
        ls = *le ? le + 1 : le;
    }
    free(uri);
}

/* The text of a file: the editor's when it is open, else the disk's. */
static char *fileText(const char *path) {
    for (int i = 0; i < ndocs; i++) {
        if (samePath(docs[i].path, path)) return strdup(docs[i].text);
    }
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    StrBuf b = {0};
    char chunk[8192];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) sbPutn(&b, chunk, n);
    fclose(f);
    sbPutc(&b, 0);
    return b.buf;
}

/* The .iris files under dir, at most a few thousand, not in hidden folders. */
static void projectFiles(const char *dir, char ***out, int *n, int depth) {
    if (depth > 8 || *n >= 4000) return;
    DIR *d = opendir(dir);
    if (!d) return;
    for (struct dirent *e; (e = readdir(d)) && *n < 4000;) {
        if (e->d_name[0] == '.') continue;
        char path[4600];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        size_t len = strlen(e->d_name);
        if (S_ISDIR(st.st_mode)) {
            projectFiles(path, out, n, depth + 1);
        } else if (len > 5 && !strcmp(e->d_name + len - 5, ".iris")) {
            *out = realloc(*out, sizeof **out * (size_t)(*n + 1));
            (*out)[(*n)++] = strdup(path);
        }
    }
    closedir(d);
}

/* workspace/symbol: the declarations whose names match the query, in the
 * project's files (the workspace folder and the open files) and in std. */
static void workspaceSymbols(StrBuf *out, J *id, const char *query) {
    WsSymbols w = {.query = query ? query : ""};
    sbPutc(&w.r, '[');
    char **files = NULL;
    int n = 0;
    if (gRoot[0]) projectFiles(gRoot, &files, &n, 0);
    for (int i = 0; i < ndocs; i++) {
        bool seen = false;
        for (int k = 0; k < n && !seen; k++) seen = samePath(files[k], docs[i].path);
        if (!seen && docs[i].path && strncmp(docs[i].path, "std/", 4) && !isBuiltinFile(docs[i].path) &&
            !stdPathOf(docs[i].path)) {
            files = realloc(files, sizeof *files * (size_t)(n + 1));
            files[n++] = strdup(docs[i].path);
        }
    }
    for (int i = 0; i < n; i++) {
        char *text = fileText(files[i]);
        if (text) textSymbols(&w, files[i], text, NULL);
        free(text);
        free(files[i]);
    }
    free(files);
    const char *text;
    for (int i = 0; stdFileAt(i, &text); i++) {
        const char *rel = stdFileAt(i, &text), *slash = strchr(rel, '/');
        if (!slash || strchr(slash + 1, '/')) continue;
        char path[300], mod[128];
        snprintf(path, sizeof path, "std/%s", rel);
        snprintf(mod, sizeof mod, "%.*s", (int)(slash - rel), rel);
        /* std files: only the pub names */
        WsSymbols pub = {.query = w.query, .n = w.n};
        pub.r = w.r;
        StrBuf only = {0};
        for (const char *ls = text; *ls;) {
            const char *le = strchr(ls, '\n');
            if (!le) le = ls + strlen(ls);
            if (!strncmp(ls, "pub ", 4)) sbPutn(&only, ls, (size_t)(le - ls));
            sbPutc(&only, '\n');
            ls = *le ? le + 1 : le;
        }
        sbPutc(&only, 0);
        textSymbols(&pub, path, only.buf, mod);
        sbFree(&only);
        w.r = pub.r;
        w.n = pub.n;
    }
    sbPutc(&w.r, ']');
    reply(out, id, w.r.buf);
    sbFree(&w.r);
}

/* Edits that taking a completion item makes besides inserting it: the line
 * that imports its module (a JSON array), or NULL. */
static const char *gItemEdits;

/* signatureHelp looks for the item with this label and keeps its detail
 * (the signature) in gFound. */
static const char *gWantLabel;
static StrBuf gFound;

/* Where the parameter list of the function called label starts in a
 * signature such as "i64 Point.sum(i64 by)": the '(' after the name, or NULL. */
static const char *paramsOf(const char *label, const char *sig) {
    size_t n = strlen(label);
    for (const char *p = strstr(sig, label); p; p = strstr(p + 1, label)) {
        bool start = p == sig || !(isalnum((unsigned char)p[-1]) || p[-1] == '_');
        if (start && p[n] == '(') return p + n;
    }
    return NULL;
}

/* Calls f for each parameter in the list that starts at open: the start
 * and the end of its text, and of its name (the last word). Commas inside
 * [], () and {} do not split, as in Map[str, i64] m. The end of the list. */
static const char *eachParam(const char *open, void (*f)(void *, const char *, const char *, const char *, const char *),
                             void *arg) {
    const char *p = open + 1;
    int depth = 0;
    const char *start = p;
    for (;; p++) {
        char ch = *p;
        if (!ch) return p;
        if (ch == '(' || ch == '[' || ch == '{') {
            depth++;
        } else if ((ch == ')' || ch == ']' || ch == '}') && depth > 0) {
            depth--;
        } else if ((ch == ',' && depth == 0) || ch == ')') {
            const char *a = start, *b = p;
            while (a < b && *a == ' ') a++;
            while (b > a && b[-1] == ' ') b--;
            if (b > a) {
                const char *w = b;
                while (w > a && (isalnum((unsigned char)w[-1]) || w[-1] == '_')) w--;
                f(arg, a, b, w, b);
            }
            if (ch == ')') return p;
            start = p + 1;
        }
    }
}

typedef struct {
    StrBuf *b;
    int n;
} SnippetArgs;

static void snippetParam(void *arg, const char *a, const char *b, const char *w, const char *we) {
    (void)a;
    (void)b;
    SnippetArgs *s = arg;
    s->n++;
    sbPrintf(s->b, "%s${%d:%.*s}", s->n > 1 ? ", " : "", s->n, (int)(we - w), w);
}

/* The snippet that a completion of a function or a method inserts:
 * "add(${1:a}, ${2:b})" from the parameter names in its signature,
 * "now()" for no parameters. False when the detail has no signature. */
static bool snippetOf(StrBuf *b, const char *label, const char *detail) {
    const char *open = detail ? paramsOf(label, detail) : NULL;
    if (!open) return false;
    sbPrintf(b, "%s(", label);
    SnippetArgs s = {b, 0};
    eachParam(open, snippetParam, &s);
    sbPutc(b, ')');
    return true;
}

static void putItem(StrBuf *r, bool *first, const char *label, int kind, const char *detail) {
    if (gWantLabel && detail && !gFound.buf && strcmp(label, gWantLabel) == 0 && paramsOf(label, detail)) {
        sbPuts(&gFound, detail);
    }
    if (!*first) sbPutc(r, ',');
    *first = false;
    sbPuts(r, "{\"label\":");
    putStr(r, label);
    sbPrintf(r, ",\"kind\":%d", kind);
    if (detail) {
        sbPuts(r, ",\"detail\":");
        putStr(r, detail);
    }
    StrBuf snip = {0};
    if ((kind == 2 || kind == 3) && snippetOf(&snip, label, detail)) {
        sbPuts(r, ",\"insertTextFormat\":2,\"insertText\":");
        putStr(r, snip.buf);
    }
    sbFree(&snip);
    if (gItemEdits) {
        sbPuts(r, ",\"additionalTextEdits\":");
        sbPuts(r, gItemEdits);
    }
    sbPutc(r, '}');
}

/* ---------------------------------------------------------------- imports */

/* Whether name is a standard library module that has to be imported. */
static bool isStdModule(const char *name) {
    const char *names[128];
    int n = stdModuleNames(names, 128);
    for (int i = 0; i < n; i++) {
        if (strcmp(names[i], name) == 0) return strcmp(name, "core") != 0;
    }
    return false;
}

/* Whether the line at s, up to end, is an import line. */
static bool isImportLine(const char *s, const char *end) {
    return end - s > 7 && memcmp(s, "import", 6) == 0 && (s[6] == ' ' || s[6] == '\t');
}

/* Whether an import line of text names the module. */
static bool importsModule(const char *text, size_t len, const char *name) {
    size_t nl = strlen(name);
    for (const char *s = text, *end = text + len; s < end;) {
        const char *e = memchr(s, '\n', (size_t)(end - s));
        if (!e) e = end;
        if (isImportLine(s, e)) {
            for (const char *p = s + 6; p < e;) {
                if (*p == '/' && p + 1 < e && p[1] == '/') break;
                if (*p == '"') {
                    const char *q = memchr(p + 1, '"', (size_t)(e - p - 1));
                    p = q ? q + 1 : e;
                } else if (isalnum((unsigned char)*p) || *p == '_') {
                    const char *w = p;
                    while (p < e && (isalnum((unsigned char)*p) || *p == '_')) p++;
                    if ((size_t)(p - w) == nl && memcmp(w, name, nl) == 0) return true;
                } else {
                    p++;
                }
            }
        }
        s = e + 1;
    }
    return false;
}

/* The edit that imports the module, as a JSON array: a line under the last
 * import line, or at the top with a blank line after it. */
static void importEdits(StrBuf *b, const char *text, size_t len, const char *name) {
    int line = 0, after = -1;
    for (const char *s = text, *end = text + len; s < end; line++) {
        const char *e = memchr(s, '\n', (size_t)(end - s));
        if (!e) e = end;
        if (isImportLine(s, e)) after = line + 1;
        s = e + 1;
    }
    char t[160];
    snprintf(t, sizeof t, after >= 0 ? "import %s\n" : "import %s\n\n", name);
    int at = after >= 0 ? after : 0;
    sbPrintf(b, "[{\"range\":{\"start\":{\"line\":%d,\"character\":0},\"end\":{\"line\":%d,\"character\":0}},\"newText\":",
             at, at);
    putStr(b, t);
    sbPuts(b, "}]");
}

/* One quick fix: {"title":..., "kind":"quickfix", "edit":{"changes":{uri: edits}}}. */
static void putFix(StrBuf *r, bool *first, const char *title, bool preferred, const char *uri, const char *edits) {
    if (!*first) sbPutc(r, ',');
    *first = false;
    sbPuts(r, "{\"title\":");
    putStr(r, title);
    sbPrintf(r, ",\"kind\":\"quickfix\"%s,\"edit\":{\"changes\":{", preferred ? ",\"isPreferred\":true" : "");
    putStr(r, uri);
    sbPutc(r, ':');
    sbPuts(r, edits);
    sbPuts(r, "}}}");
}

static void putEdit(StrBuf *b, int l1, int c1, int l2, int c2, const char *text) {
    sbPrintf(b, "{\"range\":{\"start\":{\"line\":%d,\"character\":%d},\"end\":{\"line\":%d,\"character\":%d}},\"newText\":",
             l1, c1, l2, c2);
    putStr(b, text);
    sbPutc(b, '}');
}

/* The end of the statement that starts at s: the newline after it at the
 * top level, past brackets, braces and text on more lines. */
static const char *statementEnd(const char *s, const char *end) {
    int depth = 0;
    for (const char *p = s; p < end; p++) {
        if (*p == '/' && p + 1 < end && p[1] == '/') {
            while (p < end && *p != '\n') p++;
            if (p == end) return end;
        }
        if (*p == '"') {
            for (p++; p < end && *p != '"' && *p != '\n'; p++) {
                if (*p == '\\') p++;
            }
            continue;
        }
        if (*p == '(' || *p == '[' || *p == '{') depth++;
        else if ((*p == ')' || *p == ']' || *p == '}') && depth > 0) depth--;
        else if (*p == '\n' && depth == 0) return p;
    }
    return end;
}

/* The fixes for "the variable x is not used" at line and character (0-based):
 * "Remove the variable" when its value has no call (nothing happens when the
 * value goes), else "Rename to _", which keeps the call: x := f() becomes _ = f(). */
static void unusedFixes(StrBuf *r, bool *first, const char *uri, Doc *doc, const char *name, int line, int character) {
    const char *text = doc->text, *end = text + doc->len;
    const char *ls = lineStartOf(text, doc->len, line + 1);
    const char *at = ls + byteCol(text, doc->len, line + 1, character) - 1;
    size_t n = strlen(name);
    if (at + n > end || strncmp(at, name, n) != 0) return;
    const char *start = ls;
    while (start < at && (*start == ' ' || *start == '\t')) start++;
    const char *after = at + n;
    while (after < end && (*after == ' ' || *after == '\t')) after++;
    const char *before = at;
    while (before > start && (before[-1] == ' ' || before[-1] == '\t')) before--;
    bool define = after + 1 < end && after[0] == ':' && after[1] == '=';
    bool several = (before > start && before[-1] == ',') || (after < end && *after == ',');
    bool assign = !define && after < end && after[0] == '=' && (after + 1 == end || after[1] != '=');
    const char *value = define ? after + 2 : assign ? after + 1 : after;
    if (several) {
        /* a, x := f(): only the name can go */
        const char *op = strstr(at, ":=");
        if (!op) return;
        value = op + 2;
    }
    const char *stop = statementEnd(value, end);
    bool call = false;
    for (const char *p = value; p < stop && !call; p++) call = *p == '(' || (*p == '<' && p + 1 < stop && p[1] == '-');
    int col = (int)(at - ls), first0 = (int)(start - ls);
    StrBuf e = {0};
    sbPutc(&e, '[');
    const char *title;
    if (!call && !several) {
        int lastLine = line;
        for (const char *p = ls; p < stop; p++) lastLine += *p == '\n';
        putEdit(&e, line, 0, lastLine + 1, 0, "");
        title = "Remove the variable";
    } else if (several) {
        putEdit(&e, line, col, line, col + (int)n, "_");
        title = "Rename to _";
    } else {
        /* x := f() and i64 x = f() become _ = f() */
        putEdit(&e, line, first0, line, (int)(value - ls), "_ =");
        title = "Rename to _";
    }
    sbPutc(&e, ']');
    putFix(r, first, title, true, uri, e.buf);
    sbFree(&e);
}

/* Quick fixes for the diagnostics of the range: the import of a std module
 * that the code names, a std name written without its module (readAll
 * becomes fs.readAll), and a variable that is not used. */
static void codeActions(StrBuf *out, J *id, J *params, const char *uri, Doc *doc) {
    StrBuf r = {0};
    sbPutc(&r, '[');
    bool first = true;
    char done[16][128];
    int ndone = 0;
    J *ds = jget(jget(params, "context"), "diagnostics");
    for (J *d = ds && doc ? ds->child : NULL; d; d = d->next) {
        const char *msg = jstr(d, "message");
        J *st = jget(jget(d, "range"), "start");
        J *dl = st ? jget(st, "line") : NULL, *dc = st ? jget(st, "character") : NULL;
        int line = dl ? (int)dl->num : 0, character = dc ? (int)dc->num : 0;
        char name[128];
        int used = 0;
        if (!msg) continue;
        if (sscanf(msg, "the variable %127[A-Za-z0-9_] is not used%n", name, &used) == 1 && used) {
            unusedFixes(&r, &first, uri, doc, name, line, character);
            continue;
        }
        used = 0;
        if (sscanf(msg, "%127[A-Za-z0-9_] is a standard library module that isn't imported%n", name, &used) == 1 && used) {
            bool seen = false;
            for (int i = 0; i < ndone; i++) seen |= strcmp(done[i], name) == 0;
            if (seen || importsModule(doc->text, doc->len, name) || ndone == 16) continue;
            strcpy(done[ndone++], name);
            StrBuf e = {0}, title = {0};
            importEdits(&e, doc->text, doc->len, name);
            sbPrintf(&title, "Import %s", name);
            putFix(&r, &first, title.buf, true, uri, e.buf);
            sbFree(&e);
            sbFree(&title);
            continue;
        }
        used = 0;
        if ((sscanf(msg, "%127[A-Za-z0-9_] is not declared%n", name, &used) == 1 && used && !msg[used]) ||
            ((used = 0), sscanf(msg, "%127[A-Za-z0-9_] is not a struct%n", name, &used) == 1 && used && !msg[used])) {
            const char *mods[16];
            /* the preferred fix: the only module, or the only one that the file imports */
            int nm = stdModulesExporting(name, mods, 16), shown = 0, imports = 0;
            for (int i = 0; i < nm; i++) {
                shown += isStdModule(mods[i]);
                imports += isStdModule(mods[i]) && importsModule(doc->text, doc->len, mods[i]);
            }
            for (int i = 0; i < nm; i++) {
                if (isStdModule(mods[i])) {
                    bool imported = importsModule(doc->text, doc->len, mods[i]);
                    StrBuf e = {0}, title = {0}, full = {0};
                    sbPrintf(&full, "%s.%s", mods[i], name);
                    if (imported) {
                        sbPutc(&e, '[');
                    } else {
                        importEdits(&e, doc->text, doc->len, mods[i]);
                        e.len--;        /* the ']' */
                        sbPutc(&e, ',');
                    }
                    putEdit(&e, line, character, line, character + (int)strlen(name), full.buf);
                    sbPutc(&e, ']');
                    if (imported) sbPrintf(&title, "Use %s", full.buf);
                    else sbPrintf(&title, "Import %s and use %s", mods[i], full.buf);
                    putFix(&r, &first, title.buf, shown == 1 || (imports == 1 && imported), uri, e.buf);
                    sbFree(&e);
                    sbFree(&title);
                    sbFree(&full);
                }
                free((char *)mods[i]);
            }
        }
    }
    sbPutc(&r, ']');
    reply(out, id, r.buf);
    sbFree(&r);
}

/* The functions of std/str that text has as methods. */
static void textMethods(StrBuf *r, bool *first, Program *prog) {
    Module *m = prog ? prog->text : NULL;
    for (int f = 0; m && f < m->files.len; f++) {
        Node *file = m->files.items[f];
        for (int i = 0; i < file->list.len; i++) {
            Node *d = file->list.items[i];
            if (d->kind != N_FUNC || !d->name || textMethod(prog, d->name) != d) continue;
            StrBuf sig = {0};
            signature(&sig, d);
            putItem(r, first, d->name, 2, sig.buf);
            sbFree(&sig);
        }
    }
}

/* After "name.": a module's pub names, or a value's fields and methods. */
static void completion(StrBuf *out, J *id, Program *prog, Source *src, const char *word, int line) {
    StrBuf r = {0};
    sbPutc(&r, '[');
    bool first = true;
    Type *t = NULL;
    /* the nearest use of the name above the cursor, in this file */
    Node *best = NULL;
    for (int l = line; prog && src && l >= 1 && !best; l--) {
        const char *ls = lineStartOf(src->text, src->len, l), *le = ls;
        while (le < src->text + src->len && *le != '\n') le++;
        size_t wl = strlen(word);
        for (const char *p = ls; p + wl <= le && !best; p++) {
            if (memcmp(p, word, wl) != 0) continue;
            if (p > ls && (isalnum((unsigned char)p[-1]) || p[-1] == '_' || p[-1] == '.')) continue;
            if (p + wl < le && (isalnum((unsigned char)p[wl]) || p[wl] == '_')) continue;
            Node *parent;
            Node *n = nodeAt(prog, src, l, (int)(p - ls) + 1, &parent, NULL, true);
            if (n && n->type && n->type->kind != TY_ERROR) best = n;
        }
    }
    if (best) t = best->type;
    if (t && t->kind == TY_PTR) t = t->elem;
    if (best && best->ref && best->ref->kind == N_ENUM) {
        /* Kind. gives the enum's values */
        for (int i = 0; i < best->ref->list.len; i++) {
            Node *v = best->ref->list.items[i];
            if (v->kind == N_ENUM_MEMBER && v->name) putItem(&r, &first, v->name, 20, best->ref->name);
        }
    } else if (t && t->kind == TY_STRUCT && t->decl) {
        for (int i = 0; i < t->decl->list.len; i++) {
            Node *f = t->decl->list.items[i];
            if (f->name) putItem(&r, &first, f->name, 5, f->type ? f->type->name : NULL);
        }
        for (int i = 0; i < t->methods.len; i++) {
            Node *m = t->methods.items[i];
            StrBuf sig = {0};
            signature(&sig, m);
            putItem(&r, &first, m->name, 2, sig.buf);
            sbFree(&sig);
        }
    } else if (t && t->kind == TY_STRING) {
        putItem(&r, &first, "len", 5, "i64");
        putItem(&r, &first, "append", 2, "void String.append(str text)");
        putItem(&r, &first, "free", 2, "void String.free()");
        putItem(&r, &first, "reserve", 2, "void String.reserve(i64 n)");
        putItem(&r, &first, "insert", 2, "void String.insert(i64 at, str text)");
        putItem(&r, &first, "truncate", 2, "void String.truncate(i64 n)");
        putItem(&r, &first, "clear", 2, "void String.clear()");
        putItem(&r, &first, "copy", 2, "String String.copy()");
        textMethods(&r, &first, prog);
    } else if (t && t->kind == TY_SLICE) {
        putItem(&r, &first, "len", 5, "i64");
        putItem(&r, &first, "ptr", 5, NULL);
        if (t->elem && t->elem->kind == TY_INT && t->elem->bits == 8 && !t->elem->isSigned) {
            putItem(&r, &first, "copy", 2, "String str.copy()");
            textMethods(&r, &first, prog);
        }
    } else {
        /* a module: the names in its scope */
        for (Module *m = prog ? prog->modules : NULL; m; m = m->next) {
            const char *base = strrchr(m->path, '/');
            base = base ? base + 1 : m->path;
            if (strcmp(base, word) != 0 || m == prog->main) continue;
            for (int f = 0; f < m->files.len; f++) {
                Node *file = m->files.items[f];
                for (int i = 0; i < file->list.len; i++) {
                    Node *d = file->list.items[i];
                    if (!(d->flags & F_PUB) || !d->name) continue;
                    StrBuf sig = {0};
                    signature(&sig, d);
                    putItem(&r, &first, d->name, d->kind == N_FUNC ? 3 : d->kind == N_STRUCT ? 22 : 21, sig.buf);
                    sbFree(&sig);
                }
            }
        }
    }
    sbPutc(&r, ']');
    reply(out, id, r.buf);
    sbFree(&r);
}

/* ---------------------------------------------------------------- completion of names */

typedef struct {
    StrBuf r;
    bool first;
    const char **seen;
    int nSeen, capSeen;
} Items;

/* Adds an item once: an inner name hides an outer one of the same name. */
static void addItem(Items *it, const char *label, int kind, const char *detail) {
    if (!label || !*label) return;
    for (int i = 0; i < it->nSeen; i++) {
        if (strcmp(it->seen[i], label) == 0) return;
    }
    if (it->nSeen == it->capSeen) {
        it->capSeen = it->capSeen ? it->capSeen * 2 : 64;
        it->seen = realloc(it->seen, sizeof *it->seen * (size_t)it->capSeen);
        if (!it->seen) return;
    }
    it->seen[it->nSeen++] = label;
    putItem(&it->r, &it->first, label, kind, detail);
}

static size_t offsetOf(Source *src, int line, int col) {
    const char *ls = lineStartOf(src->text, src->len, line);
    size_t off = (size_t)(ls - src->text) + (size_t)(col > 0 ? col - 1 : 0);
    return off < src->len ? off : src->len;
}

/* Where the '}' that closes the '{' at off is, skipping strings, characters
 * and comments; the end of the text when it is missing, as while typing. */
static size_t closeOf(Source *src, size_t off) {
    const char *t = src->text;
    int depth = 0;
    for (size_t i = off; i < src->len; i++) {
        char ch = t[i];
        if (ch == '/' && i + 1 < src->len && t[i + 1] == '/') {
            while (i < src->len && t[i] != '\n') i++;
        } else if (ch == '"' || ch == '\'') {
            bool triple = ch == '"' && i + 2 < src->len && t[i + 1] == '"' && t[i + 2] == '"';
            i += triple ? 3 : 1;
            while (i < src->len) {
                if (t[i] == '\\' && !triple) i++;
                else if (triple && t[i] == '"' && i + 2 < src->len && t[i + 1] == '"' && t[i + 2] == '"') { i += 2; break; }
                else if (!triple && (t[i] == ch || t[i] == '\n')) break;
                i++;
            }
        } else if (ch == '{') {
            depth++;
        } else if (ch == '}' && --depth == 0) {
            return i;
        }
    }
    return src->len;
}

/* Whether the cursor is inside the block: after its '{', up to its '}'. */
static bool inBlock(Source *src, Node *block, size_t cursor) {
    if (!block || block->kind != N_BLOCK || block->src != src) return false;
    size_t open = offsetOf(src, block->line, block->col);
    return cursor > open && cursor <= closeOf(src, open);
}

static const char *typeName(Node *n) { return n->type && n->type->kind != TY_ERROR ? n->type->name : NULL; }

/* The names a function's code can see at the cursor: what was declared above
 * it, in the blocks around it. */
static void localNames(Items *it, Source *src, Node *n, size_t cursor) {
    if (!n || n->src != src) return;
    if (offsetOf(src, n->line, n->col) >= cursor && n->kind != N_BLOCK) return;     /* below the cursor */
    switch (n->kind) {
    case N_BLOCK:
        if (!inBlock(src, n, cursor)) return;
        break;
    case N_VAR: case N_PARAM:
        addItem(it, n->name, 6, typeName(n));
        break;
    case N_DEFINE:
        for (int i = 0; i < n->list.len; i++) addItem(it, n->list.items[i]->name, 6, typeName(n->list.items[i]));
        break;
    case N_FOR:
        if (inBlock(src, n->c, cursor)) {
            for (int i = 0; i < n->list.len; i++) addItem(it, n->list.items[i]->name, 6, typeName(n->list.items[i]));
        }
        break;
    case N_CATCH:
        if (n->name && !n->ival && inBlock(src, n->c, cursor)) addItem(it, n->name, 6, "Error");
        break;
    default:
        break;
    }
    localNames(it, src, n->a, cursor);
    localNames(it, src, n->b, cursor);
    localNames(it, src, n->c, cursor);
    localNames(it, src, n->d, cursor);
    for (int i = 0; i < n->list.len; i++) localNames(it, src, n->list.items[i], cursor);
    for (int i = 0; i < n->list2.len; i++) localNames(it, src, n->list2.items[i], cursor);
}

static int declKind(Node *d) {
    switch (d->kind) {
    case N_FUNC: return 3;
    case N_STRUCT: return 22;
    case N_ENUM: return 13;
    case N_INTERFACE: return 8;
    case N_CONST: return 21;
    case N_TYPEDEF: return 7;
    default: return 6;
    }
}

/* A module's top-level names: all of them in its own files, else the pub ones. */
static void moduleNames(Items *it, Module *m, bool own) {
    for (int f = 0; f < m->files.len; f++) {
        Node *file = m->files.items[f];
        for (int i = 0; i < file->list.len; i++) {
            Node *d = file->list.items[i];
            Node *named = d->kind == N_DEFINE && d->list.len == 1 ? d->list.items[0] : d;
            if (!named->name || d->kind == N_IMPORT || d->kind == N_TEST || (d->kind == N_FUNC && d->b)) continue;
            if (d->kind == N_FUNC && (d->flags & F_TESTFN)) continue;
            if (!own && !(d->flags & F_PUB)) continue;
            StrBuf sig = {0};
            signature(&sig, d);
            addItem(it, named->name, declKind(d), sig.buf);
            sbFree(&sig);
        }
    }
}

static bool startsWord(const char *s, const char *end, const char *w);

/* The innermost '(', '[' or '{' that is open at off, skipping strings,
 * characters and comments, or -1. With want, the innermost one of that
 * kind. *commas gets the commas after it, at its own level. */
static long openAt(const char *t, size_t off, char want, int *commas) {
    long stack[256];
    int nc[256];
    int depth = 0;
    for (size_t i = 0; i < off; i++) {
        char ch = t[i];
        if (ch == '/' && i + 1 < off && t[i + 1] == '/') {
            while (i < off && t[i] != '\n') i++;
        } else if (ch == '"' || ch == '\'') {
            bool triple = ch == '"' && i + 2 < off && t[i + 1] == '"' && t[i + 2] == '"';
            i += triple ? 3 : 1;
            while (i < off) {
                if (t[i] == '\\' && !triple) i++;
                else if (triple && t[i] == '"' && i + 2 < off && t[i + 1] == '"' && t[i + 2] == '"') { i += 2; break; }
                else if (!triple && (t[i] == ch || t[i] == '\n')) break;
                i++;
            }
        } else if (ch == '(' || ch == '[' || ch == '{') {
            if (depth < 256) {
                stack[depth] = (long)i;
                nc[depth] = 0;
            }
            depth++;
        } else if ((ch == ')' || ch == ']' || ch == '}') && depth > 0) {
            depth--;
        } else if (ch == ',' && depth > 0 && depth <= 256) {
            nc[depth - 1]++;
        }
    }
    if (depth > 256) depth = 256;
    while (want && depth > 0 && t[stack[depth - 1]] != want) depth--;
    if (depth <= 0) return -1;
    if (commas) *commas = nc[depth - 1];
    return stack[depth - 1];
}

/* The start of the name that ends at end: letters, digits, '_' and dots, as
 * in models.User. */
static const char *nameBefore(const char *text, const char *end) {
    const char *s = end;
    while (s > text && (isalnum((unsigned char)s[-1]) || s[-1] == '_' || s[-1] == '.')) s--;
    return s;
}

/* In a struct literal, where a field's name goes (P{x: 1, |}): the fields
 * that the literal does not set yet. False when the cursor is not there. */
static bool literalFields(Items *it, Program *prog, Source *src, size_t cursor) {
    const char *t = src->text;
    const char *p = t + cursor;
    while (p > t && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) p--;     /* the name typed so far */
    while (p > t && isspace((unsigned char)p[-1])) p--;
    if (p == t || (p[-1] != '{' && p[-1] != ',')) return false;
    long open = openAt(t, cursor, 0, NULL);
    if (open < 0 || t[open] != '{') return false;
    const char *end = t + open;
    if (end > t && end[-1] == ']') {       /* Box[i64]{...} */
        int depth = 0;
        do {
            end--;
            if (*end == ']') depth++;
            else if (*end == '[') depth--;
        } while (end > t && depth > 0);
    }
    const char *name = nameBefore(t, end);
    if (name == end || !(isalpha((unsigned char)*name) || *name == '_')) return false;
    const char *before = name;
    while (before > t && (before[-1] == ' ' || before[-1] == '\t')) before--;
    const char *word = nameBefore(t, before);
    if (word < before && (startsWord(word, before, "struct") || startsWord(word, before, "enum") ||
                          startsWord(word, before, "interface"))) {
        return false;      /* the body of a declaration */
    }
    Node *d = NULL;
    const char *dot = memchr(name, '.', (size_t)(end - name));
    for (Module *m = prog->modules; m && !d; m = m->next) {
        if (dot) {
            if (m->name && strlen(m->name) == (size_t)(dot - name) && !strncmp(m->name, name, (size_t)(dot - name))) {
                d = typeIn(m, dot + 1, (size_t)(end - dot - 1));
            }
        } else {
            for (int f = 0; f < m->files.len && !d; f++) {
                if (m->files.items[f]->src == src) d = typeIn(m, name, (size_t)(end - name));
            }
        }
    }
    if (!d && !dot) d = typeIn(prog->core, name, (size_t)(end - name));
    if (!d || d->kind != N_STRUCT) return false;
    /* the names before a ':' at the top level of the literal */
    const char *set[256];
    size_t setLen[256];
    int nset = 0, depth = 0;
    bool head = true;
    for (const char *q = t + open + 1; q < t + cursor; q++) {
        char ch = *q;
        if (ch == '"') {
            for (q++; q < t + cursor && *q != '"' && *q != '\n'; q++) {
                if (*q == '\\') q++;
            }
            continue;
        }
        if (ch == '(' || ch == '[' || ch == '{') depth++;
        else if ((ch == ')' || ch == ']' || ch == '}') && depth > 0) depth--;
        else if (ch == ',' && depth == 0) head = true;
        else if (head && (isalpha((unsigned char)ch) || ch == '_')) {
            const char *a = q;
            while (q < t + cursor && (isalnum((unsigned char)*q) || *q == '_')) q++;
            const char *b = q;
            while (q < t + cursor && isspace((unsigned char)*q)) q++;
            if (q < t + cursor && *q == ':' && nset < 256) {
                set[nset] = a;
                setLen[nset++] = (size_t)(b - a);
            }
            head = false;
            q--;
        } else if (!isspace((unsigned char)ch)) {
            head = false;
        }
    }
    for (int i = 0; i < d->list.len; i++) {
        Node *f = d->list.items[i];
        if (f->kind == N_EMBED || !f->name) continue;
        bool done = false;
        for (int k = 0; k < nset && !done; k++) {
            done = setLen[k] == strlen(f->name) && !strncmp(set[k], f->name, setLen[k]);
        }
        if (!done) addItem(it, f->name, 5, f->type ? f->type->name : NULL);
    }
    return true;
}

/* Any name: locals, the module's names, imports, built-ins and keywords. */
static void nameCompletion(StrBuf *out, J *id, Program *prog, Source *src, int line, int col) {
    Items it = {.first = true};
    sbPutc(&it.r, '[');
    Module *own = NULL;
    Node *ownFile = NULL;
    for (Module *m = prog ? prog->modules : NULL; m && !own; m = m->next) {
        for (int f = 0; f < m->files.len; f++) {
            if (m->files.items[f]->src == src) {
                own = m;
                ownFile = m->files.items[f];
            }
        }
    }
    if (own && src && literalFields(&it, prog, src, offsetOf(src, line, col))) {
        sbPutc(&it.r, ']');
        reply(out, id, it.r.buf);
        sbFree(&it.r);
        free(it.seen);
        return;
    }
    if (own && src) {
        size_t cursor = offsetOf(src, line, col);
        for (int i = 0; i < ownFile->list.len; i++) {
            Node *d = ownFile->list.items[i];
            Node *body = d->kind == N_FUNC || d->kind == N_TEST ? d->c : NULL;
            if (inBlock(src, body, cursor)) {
                localNames(&it, src, body, cursor);
                for (int k = 0; k < d->list.len; k++) {
                    if (d->list.items[k]->kind == N_PARAM) addItem(&it, d->list.items[k]->name, 6, typeName(d->list.items[k]));
                }
            }
        }
        moduleNames(&it, own, true);
        /* the modules this file imports, by the name code calls them */
        for (int i = 0; i < ownFile->list.len; i++) {
            Node *imp = ownFile->list.items[i];
            if (imp->kind != N_IMPORT) continue;
            for (int k = 0; k < imp->list.len; k++) {
                Node *item = imp->list.items[k];
                if (item->flags & F_C_IMPORT) continue;
                const char *name = item->name;
                if (item->text) {
                    const char *slash = strrchr(item->text, '/');
                    name = slash ? slash + 1 : item->text;
                }
                addItem(&it, name, 9, "module");
            }
        }
        for (Module *m = prog->modules; m; m = m->next) {
            if (m->isStd && m->name && strcmp(m->name, "core") == 0) moduleNames(&it, m, false);
        }
    }
    static const char *builtins[] = {"print", "format", "panic", "error", "alloc", "free", "sizeof", "sleep", "after", "expect"};
    /* the standard library's modules this file doesn't import yet: taking one adds its import */
    if (src) {
        const char *mods[128];
        int n = stdModuleNames(mods, 128);
        for (int i = 0; i < n; i++) {
            if (!isStdModule(mods[i]) || importsModule(src->text, src->len, mods[i])) continue;
            StrBuf e = {0};
            importEdits(&e, src->text, src->len, mods[i]);
            gItemEdits = e.buf;
            addItem(&it, mods[i], 9, "standard library, adds its import");
            gItemEdits = NULL;
            sbFree(&e);
        }
    }
    for (size_t i = 0; i < sizeof builtins / sizeof builtins[0]; i++) {
        /* the declaration from builtin.iris, as "void print(str text)" */
        const Builtin *bi = builtinOf(builtins[i]);
        StrBuf d = {0};
        const char *decl = bi && bi->decl ? bi->decl : "built-in";
        if (!strncmp(decl, "@noreturn ", 10)) decl += 10;
        sbPrintf(&d, "%.*s", (int)strcspn(decl, "\n"), decl);
        addItem(&it, builtins[i], 3, d.buf);
        sbFree(&d);
    }
    static const char *types[] = {"void", "bool", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64",
                                  "str", "String", "Error", "Duration", "Mutex", "Thread", "Atomic"};
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++) addItem(&it, types[i], 7, "type");
    for (size_t i = 0; i < sizeof irisWords / sizeof irisWords[0]; i++) addItem(&it, irisWords[i], 14, NULL);
    sbPutc(&it.r, ']');
    reply(out, id, it.r.buf);
    sbFree(&it.r);
    free(it.seen);
}

/* ---------------------------------------------------------------- completion of attributes */

typedef struct {
    const char *name, *detail;
} Attr;

static const Attr structAttrs[] = {
    {"json", "turns it into JSON and back"},
    {"entity", "a row of a table: @entity(\"users\")"},
    {"wire", "a binary message: @wire or @wire(le)"},
    {"proto", "a protobuf message"},
    {"env", "filled from environment variables: @env or @env(\"APP\")"},
    {"flags", "filled from the program's arguments: @flags(\"what it does\")"},
};
static const Attr fieldAttrs[] = {
    {"json", "its JSON name: @json(\"id\", required)"},
    {"id", "the primary key of an @entity"},
    {"column", "its column's name: @column(\"created_at\")"},
    {"wire", "how it is sent: @wire(be), @wire(len: u16, max: 1024)"},
    {"proto", "its field number: @proto(1), @proto(2, zigzag)"},
    {"env", "its variable: @env(\"PORT\", required)"},
    {"flag", "its flag: @flag('p', \"the port\")"},
};
static const Attr funcAttrs[] = {
    {"inline", "always puts its code where it is called"},
    {"unchecked", "leaves out the checks of debug builds"},
    {"noreturn", "a call to it never comes back"},
    {"export", "callable from C by its name"},
};
static const Attr enumAttrs[] = {
    {"wire", "sent as an integer: @wire(u8), @wire(u16) or @wire(u32)"},
};

static void addAttrs(Items *it, const Attr *a, size_t n) {
    for (size_t i = 0; i < n; i++) addItem(it, a[i].name, 14, a[i].detail);
}

/* Whether the code at s starts with the word w. */
static bool startsWord(const char *s, const char *end, const char *w) {
    size_t n = strlen(w);
    return (size_t)(end - s) >= n && memcmp(s, w, n) == 0 &&
           (s + n == end || !(isalnum((unsigned char)s[n]) || s[n] == '_'));
}

/* Skips blanks, attributes and pub at the start of a declaration. */
static const char *declStart(const char *s, const char *end) {
    for (;;) {
        while (s < end && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) s++;
        if (s < end && *s == '@') {
            s++;
            while (s < end && (isalnum((unsigned char)*s) || *s == '_')) s++;
            if (s < end && *s == '(') {
                while (s < end && *s != ')' && *s != '\n') s++;
                if (s < end && *s == ')') s++;
            }
        } else if (startsWord(s, end, "pub")) {
            s += 3;
        } else if (s + 1 < end && s[0] == '/' && s[1] == '/') {
            while (s < end && *s != '\n') s++;
        } else {
            return s;
        }
    }
}

/* After @: the attributes of the declaration the @ goes on. In a struct's
 * body, a field's; at the top, those of the struct, enum or function below,
 * or all of them before nothing yet; in a function's body, none. */
static void attrCompletion(StrBuf *out, J *id, const char *text, size_t len, size_t at) {
    Items it = {.first = true};
    sbPutc(&it.r, '[');
    /* the braces open at the @, outside strings and comments */
    size_t open[64];
    int depth = 0;
    for (size_t i = 0; i < at; i++) {
        char ch = text[i];
        if (ch == '/' && i + 1 < at && text[i + 1] == '/') {
            while (i < at && text[i] != '\n') i++;
        } else if (ch == '/' && i + 1 < at && text[i + 1] == '*') {
            for (i += 2; i + 1 < at && !(text[i] == '*' && text[i + 1] == '/'); i++) {}
            i++;
        } else if (ch == '"' && i + 2 < at && text[i + 1] == '"' && text[i + 2] == '"') {
            for (i += 3; i + 2 < at && !(text[i] == '"' && text[i + 1] == '"' && text[i + 2] == '"'); i++) {}
            i += 2;
        } else if (ch == '"' || ch == '\'') {
            for (i++; i < at && text[i] != ch && text[i] != '\n'; i++) {
                if (text[i] == '\\') i++;
            }
        } else if (ch == '{') {
            size_t ls = i;
            while (ls > 0 && text[ls - 1] != '\n') ls--;
            if (depth < 64) open[depth] = ls;
            depth++;
        } else if (ch == '}' && depth > 0) {
            depth--;
        }
    }
    const char *end = text + len;
    if (depth > 0) {
        const char *d = depth <= 64 ? declStart(text + open[depth - 1], end) : end;
        if (depth == 1 && startsWord(d, end, "struct")) addAttrs(&it, fieldAttrs, sizeof fieldAttrs / sizeof *fieldAttrs);
    } else {
        /* the declaration below: past the name being typed and other attributes */
        const char *s = text + at + 1;
        while (s < end && (isalnum((unsigned char)*s) || *s == '_')) s++;
        if (s < end && *s == '(') {
            while (s < end && *s != ')' && *s != '\n') s++;
        }
        while (s < end && *s != '\n') s++;
        s = declStart(s, end);
        if (s >= end) {
            addAttrs(&it, structAttrs, sizeof structAttrs / sizeof *structAttrs);
            addAttrs(&it, funcAttrs, sizeof funcAttrs / sizeof *funcAttrs);
        } else if (startsWord(s, end, "struct")) {
            addAttrs(&it, structAttrs, sizeof structAttrs / sizeof *structAttrs);
        } else if (startsWord(s, end, "enum")) {
            addAttrs(&it, enumAttrs, sizeof enumAttrs / sizeof *enumAttrs);
        } else {
            addAttrs(&it, funcAttrs, sizeof funcAttrs / sizeof *funcAttrs);
        }
    }
    sbPutc(&it.r, ']');
    reply(out, id, it.r.buf);
    sbFree(&it.r);
    free(it.seen);
}

/* In an import line: the standard library's modules. */
static void importCompletion(StrBuf *out, J *id) {
    Items it = {.first = true};
    sbPutc(&it.r, '[');
    const char *names[128];
    int n = stdModuleNames(names, 128);
    for (int i = 0; i < n; i++) addItem(&it, names[i], 9, "standard library");
    sbPutc(&it.r, ']');
    reply(out, id, it.r.buf);
    sbFree(&it.r);
    free(it.seen);
}

static void formatting(StrBuf *out, J *id, Doc *doc) {
    if (!doc) {
        reply(out, id, NULL);
        return;
    }
    Source src = {doc->path, doc->text, doc->len};
    char *text = formatSource(&gA, &src);
    if (!text || strcmp(text, doc->text) == 0) {
        reply(out, id, "[]");
        return;
    }
    int lines = 1;
    for (size_t i = 0; i < doc->len; i++) lines += doc->text[i] == '\n';
    StrBuf r = {0};
    sbPrintf(&r, "[{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":%d,\"character\":0}},"
                 "\"newText\":", lines);
    putStr(&r, text);
    sbPuts(&r, "}]");
    reply(out, id, r.buf);
    sbFree(&r);
}

/* Closes what the text leaves open, as while typing: the ( [ { that have no
 * partner get one at the end, so the function being written still parses.
 * Only in the child, whose copy of the document this changes. */
static void closeOpen(Doc *doc) {
    char stack[256];
    int depth = 0;
    const char *t = doc->text;
    size_t n = doc->len;
    for (size_t i = 0; i < n; i++) {
        char ch = t[i];
        if (ch == '/' && i + 1 < n && t[i + 1] == '/') {
            while (i < n && t[i] != '\n') i++;
        } else if (ch == '"' || ch == '\'') {
            bool triple = ch == '"' && i + 2 < n && t[i + 1] == '"' && t[i + 2] == '"';
            i += triple ? 3 : 1;
            while (i < n) {
                if (t[i] == '\\' && !triple) i++;
                else if (triple && t[i] == '"' && i + 2 < n && t[i + 1] == '"' && t[i + 2] == '"') { i += 2; break; }
                else if (!triple && (t[i] == ch || t[i] == '\n')) break;
                i++;
            }
        } else if (ch == '(' || ch == '[' || ch == '{') {
            if (depth < (int)sizeof stack) stack[depth] = ch;
            depth++;
        } else if ((ch == ')' || ch == ']' || ch == '}') && depth > 0) {
            depth--;
        }
    }
    if (depth <= 0) return;
    if (depth > (int)sizeof stack) depth = sizeof stack;
    char *text = malloc(n + (size_t)depth * 2 + 2);
    if (!text) return;
    memcpy(text, t, n);
    size_t len = n;
    text[len++] = '\n';
    for (int k = depth - 1; k >= 0; k--) {
        text[len++] = stack[k] == '(' ? ')' : stack[k] == '[' ? ']' : '}';
        text[len++] = '\n';
    }
    text[len] = 0;
    doc->text = text;
    doc->len = len;
}

/* ---------------------------------------------------------------- inlay hints */

typedef struct {
    StrBuf r;
    bool first;
    Source *src;
    int from, to;       /* the lines of the range that the editor asks for */
} Hints;

/* A value written as it is: 1, -2.5, "text", 'c', true, null. */
static bool isLiteral(Node *n) {
    if (n->kind == N_UNARY && n->a && (n->a->kind == N_INT || n->a->kind == N_FLOAT)) return true;
    return n->kind == N_INT || n->kind == N_FLOAT || n->kind == N_STRING || n->kind == N_INTERP ||
           n->kind == N_CHAR || n->kind == N_BOOL || n->kind == N_NULL;
}

/* Whether the argument at off already has its name in the code: add(b: 2). */
static bool namedInText(Source *src, size_t off) {
    const char *t = src->text;
    while (off > 0 && isspace((unsigned char)t[off - 1])) off--;
    return off > 0 && t[off - 1] == ':';
}

/* The parameter names of a call whose arguments are literals: add(a: 1, b: 2).
 * A call with one argument gets no hint, nor does an argument with a name. */
static void callHints(Hints *h, Node *call) {
    Node *fn = call->ref;
    if (!fn || fn->kind != N_FUNC || call->list.len < fn->list.len) return;
    int first = fn->list.len && (fn->list.items[0]->flags & F_SELF) ? 1 : 0;
    Node *given[64];
    Node *params[64];
    int n = 0;
    for (int j = first; j < fn->list.len && n < 64; j++) {
        Node *param = fn->list.items[j], *v = call->list.items[j];
        while (v && v->kind == N_CONVERT && v->a) v = v->a;
        /* a default value and the T... list are not in the call's text */
        if (!v || v == param->b || (param->flags & F_REST) || v->src != h->src || !param->name) continue;
        given[n] = v;
        params[n++] = param;
    }
    if (n < 2) return;
    for (int i = 0; i < n; i++) {
        Node *v = given[i];
        if (!isLiteral(v) || v->line < h->from || v->line > h->to) continue;
        if (namedInText(h->src, offsetOf(h->src, v->line, v->col))) continue;
        if (!h->first) sbPutc(&h->r, ',');
        h->first = false;
        sbPuts(&h->r, "{\"position\":");
        putPos(&h->r, h->src, v->line, v->col);
        sbPuts(&h->r, ",\"label\":");
        StrBuf label = {0};
        sbPrintf(&label, "%s:", params[i]->name);
        putStr(&h->r, label.buf);
        sbFree(&label);
        sbPuts(&h->r, ",\"kind\":2,\"paddingRight\":true}");
    }
}

static void hintWalk(Hints *h, Node *n) {
    if (!n) return;
    if (n->kind == N_CALL && n->src == h->src) callHints(h, n);
    hintWalk(h, n->a);
    hintWalk(h, n->b);
    hintWalk(h, n->c);
    hintWalk(h, n->d);
    for (int i = 0; i < n->list.len; i++) hintWalk(h, n->list.items[i]);
    for (int i = 0; i < n->list2.len; i++) hintWalk(h, n->list2.items[i]);
}

/* textDocument/inlayHint: the parameter names at calls with literals. */
static void inlayHints(StrBuf *out, J *id, J *params, Program *prog, Source *src) {
    Hints h = {.first = true, .src = src, .from = 1, .to = 1 << 30};
    J *range = jget(params, "range");
    J *a = range ? jget(jget(range, "start"), "line") : NULL, *b = range ? jget(jget(range, "end"), "line") : NULL;
    if (a) h.from = (int)a->num + 1;
    if (b) h.to = (int)b->num + 1;
    sbPutc(&h.r, '[');
    for (Module *m = prog && src ? prog->modules : NULL; m; m = m->next) {
        for (int f = 0; f < m->files.len; f++) {
            if (m->files.items[f]->src == src) hintWalk(&h, m->files.items[f]);
        }
    }
    sbPutc(&h.r, ']');
    reply(out, id, h.r.buf);
    sbFree(&h.r);
}

typedef struct {
    StrBuf *b;
    const char *sig;
    int n;
} ParamLabels;

static void paramLabel(void *arg, const char *a, const char *b, const char *w, const char *we) {
    (void)w;
    (void)we;
    ParamLabels *l = arg;
    sbPrintf(l->b, "%s{\"label\":[%d,%d]}", l->n++ ? "," : "", (int)(a - l->sig), (int)(b - l->sig));
}

/* The call around the cursor, which handleInChild finds before it analyzes
 * the text: the name of the function, the name before its dot (a value or
 * a module), and the argument at the cursor. */
typedef struct {
    char label[128], word[128];
    int active;
} Call;

/* Finds the call that the cursor is in and blanks it, so the line parses as
 * for a completion. False when the cursor is in no call. */
static bool callAt(Doc *doc, size_t off, Call *call) {
    char *t = (char *)doc->text;
    long open = openAt(t, off, '(', &call->active);
    if (open < 0) return false;
    char *end = t + open;
    while (end > t && (end[-1] == ' ' || end[-1] == '\t')) end--;
    char *name = (char *)nameBefore(t, end);
    if (name == end) return false;
    char *dot = NULL;
    for (char *q = name; q < end; q++) {
        if (*q == '.') dot = q;
    }
    char *label = dot ? dot + 1 : name;
    if (end - label <= 0 || end - label >= (long)sizeof call->label) return false;
    snprintf(call->label, sizeof call->label, "%.*s", (int)(end - label), label);
    call->word[0] = 0;
    if (dot) {
        char *w = dot;
        while (w > name && w[-1] != '.') w--;
        if (dot - w <= 0 || dot - w >= (long)sizeof call->word) return false;
        snprintf(call->word, sizeof call->word, "%.*s", (int)(dot - w), w);
    }
    /* blank from the dot (or the name) to the ')' that closes the call */
    size_t stop = off;
    int depth = 0;
    for (size_t i = (size_t)open; i < doc->len; i++) {
        if (t[i] == '(') depth++;
        else if (t[i] == ')' && --depth == 0) {
            stop = i + 1;
            break;
        } else if (t[i] == '\n' && i >= off) {
            break;
        }
    }
    for (char *q = dot ? dot : name; q < t + stop; q++) {
        if (*q != '\n') *q = ' ';
    }
    return true;
}

/* textDocument/signatureHelp: the signature of the function that the cursor
 * calls, with the argument at the cursor. */
static void signatureHelp(StrBuf *out, J *id, Program *prog, Source *src, Call *call, int line, int col) {
    StrBuf scratch = {0};
    gWantLabel = call->label;
    if (call->word[0]) completion(&scratch, id, prog, src, call->word, line);
    else nameCompletion(&scratch, id, prog, src, line, col);
    gWantLabel = NULL;
    sbFree(&scratch);
    const char *sig = gFound.buf;
    const char *params = sig ? paramsOf(call->label, sig) : NULL;
    if (!params) {
        reply(out, id, NULL);
        sbFree(&gFound);
        return;
    }
    StrBuf r = {0};
    sbPuts(&r, "{\"signatures\":[{\"label\":");
    putStr(&r, sig);
    sbPuts(&r, ",\"parameters\":[");
    ParamLabels l = {&r, sig, 0};
    eachParam(params, paramLabel, &l);
    int active = call->active < l.n ? call->active : (l.n ? l.n - 1 : 0);
    sbPrintf(&r, "]}],\"activeSignature\":0,\"activeParameter\":%d}", active);
    reply(out, id, r.buf);
    sbFree(&r);
    sbFree(&gFound);
}

/* Handles one message in the child; the output is messages separated by
 * NUL bytes. */
static void handleInChild(J *msg, StrBuf *out) {
    const char *method = jstr(msg, "method");
    J *id = jget(msg, "id");
    J *params = jget(msg, "params");
    J *td = jget(params, "textDocument");
    const char *uri = jstr(td, "uri");
    Doc *doc = uri ? findDoc(uri) : NULL;
    char *path = uri ? pathOf(uri) : NULL;
    if (strcmp(method, "textDocument/formatting") == 0) {
        formatting(out, id, doc);
        free(path);
        return;
    }
    if (strcmp(method, "textDocument/codeAction") == 0) {
        codeActions(out, id, params, uri, doc);
        free(path);
        return;
    }
    if (strcmp(method, "workspace/symbol") == 0) {
        workspaceSymbols(out, id, jstr(params, "query"));
        free(path);
        return;
    }
    J *pos = jget(params, "position");
    int line = 0, character = 0;
    if (pos) {
        J *l = jget(pos, "line"), *c = jget(pos, "character");
        line = l ? (int)l->num + 1 : 1;
        character = c ? (int)c->num : 0;
    }
    char word[256] = "";
    bool inImport = false;
    if (strcmp(method, "textDocument/completion") == 0 && doc) {
        /* the name before the dot; the dot itself would not parse, so it goes,
         * and so does what was typed after it */
        int col = byteCol(doc->text, doc->len, line, character);
        char *ls = (char *)lineStartOf(doc->text, doc->len, line);
        char *typed = ls + col - 1;
        while (typed > ls && (isalnum((unsigned char)typed[-1]) || typed[-1] == '_')) typed--;
        if (typed > ls && typed[-1] == '@') {
            attrCompletion(out, id, doc->text, doc->len, (size_t)(typed - 1 - doc->text));
            free(path);
            return;
        }
        const char *t = ls;
        while (t < typed && (*t == ' ' || *t == '\t')) t++;
        inImport = !strncmp(t, "import", 6) && (t[6] == ' ' || t[6] == '\t') && !memchr(t, '"', (size_t)(typed - t));
        char *dot = typed - 1;
        if (dot >= ls && *dot == '.') {
            for (char *q = typed; q < ls + col - 1; q++) *q = ' ';
            char *s = dot;
            while (s > ls && (isalnum((unsigned char)s[-1]) || s[-1] == '_')) s--;
            size_t n = (size_t)(dot - s);
            if (n < sizeof word) {
                memcpy(word, s, n);
                word[n] = 0;
            }
            *dot = ' ';
        }
    }
    Call call = {0};
    bool inCall = false;
    if (strcmp(method, "textDocument/signatureHelp") == 0 && doc) {
        const char *ls = lineStartOf(doc->text, doc->len, line);
        size_t off = (size_t)(ls - doc->text) + (size_t)byteCol(doc->text, doc->len, line, character) - 1;
        inCall = callAt(doc, off < doc->len ? off : doc->len, &call);
    }
    /* a line being typed seldom parses: answer from what the parser could read */
    gLenient = strcmp(method, "textDocument/completion") == 0 || strcmp(method, "textDocument/signatureHelp") == 0 ||
               strcmp(method, "textDocument/inlayHint") == 0 || strcmp(method, "textDocument/documentHighlight") == 0 ||
               strcmp(method, "textDocument/hover") == 0 ||
               strcmp(method, "textDocument/references") == 0 ||
               strcmp(method, "textDocument/definition") == 0 || strcmp(method, "textDocument/documentSymbol") == 0;
    if (gLenient && doc) closeOpen(doc);
    /* time. before import time: the module is loaded by an import at the end,
     * where no line moves, and each item adds the real import */
    StrBuf autoImport = {0};
    if (word[0] && doc && strcmp(method, "textDocument/completion") == 0 && isStdModule(word) &&
        !importsModule(doc->text, doc->len, word)) {
        importEdits(&autoImport, doc->text, doc->len, word);
        size_t wl = strlen(word);
        char *text = malloc(doc->len + wl + 10);
        if (text) {
            memcpy(text, doc->text, doc->len);
            size_t len = doc->len;
            len += (size_t)sprintf(text + len, "\nimport %s\n", word);
            doc->text = text;
            doc->len = len;
        }
    }
    char *std = path ? stdPathOf(path) : NULL;
    Program *prog;
    Source *src;
    if (std) {
        /* a std file opened by go to definition: its module as the compiler
         * loads it, which can use @extern; std files are never reported */
        char mod[128] = "";
        sscanf(std + 4, "%127[^/]", mod);
        snprintf(viewText, sizeof viewText, "%s%s\n", strcmp(mod, "core") ? "import " : "", strcmp(mod, "core") ? mod : "");
        prog = analyze(viewPath);
        src = sourceOf(prog, std);
        ndiags = 0;
    } else if (path && isBuiltinFile(path)) {
        /* builtin.iris only shows the built-in names: nothing to check */
        prog = NULL;
        src = NULL;
        ndiags = 0;
    } else {
        prog = NULL;
        src = NULL;
        bool whole = strcmp(method, "textDocument/references") == 0 || strcmp(method, "textDocument/rename") == 0;
        char root[4096] = "";
        if (whole && path) projectRoot(path, root, sizeof root);
        const char *slash = path ? strrchr(path, '/') : NULL;
        if (root[0] && slash && (strlen(root) != (size_t)(slash - path) || strncmp(root, path, strlen(root)) != 0)) {
            /* a file in a module of the project: the root's program also has
             * the files that import it */
            prog = analyze(root);
            src = sourceOf(prog, path);
        }
        if (!src) {
            prog = path ? analyze(path) : NULL;
            src = path ? sourceOf(prog, path) : NULL;
        }
    }
    free(std);
    int col = doc ? byteCol(doc->text, doc->len, line, character) : character + 1;
    if (strcmp(method, "textDocument/hover") == 0) {
        hover(out, id, prog, src, line, col);
    } else if (strcmp(method, "textDocument/definition") == 0) {
        definition(out, id, prog, src, line, col);
    } else if (strcmp(method, "textDocument/documentSymbol") == 0) {
        symbols(out, id, prog, src);
    } else if (strcmp(method, "textDocument/references") == 0) {
        J *ctx = jget(params, "context");
        J *decl = ctx ? jget(ctx, "includeDeclaration") : NULL;
        references(out, id, prog, src, line, col, !decl || decl->type != 'f');
    } else if (strcmp(method, "textDocument/prepareRename") == 0) {
        prepareRename(out, id, prog, src, line, col);
    } else if (strcmp(method, "textDocument/rename") == 0) {
        renameAll(out, id, prog, src, line, col, jstr(params, "newName"));
    } else if (strcmp(method, "textDocument/documentHighlight") == 0) {
        highlights(out, id, prog, src, line, col);
    } else if (strcmp(method, "textDocument/inlayHint") == 0) {
        inlayHints(out, id, params, prog, src);
    } else if (strcmp(method, "textDocument/signatureHelp") == 0) {
        if (inCall) signatureHelp(out, id, prog, src, &call, line, col);
        else reply(out, id, NULL);
    } else if (strcmp(method, "textDocument/completion") == 0) {
        if (inImport) {
            importCompletion(out, id);
        } else if (word[0]) {
            gItemEdits = autoImport.buf;
            completion(out, id, prog, src, word, line);
            gItemEdits = NULL;
        } else {
            nameCompletion(out, id, prog, src, line, col);
        }
    } else {
        addUnused(prog);
        putDiagnostics(out, prog, path);
    }
    sbFree(&autoImport);
    free(path);
}

/* Runs a message in a child; sends what it wrote. */
static void runInChild(J *msg) {
    int fds[2];
    if (pipe(fds) != 0) return;
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
#ifdef __APPLE__
        /* macOS drops the signal stack in a forked child, and ASan's
           longjmp check then fails on the parser's error recovery */
        static char altStack[1 << 16];
        stack_t ss = {.ss_sp = altStack, .ss_size = sizeof altStack, .ss_flags = 0};
        sigaltstack(&ss, NULL);
#endif
        StrBuf out = {0};
        handleInChild(msg, &out);
        size_t off = 0;
        while (off < out.len) {
            ssize_t w = write(fds[1], out.buf + off, out.len - off);
            if (w <= 0) break;
            off += (size_t)w;
        }
        _exit(0);
    }
    close(fds[1]);
    if (pid < 0) {
        close(fds[0]);
        return;
    }
    StrBuf in = {0};
    char buf[65536];
    struct pollfd pfd = {fds[0], POLLIN, 0};
    for (;;) {
        int r = poll(&pfd, 1, 10000);
        if (r <= 0) {
            kill(pid, SIGKILL);     /* the compiler hung: no reply */
            break;
        }
        ssize_t n = read(fds[0], buf, sizeof buf);
        if (n <= 0) break;
        sbPutn(&in, buf, (size_t)n);
    }
    close(fds[0]);
    waitpid(pid, NULL, 0);
    bool answered = false;
    size_t i = 0;
    while (i < in.len) {
        size_t j = i;
        while (j < in.len && in.buf[j]) j++;
        if (j - i >= 5 && memcmp(in.buf + i, "@uris", 5) == 0) {
            for (int k = 0; k < ndiagUris; k++) free(diagUris[k]);
            ndiagUris = 0;
            char *p = in.buf + i + 5;
            while (p < in.buf + j && ndiagUris < 512) {
                if (*p == '\n') p++;
                char *e = p;
                while (e < in.buf + j && *e != '\n') e++;
                if (e > p) diagUris[ndiagUris++] = strndup(p, (size_t)(e - p));
                p = e;
            }
        } else if (j > i) {
            sendMsg(in.buf + i, j - i);
            answered = true;
        }
        i = j + 1;
    }
    sbFree(&in);
    J *id = jget(msg, "id");
    if (!answered && id) {
        /* the child died before replying: answer with nothing */
        StrBuf b = {0};
        reply(&b, id, NULL);
        sendMsg(b.buf, strlen(b.buf));
        sbFree(&b);
    }
}

int cmdLsp(void) {
    signal(SIGPIPE, SIG_IGN);
    bool shutdown = false;
    for (;;) {
        char *body = readMessage();
        if (!body) return shutdown ? 0 : 1;
        const char *p = body;
        J *msg = jparse(&p);
        const char *method = jstr(msg, "method");
        J *id = jget(msg, "id");
        J *params = jget(msg, "params");
        if (!method) {
            /* a reply to something the server never asks */
        } else if (strcmp(method, "initialize") == 0) {
            /* the workspace folder, for workspace/symbol */
            const char *root = jstr(params, "rootUri");
            J *folders = jget(params, "workspaceFolders");
            if (!root && folders && folders->type == 'a' && folders->child) root = jstr(folders->child, "uri");
            char *rp = root ? pathOf(root) : NULL;
            if (!rp && jstr(params, "rootPath")) rp = strdup(jstr(params, "rootPath"));
            if (rp) snprintf(gRoot, sizeof gRoot, "%s", rp);
            free(rp);
            StrBuf b = {0};
            sbPuts(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
            putId(&b, id);
            sbPrintf(&b, ",\"result\":{\"capabilities\":{\"textDocumentSync\":1,\"hoverProvider\":true,"
                         "\"definitionProvider\":true,\"documentSymbolProvider\":true,\"referencesProvider\":true,"
                         "\"renameProvider\":{\"prepareProvider\":true},"
                         "\"documentFormattingProvider\":true,\"codeActionProvider\":true,\"completionProvider\":{\"triggerCharacters\":[\".\",\"@\"]},"
                         "\"signatureHelpProvider\":{\"triggerCharacters\":[\"(\",\",\"]},\"inlayHintProvider\":true,\"documentHighlightProvider\":true,\"workspaceSymbolProvider\":true},"
                         "\"serverInfo\":{\"name\":\"iris\",\"version\":\"%s\"}}}", IRIS_VERSION);
            sendMsg(b.buf, b.len);
            sbFree(&b);
        } else if (strcmp(method, "shutdown") == 0) {
            shutdown = true;
            StrBuf b = {0};
            reply(&b, id, NULL);
            sendMsg(b.buf, strlen(b.buf));
            sbFree(&b);
        } else if (strcmp(method, "exit") == 0) {
            jfree(msg);
            free(body);
            return shutdown ? 0 : 1;
        } else if (strcmp(method, "textDocument/didOpen") == 0 || strcmp(method, "textDocument/didChange") == 0) {
            J *td = jget(params, "textDocument");
            const char *uri = jstr(td, "uri");
            const char *text = jstr(td, "text");
            if (!text) {
                J *changes = jget(params, "contentChanges");
                J *last = changes ? changes->child : NULL;
                while (last && last->next) last = last->next;
                text = jstr(last, "text");
            }
            if (uri && text) {
                setDoc(uri, text);
                runInChild(msg);        /* diagnostics */
            }
        } else if (strcmp(method, "textDocument/didSave") == 0) {
            runInChild(msg);
        } else if (strcmp(method, "textDocument/didClose") == 0) {
            const char *uri = jstr(jget(params, "textDocument"), "uri");
            if (uri) closeDoc(uri);
        } else if (strcmp(method, "textDocument/hover") == 0 || strcmp(method, "textDocument/definition") == 0 ||
                   strcmp(method, "textDocument/documentSymbol") == 0 ||
                   strcmp(method, "textDocument/completion") == 0 ||
                   strcmp(method, "textDocument/signatureHelp") == 0 ||
                   strcmp(method, "textDocument/inlayHint") == 0 ||
                   strcmp(method, "textDocument/documentHighlight") == 0 || strcmp(method, "workspace/symbol") == 0 ||
                   strcmp(method, "textDocument/codeAction") == 0 ||
                   strcmp(method, "textDocument/references") == 0 ||
                   strcmp(method, "textDocument/prepareRename") == 0 || strcmp(method, "textDocument/rename") == 0 ||
                   strcmp(method, "textDocument/formatting") == 0) {
            runInChild(msg);
        } else if (id) {
            StrBuf b = {0};
            sbPuts(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
            putId(&b, id);
            sbPuts(&b, ",\"error\":{\"code\":-32601,\"message\":\"not supported\"}}");
            sendMsg(b.buf, b.len);
            sbFree(&b);
        }
        jfree(msg);
        free(body);
    }
}
