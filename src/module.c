/* The program loader: finds the files of the program and of every module
 * it imports, and parses them.
 *
 * A folder with an iris.mod file is a project. A program run from inside
 * one is the whole folder: every .iris file in it. Without iris.mod a file
 * is a program on its own. Imports:
 *
 *   import fs                       the standard library, carried inside iris
 *   import "net/http"               a folder of the project, from its root
 *   import "github.com/someone/pg"  a package: replace in iris.mod points at a
 *                                   folder, otherwise require gives the version
 *                                   and it is read from the package cache */
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include "iris.h"

#include <dirent.h>
#include <limits.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* the standard library's files, turned into bytes by the Makefile */
#include "std.inc"

typedef struct {
    const char *path, *version, *dir;   /* require path version / replace path => dir */
} Requirement;

typedef struct {
    Arena *arena;
    Program *prog;
    const char *root;       /* the project's folder, or NULL */
    Requirement reqs[256];
    int nreqs;
    const char *includes[64];   /* for C headers */
    int nincludes;
    bool failed;
} Loader;

static void loadError(Loader *L, Node *at, const char *fmt, const char *arg) {
    if (at && at->src) {
        errorAt(at->src, at->line, at->col, fmt, arg);
    } else {
        fprintf(stderr, "iris: ");
        fprintf(stderr, fmt, arg);
        fputc('\n', stderr);
        gErrors++;
    }
    L->failed = true;
}

const char *(*gReadHook)(const char *path, size_t *len);
int (*gFolderHook)(const char *dir, const char **names, int max);

static char *readWhole(Arena *a, const char *path, size_t *len) {
    if (gReadHook) {
        /* the language server's open documents, with changes not saved yet */
        const char *text = gReadHook(path, len);
        if (text) {
            return arenaStrndup(a, text, *len);
        }
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    StrBuf b = {0};
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        sbPutn(&b, buf, n);
    }
    fclose(f);
    *len = b.len;
    return sbFinish(&b, a);
}

static bool isDir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool isFile(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static char *join(Arena *a, const char *dir, const char *name) {
    size_t n = strlen(dir) + strlen(name) + 2;
    char *p = arenaAlloc(a, n);
    snprintf(p, n, "%s/%s", dir, name);
    return p;
}

static char *dirOf(Arena *a, const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) {
        return arenaStrndup(a, ".", 1);
    }
    if (slash == path) {
        return arenaStrndup(a, "/", 1);
    }
    return arenaStrndup(a, path, (size_t)(slash - path));
}

static Node *parseText(Loader *L, const char *path, const char *text, size_t len) {
    Source *src = arenaAlloc(L->arena, sizeof(Source));
    src->path = path;
    src->text = text;
    src->len = len;
    int before = gErrors;
    Node *file = parseSource(L->arena, src, false);
    if (gErrors != before) {
        L->failed = true;
    }
    return file;
}

static int cmpStr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* Every .iris file in a folder, sorted, parsed into the module. */
static void readFolder(Loader *L, Module *m, const char *dir, Node *at) {
    DIR *d = opendir(dir);
    if (!d) {
        loadError(L, at, "can't read the folder %s", dir);
        return;
    }
    char *names[1024];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < 1024) {
        size_t len = strlen(e->d_name);
        if (len > 5 && strcmp(e->d_name + len - 5, ".iris") == 0 && e->d_name[0] != '.') {
            names[n++] = arenaStrndup(L->arena, e->d_name, len);
        } else if (len > 2 && strcmp(e->d_name + len - 2, ".c") == 0 && e->d_name[0] != '.' &&
                   L->prog->ncsources < 64) {
            /* C files of the module are compiled with the program */
            char full[PATH_MAX];
            char *c = join(L->arena, dir, e->d_name);
            L->prog->csources[L->prog->ncsources++] = realpath(c, full) ? arenaStrndup(L->arena, full, strlen(full)) : c;
        }
    }
    closedir(d);
    if (gFolderHook) {
        /* files open in the editor that aren't on disk yet */
        const char *open[256];
        int k = gFolderHook(dir, open, 256);
        for (int i = 0; i < k && n < 1024; i++) {
            bool seen = false;
            for (int j = 0; j < n && !seen; j++) seen = strcmp(names[j], open[i]) == 0;
            if (!seen) names[n++] = arenaStrndup(L->arena, open[i], strlen(open[i]));
        }
    }
    qsort(names, (size_t)n, sizeof names[0], cmpStr);
    if (n == 0) {
        loadError(L, at, "the folder %s has no .iris files", dir);
    }
    for (int i = 0; i < n; i++) {
        char *path = join(L->arena, dir, names[i]);
        size_t len;
        char *text = readWhole(L->arena, path, &len);
        if (!text) {
            loadError(L, at, "can't read %s", path);
            continue;
        }
        listPush(L->arena, &m->files, parseText(L, path, text, len));
    }
}

static Module *newModule(Loader *L, const char *path) {
    Module *m = arenaAlloc(L->arena, sizeof(Module));
    m->path = path;
    const char *slash = strrchr(path, '/');
    m->name = slash ? slash + 1 : path;
    m->anchor = newNode(L->arena, N_IMPORT, 0, 0);
    m->anchor->name = m->name;
    m->anchor->module = path;
    /* the program's module goes first; the rest after it */
    if (!L->prog->modules) {
        L->prog->modules = m;
    } else {
        Module *last = L->prog->modules;
        while (last->next) {
            last = last->next;
        }
        last->next = m;
    }
    return m;
}

static Module *findModule(Loader *L, const char *path) {
    for (Module *m = L->prog->modules; m; m = m->next) {
        if (strcmp(m->path, path) == 0) {
            return m;
        }
    }
    return NULL;
}

static void loadImports(Loader *L, Module *m);

/* A package path starts with a domain: github.com/... */
static bool isPackagePath(const char *path) {
    const char *slash = strchr(path, '/');
    size_t first = slash ? (size_t)(slash - path) : strlen(path);
    return memchr(path, '.', first) != NULL;
}

static const char *packageDir(Loader *L, const char *path, Node *at) {
    for (int i = 0; i < L->nreqs; i++) {
        Requirement *r = &L->reqs[i];
        if (strcmp(r->path, path) != 0) continue;
        if (r->dir) {
            return r->dir[0] == '/' ? r->dir : join(L->arena, L->root, r->dir);
        }
        const char *cache = getenv("IRIS_PATH");
        char buf[4096];
        if (!cache || !*cache) {
            const char *home = getenv("HOME");
            snprintf(buf, sizeof buf, "%s/.cache/iris", home ? home : ".");
            cache = buf;
        }
        size_t n = strlen(cache) + strlen(path) + strlen(r->version) + 8;
        char *dir = arenaAlloc(L->arena, n);
        snprintf(dir, n, "%s/pkg/%s@%s", cache, path, r->version);
        if (!isDir(dir)) {
            loadError(L, at, "the package %s isn't downloaded; run iris get", path);
            return NULL;
        }
        return dir;
    }
    loadError(L, at, "iris.mod doesn't list the package %s; run iris get to add it", path);
    return NULL;
}

/* Finds and reads the module an import names. */
static Module *loadModule(Loader *L, Node *item) {
    bool std = item->name != NULL;
    const char *path = std ? item->name : item->text;
    Module *m = findModule(L, path);
    if (m) {
        if (m->state == 1) {
            loadError(L, item, "modules import each other in a circle, through %s", path);
        }
        return m;
    }
    if (std) {
        size_t n = strlen(path);
        bool found = false;
        for (size_t i = 0; i < sizeof stdFiles / sizeof stdFiles[0]; i++) {
            if (strncmp(stdFiles[i].path, path, n) == 0 && stdFiles[i].path[n] == '/') {
                found = true;
            }
        }
        if (!found) {
            loadError(L, item, "there is no standard module named %s", path);
            return NULL;
        }
        m = newModule(L, path);
        m->isStd = true;
        m->state = 1;
        for (size_t i = 0; i < sizeof stdFiles / sizeof stdFiles[0]; i++) {
            if (strncmp(stdFiles[i].path, path, n) == 0 && stdFiles[i].path[n] == '/') {
                char *where = join(L->arena, "std", stdFiles[i].path);
                listPush(L->arena, &m->files, parseText(L, where, stdFiles[i].data, strlen(stdFiles[i].data)));
            }
        }
    } else {
        const char *dir;
        if (isPackagePath(path)) {
            dir = packageDir(L, path, item);
        } else if (!L->root) {
            loadError(L, item, "importing %s needs an iris.mod at the project's root", path);
            return NULL;
        } else {
            dir = join(L->arena, L->root, path);
            if (!isDir(dir)) {
                loadError(L, item, "there is no folder %s in the project", path);
                return NULL;
            }
        }
        if (!dir) {
            return NULL;
        }
        m = newModule(L, path);
        m->state = 1;
        readFolder(L, m, dir, item);
    }
    loadImports(L, m);
    m->state = 2;
    return m;
}

/* The C headers a file imports become one module, "c", made by clang. */
static void loadCImports(Loader *L, Node *file) {
    const char *headers[64];
    Node *items[64];
    int n = 0;
    for (int j = 0; j < file->list.len; j++) {
        Node *d = file->list.items[j];
        if (d->kind != N_IMPORT) continue;
        for (int k = 0; k < d->list.len && n < 64; k++) {
            Node *item = d->list.items[k];
            if (item->flags & F_C_IMPORT) {
                items[n] = item;
                headers[n++] = item->text;
            }
        }
    }
    if (n == 0) {
        return;
    }
    StrBuf key = {0};
    sbPuts(&key, "c-import:");
    for (int i = 0; i < n; i++) {
        sbPrintf(&key, "%s%s", i ? "," : "", headers[i]);
    }
    char *source = sbFinish(&key, L->arena);
    Module *m = NULL;
    int count = 0;
    for (Module *x = L->prog->modules; x; x = x->next) {
        if (x->isC) {
            count++;
            if (strcmp(x->anchor->text, source) == 0) m = x;
        }
    }
    const char *path = source;
    if (!m) {
        /* a short name for the compiled code: c1, c2 ... */
        char shortName[32];
        snprintf(shortName, sizeof shortName, "c%d", count + 1);
        m = newModule(L, arenaStrndup(L->arena, shortName, strlen(shortName)));
        m->name = "c";
        m->anchor->name = "c";
        m->anchor->text = source;
        m->anchor->flags |= F_C_IMPORT;
        m->isC = true;
        m->state = 2;
        int before = gErrors;
        char *text = cImport(L->arena, headers, n, L->includes, L->nincludes, &m->anchor->list);
        if (!text) {
            if (gErrors == before) gErrors++;
            errorAt(items[0]->src, items[0]->line, items[0]->col, "can't import these C headers");
            L->failed = true;
            return;
        }
        listPush(L->arena, &m->files, parseText(L, path, text, strlen(text)));
    }
    for (int i = 0; i < n; i++) {
        items[i]->ref = m->anchor;
    }
}

static void loadImports(Loader *L, Module *m) {
    for (int i = 0; i < m->files.len; i++) {
        Node *file = m->files.items[i];
        for (int j = 0; j < file->list.len; j++) {
            Node *d = file->list.items[j];
            if (d->kind != N_IMPORT) continue;
            for (int k = 0; k < d->list.len; k++) {
                Node *item = d->list.items[k];
                if (item->flags & F_C_IMPORT) continue;
                Module *dep = loadModule(L, item);
                if (dep) {
                    item->ref = dep->anchor;
                }
            }
        }
        if (!m->isC) {
            loadCImports(L, file);
        }
    }
}

/* iris.mod: module NAME, require PATH VERSION, replace PATH => DIR */
static void readModFile(Loader *L, const char *path) {
    size_t len;
    char *text = readWhole(L->arena, path, &len);
    if (!text) {
        return;
    }
    int lineNo = 0;
    for (char *line = text; line && *line;) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        lineNo++;
        char *c = strstr(line, "//");
        if (c) *c = 0;
        char a[512] = "", b[512] = "", d[512] = "", e[512] = "";
        int n = sscanf(line, "%511s %511s %511s %511s", a, b, d, e);
        if (n > 0) {
            if (strcmp(a, "module") == 0 && n == 2) {
                /* the project's name */
            } else if (strcmp(a, "require") == 0 && n == 3 && L->nreqs < 256) {
                L->reqs[L->nreqs++] = (Requirement){arenaStrndup(L->arena, b, strlen(b)),
                                                    arenaStrndup(L->arena, d, strlen(d)), NULL};
            } else if (strcmp(a, "link") == 0 && n == 2 && L->prog->nlinks < 64) {
                L->prog->links[L->prog->nlinks++] = arenaStrndup(L->arena, b, strlen(b));
            } else if (strcmp(a, "replace") == 0 && n == 4 && strcmp(d, "=>") == 0 && L->nreqs < 256) {
                L->reqs[L->nreqs++] = (Requirement){arenaStrndup(L->arena, b, strlen(b)), "",
                                                    arenaStrndup(L->arena, e, strlen(e))};
            } else {
                fprintf(stderr, "%s:%d: error: iris.mod lines are module, require, replace or link\n", path, lineNo);
                gErrors++;
                L->failed = true;
            }
        }
        line = end ? end + 1 : NULL;
    }
}

/* The folder with iris.mod, from dir upward, or NULL. */
static const char *findRoot(Arena *a, const char *dir) {
    char cur[4096];
    if (!realpath(dir, cur)) {
        return NULL;
    }
    for (;;) {
        char mod[4200];
        snprintf(mod, sizeof mod, "%s/iris.mod", cur);
        if (isFile(mod)) {
            return arenaStrndup(a, cur, strlen(cur));
        }
        char *slash = strrchr(cur, '/');
        if (!slash || slash == cur) {
            return NULL;
        }
        *slash = 0;
    }
}

bool gLenient;
void (*gLoadFailedHook)(void);

Program *loadProgram(Arena *a, const char *path, const char **includes, int nincludes) {
    Loader L = {0};
    L.arena = a;
    L.prog = arenaAlloc(a, sizeof(Program));
    for (int i = 0; i < nincludes && L.nincludes < 63; i++) {
        L.includes[L.nincludes++] = includes[i];
    }
    bool dir = isDir(path);
    size_t openLen;
    if (!dir && !isFile(path) && !(gReadHook && gReadHook(path, &openLen))) {
        fprintf(stderr, "iris: can't open %s\n", path);
        return NULL;
    }
    const char *folder = dir ? path : dirOf(a, path);
    L.root = findRoot(a, folder);
    if (L.root) {
        readModFile(&L, join(a, L.root, "iris.mod"));
        L.includes[L.nincludes++] = L.root;     /* the project's own C headers */
    } else {
        L.includes[L.nincludes++] = folder;
    }
    Module *m = newModule(&L, "main");
    m->name = "main";
    m->state = 1;
    L.prog->main = m;
    if (dir || L.root) {
        readFolder(&L, m, folder, NULL);
    } else {
        size_t len;
        char *text = readWhole(a, path, &len);
        if (!text) {
            fprintf(stderr, "iris: can't read %s\n", path);
            return NULL;
        }
        listPush(a, &m->files, parseText(&L, path, text, len));
    }
    if (L.failed && !gLenient) {
        return NULL;
    }
    if (L.failed && gLoadFailedHook) gLoadFailedHook();     /* a strict load stops here */
    if (gExtraText) {
        /* comptime's helper: one more file of the program's module */
        listPush(a, &m->files, parseText(&L, gExtraPath, gExtraText, strlen(gExtraText)));
        if (L.failed) {
            return NULL;
        }
    }
    /* core: List, Map and the rest that every module sees without an import */
    Node *coreItem = newNode(a, N_IDENT, 0, 0);
    coreItem->name = "core";
    Module *core = loadModule(&L, coreItem);
    if (core) {
        loadImports(&L, core);
        core->state = 2;
        L.prog->core = core;
    }
    loadImports(&L, m);
    m->state = 2;
    /* str: its functions are methods of text, s.startsWith("/api") */
    Node *textItem = newNode(a, N_IDENT, 0, 0);
    textItem->name = "str";
    L.prog->textImplicit = !findModule(&L, "str");
    Module *text = loadModule(&L, textItem);
    if (text) {
        loadImports(&L, text);
        text->state = 2;
        L.prog->text = text;
    }
    if (L.failed && gLenient && gLoadFailedHook) gLoadFailedHook();
    return L.failed && !gLenient ? NULL : L.prog;
}

/* The text of a standard library file by its path ("std/json/json.iris"),
 * or NULL. */
const char *stdSource(const char *path) {
    if (strncmp(path, "std/", 4) != 0) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof stdFiles / sizeof stdFiles[0]; i++) {
        if (strcmp(stdFiles[i].path, path + 4) == 0) {
            return stdFiles[i].data;
        }
    }
    return NULL;
}

/* Writes text to path through a file of its own, then renames it, so
 * builds at the same time never see half a file. The file is read-only. */
static bool writeReadOnly(const char *path, const char *text, size_t n) {
    char part[4200];
    snprintf(part, sizeof part, "%s.%d.part", path, (int)getpid());
    FILE *f = fopen(part, "wb");
    if (!f) return false;
    bool ok = fwrite(text, 1, n, f) == n;
    ok = fclose(f) == 0 && ok;
    ok = ok && chmod(part, 0444) == 0 && rename(part, path) == 0;
    if (!ok) unlink(part);
    return ok;
}

/* Makes each folder of path, up to its last '/'. */
static void makeParents(char *path) {
    for (char *p = path + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(path, 0755);
            *p = '/';
        }
    }
}

/* The folder of this version's files that iris keeps on disk:
 * $IRIS_PATH/<version>, else ~/.cache/iris/<version>, next to the
 * runtimes and the packages. Made when missing; NULL when it can't be. */
const char *irisVersionDir(void) {
    static bool done;
    static char dir[3100];
    if (done) return dir[0] ? dir : NULL;
    done = true;
    const char *base = getenv("IRIS_PATH"), *home = getenv("HOME");
    if (base && *base && strlen(base) < 3000) {
        snprintf(dir, sizeof dir, "%s/%s/", base, IRIS_VERSION);
    } else if (home && *home && strlen(home) < 3000) {
        snprintf(dir, sizeof dir, "%s/.cache/iris/%s/", home, IRIS_VERSION);
    } else {
        return NULL;
    }
    makeParents(dir);
    dir[strlen(dir) - 1] = 0;
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        dir[0] = 0;
        return NULL;
    }
    return dir;
}

/* Writes text to path in the folder of irisVersionDir, read-only, when the
 * file is not there with that text. The path of the file, which the caller
 * frees, or NULL. */
const char *irisVersionFile(const char *name, const char *text) {
    const char *base = irisVersionDir();
    if (!base) return NULL;
    char *path = malloc(strlen(base) + strlen(name) + 2);
    sprintf(path, "%s/%s", base, name);
    size_t n = strlen(text);
    FILE *f = fopen(path, "rb");
    if (f) {
        char *have = malloc(n + 1);
        size_t got = fread(have, 1, n + 1, f);
        fclose(f);
        bool same = got == n && memcmp(have, text, n) == 0;
        free(have);
        if (same) return path;
    }
    makeParents(path);
    if (!writeReadOnly(path, text, n)) {
        free(path);
        return NULL;
    }
    return path;
}

/* The folder with a copy of the standard library's files, for debuggers:
 * std in irisVersionDir. The files are written when the folder has none,
 * or other ones (its .hash file tells). NULL when it can't be made. */
const char *stdSourceDir(void) {
    static bool done;
    static char dir[3200];
    if (done) return dir[0] ? dir : NULL;
    done = true;
    const char *base = irisVersionDir();
    if (!base) return NULL;
    snprintf(dir, sizeof dir, "%s/std", base);
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < sizeof stdFiles / sizeof stdFiles[0]; i++) {
        for (const char *p = stdFiles[i].path; *p; p++) h = (h ^ (unsigned char)*p) * 1099511628211ULL;
        for (const char *p = stdFiles[i].data; *p; p++) h = (h ^ (unsigned char)*p) * 1099511628211ULL;
    }
    char stamp[3300], want[32], have[32] = {0};
    snprintf(stamp, sizeof stamp, "%s/.hash", dir);
    snprintf(want, sizeof want, "%016llx\n", (unsigned long long)h);
    FILE *f = fopen(stamp, "rb");
    if (f) {
        size_t n = fread(have, 1, sizeof have - 1, f);
        have[n] = 0;
        fclose(f);
        if (strcmp(have, want) == 0) return dir;
    }
    bool ok = true;
    char path[3400];
    for (size_t i = 0; i < sizeof stdFiles / sizeof stdFiles[0] && ok; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, stdFiles[i].path);
        makeParents(path);
        ok = writeReadOnly(path, stdFiles[i].data, strlen(stdFiles[i].data));
    }
    if (!ok || !writeReadOnly(stamp, want, strlen(want))) {
        dir[0] = 0;
        return NULL;
    }
    return dir;
}

/* The names of the standard library's modules, each once, in order; how
 * many there are. */
int stdModuleNames(const char **out, int max) {
    int n = 0;
    for (size_t i = 0; i < sizeof stdFiles / sizeof stdFiles[0]; i++) {
        const char *path = stdFiles[i].path, *slash = strchr(path, '/');
        if (!slash) continue;
        size_t len = (size_t)(slash - path);
        bool seen = false;
        for (int k = 0; k < n && !seen; k++) seen = strlen(out[k]) == len && !strncmp(out[k], path, len);
        if (seen || n >= max) continue;
        char *name = malloc(len + 1);
        if (!name) continue;
        memcpy(name, path, len);
        name[len] = 0;
        out[n++] = name;
    }
    return n;
}

/* The name that the rest of a top-level "pub" line declares (s is after
 * "pub "), and its kind as LSP numbers it: 23 struct, 10 enum, 11 interface,
 * 26 type, 14 const, 12 function, 6 method. A method's name is Reader.read;
 * with methods false it gives 0 for one, as for a line it can't read. */
size_t pubDeclName(const char *s, const char *end, const char **name, int *kind, bool methods) {
    static const struct { const char *word; int kind; } kinds[] = {
        {"struct ", 23}, {"enum ", 10}, {"interface ", 11}, {"union ", 23}, {"type ", 26}, {"const ", 14}};
    for (size_t k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
        size_t n = strlen(kinds[k].word);
        if ((size_t)(end - s) > n && !strncmp(s, kinds[k].word, n) && kinds[k].kind != 14) {
            const char *a = s + n, *b = a;
            while (b < end && (isalnum((unsigned char)*b) || *b == '_')) b++;
            *name = a;
            *kind = kinds[k].kind;
            return (size_t)(b - a);
        }
    }
    /* a function, a const or a variable: the name before '(', '[' or '=' */
    const char *stop = s;
    while (stop < end && *stop != '(' && *stop != '=') stop++;
    *kind = !strncmp(s, "const ", 6) ? 14 : stop < end && *stop == '(' ? 12 : 13;
    const char *b = stop;
    while (b > s && b[-1] == ' ') b--;
    if (b > s && b[-1] == ']') {        /* first[T](...) */
        while (b > s && b[-1] != '[') b--;
        if (b > s) b--;
    }
    const char *a = b;
    while (a > s && (isalnum((unsigned char)a[-1]) || a[-1] == '_')) a--;
    if (a == b || isdigit((unsigned char)*a)) return 0;
    if (a > s && a[-1] == '.') {
        if (!methods) return 0;
        a--;        /* Reader.read: the receiver too */
        while (a > s && (isalnum((unsigned char)a[-1]) || a[-1] == '_')) a--;
        *kind = 6;
    }
    *name = a;
    return (size_t)(b - a);
}

/* The std file at i, as "fs/fs.iris", and its text; NULL past the last one. */
const char *stdFileAt(int i, const char **text) {
    if (i < 0 || (size_t)i >= sizeof stdFiles / sizeof stdFiles[0]) return NULL;
    *text = stdFiles[i].data;
    return stdFiles[i].path;
}

/* The std modules that declare name as pub, as "fs". At most max; the caller
 * frees each one. */
int stdModulesExporting(const char *name, const char **out, int max) {
    int n = 0;
    size_t want = strlen(name);
    for (size_t i = 0; i < sizeof stdFiles / sizeof stdFiles[0] && n < max; i++) {
        const char *path = stdFiles[i].path, *slash = strchr(path, '/');
        if (!slash || strchr(slash + 1, '/')) continue;     /* not a file of a module's own folder */
        size_t len = (size_t)(slash - path);
        bool seen = false;
        for (int k = 0; k < n && !seen; k++) seen = strlen(out[k]) == len && !strncmp(out[k], path, len);
        if (seen) continue;
        for (const char *line = stdFiles[i].data; line && *line;) {
            const char *end = strchr(line, '\n');
            if (!end) end = line + strlen(line);
            const char *got;
            size_t gl;
            int kind;
            if (!strncmp(line, "pub ", 4) && (gl = pubDeclName(line + 4, end, &got, &kind, false)) == want &&
                !strncmp(got, name, want)) {
                char *m = malloc(len + 1);
                if (m) {
                    memcpy(m, path, len);
                    m[len] = 0;
                    out[n++] = m;
                }
                break;
            }
            line = *end ? end + 1 : end;
        }
    }
    return n;
}

/* Whether the standard library has a module of this name. */
bool stdModuleExists(const char *name) {
    size_t n = strlen(name);
    for (size_t i = 0; i < sizeof stdFiles / sizeof stdFiles[0]; i++) {
        if (strncmp(stdFiles[i].path, name, n) == 0 && stdFiles[i].path[n] == '/') return true;
    }
    return false;
}
