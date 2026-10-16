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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

static char *readWhole(Arena *a, const char *path, size_t *len) {
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
        }
    }
    closedir(d);
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
    sbPuts(&key, "c:");
    for (int i = 0; i < n; i++) {
        sbPrintf(&key, "%s%s", i ? "," : "", headers[i]);
    }
    char *path = sbFinish(&key, L->arena);
    Module *m = findModule(L, path);
    if (!m) {
        m = newModule(L, path);
        m->name = "c";
        m->anchor->name = "c";
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

Program *loadProgram(Arena *a, const char *path, const char **includes, int nincludes) {
    Loader L = {0};
    L.arena = a;
    L.prog = arenaAlloc(a, sizeof(Program));
    for (int i = 0; i < nincludes && L.nincludes < 63; i++) {
        L.includes[L.nincludes++] = includes[i];
    }
    bool dir = isDir(path);
    if (!dir && !isFile(path)) {
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
    if (L.failed) {
        return NULL;
    }
    loadImports(&L, m);
    m->state = 2;
    return L.failed ? NULL : L.prog;
}
