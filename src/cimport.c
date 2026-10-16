/* import c: C headers to an Iris module.
 *
 * clang reads the headers twice: once for their declarations
 * (-Xclang -ast-dump=json) and once for their macros (-dM -E). This file
 * turns what it finds into Iris source:
 *
 *   int puts(const char *)        @extern("puts") pub i32 puts(u8 *p0)
 *   int printf(const char *, ...) @extern("printf", variadic) pub i32 printf(u8 *p0)
 *   struct timeval { long ... }   pub struct timeval { pub i64 tv_sec ... }
 *   typedef struct _IO_FILE FILE  pub type FILE = _IO_FILE
 *   #define EOF (-1)              pub const EOF = -1
 *
 * which the loader then parses like any other module. Declarations with
 * types Iris can't use yet (floating point, unions, structs passed by
 * value) are left out and listed in skipped, so an error can say why. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "iris.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---------------------------------------------------------------- JSON */

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JKind;

typedef struct JVal JVal;
struct JVal {
    JKind kind;
    const char *str;        /* strings, and object keys in keys[] */
    bool b;
    JVal **items;           /* arrays and objects */
    const char **keys;      /* objects */
    int n;
};

typedef struct {
    Arena *a;
    const char *p, *end;
    bool bad;
} JParser;

static void jSpace(JParser *j) {
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\n' || *j->p == '\r' || *j->p == '\t')) {
        j->p++;
    }
}

static const char *jString(JParser *j) {
    j->p++;                     /* " */
    StrBuf b = {0};
    while (j->p < j->end && *j->p != '"') {
        char c = *j->p++;
        if (c == '\\' && j->p < j->end) {
            char e = *j->p++;
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u':
                c = '?';        /* not needed for declarations */
                j->p += 4;
                if (j->p > j->end) j->p = j->end;
                break;
            default: c = e; break;
            }
        }
        sbPutc(&b, c);
    }
    if (j->p < j->end) {
        j->p++;
    } else {
        j->bad = true;
    }
    return sbFinish(&b, j->a);
}

static JVal *jValue(JParser *j, int depth);

static void jPush(JParser *j, JVal *v, const char *key, JVal *item, int *cap) {
    if (v->n == *cap) {
        int nc = *cap ? *cap * 2 : 8;
        JVal **items = arenaAlloc(j->a, sizeof(JVal *) * (size_t)nc);
        const char **keys = arenaAlloc(j->a, sizeof(char *) * (size_t)nc);
        if (v->n) {
            memcpy(items, v->items, sizeof(JVal *) * (size_t)v->n);
            if (v->keys) memcpy(keys, v->keys, sizeof(char *) * (size_t)v->n);
        }
        v->items = items;
        v->keys = keys;
        *cap = nc;
    }
    v->keys[v->n] = key;
    v->items[v->n++] = item;
}

static JVal *jValue(JParser *j, int depth) {
    jSpace(j);
    JVal *v = arenaAlloc(j->a, sizeof(JVal));
    if (j->p >= j->end || depth > 400) {
        j->bad = true;
        return v;
    }
    char c = *j->p;
    if (c == '{' || c == '[') {
        bool obj = c == '{';
        v->kind = obj ? J_OBJ : J_ARR;
        j->p++;
        int cap = 0;
        jSpace(j);
        if (j->p < j->end && *j->p == (obj ? '}' : ']')) {
            j->p++;
            return v;
        }
        while (j->p < j->end && !j->bad) {
            const char *key = NULL;
            if (obj) {
                jSpace(j);
                if (j->p >= j->end || *j->p != '"') {
                    j->bad = true;
                    break;
                }
                key = jString(j);
                jSpace(j);
                if (j->p >= j->end || *j->p != ':') {
                    j->bad = true;
                    break;
                }
                j->p++;
            }
            jPush(j, v, key, jValue(j, depth + 1), &cap);
            jSpace(j);
            if (j->p < j->end && *j->p == ',') {
                j->p++;
                continue;
            }
            if (j->p < j->end && *j->p == (obj ? '}' : ']')) {
                j->p++;
                break;
            }
            j->bad = true;
        }
        return v;
    }
    if (c == '"') {
        v->kind = J_STR;
        v->str = jString(j);
        return v;
    }
    if (strncmp(j->p, "true", 4) == 0 || strncmp(j->p, "false", 5) == 0) {
        v->kind = J_BOOL;
        v->b = *j->p == 't';
        j->p += v->b ? 4 : 5;
        return v;
    }
    if (strncmp(j->p, "null", 4) == 0) {
        j->p += 4;
        return v;
    }
    v->kind = J_NUM;
    const char *s = j->p;
    while (j->p < j->end && (isdigit((unsigned char)*j->p) || strchr("+-.eE", *j->p))) {
        j->p++;
    }
    v->str = arenaStrndup(j->a, s, (size_t)(j->p - s));
    if (j->p == s) {
        j->bad = true;
        j->p++;
    }
    return v;
}

static JVal *jGet(JVal *o, const char *key) {
    if (!o || o->kind != J_OBJ) return NULL;
    for (int i = 0; i < o->n; i++) {
        if (strcmp(o->keys[i], key) == 0) return o->items[i];
    }
    return NULL;
}

static const char *jStr(JVal *o, const char *key) {
    JVal *v = jGet(o, key);
    return v && (v->kind == J_STR || v->kind == J_NUM) ? v->str : NULL;
}

static bool jBool(JVal *o, const char *key) {
    JVal *v = jGet(o, key);
    return v && v->kind == J_BOOL && v->b;
}

/* The type of a declaration, with typedefs resolved. */
static const char *jType(JVal *o) {
    JVal *t = jGet(o, "type");
    const char *d = jStr(t, "desugaredQualType");
    return d ? d : jStr(t, "qualType");
}

/* ---------------------------------------------------------------- C types */

static const char *irisKeywords[] = {
    "if", "else", "for", "in", "match", "break", "continue", "return", "struct", "union", "enum",
    "interface", "type", "dyn", "pub", "const", "comptime", "when", "import", "try", "catch",
    "defer", "errdefer", "scoped", "spawn", "select", "chan", "thread", "threadlocal", "asm",
    "test", "true", "false", "null", "self", "_",
    /* built-in names and types */
    "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "bool", "void", "str", "String",
    "Error", "print", "panic", "format", "alloc", "free", "error", "sleep", "after", "Duration", "Mutex", "Thread", "Atomic",
};

/* A C name as an Iris name: keywords and built-in names get a _ after them. */
static const char *irisName(Arena *a, const char *name) {
    for (size_t i = 0; i < sizeof irisKeywords / sizeof irisKeywords[0]; i++) {
        if (strcmp(irisKeywords[i], name) == 0) {
            size_t n = strlen(name);
            char *s = arenaAlloc(a, n + 2);
            memcpy(s, name, n);
            s[n] = '_';
            return s;
        }
    }
    return name;
}

/* Names of functions and typedefs: a struct with one of these names is
 * called struct_NAME in Iris, since C keeps struct names apart and Iris
 * doesn't (struct stat and the function stat). */
static const char **gTaken;
static int gTakenN;

/* names of the typedefs written so far, which types can use by name */
static const char **gTypedefs;
static int gTypedefsN, gTypedefsCap;

static void addTypedef(Arena *a, const char *name) {
    if (gTypedefsN == gTypedefsCap) {
        int cap = gTypedefsCap ? gTypedefsCap * 2 : 128;
        const char **n = arenaAlloc(a, sizeof(char *) * (size_t)cap);
        if (gTypedefsN) memcpy(n, gTypedefs, sizeof(char *) * (size_t)gTypedefsN);
        gTypedefs = n;
        gTypedefsCap = cap;
    }
    gTypedefs[gTypedefsN++] = name;
}

static bool isTypedef(const char *name) {
    for (int i = 0; i < gTypedefsN; i++) {
        if (strcmp(gTypedefs[i], name) == 0) return true;
    }
    return false;
}

/* typedefs that name a struct (by value), like div_t */
static const char *gStructTypedefs[4096];
static int gStructTypedefsN;

static bool isStructTypedef(const char *name) {
    for (int i = 0; i < gStructTypedefsN; i++) {
        if (strcmp(gStructTypedefs[i], name) == 0) return true;
    }
    return false;
}

static void addStructTypedef(const char *name) {
    if (gStructTypedefsN < 4096) gStructTypedefs[gStructTypedefsN++] = name;
}

/* gByValue: the last type cType converted is a struct, not behind a pointer */
static bool gByValue;

/* structs that types refer to; any not written get an empty one at the end */
static const char *gRefs[4096];
static int gRefsN;

static const char *structName(Arena *a, const char *name) {
    for (int i = 0; i < gTakenN; i++) {
        if (strcmp(gTaken[i], name) == 0) {
            size_t n = strlen(name) + 8;
            char *s = arenaAlloc(a, n);
            snprintf(s, n, "struct_%s", name);
            return s;
        }
    }
    return irisName(a, name);
}

/* Converts a C type, as clang writes it, to Iris. Returns false for types
 * Iris can't use yet; why says which. param: arrays become pointers. */
static bool cType(Arena *a, const char *ct, bool param, StrBuf *out, const char **why) {
    gByValue = false;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", ct);
    /* an enum without a name, as typedefs give it: an i32 */
    if (strncmp(buf, "enum ", 5) == 0 && strchr(buf, '(') && !strchr(buf, '*')) {
        sbPuts(out, "i32");
        return true;
    }
    /* a pointer to a function: an Iris function type, R(A, B) */
    char *fp = strstr(buf, "(*)");
    if (fp) {
        char retPart[256], argPart[512];
        snprintf(retPart, sizeof retPart, "%.*s", (int)(fp - buf), buf);
        char *open = fp + 3;
        size_t al = strlen(open);
        bool ok = open[0] == '(' && al >= 2 && open[al - 1] == ')';
        if (ok) {
            snprintf(argPart, sizeof argPart, "%.*s", (int)(al - 2), open + 1);
        }
        StrBuf f = {0};
        const char *w2 = NULL;
        if (ok && strstr(argPart, "...")) {
            ok = false;
        }
        if (ok && cType(a, retPart, false, &f, &w2) && !gByValue) {
            sbPutc(&f, '(');
            /* the parameters, split at commas outside parentheses */
            int depth = 0, np = 0;
            char *start = argPart;
            bool none = strcmp(argPart, "void") == 0 || argPart[0] == 0;
            for (char *q = argPart; !none && ok; q++) {
                if (*q == '(') depth++;
                if (*q == ')') depth--;
                if ((*q == ',' && depth == 0) || *q == 0) {
                    char one[256];
                    snprintf(one, sizeof one, "%.*s", (int)(q - start), start);
                    StrBuf pt = {0};
                    if (!cType(a, one, true, &pt, &w2) || gByValue) {
                        ok = false;
                    } else {
                        sbPrintf(&f, "%s%s", np++ ? ", " : "", pt.buf);
                    }
                    sbFree(&pt);
                    start = q + 1;
                    if (*q == 0) break;
                }
            }
            sbPutc(&f, ')');
        } else {
            ok = false;
        }
        gByValue = false;
        if (ok) {
            sbPuts(out, f.buf);
        } else {
            sbPuts(out, "u8 *");        /* a function type Iris can't write: an opaque pointer */
        }
        sbFree(&f);
        return true;
    }
    if (strchr(buf, '(')) {
        *why = "a function type";
        return false;
    }
    /* words, stars and an array size */
    char *words[16];
    int nw = 0, stars = 0;
    long long array = -1;
    for (char *t = strtok(buf, " "); t; t = strtok(NULL, " ")) {
        while (*t == '*') {
            stars++;
            t++;
        }
        if (!*t) continue;
        char *br = strchr(t, '[');
        if (br) {
            if (strchr(br + 1, '[')) {
                *why = "an array of arrays";
                return false;
            }
            array = br[1] == ']' ? 0 : atoll(br + 1);
            *br = 0;
            if (!*t) continue;
        }
        char *star = strchr(t, '*');
        while (star) {
            *star = 0;
            stars++;
            star = strchr(star + 1, '*');
        }
        if (!*t) continue;
        if (strcmp(t, "const") == 0 || strcmp(t, "volatile") == 0 || strcmp(t, "restrict") == 0 ||
            strcmp(t, "__restrict") == 0 || strcmp(t, "_Nonnull") == 0 || strcmp(t, "_Nullable") == 0 ||
            strcmp(t, "_Null_unspecified") == 0 || (strcmp(t, "signed") == 0 && nw > 0)) {
            continue;
        }
        if (nw < 16) words[nw++] = t;
    }
    if (nw == 0) {
        *why = "an unknown type";
        return false;
    }
    char base[256] = "";
    for (int i = 0; i < nw; i++) {
        if (i) strncat(base, " ", sizeof base - strlen(base) - 1);
        strncat(base, words[i], sizeof base - strlen(base) - 1);
    }
    static const struct { const char *c, *iris; } map[] = {
        {"char", "u8"}, {"signed char", "i8"}, {"unsigned char", "u8"}, {"_Bool", "u8"}, {"bool", "u8"},
        {"short", "i16"}, {"short int", "i16"}, {"signed short", "i16"}, {"unsigned short", "u16"},
        {"unsigned short int", "u16"}, {"int", "i32"}, {"signed", "i32"}, {"signed int", "i32"},
        {"unsigned", "u32"}, {"unsigned int", "u32"}, {"long", "i64"}, {"long int", "i64"},
        {"signed long", "i64"}, {"unsigned long", "u64"}, {"unsigned long int", "u64"},
        {"long long", "i64"}, {"long long int", "i64"}, {"unsigned long long", "u64"},
        {"unsigned long long int", "u64"},
    };
    const char *iris = NULL;
    char named[300];
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        if (strcmp(map[i].c, base) == 0) iris = map[i].iris;
    }
    if (!iris) {
        if (strcmp(base, "void") == 0) {
            if (stars == 0 && array < 0) {
                sbPuts(out, "void");
                return true;
            }
            iris = "u8";            /* void * is a pointer to bytes */
        } else if (strncmp(base, "struct ", 7) == 0 && !strchr(base + 7, ' ')) {
            snprintf(named, sizeof named, "%s", structName(a, base + 7));
            iris = named;
            if (gRefsN < 4096) {
                gRefs[gRefsN++] = arenaStrndup(a, named, strlen(named));
            }
        } else if (strncmp(base, "enum ", 5) == 0) {
            iris = "i32";
        } else if (strncmp(base, "union ", 6) == 0) {
            if (stars == 0) {
                *why = "a union";
                return false;
            }
            iris = "u8";            /* a pointer to a union: bytes */
        } else if (strcmp(base, "float") == 0) {
            iris = "f32";
        } else if (strcmp(base, "double") == 0) {
            iris = "f64";
        } else if (strcmp(base, "long double") == 0 || strncmp(base, "_Float", 6) == 0 || strcmp(base, "__float128") == 0) {
            *why = "a floating point type wider than f64";
            return false;
        } else if (nw == 1 && isTypedef(base)) {
            snprintf(named, sizeof named, "%s", irisName(a, base));
            iris = named;
        } else {
            *why = "a C type Iris doesn't know";
            return false;
        }
    }
    sbPuts(out, iris);
    if (stars == 0 && array < 0 &&
        ((strncmp(base, "struct ", 7) == 0) || (nw == 1 && isStructTypedef(base)))) {
        gByValue = true;
    }
    if (array >= 0) {
        if (param || array == 0) {
            stars++;
        } else {
            for (int i = 0; i < stars; i++) sbPuts(out, i ? "*" : " *");
            sbPrintf(out, "[%lld]", array);
            return true;
        }
    }
    for (int i = 0; i < stars; i++) {
        sbPuts(out, i ? "*" : " *");
    }
    return true;
}

/* ---------------------------------------------------------------- the module */

typedef struct {
    Arena *a;
    StrBuf out;
    NodeList *skipped;
    JVal *decls;
    const char **names;     /* every name written, to skip duplicates */
    int nnames, capNames;
} Gen;

static bool seen(Gen *g, const char *name) {
    for (int i = 0; i < g->nnames; i++) {
        if (strcmp(g->names[i], name) == 0) return true;
    }
    return false;
}

static void remember(Gen *g, const char *name) {
    if (g->nnames == g->capNames) {
        int cap = g->capNames ? g->capNames * 2 : 256;
        const char **n = arenaAlloc(g->a, sizeof(char *) * (size_t)cap);
        if (g->nnames) memcpy(n, g->names, sizeof(char *) * (size_t)g->nnames);
        g->names = n;
        g->capNames = cap;
    }
    g->names[g->nnames++] = name;
}

static void skip(Gen *g, const char *name, const char *why) {
    Node *n = newNode(g->a, N_IDENT, 0, 0);
    n->name = irisName(g->a, name);
    n->text = why;
    listPush(g->a, g->skipped, n);
}

/* The id of the declaration a typedef's type refers to. */
static const char *declId(JVal *v) {
    if (!v) return NULL;
    if (v->kind == J_OBJ) {
        JVal *d = jGet(v, "decl");
        if (d) {
            const char *id = jStr(d, "id");
            if (id) return id;
        }
    }
    if (v->kind == J_OBJ || v->kind == J_ARR) {
        for (int i = 0; i < v->n; i++) {
            const char *id = declId(v->items[i]);
            if (id) return id;
        }
    }
    return NULL;
}

static bool hidden(const char *name) { return !name || !*name || (name[0] == '_' && name[1] == '_'); }

static void genRecordAs(Gen *g, JVal *d, const char *iname);

static void genRecord(Gen *g, JVal *d) {
    const char *name = jStr(d, "name");
    if (!name || !*name) return;
    genRecordAs(g, d, structName(g->a, name));
}

static void genRecordAs(Gen *g, JVal *d, const char *iname) {
    if (seen(g, iname)) return;
    remember(g, iname);
    const char *tag = jStr(d, "tagUsed");
    JVal *inner = jGet(d, "inner");
    StrBuf fields = {0};
    bool full = tag && strcmp(tag, "struct") == 0 && jBool(d, "completeDefinition");
    if (full && inner && inner->kind == J_ARR) {
        for (int i = 0; i < inner->n && full; i++) {
            JVal *f = inner->items[i];
            const char *kind = jStr(f, "kind");
            if (!kind || strcmp(kind, "FieldDecl") != 0) {
                if (kind && strcmp(kind, "RecordDecl") == 0) full = false;     /* nested struct or union */
                continue;
            }
            const char *fname = jStr(f, "name");
            const char *why = NULL;
            StrBuf t = {0};
            if (!fname || !*fname || jBool(f, "isBitfield") || !cType(g->a, jType(f), false, &t, &why)) {
                full = false;
            } else {
                sbPrintf(&fields, "    pub %s %s\n", t.buf, irisName(g->a, fname));
            }
            sbFree(&t);
        }
    }
    if (full) {
        sbPrintf(&g->out, "pub struct %s {\n%s}\n\n", iname, fields.buf ? fields.buf : "");
    } else {
        /* only usable through a pointer */
        sbPrintf(&g->out, "pub struct %s {\n}\n\n", iname);
    }
    sbFree(&fields);
}

static void genFunction(Gen *g, JVal *d) {
    const char *name = jStr(d, "name");
    if (hidden(name) || seen(g, irisName(g->a, name))) return;
    const char *sc = jStr(d, "storageClass");
    if ((sc && strcmp(sc, "static") == 0) || jBool(d, "inline")) {
        skip(g, name, "an inline function, which has no code to call");
        return;
    }
    const char *ft = jType(d);
    if (!ft) return;
    /* the return type: what comes before the parameters' ( */
    const char *paren = strchr(ft, '(');
    if (!paren || paren[1] == '*') {
        skip(g, name, "a function that returns a function");
        return;
    }
    char ret[256];
    snprintf(ret, sizeof ret, "%.*s", (int)(paren - ft), ft);
    const char *why = NULL;
    StrBuf rt = {0}, params = {0};
    bool ok = cType(g->a, ret, false, &rt, &why);
    if (ok && gByValue) {
        ok = false;
        why = "a struct returned by value";
    }
    JVal *inner = jGet(d, "inner");
    int np = 0;
    for (int i = 0; ok && inner && inner->kind == J_ARR && i < inner->n; i++) {
        JVal *p = inner->items[i];
        const char *kind = jStr(p, "kind");
        if (!kind || strcmp(kind, "ParmVarDecl") != 0) continue;
        const char *pt = jType(p);
        StrBuf t = {0};
        if (!pt || !cType(g->a, pt, true, &t, &why)) {
            ok = false;
        } else if (gByValue) {
            ok = false;
            why = "a struct passed by value";
        } else {
            sbPrintf(&params, "%s%s p%d", np ? ", " : "", t.buf, np);
            np++;
        }
        sbFree(&t);
    }
    if (ok) {
        const char *iname = irisName(g->a, name);
        remember(g, iname);
        sbPrintf(&g->out, "@extern(\"%s\"%s)\npub %s %s(%s)\n\n", name, jBool(d, "variadic") ? ", variadic" : "",
                 rt.buf, iname, params.buf ? params.buf : "");
    } else {
        skip(g, name, why ? why : "a type Iris can't use yet");
    }
    sbFree(&rt);
    sbFree(&params);
}

static void genTypedef(Gen *g, JVal *d) {
    const char *name = jStr(d, "name");
    if (hidden(name)) return;
    const char *iname = irisName(g->a, name);
    const char *t = jType(d);
    if (!t) return;
    /* typedef struct foo foo: the struct already has the name */
    if (strncmp(t, "struct ", 7) == 0 && strcmp(t + 7, name) == 0) {
        if (!isTypedef(name)) addTypedef(g->a, name);
        addStructTypedef(name);
        return;
    }
    if (seen(g, iname)) return;
    /* typedef struct { ... } foo: the struct gets the typedef's name */
    JVal *in = jGet(d, "inner");
    JVal *owned = in && in->kind == J_ARR && in->n ? jGet(in->items[0], "ownedTagDecl") : NULL;
    const char *ownedName = jStr(owned, "name");
    bool anonymous = (owned && (!ownedName || !*ownedName)) ||
                     (strncmp(t, "struct ", 7) == 0 && strchr(t, '(') && !strchr(t, '*'));
    if (anonymous && g->decls) {
        const char *kind = jStr(owned, "kind");
        if (kind && strcmp(kind, "EnumDecl") == 0) {
            remember(g, iname);
            addTypedef(g->a, name);
            sbPrintf(&g->out, "pub type %s = i32\n\n", iname);
            return;
        }
        const char *id = owned ? jStr(owned, "id") : declId(jGet(d, "inner"));
        for (int i = 0; id && i < g->decls->n; i++) {
            JVal *r = g->decls->items[i];
            const char *rid = jStr(r, "id");
            if (rid && strcmp(rid, id) == 0) {
                genRecordAs(g, r, iname);
                addTypedef(g->a, name);
                addStructTypedef(name);
                return;
            }
        }
    }
    const char *why = NULL;
    StrBuf it = {0};
    if (cType(g->a, t, false, &it, &why) && strcmp(it.buf, "void") != 0) {
        remember(g, iname);
        addTypedef(g->a, name);
        if (gByValue) addStructTypedef(name);
        sbPrintf(&g->out, "pub type %s = %s\n\n", iname, it.buf);
    } else {
        skip(g, name, why ? why : "a type Iris can't use yet");
    }
    sbFree(&it);
}

static void genEnum(Gen *g, JVal *d) {
    JVal *inner = jGet(d, "inner");
    long long next = 0;
    for (int i = 0; inner && inner->kind == J_ARR && i < inner->n; i++) {
        JVal *c = inner->items[i];
        const char *kind = jStr(c, "kind");
        if (!kind || strcmp(kind, "EnumConstantDecl") != 0) continue;
        JVal *ci = jGet(c, "inner");
        if (ci && ci->kind == J_ARR && ci->n) {
            const char *v = jStr(ci->items[0], "value");
            if (v) next = atoll(v);
        }
        const char *name = jStr(c, "name");
        if (!hidden(name) && !seen(g, irisName(g->a, name))) {
            remember(g, irisName(g->a, name));
            sbPrintf(&g->out, "pub const %s = %lld\n", irisName(g->a, name), next);
        }
        next++;
    }
    const char *name = jStr(d, "name");
    if (!hidden(name) && !seen(g, irisName(g->a, name))) {
        remember(g, irisName(g->a, name));
        sbPrintf(&g->out, "pub type %s = i32\n", irisName(g->a, name));
    }
    sbPuts(&g->out, "\n");
}

static bool predefinedName(const char *pre, const char *name) {
    if (!pre) return false;
    char key[600];
    snprintf(key, sizeof key, "#define %s ", name);
    return strstr(pre, key) != NULL;
}

/* #define NAME 42, (-1), 0x10u, "text": constants. pre holds the macros
 * clang defines by itself (linux, unix ...), which are left out. */
static void genMacros(Gen *g, const char *text, const char *pre) {
    for (const char *line = text; line && *line;) {
        const char *nl = strchr(line, '\n');
        char buf[512];
        snprintf(buf, sizeof buf, "%.*s", (int)(nl ? nl - line : (long)strlen(line)), line);
        line = nl ? nl + 1 : NULL;
        if (strncmp(buf, "#define ", 8) != 0) continue;
        char *name = buf + 8;
        char *sp = strchr(name, ' ');
        if (!sp) continue;
        *sp = 0;
        if (strchr(name, '(') || hidden(name) || name[0] == '_' || predefinedName(pre, name)) continue;
        char *v = sp + 1;
        size_t sl = strlen(v);
        if (sl >= 2 && v[0] == '"' && v[sl - 1] == '"') {
            /* a plain string literal, without quotes inside */
            bool plain = true;
            for (size_t k = 1; k + 1 < sl; k++) {
                if (v[k] == '"' || v[k] == '\\') plain = false;
            }
            const char *iname = irisName(g->a, arenaStrndup(g->a, name, strlen(name)));
            if (plain && !seen(g, iname)) {
                remember(g, iname);
                sbPrintf(&g->out, "pub const %s = %s\n", iname, v);
            }
            continue;
        }
        /* strip parentheses around the value */
        size_t vl = strlen(v);
        while (vl >= 2 && v[0] == '(' && v[vl - 1] == ')') {
            v[vl - 1] = 0;
            v++;
            vl -= 2;
        }
        bool neg = false;
        if (*v == '-') {
            neg = true;
            v++;
        }
        if (!isdigit((unsigned char)*v)) continue;
        char *end;
        unsigned long long n = strtoull(v, &end, 0);
        while (*end && strchr("uUlL", *end)) end++;
        if (*end || n > 9223372036854775807ULL) continue;
        const char *iname = irisName(g->a, arenaStrndup(g->a, name, strlen(name)));
        if (seen(g, iname)) continue;
        remember(g, iname);
        sbPrintf(&g->out, "pub const %s = %s%llu\n", iname, neg ? "-" : "", n);
    }
}


char *cImport(Arena *a, const char **headers, int n, const char **includeDirs, int nInclude,
              NodeList *skipped) {
    const char *base = getenv("TMPDIR");
    char dir[4096];
    snprintf(dir, sizeof dir, "%s/iris-c-XXXXXX", base && *base ? base : "/tmp");
    if (!mkdtemp(dir)) {
        perror("iris: mkdtemp");
        return NULL;
    }
    char cPath[4200], jsonPath[4200], macroPath[4200], errPath[4200];
    snprintf(cPath, sizeof cPath, "%s/h.c", dir);
    snprintf(jsonPath, sizeof jsonPath, "%s/h.json", dir);
    snprintf(macroPath, sizeof macroPath, "%s/h.macros", dir);
    snprintf(errPath, sizeof errPath, "%s/h.err", dir);
    StrBuf src = {0};
    for (int i = 0; i < n; i++) {
        sbPrintf(&src, "#include <%s>\n", headers[i]);
    }
    writeText(cPath, src.buf, src.len);
    sbFree(&src);

    const char *cc = getenv("IRIS_CC");
    if (!cc || !*cc) cc = "clang";
    char *args[64];
    int na = 0;
    args[na++] = (char *)cc;
    args[na++] = "-Xclang";
    args[na++] = "-ast-dump=json";
    args[na++] = "-fsyntax-only";
    args[na++] = "-w";
    if (gTarget.triple) {
        args[na++] = "-target";
        args[na++] = (char *)gTarget.triple;
    }
    for (int i = 0; i < nInclude && na < 50; i++) {
        args[na++] = "-I";
        args[na++] = (char *)includeDirs[i];
    }
    args[na++] = cPath;
    args[na] = NULL;
    int rc = runCommand(args, jsonPath, errPath);
    char *result = NULL;
    if (rc != 0) {
        size_t elen = 0;
        char *err = readAllFile(a, errPath, &elen);
        fprintf(stderr, "iris: clang can't read the C header%s:\n%s", n == 1 ? "" : "s", err ? err : "");
        if (rc == 127) {
            fprintf(stderr, "iris: import c needs clang; install it or set IRIS_CC\n");
        }
        gErrors++;
        goto done;
    }
    na = 0;
    args[na++] = (char *)cc;
    args[na++] = "-dM";
    args[na++] = "-E";
    args[na++] = "-w";
    if (gTarget.triple) {
        args[na++] = "-target";
        args[na++] = (char *)gTarget.triple;
    }
    for (int i = 0; i < nInclude && na < 50; i++) {
        args[na++] = "-I";
        args[na++] = (char *)includeDirs[i];
    }
    args[na++] = cPath;
    args[na] = NULL;
    runCommand(args, macroPath, errPath);

    /* the macros clang defines with no headers at all */
    char emptyPath[4200], prePath[4200];
    snprintf(emptyPath, sizeof emptyPath, "%s/empty.c", dir);
    snprintf(prePath, sizeof prePath, "%s/pre.macros", dir);
    writeText(emptyPath, "", 0);
    char *preArgs[] = {(char *)cc, "-dM", "-E", "-w", emptyPath, NULL};
    runCommand(preArgs, prePath, errPath);

    size_t jlen = 0, mlen = 0;
    char *json = readAllFile(a, jsonPath, &jlen);
    char *macros = readAllFile(a, macroPath, &mlen);
    size_t plen = 0;
    char *pre = readAllFile(a, prePath, &plen);
    JParser jp = {a, json, json + jlen, false};
    JVal *root = json ? jValue(&jp, 0) : NULL;
    if (!root || jp.bad) {
        fprintf(stderr, "iris: can't understand clang's description of the C headers\n");
        gErrors++;
        goto done;
    }

    Gen g = {0};
    g.a = a;
    g.skipped = skipped;
    sbPuts(&g.out, "// generated by import c from clang's reading of the headers\n\n");
    JVal *decls = jGet(root, "inner");
    g.decls = decls && decls->kind == J_ARR ? decls : NULL;
    /* the names functions and typedefs use, so structs can step aside */
    gTakenN = 0;
    gRefsN = 0;
    gTypedefsN = 0;
    gTypedefsCap = 0;
    gStructTypedefsN = 0;
    gTypedefs = NULL;
    gTaken = arenaAlloc(a, sizeof(char *) * (size_t)(g.decls ? g.decls->n + 1 : 1));
    for (int i = 0; g.decls && i < g.decls->n; i++) {
        JVal *d = g.decls->items[i];
        const char *kind = jStr(d, "kind");
        const char *name = jStr(d, "name");
        if (kind && name && (strcmp(kind, "FunctionDecl") == 0 || strcmp(kind, "TypedefDecl") == 0)) {
            const char *t = strcmp(kind, "TypedefDecl") == 0 ? jType(d) : NULL;
            if (t && strncmp(t, "struct ", 7) == 0 && strcmp(t + 7, name) == 0) {
                continue;               /* typedef struct foo foo */
            }
            gTaken[gTakenN++] = name;
        }
    }
    /* structs first: the best (complete) definition of each name */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; decls && decls->kind == J_ARR && i < decls->n; i++) {
            JVal *d = decls->items[i];
            const char *kind = jStr(d, "kind");
            if (!kind || strcmp(kind, "RecordDecl") != 0) continue;
            if (pass == 0 && !jBool(d, "completeDefinition")) continue;
            genRecord(&g, d);
        }
    }
    for (int i = 0; decls && decls->kind == J_ARR && i < decls->n; i++) {
        JVal *d = decls->items[i];
        const char *kind = jStr(d, "kind");
        if (!kind) continue;
        if (strcmp(kind, "EnumDecl") == 0) genEnum(&g, d);
    }
    for (int i = 0; decls && decls->kind == J_ARR && i < decls->n; i++) {
        JVal *d = decls->items[i];
        const char *kind = jStr(d, "kind");
        if (!kind) continue;
        if (strcmp(kind, "TypedefDecl") == 0) genTypedef(&g, d);
    }
    for (int i = 0; decls && decls->kind == J_ARR && i < decls->n; i++) {
        JVal *d = decls->items[i];
        const char *kind = jStr(d, "kind");
        if (!kind) continue;
        if (strcmp(kind, "FunctionDecl") == 0) genFunction(&g, d);
    }
    if (macros) {
        genMacros(&g, macros, pre);
    }
    for (int i = 0; i < gRefsN; i++) {
        if (!seen(&g, gRefs[i])) {
            remember(&g, gRefs[i]);
            sbPrintf(&g.out, "pub struct %s {\n}\n", gRefs[i]);
        }
    }
    result = sbFinish(&g.out, a);

done:
    {
        char extra[4200];
        snprintf(extra, sizeof extra, "%s/empty.c", dir);
        unlink(extra);
        snprintf(extra, sizeof extra, "%s/pre.macros", dir);
        unlink(extra);
    }
    unlink(cPath);
    unlink(jsonPath);
    unlink(macroPath);
    unlink(errPath);
    rmdir(dir);
    return result;
}
