/* iris doc: the documentation in the comments, in the terminal.
 *
 *   iris doc http              a module of the standard library
 *   iris doc http.Router       one of its names, with its methods
 *   iris doc main.iris         a file or a project's folder
 *   iris doc main.iris User    one name in it
 *
 * Declarations are shown as they are written in the source, so defaults
 * and attributes read the same as in the code, followed by the comment
 * lines right above them. For a module of the standard library or a
 * package only pub names are shown. It needs the parsed files only, not
 * checked ones, so it also works on code that doesn't compile. */

#include "iris.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *lineStart(Source *src, int line) {
    const char *p = src->text, *end = src->text + src->len;
    for (int l = 1; l < line && p < end; p++) {
        if (*p == '\n') l++;
    }
    return p;
}

static const char *lineEnd(Source *src, const char *p) {
    const char *end = src->text + src->len;
    while (p < end && *p != '\n') p++;
    return p;
}

static bool isCommentLine(const char *s, const char *e) {
    while (s < e && (*s == ' ' || *s == '\t')) s++;
    return e - s >= 2 && s[0] == '/' && s[1] == '/';
}

static bool isAttrLine(const char *s, const char *e) {
    while (s < e && (*s == ' ' || *s == '\t')) s++;
    return s < e && *s == '@';
}

/* The comment lines above line, skipping attribute lines between. */
static void comment(StrBuf *out, Source *src, int line, const char *indent) {
    int l = line - 1, first = line;
    while (l >= 1) {
        const char *s = lineStart(src, l), *e = lineEnd(src, s);
        if (isCommentLine(s, e)) {
            first = l--;
        } else if (isAttrLine(s, e) && first == line) {
            l--;
        } else {
            break;
        }
    }
    for (l = first; l < line; l++) {
        const char *s = lineStart(src, l), *e = lineEnd(src, s);
        if (!isCommentLine(s, e)) continue;
        while (s < e && (*s == ' ' || *s == '\t')) s++;
        s += 2;
        if (s < e && *s == ' ') s++;
        sbPrintf(out, "%s%.*s\n", indent, (int)(e - s), s);
    }
}

/* The attributes above a declaration, as written, but not the ones that
 * only the standard library's own code needs. */
static void attributes(StrBuf *out, Source *src, int line) {
    int l = line - 1, first = line;
    while (l >= 1) {
        const char *s = lineStart(src, l), *e = lineEnd(src, s);
        if (!isAttrLine(s, e)) break;
        first = l--;
    }
    for (l = first; l < line; l++) {
        const char *s = lineStart(src, l), *e = lineEnd(src, s);
        while (s < e && (*s == ' ' || *s == '\t')) s++;
        if (strncmp(s, "@extern", 7) == 0 || strncmp(s, "@builtin", 8) == 0) continue;
        sbPrintf(out, "%.*s\n", (int)(e - s), s);
    }
}

/* The head of a declaration as written: up to its body's { or =>, or the
 * whole line. */
static void head(StrBuf *out, Source *src, int line) {
    const char *s = lineStart(src, line), *end = src->text + src->len;
    while (s < end && (*s == ' ' || *s == '\t')) s++;
    const char *p = s;
    int depth = 0;
    bool inString = false;
    for (; p < end; p++) {
        char c = *p;
        if (inString) {
            if (c == '\\') p++;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') inString = true;
        else if (c == '(' || c == '[') depth++;
        else if (c == ')' || c == ']') depth--;
        else if (depth <= 0 && (c == '{' || (c == '=' && p + 1 < end && p[1] == '>'))) break;
        else if (depth <= 0 && c == '\n') break;
        else if (depth <= 0 && c == '/' && p + 1 < end && p[1] == '/') break;
    }
    const char *e = p;
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) e--;
    /* a signature over several lines: one line, with single spaces */
    bool space = false;
    for (const char *q = s; q < e; q++) {
        if (*q == '\n' || *q == ' ' || *q == '\t' || *q == '\r') {
            space = true;
            continue;
        }
        if (space && out->len && q[0] != ')' && q[0] != ']') sbPutc(out, ' ');
        space = false;
        sbPutc(out, *q);
    }
    sbPutc(out, '\n');
}

static bool shown(Node *d, bool all) {
    if (d->kind == N_IMPORT || d->kind == N_TEST) return false;
    if (d->kind == N_FUNC && (d->flags & F_EXTERN)) return false;
    if (all) return true;
    return (d->flags & F_PUB) != 0;
}

static const char *declName(Node *d) {
    if (d->kind == N_DEFINE && d->list.len == 1) return d->list.items[0]->name;
    return d->name;
}

/* The type a method belongs to: List for List[T].push. */
static const char *receiverName(Node *d) {
    if (d->kind != N_FUNC || !d->b) return NULL;
    Node *r = d->b;
    if (r->kind == N_T_GENERIC) r = r->a;
    return r->kind == N_T_NAME ? r->name : NULL;
}

static void showStruct(StrBuf *out, Node *d, bool all) {
    Source *src = d->src;
    head(out, src, d->line);
    out->len--;                     /* the head's line break */
    int fields = 0;
    for (int i = 0; i < d->list.len; i++) {
        Node *f = d->list.items[i];
        if (!all && !(f->flags & F_PUB) && f->kind != N_EMBED) continue;
        if (!fields++) sbPuts(out, " {\n");
        const char *s = lineStart(src, f->line), *e = lineEnd(src, s);
        while (s < e && (*s == ' ' || *s == '\t')) s++;
        sbPrintf(out, "    %.*s\n", (int)(e - s), s);
    }
    sbPuts(out, fields ? "}\n" : " { }\n");
}

static bool builtinDecl(Node *d) {
    const char *s = d->line > 1 ? lineStart(d->src, d->line - 1) : NULL;
    if (!s) return false;
    const char *e = lineEnd(d->src, s);
    while (s < e && (*s == ' ' || *s == '\t')) s++;
    return strncmp(s, "@builtin", 8) == 0;
}

static void showDecl(StrBuf *out, Node *d, bool all, NodeList *decls) {
    if (builtinDecl(d)) {
        /* made by the compiler: its comment says how it is called */
        sbPrintf(out, "%s\n", d->name);
        comment(out, d->src, d->line, "    ");
        return;
    }
    attributes(out, d->src, d->line);
    if (d->kind == N_STRUCT) {
        showStruct(out, d, all);
    } else if (d->kind == N_ENUM || d->kind == N_INTERFACE) {
        /* the whole declaration */
        const char *s = lineStart(d->src, d->line), *p = s, *end = d->src->text + d->src->len;
        int depth = 0;
        while (p < end) {
            if (*p == '{') depth++;
            if (*p == '}' && --depth == 0) {
                p++;
                break;
            }
            p++;
        }
        sbPrintf(out, "%.*s\n", (int)(p - s), s);
    } else {
        head(out, d->src, d->line);
    }
    comment(out, d->src, d->line, "    ");
    if (d->kind != N_STRUCT) return;
    /* its methods, after it */
    for (int i = 0; i < decls->len; i++) {
        Node *m = decls->items[i];
        const char *rn = receiverName(m);
        if (!rn || strcmp(rn, d->name) != 0 || !shown(m, all)) continue;
        sbPuts(out, "\n");
        head(out, m->src, m->line);
        comment(out, m->src, m->line, "    ");
    }
}

/* The comment at the top of a module's first file. */
static void moduleComment(StrBuf *out, Module *m) {
    for (int f = 0; f < m->files.len; f++) {
        Node *file = m->files.items[f];
        Source *src = file->src;
        int l = 1;
        bool any = false;
        for (;;) {
            const char *s = lineStart(src, l), *e = lineEnd(src, s);
            if (s >= src->text + src->len || !isCommentLine(s, e)) break;
            while (s < e && (*s == ' ' || *s == '\t')) s++;
            s += 2;
            if (s < e && *s == ' ') s++;
            sbPrintf(out, "%.*s\n", (int)(e - s), s);
            any = true;
            l++;
        }
        /* a comment that belongs to the first declaration isn't the module's */
        if (any) return;
    }
}

static int showModule(Module *m, const char *name, bool all) {
    NodeList decls = {0};
    Arena a = {0};
    for (int f = 0; f < m->files.len; f++) {
        Node *file = m->files.items[f];
        for (int i = 0; i < file->list.len; i++) listPush(&a, &decls, file->list.items[i]);
    }
    StrBuf out = {0};
    int found = 0;
    if (!name) {
        sbPrintf(&out, "module %s\n", m->name ? m->name : m->path);
        StrBuf top = {0};
        moduleComment(&top, m);
        if (top.len) {
            sbPuts(&out, "\n");
            sbPutn(&out, top.buf, top.len);
        }
        sbFree(&top);
    }
    for (int i = 0; i < decls.len; i++) {
        Node *d = decls.items[i];
        const char *dn = declName(d);
        if (!dn || !shown(d, all) || receiverName(d)) continue;
        if (name && strcmp(dn, name) != 0) continue;
        sbPuts(&out, "\n");
        showDecl(&out, d, all, &decls);
        found++;
    }
    if (name && !found) {
        /* a method: Router.serve */
        const char *dot = strchr(name, '.');
        for (int i = 0; dot && i < decls.len; i++) {
            Node *d = decls.items[i];
            const char *rn = receiverName(d);
            if (!rn || !shown(d, all) || strlen(rn) != (size_t)(dot - name) || strncmp(rn, name, (size_t)(dot - name))) continue;
            if (strcmp(d->name, dot + 1) != 0) continue;
            sbPuts(&out, "\n");
            head(&out, d->src, d->line);
            comment(&out, d->src, d->line, "    ");
            found++;
        }
    }
    if (name && !found) {
        fprintf(stderr, "iris doc: %s has no %s%s\n", m->name ? m->name : m->path, name,
                all ? "" : " (or it isn't pub)");
        sbFree(&out);
        arenaFree(&a);
        return 1;
    }
    fputs(out.buf ? out.buf + (out.buf[0] == '\n') : "", stdout);
    sbFree(&out);
    arenaFree(&a);
    return 0;
}

int cmdDoc(int argc, char **argv) {
    if (argc < 3) {
        fputs("usage: iris doc <module>[.name], or iris doc <file or folder> [name]\n", stderr);
        return 2;
    }
    const char *what = argv[2];
    const char *name = argc > 3 ? argv[3] : NULL;
    struct stat st;
    Arena a = {0};
    int rc = 1;
    gTesting = true;            /* no main needed */
    if (stat(what, &st) == 0) {
        Program *prog = loadProgram(&a, what, NULL, 0);
        if (prog) rc = showModule(prog->main, name, true);
        arenaFree(&a);
        return rc;
    }
    /* a module of the standard library: http, or http.Router */
    char mod[256];
    snprintf(mod, sizeof mod, "%s", what);
    char *dot = strchr(mod, '.');
    if (dot) {
        *dot = 0;
        name = dot + 1;
    }
    if (!stdModuleExists(mod)) {
        /* a name every module sees: List, Map.set */
        name = what;
        snprintf(mod, sizeof mod, "core");
    }
    const char *base = getenv("TMPDIR");
    char path[4096];
    snprintf(path, sizeof path, "%s/iris-doc-%d.iris", base && *base ? base : "/tmp", (int)getpid());
    char src[512];
    snprintf(src, sizeof src, "import %s\n\nvoid main() { }\n", mod);
    if (!writeText(path, src, strlen(src))) {
        return 1;
    }
    Program *prog = loadProgram(&a, path, NULL, 0);
    unlink(path);
    if (prog) {
        Module *m = NULL;
        for (Module *x = prog->modules; x; x = x->next) {
            if (x != prog->main && x->path && strcmp(x->path, mod) == 0) m = x;
        }
        if (strcmp(mod, "core") == 0) m = prog->core;
        if (m) {
            rc = showModule(m, name, false);
        } else {
            fprintf(stderr, "iris doc: there is no module or name %s\n", what);
        }
    }
    arenaFree(&a);
    return rc;
}
