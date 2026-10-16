/* comptime: running Iris code while compiling.
 *
 * `const X = comptime f()` is worked out by a helper program. The compiler
 * finds every such constant, adds a file to the program's module with a
 * function that prints each value as JSON (through json.encode), builds
 * that program for the machine it runs on, and runs it. Then it compiles
 * the real program, with each comptime expression replaced by the literal
 * the helper printed: numbers, bool and text become constants; arrays and
 * structs become read-only globals.
 *
 * gComptime is 0 for a program without comptime, 1 while checking the
 * helper (comptime constants are then zero-valued globals, so code that
 * uses them still checks), and 2 for the real program. */

#include "iris.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int gComptime;
ComptimeResult *gComptimeResults;
int gComptimeCount;
const char *gExtraPath, *gExtraText;
char **gComptimeKeys;
int gComptimeKeyCount;

/* Where a comptime expression is: its key in the results. */
char *comptimeKey(Arena *a, Node *n) {
    char buf[4200];
    snprintf(buf, sizeof buf, "%s:%d:%d", n->src ? n->src->path : "?", n->line, n->col);
    return arenaStrndup(a, buf, strlen(buf));
}

/* The text of the expression after `comptime`, to the end of the line, or
 * further while brackets are still open. */
static void expressionText(StrBuf *out, Node *n) {
    const char *text = n->src->text, *end = text + n->src->len;
    const char *p = text;
    for (int l = 1; l < n->line && p < end; p++) {
        if (*p == '\n') l++;
    }
    p += n->col - 1;
    p += strlen("comptime");
    int depth = 0;
    bool inString = false;
    const char *s = p;
    for (; p < end; p++) {
        char c = *p;
        if (inString) {
            if (c == '\\') {
                p++;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }
        if (c == '"') inString = true;
        if (c == '(' || c == '[' || c == '{') depth++;
        if (c == ')' || c == ']' || c == '}') depth--;
        if (c == '\n' && depth <= 0) break;
        if (c == '/' && p + 1 < end && p[1] == '/' && depth <= 0) break;
    }
    sbPutn(out, s, (size_t)(p - s));
}

/* The helper's source, or NULL when the program has no comptime. Only
 * comptime constants at the top of the program's own files are worked
 * out; the checker reports the others. */
char *comptimeHelperSource(Arena *a, Program *prog) {
    NodeList found = {0};
    for (int f = 0; f < prog->main->files.len; f++) {
        Node *file = prog->main->files.items[f];
        for (int i = 0; i < file->list.len; i++) {
            Node *d = file->list.items[i];
            if (d->kind == N_CONST && d->b && d->b->kind == N_COMPTIME) {
                listPush(a, &found, d->b);
            }
        }
    }
    if (!found.len) {
        return NULL;
    }
    StrBuf src = {0};
    /* the imports of the program's files, so the expressions can use them */
    sbPuts(&src, "import json\n");
    for (int f = 0; f < prog->main->files.len; f++) {
        Node *file = prog->main->files.items[f];
        for (int i = 0; i < file->list.len; i++) {
            Node *d = file->list.items[i];
            if (d->kind != N_IMPORT || (d->flags & F_C_IMPORT)) continue;
            for (int k = 0; k < d->list.len; k++) {
                Node *item = d->list.items[k];
                if (item->name && strcmp(item->name, "json") == 0) continue;
                if (item->name) {
                    sbPrintf(&src, "import %s\n", item->name);
                } else if (item->text) {
                    sbPrintf(&src, "import \"%s\"\n", item->text);
                }
            }
        }
    }
    /* each value on a line of its own: \x01, its number, \x02, its JSON */
    sbPuts(&src, "\nvoid irisComptimeMain() {\n");
    gComptimeKeys = arenaAlloc(a, sizeof(char *) * (size_t)found.len);
    gComptimeKeyCount = found.len;
    for (int i = 0; i < found.len; i++) {
        Node *n = found.items[i];
        gComptimeKeys[i] = comptimeKey(a, n);
        StrBuf e = {0};
        expressionText(&e, n);
        sbPrintf(&src, "    scoped String v%d = json.encode(%s)\n", i, e.buf ? e.buf : "");
        sbPrintf(&src, "    print(\"\\x01%d\\x02{v%d}\")\n", i, i);
        sbFree(&e);
    }
    sbPuts(&src, "}\n");
    return sbFinish(&src, a);
}

/* ---------------------------------------------------------------- JSON to literals */

typedef struct {
    Arena *a;
    const char *p;
    Node *at;
    bool failed;
} Reader;

static void skipWs(Reader *r) {
    while (*r->p == ' ' || *r->p == '\n' || *r->p == '\t' || *r->p == '\r') r->p++;
}

static Node *lit(Reader *r, NodeKind k) {
    Node *n = newNode(r->a, k, r->at->line, r->at->col);
    n->src = r->at->src;
    return n;
}

static char *readString(Reader *r, size_t *len) {
    StrBuf b = {0};
    r->p++;
    while (*r->p && *r->p != '"') {
        char c = *r->p++;
        if (c != '\\') {
            sbPutc(&b, c);
            continue;
        }
        char e = *r->p++;
        switch (e) {
        case 'n': sbPutc(&b, '\n'); break;
        case 't': sbPutc(&b, '\t'); break;
        case 'r': sbPutc(&b, '\r'); break;
        case 'b': sbPutc(&b, '\b'); break;
        case 'f': sbPutc(&b, '\f'); break;
        case 'u': {
            unsigned v = 0;
            for (int i = 0; i < 4 && *r->p; i++) {
                char h = *r->p++;
                v = v * 16 + (unsigned)(h >= '0' && h <= '9' ? h - '0' : (h | 32) - 'a' + 10);
            }
            if (v < 0x80) {
                sbPutc(&b, (char)v);
            } else if (v < 0x800) {
                sbPutc(&b, (char)(0xc0 | (v >> 6)));
                sbPutc(&b, (char)(0x80 | (v & 0x3f)));
            } else {
                sbPutc(&b, (char)(0xe0 | (v >> 12)));
                sbPutc(&b, (char)(0x80 | ((v >> 6) & 0x3f)));
                sbPutc(&b, (char)(0x80 | (v & 0x3f)));
            }
            break;
        }
        default: sbPutc(&b, e); break;
        }
    }
    if (*r->p == '"') r->p++;
    *len = b.len;
    char *s = sbFinish(&b, r->a);
    return s ? s : arenaStrndup(r->a, "", 0);
}

static Node *readValue(Reader *r, Type *t) {
    skipWs(r);
    switch (t->kind) {
    case TY_INT: case TY_FLOAT: {
        char *end;
        Node *n;
        if (t->kind == TY_INT) {
            n = lit(r, N_INT);
            bool neg = *r->p == '-';
            unsigned long long u = strtoull(r->p + (neg ? 1 : 0), &end, 10);
            n->ival = (uint64_t)u;
            n->cval = neg ? -(int64_t)u : (int64_t)u;
            n->text = arenaStrndup(r->a, r->p, (size_t)(end - r->p));
            if (neg) {
                /* -n is written as a unary minus over n */
                Node *m = lit(r, N_UNARY);
                m->op = P_MINUS;
                n->text = arenaStrndup(r->a, r->p + 1, (size_t)(end - r->p - 1));
                n->cval = (int64_t)u;
                m->a = n;
                n = m;
            }
        } else {
            if (strncmp(r->p, "null", 4) == 0) {
                r->failed = true;
                return NULL;
            }
            n = lit(r, N_FLOAT);
            n->fval = strtod(r->p, &end);
            n->text = arenaStrndup(r->a, r->p, (size_t)(end - r->p));
        }
        r->p = end;
        return n;
    }
    case TY_BOOL: {
        Node *n = lit(r, N_BOOL);
        n->ival = strncmp(r->p, "true", 4) == 0;
        r->p += n->ival ? 4 : 5;
        return n;
    }
    case TY_SLICE: case TY_STRING: {
        if (t->kind == TY_SLICE && !(t->elem->kind == TY_INT && t->elem->bits == 8 && !t->elem->isSigned)) {
            r->failed = true;      /* slices other than str have no memory to point to */
            return NULL;
        }
        Node *n = lit(r, N_STRING);
        n->text = readString(r, &n->textLen);
        return n;
    }
    case TY_ENUM: {
        Node *n = lit(r, N_ENUM_DOT);
        size_t len;
        n->name = readString(r, &len);
        return n;
    }
    case TY_ARRAY: {
        Node *n = lit(r, N_ARRAY_LIT);
        r->p++;
        for (;;) {
            skipWs(r);
            if (*r->p == ']') {
                r->p++;
                break;
            }
            Node *v = readValue(r, t->elem);
            if (r->failed) return NULL;
            listPush(r->a, &n->list, v);
            skipWs(r);
            if (*r->p == ',') r->p++;
        }
        return n;
    }
    case TY_STRUCT: {
        Node *n = lit(r, N_STRUCT_LIT);
        Node *name = lit(r, N_IDENT);
        name->name = t->decl->name;
        n->a = NULL;
        r->p++;
        for (;;) {
            skipWs(r);
            if (*r->p == '}') {
                r->p++;
                break;
            }
            size_t kl;
            char *key = readString(r, &kl);
            skipWs(r);
            if (*r->p == ':') r->p++;
            Node *field = NULL;
            for (int i = 0; i < t->decl->list.len; i++) {
                Node *f = t->decl->list.items[i];
                const char *fk = f->text ? f->text : f->name;
                if (fk && strcmp(fk, key) == 0) field = f;
            }
            if (!field || !field->type) {
                r->failed = true;
                return NULL;
            }
            Node *init = lit(r, N_FIELD_INIT);
            init->name = field->name;
            init->a = readValue(r, field->type);
            if (r->failed) return NULL;
            listPush(r->a, &n->list, init);
            skipWs(r);
            if (*r->p == ',') r->p++;
        }
        return n;
    }
    default:
        r->failed = true;
        return NULL;
    }
}

/* The literal for a comptime value of type t, from the JSON the helper
 * printed; NULL when t can't be a constant. */
Node *comptimeLiteral(Arena *a, const char *json, Type *t, Node *at) {
    Reader r = {a, json, at, false};
    Node *n = readValue(&r, t);
    return r.failed ? NULL : n;
}

const char *comptimeResult(const char *key) {
    for (int i = 0; i < gComptimeCount; i++) {
        if (strcmp(gComptimeResults[i].key, key) == 0) return gComptimeResults[i].json;
    }
    return NULL;
}
