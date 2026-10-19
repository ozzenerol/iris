/* The checker: looks up names, works out the type of every expression and
 * checks the rules the parser can't see. It fills in node->type, node->ref
 * and, for values known at compile time, node->isConst and node->cval.
 *
 * It also rewrites a few things so the IR writer has less to work out:
 * implicit conversions become N_CONVERT nodes, a field reached through an
 * embedded struct gets the missing steps, and a method call gets its
 * receiver as the first argument.
 *
 * Errors don't stop it. An expression with an error gets tyError, which
 * fits anywhere, so one mistake gives one message. */
#include "iris.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Type tyError = {.kind = TY_ERROR, .name = "error"};
Type tyVoid = {.kind = TY_VOID, .name = "void"};
Type tyNoreturn = {.kind = TY_NORETURN, .name = "noreturn"};
Type tyBool = {.kind = TY_BOOL, .bits = 1, .name = "bool"};
Type tyNull = {.kind = TY_NULL, .name = "null"};
Type tyI8 = {.kind = TY_INT, .bits = 8, .isSigned = true, .name = "i8"};
Type tyI16 = {.kind = TY_INT, .bits = 16, .isSigned = true, .name = "i16"};
Type tyI32 = {.kind = TY_INT, .bits = 32, .isSigned = true, .name = "i32"};
Type tyI64 = {.kind = TY_INT, .bits = 64, .isSigned = true, .name = "i64"};
Type tyU8 = {.kind = TY_INT, .bits = 8, .name = "u8"};
Type tyU16 = {.kind = TY_INT, .bits = 16, .name = "u16"};
Type tyU32 = {.kind = TY_INT, .bits = 32, .name = "u32"};
Type tyU64 = {.kind = TY_INT, .bits = 64, .name = "u64"};
Type tyDuration = {.kind = TY_INT, .bits = 64, .isSigned = true, .name = "Duration"};
Type tyF32 = {.kind = TY_FLOAT, .bits = 32, .isSigned = true, .name = "f32"};
Type tyF64 = {.kind = TY_FLOAT, .bits = 64, .isSigned = true, .name = "f64"};
Type tyStr = {.kind = TY_SLICE, .name = "str", .elem = &tyU8};
Type tyString = {.kind = TY_STRING, .name = "String", .ir = "%String"};
Type tyErrorValue = {.kind = TY_ERRVAL, .name = "Error"};
Type tyErrCode = {.kind = TY_ERRCODE, .name = "error code"};
Type tyMutex = {.kind = TY_MUTEX, .name = "Mutex"};
Type tyThread = {.kind = TY_THREAD, .name = "Thread"};

static Type *builtinTypes[] = {&tyI8, &tyI16, &tyI32, &tyI64, &tyU8, &tyU16, &tyU32,
                               &tyU64, &tyF32, &tyF64, &tyBool, &tyStr, &tyVoid, &tyString, &tyErrorValue,
                               &tyDuration, &tyMutex, &tyThread};

/* types that exist in the language but not in this version yet */
static const struct {
    const char *name, *version;
} laterTypes[] = {
    {"Arena", "a later version"},
};

bool gTesting, gWithTests;

/* ---------------------------------------------------------------- target */

Target gTarget = {
#ifdef __APPLE__
    .os = "macos",
#else
    .os = "linux",
#endif
#if defined(__aarch64__)
    .arch = "arm64",
#else
    .arch = "x86_64",
#endif
};

bool setTarget(const char *name) {
    static const struct { const char *name, *os, *arch, *triple; } targets[] = {
        {"x86_64-linux", "linux", "x86_64", "x86_64-linux-gnu"},
        {"aarch64-linux", "linux", "arm64", "aarch64-linux-gnu"},
        {"arm64-linux", "linux", "arm64", "aarch64-linux-gnu"},
        {"x86_64-macos", "macos", "x86_64", "x86_64-apple-macos11"},
        {"aarch64-macos", "macos", "arm64", "arm64-apple-macos11"},
        {"arm64-macos", "macos", "arm64", "arm64-apple-macos11"},
    };
    for (size_t i = 0; i < sizeof targets / sizeof targets[0]; i++) {
        if (strcmp(targets[i].name, name) == 0) {
            gTarget.os = targets[i].os;
            gTarget.arch = targets[i].arch;
            gTarget.triple = targets[i].triple;
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------- scopes */

typedef struct {
    const char *name;
    Node *decl;
} Sym;

typedef struct Scope Scope;
struct Scope {
    Scope *parent;
    Sym *syms;
    int n, cap;
    bool function;          /* the outermost scope of a function */
};

typedef struct Loop Loop;
struct Loop {
    Loop *outer;
    const char *label;
};

typedef struct {
    Arena *arena;
    Source *src;
    Scope *scope;
    const char *module;     /* the module of the code being checked */
    Node *func;             /* the function being checked */
    Loop *loop;
    const char *pendingLabel;
    bool quiet;             /* evalConst: don't report, just answer */
    bool allowResult;       /* the next expression may be a !T call: try, catch, _ = */
    int enumKinds;          /* enums numbered so far */
    bool inDefer;           /* checking the statement of a defer */
    /* generics */
    struct Instance *instances;
    NodeList *instOut;      /* instances for the IR writer: the file's list2 */
    NodeList pendFns;       /* instance functions whose bodies are still to check */
    NodeList pendSites;     /* ... and where each was first used */
    Scope **pendScopes;
    int pendCap;
    int instDepth;
    int instGen;            /* how many instances deep the body being checked is */
    NodeList genericMethods;
    NodeList exports;       /* @export functions, to catch two with one name */
    Program *prog;
} Checker;

typedef struct Instance Instance;
struct Instance {
    Instance *next;
    Node *generic;
    Type **args;
    int nargs;
    Type *type;             /* a struct instance */
    Node *fn;               /* a function instance */
};

static void checkStmt(Checker *c, Node *s);

static void error(Checker *c, Node *at, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    verrorAt(at->src ? at->src : c->src, at->line, at->col, fmt, ap);
    va_end(ap);
}

/* A new node at the same place as another. */
static Node *like(Checker *c, NodeKind kind, Node *at) {
    Node *n = newNode(c->arena, kind, at->line, at->col);
    n->src = at->src;
    return n;
}

/* A name that isn't declared: if it is a standard library module, the
 * import is what's missing. */
static void notDeclared(Checker *c, Node *at, const char *name) {
    static const char *mods[64];
    static int nmods = -1;
    if (nmods < 0) {
        nmods = stdModuleNames(mods, 64);
    }
    for (int i = 0; i < nmods; i++) {
        if (strcmp(mods[i], name) == 0) {
            error(c, at, "%s is a standard library module that isn't imported; add import %s", name, name);
            return;
        }
    }
    error(c, at, "%s is not declared", name);
}

static void notYet(Checker *c, Node *at, const char *what, const char *version) {
    /* "generic types are", "this expression is", "enums whose values carry data are" */
    const char *sp = strchr(what, ' ');
    bool firstPlural = sp && sp > what && sp[-1] == 's' && strncmp(what, "this ", 5) != 0;
    bool plural = what[strlen(what) - 1] == 's' || strstr(what, " and ") || firstPlural;
    error(c, at, "%s %s not supported yet (planned for %s)", what, plural ? "are" : "is", version);
}

static Scope *pushScope(Checker *c, bool function) {
    Scope *s = arenaAlloc(c->arena, sizeof(Scope));
    s->parent = c->scope;
    s->function = function;
    c->scope = s;
    return s;
}

static void popScope(Checker *c) { c->scope = c->scope->parent; }

static Sym *findIn(Scope *s, const char *name) {
    for (int i = 0; i < s->n; i++) {
        if (strcmp(s->syms[i].name, name) == 0) {
            return &s->syms[i];
        }
    }
    return NULL;
}

static Node *lookupPlain(Checker *c, const char *name) {
    for (Scope *s = c->scope; s; s = s->parent) {
        Sym *sym = findIn(s, name);
        if (sym) {
            return sym->decl;
        }
    }
    return NULL;
}

static bool visible(Checker *c, Node *d) {
    return !d->module || !c->module || strcmp(d->module, c->module) == 0 || (d->flags & F_PUB);
}

/* A name, or mod.name for a name in an imported module (pub only). */
static Node *lookup(Checker *c, const char *name) {
    const char *dot = strchr(name, '.');
    if (!dot) {
        return lookupPlain(c, name);
    }
    char mod[128];
    size_t n = (size_t)(dot - name);
    if (n >= sizeof mod) {
        return NULL;
    }
    memcpy(mod, name, n);
    mod[n] = 0;
    Node *imp = lookupPlain(c, mod);
    if (!imp || imp->kind != N_IMPORT || !imp->scope) {
        return NULL;
    }
    Sym *sym = findIn((Scope *)imp->scope, dot + 1);
    if (!sym || !visible(c, sym->decl)) {
        return NULL;
    }
    return sym->decl;
}

static void addSym(Checker *c, Scope *s, const char *name, Node *decl) {
    if (s->n == s->cap) {
        int cap = s->cap ? s->cap * 2 : 8;
        Sym *syms = arenaAlloc(c->arena, sizeof(Sym) * (size_t)cap);
        if (s->n) {
            memcpy(syms, s->syms, sizeof(Sym) * (size_t)s->n);
        }
        s->syms = syms;
        s->cap = cap;
    }
    s->syms[s->n++] = (Sym){name, decl};
}

/* Declares a local. A name used by a local in an enclosing block of the
 * same function is an error: no shadowing. */
static void declareLocal(Checker *c, const char *name, Node *decl) {
    if (strcmp(name, "_") == 0) {
        error(c, decl, "'_' can't be used as a variable name");
        return;
    }
    for (Scope *s = c->scope; s; s = s->parent) {
        Sym *old = findIn(s, name);
        if (old) {
            error(c, decl, "%s is already declared on line %d", name, old->decl->line);
            return;
        }
        if (s->function) {
            break;
        }
    }
    Node *imp = lookupPlain(c, name);
    if (imp && imp->kind == N_IMPORT) {
        error(c, decl, "%s is the name of an imported module", name);
        return;
    }
    addSym(c, c->scope, name, decl);
}

/* ---------------------------------------------------------------- types */

static bool isInt(Type *t) { return t->kind == TY_INT; }
static bool isFloat(Type *t) { return t->kind == TY_FLOAT; }
static bool isNumber(Type *t) { return t->kind == TY_INT || t->kind == TY_FLOAT; }
static bool isErr(Type *t) { return t->kind == TY_ERROR; }
static bool isStr(Type *t) { return t->kind == TY_SLICE && t->elem == &tyU8; }

static Arena *gArena;       /* for compound types, which live as long as the compile */

typedef struct TypeCache TypeCache;
struct TypeCache {
    TypeCache *next;
    Type *t;
};
static TypeCache *gTypes;

static bool sameType(Type *a, Type *b);

static Type *compound(TypeKind kind, Type *elem, int64_t count) {
    if (kind == TY_SLICE && elem == &tyU8) {
        return &tyStr;
    }
    for (TypeCache *tc = gTypes; tc; tc = tc->next) {
        /* Duration is an i64 that prints as 1m30s: !Duration isn't !i64 */
        if (tc->t->kind == kind && tc->t->count == count && sameType(tc->t->elem, elem) &&
            (tc->t->elem == &tyDuration) == (elem == &tyDuration)) {
            return tc->t;
        }
    }
    Type *t = arenaAlloc(gArena, sizeof(Type));
    t->kind = kind;
    t->elem = elem;
    t->count = count;
    StrBuf b = {0};
    if (kind == TY_PTR) {
        sbPrintf(&b, "%s%s", elem->name, elem->kind == TY_PTR ? "*" : " *");
    } else if (kind == TY_CHAN) {
        sbPrintf(&b, "chan[%s]", elem->name);
    } else if (kind == TY_ATOMIC) {
        sbPrintf(&b, "Atomic[%s]", elem->name);
    } else if (kind == TY_RESULT) {
        sbPrintf(&b, "!%s", elem->name);
    } else if (kind == TY_SLICE) {
        sbPrintf(&b, "%s[]", elem->name);
    } else {
        sbPrintf(&b, "%s[%lld]", elem->name, (long long)count);
    }
    t->name = sbFinish(&b, gArena);
    TypeCache *tc = arenaAlloc(gArena, sizeof(TypeCache));
    tc->t = t;
    tc->next = gTypes;
    gTypes = tc;
    return t;
}

static Type *ptrTo(Type *t) { return compound(TY_PTR, t, 0); }
static Type *sliceOf(Type *t) { return compound(TY_SLICE, t, 0); }
static Type *arrayOf(Type *t, int64_t n) { return compound(TY_ARRAY, t, n); }
static Type *resultOf(Type *t) { return compound(TY_RESULT, t, 0); }
static Type *chanOf(Type *t) { return compound(TY_CHAN, t, 0); }

/* R(A, B): a function type */
static Type *funcOf(Type *ret, Type **params, int n) {
    for (TypeCache *tc = gTypes; tc; tc = tc->next) {
        Type *t = tc->t;
        if (t->kind != TY_FUNC || t->nelems != n || !sameType(t->elem, ret)) continue;
        bool same = true;
        for (int i = 0; i < n && same; i++) {
            same = sameType(t->elems[i], params[i]);
        }
        if (same) return t;
    }
    Type *t = arenaAlloc(gArena, sizeof(Type));
    t->kind = TY_FUNC;
    t->elem = ret;
    t->nelems = n;
    t->elems = arenaAlloc(gArena, sizeof(Type *) * (size_t)(n ? n : 1));
    StrBuf b = {0};
    sbPrintf(&b, "%s(", ret->name);
    for (int i = 0; i < n; i++) {
        t->elems[i] = params[i];
        sbPrintf(&b, "%s%s", i ? ", " : "", params[i]->name);
    }
    sbPutc(&b, ')');
    t->name = sbFinish(&b, gArena);
    TypeCache *tc = arenaAlloc(gArena, sizeof(TypeCache));
    tc->t = t;
    tc->next = gTypes;
    gTypes = tc;
    return t;
}

/* (A, B, ...) */
static Type *tupleOf(Type **elems, int n) {
    for (TypeCache *tc = gTypes; tc; tc = tc->next) {
        Type *t = tc->t;
        if (t->kind != TY_TUPLE || t->nelems != n) continue;
        bool same = true;
        for (int i = 0; i < n && same; i++) {
            same = sameType(t->elems[i], elems[i]);
        }
        if (same) return t;
    }
    Type *t = arenaAlloc(gArena, sizeof(Type));
    t->kind = TY_TUPLE;
    t->nelems = n;
    t->elems = arenaAlloc(gArena, sizeof(Type *) * (size_t)n);
    StrBuf b = {0};
    sbPutc(&b, '(');
    for (int i = 0; i < n; i++) {
        t->elems[i] = elems[i];
        sbPrintf(&b, "%s%s", i ? ", " : "", elems[i]->name);
    }
    sbPutc(&b, ')');
    t->name = sbFinish(&b, gArena);
    TypeCache *tc = arenaAlloc(gArena, sizeof(TypeCache));
    tc->t = t;
    tc->next = gTypes;
    gTypes = tc;
    return t;
}

/* exact also tells Duration from i64: they are one type to the code, but a
   generic instance for each prints its values differently */
static bool sameTypeAs(Type *a, Type *b, bool exact) {
    if (a == b) return true;
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case TY_INT:
        if (exact && (a == &tyDuration) != (b == &tyDuration)) return false;
        return a->bits == b->bits && a->isSigned == b->isSigned;
    case TY_FLOAT: return a->bits == b->bits;
    case TY_PTR: case TY_SLICE: case TY_RESULT: case TY_CHAN: case TY_ATOMIC:
        return sameTypeAs(a->elem, b->elem, exact);
    case TY_ARRAY: return a->count == b->count && sameTypeAs(a->elem, b->elem, exact);
    case TY_STRUCT: case TY_ENUM: return a->decl == b->decl;
    case TY_TUPLE: case TY_FUNC:
        if (a->nelems != b->nelems) return false;
        if (a->kind == TY_FUNC && !sameTypeAs(a->elem, b->elem, exact)) return false;
        for (int i = 0; i < a->nelems; i++) {
            if (!sameTypeAs(a->elems[i], b->elems[i], exact)) return false;
        }
        return true;
    default: return true;
    }
}

static bool sameType(Type *a, Type *b) {
    return sameTypeAs(a, b, false);
}

static void resolveStruct(Checker *c, Type *t);

static Type *structType(Checker *c, Node *decl) {
    if (!decl->type) {
        Type *t = arenaAlloc(c->arena, sizeof(Type));
        t->kind = TY_STRUCT;
        t->name = decl->name;
        t->decl = decl;
        t->ir = arenaStrndup(c->arena, "", 0);
        StrBuf b = {0};
        sbPrintf(&b, "%%\"T.%s.%s\"", decl->module ? decl->module : "main", decl->name);
        t->ir = sbFinish(&b, c->arena);
        decl->type = t;
    }
    return decl->type;
}

static Type *resolveType(Checker *c, Node *t);
static bool evalConst(Checker *c, Node *e, int64_t *out, bool *isBool);

/* Sizes and alignments, by the same rules as C on 64-bit machines. They
 * decide how much room a data enum keeps for its values' data. */
static int64_t typeAlign(Type *t);

static int64_t typeSize(Type *t) {
    switch (t->kind) {
    case TY_BOOL: return 1;
    case TY_INT: case TY_FLOAT: return t->bits / 8;
    case TY_PTR: case TY_NULL: case TY_FUNC: case TY_CHAN: case TY_THREAD: return 8;
    case TY_ATOMIC: return typeSize(t->elem);
    case TY_MUTEX: return 24;
    case TY_SLICE: return 16;
    case TY_STRING: return 24;
    case TY_ERRVAL: case TY_ERRCODE: return 8;
    case TY_ARRAY: return t->count * typeSize(t->elem);
    case TY_ENUM:
        return t->hasData ? 8 + ((t->payload + 7) / 8) * 8 : 4;
    case TY_RESULT:
        return t->elem->kind == TY_VOID ? 8 : ((typeSize(t->elem) + 7) / 8) * 8 + 8;
    case TY_STRUCT: case TY_TUPLE: {
        int64_t off = 0, align = 1;
        int n = t->kind == TY_TUPLE ? t->nelems : t->decl->list.len;
        for (int i = 0; i < n; i++) {
            Type *ft = t->kind == TY_TUPLE ? t->elems[i] : t->decl->list.items[i]->type;
            if (!ft) continue;
            int64_t a = typeAlign(ft);
            off = (off + a - 1) / a * a + typeSize(ft);
            if (a > align) align = a;
        }
        return (off + align - 1) / align * align;
    }
    default: return 0;
    }
}

static int64_t typeAlign(Type *t) {
    switch (t->kind) {
    case TY_BOOL: return 1;
    case TY_ATOMIC: return typeAlign(t->elem);
    case TY_INT: case TY_FLOAT: return t->bits / 8;
    case TY_ARRAY: return typeAlign(t->elem);
    case TY_ENUM: return t->hasData ? 8 : 4;
    case TY_STRUCT: case TY_TUPLE: {
        int64_t align = 1;
        int n = t->kind == TY_TUPLE ? t->nelems : t->decl->list.len;
        for (int i = 0; i < n; i++) {
            Type *ft = t->kind == TY_TUPLE ? t->elems[i] : t->decl->list.items[i]->type;
            if (ft && typeAlign(ft) > align) align = typeAlign(ft);
        }
        return align;
    }
    default: return 8;
    }
}

int64_t typeSizeOf(Type *t) {
    return typeSize(t);
}

/* Where field `index` of struct t starts, by the same rules as typeSize. */
int64_t fieldOffset(Type *t, int index) {
    int64_t off = 0;
    for (int i = 0; i <= index; i++) {
        Type *ft = t->decl->list.items[i]->type;
        int64_t a = typeAlign(ft);
        off = (off + a - 1) / a * a;
        if (i == index) break;
        off += typeSize(ft);
    }
    return off;
}

/* An enum's type. Its values are worked out the first time. Each enum
 * gets a number, used to tell error codes from different enums apart.
 * A value with data carries fields, like a function's parameters; a flag
 * enum's values are powers of two. */
static Type *enumType(Checker *c, Node *decl) {
    if (decl->type) {
        return decl->type;
    }
    Type *t = arenaAlloc(c->arena, sizeof(Type));
    t->kind = TY_ENUM;
    t->name = decl->name;
    t->decl = decl;
    t->bits = 32;
    t->count = ++c->enumKinds;
    t->isFlags = (decl->flags & F_FLAGS) != 0;
    decl->type = t;
    for (int i = 0; i < decl->list.len; i++) {
        if (decl->list.items[i]->list.len) {
            t->hasData = true;
        }
    }
    if (t->hasData) {
        StrBuf b = {0};
        sbPrintf(&b, "%%\"E.%s.%s\"", decl->module ? decl->module : "main", decl->name);
        t->ir = sbFinish(&b, c->arena);
    }
    if (decl->attrs.len) {
        notYet(c, decl->attrs.items[0], "attributes", "a later version");
    }
    if (t->isFlags && t->hasData) {
        error(c, decl, "a flag enum's values can't carry data");
    }
    if (t->isFlags && decl->list.len > 31) {
        error(c, decl, "a flag enum can have at most 31 values");
    }
    Scope *saved = c->scope;
    c->scope = decl->scope;
    int64_t next = t->isFlags ? 1 : 0;
    for (int i = 0; i < decl->list.len; i++) {
        Node *m = decl->list.items[i];
        if (m->b) {
            int64_t v;
            bool b;
            if (t->hasData) {
                error(c, m->b, "the values of an enum with data can't be given numbers");
            } else if (!evalConst(c, m->b, &v, &b) || b) {
                error(c, m->b, "an enum value must be an integer known when compiling");
            } else if (v < INT32_MIN || v > INT32_MAX) {
                error(c, m->b, "an enum value must fit in i32");
            } else {
                next = v;
            }
        }
        m->cval = next;
        next = t->isFlags ? (next > 0 && next < (1 << 30) ? next * 2 : next + 1) : next + 1;
        m->isConst = !t->hasData;
        m->type = t;
        /* the data a value carries: a struct of its fields */
        int64_t size = 0;
        Type *fields = NULL;
        if (m->list.len) {
            fields = arenaAlloc(c->arena, sizeof(Type));
            fields->kind = TY_TUPLE;
            fields->nelems = m->list.len;
            fields->elems = arenaAlloc(c->arena, sizeof(Type *) * (size_t)m->list.len);
        }
        for (int j = 0; j < m->list.len; j++) {
            Node *f = m->list.items[j];
            Type *ft = resolveType(c, f->a);
            if (ft == &tyVoid) {
                error(c, f, "a field can't be void");
                ft = &tyError;
            }
            if (ft == t) {
                error(c, f, "%s can't contain itself; use a pointer (%s *)", decl->name, decl->name);
                ft = &tyError;
            }
            if (ft->kind == TY_STRUCT) {
                resolveStruct(c, ft);
            }
            f->type = ft;
            fields->elems[j] = ft;
            for (int k = 0; k < j; k++) {
                if (strcmp(m->list.items[k]->name, f->name) == 0) {
                    error(c, f, "%s.%s already has a field named %s", decl->name, m->name, f->name);
                }
            }
        }
        if (fields) {
            size = typeSize(fields);
            StrBuf b = {0};
            sbPrintf(&b, "%%\"E.%s.%s.%s\"", decl->module ? decl->module : "main", decl->name, m->name);
            fields->ir = sbFinish(&b, c->arena);
            fields->name = m->name;
            m->d = like(c, N_T_TUPLE, m);   /* holds the data's type */
            m->d->type = fields;
        }
        if (size > t->payload) {
            t->payload = size;
        }
        for (int j = 0; j < i; j++) {
            Node *o = decl->list.items[j];
            if (strcmp(o->name, m->name) == 0) {
                error(c, m, "%s already has a value named %s", decl->name, m->name);
            } else if (o->cval == m->cval) {
                error(c, m, "%s.%s and %s.%s have the same number, %lld", decl->name, o->name, decl->name,
                      m->name, (long long)m->cval);
            }
        }
    }
    c->scope = saved;
    return t;
}

/* The fields of a data enum value, or NULL for a value without data. */
static Type *memberData(Node *m) { return m->d ? m->d->type : NULL; }

static Node *enumMember(Type *t, const char *name) {
    for (int i = 0; i < t->decl->list.len; i++) {
        if (strcmp(t->decl->list.items[i]->name, name) == 0) {
            return t->decl->list.items[i];
        }
    }
    return NULL;
}

static Type *instantiateStruct(Checker *c, Node *decl, Type **args, int n, Node *at);
static void resolveSignatureType(Checker *c, Type *t);
static Type *intTypeNamed(const char *name);
static void checkEscape(Checker *c, Node *e);
static void checkConstDecl(Checker *c, Node *decl);
static bool hasDuration(Node *e);
static Type *dynType(Checker *c, Node *iface);

/* A type named in an expression, like the first argument of alloc. */
static Type *typeFromName(Checker *c, const char *name, Node *at, bool report) {
    for (size_t i = 0; i < sizeof builtinTypes / sizeof builtinTypes[0]; i++) {
        if (strcmp(builtinTypes[i]->name, name) == 0) {
            return builtinTypes[i];
        }
    }
    Node *d = lookup(c, name);
    if (d && d->kind == N_TYPE_PARAM) {
        return d->type;
    }
    if (d && d->kind == N_STRUCT && d->list2.len) {
        if (report) {
            error(c, at, "%s is generic; give its types: %s[...]", name, name);
            return &tyError;
        }
        return NULL;
    }
    if (d && d->kind == N_INTERFACE) {
        if (report) {
            error(c, at, "%s is an interface; use dyn %s, or a type parameter like [T: %s]", name, name, name);
            return &tyError;
        }
        return NULL;
    }
    if (d && d->kind == N_STRUCT) {
        return structType(c, d);
    }
    if (d && d->kind == N_ENUM) {
        return enumType(c, d);
    }
    if (d && d->kind == N_TYPEDEF) {
        if (d->state == 1) {
            error(c, d, "the type %s is defined in terms of itself", d->name);
            d->type = &tyError;
            d->state = 2;
        }
        if (d->state == 0) {
            d->state = 1;
            Scope *saved = c->scope;
            c->scope = d->scope;
            Type *t = resolveType(c, d->a);
            c->scope = saved;
            if (d->state == 1) {
                d->type = t;
                d->state = 2;
            }
        }
        return d->type;
    }
    if (!report) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof laterTypes / sizeof laterTypes[0]; i++) {
        if (strcmp(laterTypes[i].name, name) == 0) {
            error(c, at, "the type %s is not supported yet (planned for %s)", name, laterTypes[i].version);
            return &tyError;
        }
    }
    if (d) {
        error(c, at, "%s is not a type", name);
    } else {
        error(c, at, "unknown type %s", name);
    }
    return &tyError;
}

static Type *resolveType(Checker *c, Node *t) {
    if (!t) {
        return &tyError;
    }
    switch (t->kind) {
    case N_T_NAME:
        if (strchr(t->name, '.') && !lookup(c, t->name)) {
            /* mod.Type: say what is wrong with it */
            const char *dot = strchr(t->name, '.');
            char mod[128];
            size_t n = (size_t)(dot - t->name) < sizeof mod - 1 ? (size_t)(dot - t->name) : sizeof mod - 1;
            memcpy(mod, t->name, n);
            mod[n] = 0;
            Node *imp = lookupPlain(c, mod);
            if (!imp || imp->kind != N_IMPORT) {
                error(c, t, "%s is not an imported module", mod);
            } else if (findIn((Scope *)imp->scope, dot + 1)) {
                error(c, t, "%s isn't pub, so only its own module can use it", t->name);
            } else {
                error(c, t, "the module %s has no %s", mod, dot + 1);
            }
            return &tyError;
        }
        t->type = typeFromName(c, t->name, t, true);   /* kept for the language server */
        return t->type;
    case N_T_PTR: {
        Type *e = resolveType(c, t->a);
        if (isErr(e)) return e;
        if (e == &tyVoid) {
            notYet(c, t, "void pointers", "a later version");
            return &tyError;
        }
        return ptrTo(e);
    }
    case N_T_SLICE: {
        Type *e = resolveType(c, t->a);
        if (isErr(e)) return e;
        if (e == &tyVoid) {
            error(c, t, "a slice of void has no items");
            return &tyError;
        }
        return sliceOf(e);
    }
    case N_T_ARRAY: {
        Type *e = resolveType(c, t->a);
        if (isErr(e)) return e;
        if (e == &tyVoid) {
            error(c, t, "an array of void has no items");
            return &tyError;
        }
        if (t->b->ival == 0 || t->b->ival > (1u << 30)) {
            error(c, t->b, "an array's size must be between 1 and %u", 1u << 30);
            return &tyError;
        }
        return arrayOf(e, (int64_t)t->b->ival);
    }
    case N_T_ERR: {
        Type *e = resolveType(c, t->a);
        if (isErr(e)) return e;
        if (e->kind == TY_FUNC && t->a->kind != N_T_TUPLE) {
            /* !void(i64) is a function that returns !void */
            if (e->elem->kind == TY_RESULT) return e;
            return funcOf(resultOf(e->elem), e->elems, e->nelems);
        }
        if (e->kind == TY_RESULT) {
            error(c, t, "!!T is the same as !T; write one !");
            return e;
        }
        return resultOf(e);
    }
    case N_T_TUPLE: {
        Type *elems[32];
        if (t->list.len > 32) {
            error(c, t, "a tuple can have at most 32 values");
            return &tyError;
        }
        for (int i = 0; i < t->list.len; i++) {
            elems[i] = resolveType(c, t->list.items[i]);
            if (isErr(elems[i])) return elems[i];
            if (elems[i] == &tyVoid) {
                error(c, t->list.items[i], "a tuple can't hold void");
                return &tyError;
            }
            if (elems[i]->kind == TY_STRUCT) resolveSignatureType(c, elems[i]);
        }
        return tupleOf(elems, t->list.len);
    }
    case N_T_GENERIC: {
        Node *base = t->a;
        Node *d = base->kind == N_T_NAME ? lookup(c, base->name) : NULL;
        if (!d && base->kind == N_T_NAME && strcmp(base->name, "Atomic") == 0) {
            if (t->list.len != 1) {
                error(c, t, "Atomic takes one type: Atomic[i64]");
                return &tyError;
            }
            Type *e = resolveType(c, t->list.items[0]);
            if (isErr(e)) return e;
            if (e->kind != TY_INT && e->kind != TY_BOOL) {
                error(c, t->list.items[0], "Atomic holds an integer or a bool, not %s", e->name);
                return &tyError;
            }
            return compound(TY_ATOMIC, e, 0);
        }
        if ((!d || d->kind != N_STRUCT || !d->list2.len) && t->list.len == 1 && t->list.items[0]->kind == N_T_NAME) {
            /* i64[SLOTS]: an array whose size is a constant */
            Node *k = lookup(c, t->list.items[0]->name);
            if (k && k->kind == N_CONST) {
                checkConstDecl(c, k);
                Type *e = resolveType(c, base);
                if (isErr(e)) return e;
                if (!k->isConst || !k->type || !isInt(k->type)) {
                    error(c, t->list.items[0], "an array's size must be an integer constant");
                    return &tyError;
                }
                if (k->cval < 1 || k->cval > (1 << 30)) {
                    error(c, t->list.items[0], "an array's size must be between 1 and %u", 1u << 30);
                    return &tyError;
                }
                return arrayOf(e, k->cval);
            }
        }
        if (!d || d->kind != N_STRUCT || !d->list2.len) {
            if (d && d->kind == N_STRUCT) {
                error(c, t, "%s is not generic", base->name);
            } else {
                error(c, t, "%s is not a generic struct", base->kind == N_T_NAME ? base->name : "this");
            }
            return &tyError;
        }
        Type *args[16];
        if (t->list.len > 16) {
            error(c, t, "too many type arguments");
            return &tyError;
        }
        for (int i = 0; i < t->list.len; i++) {
            args[i] = resolveType(c, t->list.items[i]);
            if (isErr(args[i])) return args[i];
        }
        if (d->module && strcmp(d->module, "core") == 0 && strcmp(d->name, "Map") == 0 && t->list.len >= 1) {
            /* said here, where the map is declared, not inside Map's code */
            Type *k = args[0];
            bool ok = k->kind == TY_INT || k->kind == TY_BOOL || k->kind == TY_PTR || isStr(k) ||
                      (k->kind == TY_ENUM && !k->hasData);
            if (!ok) {
                error(c, t->list.items[0], "a Map's keys can be integers, bool, enums, pointers or str, not %s%s",
                      k->name, k->kind == TY_STRING ? " (use str: the map keeps its own copy)" : "");
                return &tyError;
            }
        }
        return instantiateStruct(c, d, args, t->list.len, t);
    }
    case N_T_CHAN: {
        Type *e = resolveType(c, t->a);
        if (isErr(e)) return e;
        if (e == &tyVoid) {
            error(c, t, "a channel of void carries nothing");
            return &tyError;
        }
        if (e->kind == TY_STRUCT) resolveSignatureType(c, e);
        return chanOf(e);
    }
    case N_T_DYN: {
        Node *in = t->a;
        Node *d = in->kind == N_T_NAME ? lookup(c, in->name) : NULL;
        if (!d || d->kind != N_INTERFACE) {
            error(c, t, "dyn needs an interface, not %s", in->kind == N_T_NAME ? in->name : "this");
            return &tyError;
        }
        return dynType(c, d);
    }
    case N_T_FUNC: {
        Type *ret = resolveType(c, t->a);
        if (isErr(ret)) return ret;
        Type *params[32];
        if (t->list.len > 32) {
            error(c, t, "too many parameters");
            return &tyError;
        }
        for (int i = 0; i < t->list.len; i++) {
            params[i] = resolveType(c, t->list.items[i]);
            if (isErr(params[i])) return params[i];
            if (params[i] == &tyVoid) {
                error(c, t->list.items[i], "a parameter can't be void");
                return &tyError;
            }
        }
        return funcOf(ret, params, t->list.len);
    }
    default:
        notYet(c, t, "this kind of type", "a later version");
        return &tyError;
    }
}

/* Every value of `from` fits in `to`. */
static bool widens(Type *from, Type *to) {
    if (isFloat(from) && isFloat(to)) {
        return from->bits <= to->bits;
    }
    if (!isInt(from) || !isInt(to)) {
        return false;
    }
    if (from->isSigned == to->isSigned) {
        return from->bits <= to->bits;
    }
    return !from->isSigned && to->isSigned && from->bits < to->bits;
}

static bool fitsIn(int64_t v, Type *t) {
    if (t->isSigned) {
        if (t->bits == 64) return true;
        int64_t lim = (int64_t)1 << (t->bits - 1);
        return v >= -lim && v < lim;
    }
    if (v < 0) return false;
    if (t->bits == 64) return true;
    return (uint64_t)v < ((uint64_t)1 << t->bits);
}

static bool conforms(Checker *c, Type *t, Node *iface, Node *at, bool report);

static Node *convertNode(Checker *c, Node *e, Type *to, bool explicit) {
    Node *n = like(c, N_CONVERT, e);
    n->a = e;
    n->type = to;
    if (explicit) {
        n->flags |= F_EXPLICIT;
    }
    return n;
}

/* The Mutex or Atomic a value holds, if any. */
static Type *syncInside(Type *t) {
    switch (t->kind) {
    case TY_MUTEX: case TY_ATOMIC:
        return t;
    case TY_ARRAY:
        return syncInside(t->elem);
    case TY_STRUCT:
        if (!t->decl) return NULL;
        for (int i = 0; i < t->decl->list.len; i++) {
            Type *ft = t->decl->list.items[i]->type;
            Type *in = ft ? syncInside(ft) : NULL;
            if (in) return in;
        }
        return NULL;
    default:
        return NULL;
    }
}

static bool addressable(Node *e);

/* Copying a Mutex or an Atomic makes a second one that guards nothing:
 * reading one out of a variable, a field or an element is an error. */
static bool noCopy(Checker *c, Node *e) {
    Type *in = e->type ? syncInside(e->type) : NULL;
    if (!in || !addressable(e)) {
        return true;
    }
    if (in == e->type) {
        error(c, e, "this %s can't be copied; use a pointer to it: &...", in->name);
    } else {
        error(c, e, "this %s holds a %s, so it can't be copied; use a pointer to it: &...", e->type->name, in->name);
    }
    return false;
}

/* Makes the expression in *slot fit `to`, adding a conversion where one
 * happens on its own. what describes the place for the error message.
 * Returns false after an error. */
static bool coerce(Checker *c, Node **slot, Type *to, const char *what) {
    Node *e = *slot;
    Type *from = e->type;
    if (!from || isErr(from) || isErr(to) || from->kind == TY_NORETURN) {
        return true;
    }
    if (!noCopy(c, e)) {
        return false;
    }
    if (sameType(from, to)) {
        return true;
    }
    if (e->isConst && isInt(from) && isInt(to)) {
        if (fitsIn(e->cval, to)) {
            e->type = to;
            return true;
        }
        error(c, e, "%lld doesn't fit in %s", (long long)e->cval, to->name);
        return false;
    }
    if (e->isConst && isInt(from) && isFloat(to)) {
        /* 2 where a float is needed: 2.0 */
        e->fval = from->isSigned ? (double)e->cval : (double)(uint64_t)e->cval;
        e->type = to;
        return true;
    }
    if (e->isConst && isFloat(from) && isFloat(to)) {
        e->type = to;
        return true;
    }
    if (widens(from, to)) {
        *slot = convertNode(c, e, to, false);
        return true;
    }
    if (from->kind == TY_NULL && (to->kind == TY_PTR || to->kind == TY_FUNC)) {
        e->type = to;
        return true;
    }
    if (from->kind == TY_FUNC && to->kind == TY_FUNC && from->elem == &tyVoid && to->elem->kind == TY_RESULT &&
        to->elem->elem == &tyVoid && from->nelems == to->nelems && e->kind == N_IDENT && e->ref &&
        e->ref->kind == N_FUNC) {
        /* a function that can't fail, where one that can is wanted: it never does */
        bool same = true;
        for (int i = 0; i < from->nelems; i++) same = same && sameType(from->elems[i], to->elems[i]);
        if (same) {
            *slot = convertNode(c, e, to, false);
            return true;
        }
    }
    if (from->kind == TY_PTR && to->kind == TY_PTR && (from->elem == &tyU8 || to->elem == &tyU8)) {
        /* u8 * is also C's void *: any pointer converts to and from it */
        *slot = convertNode(c, e, to, false);
        return true;
    }
    if (from->kind == TY_ARRAY && to->kind == TY_SLICE && sameType(from->elem, to->elem)) {
        *slot = convertNode(c, e, to, false);
        return true;
    }
    if (from->kind == TY_STRING && isStr(to)) {
        *slot = convertNode(c, e, to, false);
        return true;
    }
    if (from->kind == TY_ENUM && to->kind == TY_ERRCODE) {
        if (e->isConst) {
            e->cval = (int64_t)(((uint64_t)from->count << 32) | (uint32_t)e->cval);
            e->type = to;
            e->flags |= F_EXPLICIT;     /* already packed */
            return true;
        }
        *slot = convertNode(c, e, to, false);
        return true;
    }
    if (from->kind == TY_RESULT) {
        error(c, e, "the error from this call is not handled; use try, catch or _ =");
        return false;
    }
    if (to->kind == TY_DYN) {
        /* &file -> dyn Writer: a pointer to a struct with the interface's methods */
        if (from->kind == TY_STRUCT) {
            error(c, e, "dyn %s takes a pointer; write &", to->decl->name);
            return false;
        }
        if (from->kind != TY_PTR || from->elem->kind != TY_STRUCT) {
            error(c, e, "%s is %s, but dyn %s needs a pointer to a struct", what, from->name, to->decl->name);
            return false;
        }
        if (!conforms(c, from->elem, to->decl, e, true)) {
            return false;
        }
        *slot = convertNode(c, e, to, false);
        return true;
    }
    if (isNumber(from) && isNumber(to)) {
        error(c, e, "%s is %s, which doesn't always fit in %s; convert it with %s(...)", what,
              from->name, to->name, to->name);
        return false;
    }
    if (from->kind == TY_NULL) {
        error(c, e, "null can only be a pointer, not %s", to->name);
        return false;
    }
    error(c, e, "%s is %s, but %s is needed", what, from->name, to->name);
    return false;
}

/* ---------------------------------------------------------------- structs */

/* @json("key"), @json(required) or both, on a field of a @json struct. */
static void checkFieldAttr(Checker *c, Node *d, Node *f, Node *at) {
    if (strcmp(at->name, "env") == 0) {
        /* @env("NAME"), @env(required) or both; read again when the table is written */
        if (!(d->flags & F_ENV)) {
            error(c, at, "@env on a field needs @env on the struct %s too", d->name);
            return;
        }
        for (int i = 0; i < at->list.len; i++) {
            Node *a = at->list.items[i];
            bool ok = (a->kind == N_STRING && i == 0 && a->textLen > 0) ||
                      (a->kind == N_IDENT && strcmp(a->name, "required") == 0);
            if (!ok) {
                error(c, a, "@env on a field takes the variable's name in quotes, required, or both: @env(\"PORT\", required)");
            }
        }
        return;
    }
    if (strcmp(at->name, "flag") == 0) {
        /* @flag('p', "help", required) in any order; @flag(args) on a str[] */
        if (!(d->flags & F_CLI)) {
            error(c, at, "@flag on a field needs @flags on the struct %s too", d->name);
            return;
        }
        for (int i = 0; i < at->list.len; i++) {
            Node *a = at->list.items[i];
            bool ok = a->kind == N_STRING ||
                      (a->kind == N_CHAR && a->ival > ' ' && a->ival < 127 && a->ival != '-' && a->ival != 'h') ||
                      (a->kind == N_IDENT && (strcmp(a->name, "required") == 0 || strcmp(a->name, "args") == 0));
            if (!ok && a->kind == N_CHAR && a->ival == 'h') {
                error(c, a, "-h is taken: -h and --help show the help made from the struct, so pick another letter");
            } else if (!ok) {
                error(c, a, "@flag takes a letter for the short form ('p', but not 'h' or '-'), the help in quotes, required, or args: @flag('p', \"the port\")");
            }
        }
        return;
    }
    if (strcmp(at->name, "json") != 0) {
        notYet(c, at, "attributes", "a later version");
        return;
    }
    if (!(d->flags & F_JSON)) {
        error(c, at, "@json on a field needs @json on the struct %s too", d->name);
        return;
    }
    if (!at->list.len) {
        error(c, at, "@json on a field takes the JSON name in quotes, required, or both");
    }
    for (int i = 0; i < at->list.len; i++) {
        Node *a = at->list.items[i];
        if (a->kind == N_STRING && i == 0) {
            if (!a->textLen) {
                error(c, a, "a JSON name can't be empty");
            }
            f->text = a->text;
            f->textLen = a->textLen;
        } else if (a->kind == N_IDENT && strcmp(a->name, "required") == 0) {
            f->flags |= F_REQUIRED;
        } else {
            error(c, a, "@json on a field takes the JSON name in quotes, required, or both: @json(\"id\", required)");
        }
    }
}

bool isJsonStruct(Type *t) {
    return t->kind == TY_STRUCT && t->decl && (t->decl->flags & F_JSON);
}

/* A List[T] of std/core, which JSON reads and writes as an array. */
bool isJsonList(Type *t) {
    return t->kind == TY_STRUCT && t->generic && t->nargs == 1 && t->generic->module &&
           strcmp(t->generic->module, "core") == 0 && strcmp(t->generic->name, "List") == 0;
}

/* What JSON can hold: numbers, bool, text, plain enums, @json structs, and
 * slices, arrays and Lists of those. */
static bool jsonType(Checker *c, Type *t, Node *at, const char *owner) {
    switch (t->kind) {
    case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_STRING:
        return true;
    case TY_ENUM:
        if (t->hasData || t->isFlags) {
            error(c, at, "%s is an enum %s, which @json can't turn into JSON yet", t->name,
                  t->hasData ? "with data" : "of flags");
            return false;
        }
        return true;
    case TY_SLICE: case TY_ARRAY:
        return isStr(t) || jsonType(c, t->elem, at, owner);
    case TY_STRUCT:
        if (isJsonList(t)) return jsonType(c, t->args[0], at, owner);
        if (!t->decl || !(t->decl->flags & F_JSON)) {
            if (t->decl && !t->state) resolveStruct(c, t);
            if (t->decl && (t->decl->flags & F_JSON)) return true;
            error(c, at, "%s holds a %s, which needs @json too", owner, t->name);
            return false;
        }
        return true;
    default:
        if (!isErr(t)) {
            error(c, at, "%s can't be turned into JSON", t->name);
        }
        return false;
    }
}

static void checkJsonFields(Checker *c, Type *t, Node *d) {
    for (int i = 0; i < d->list.len; i++) {
        Node *f = d->list.items[i];
        if (!f->type || isErr(f->type)) continue;
        if (f->kind == N_EMBED) {
            if (!isJsonStruct(f->type)) {
                error(c, f, "%s embeds %s, which needs @json too", d->name, f->type->name);
            }
            continue;
        }
        jsonType(c, f->type, f, d->name);
    }
    (void)t;
}

/* The structs a field holds by value, in arrays and tuples too, must be
 * complete first: a struct that holds itself that way is an error. */
static void completeInside(Checker *c, Type *t) {
    while (t->kind == TY_ARRAY) {
        t = t->elem;
    }
    if (t->kind == TY_STRUCT) {
        resolveStruct(c, t);
    } else if (t->kind == TY_TUPLE) {
        for (int i = 0; i < t->nelems; i++) {
            completeInside(c, t->elems[i]);
        }
    }
}

/* Whether d is a variable or parameter of the function being checked. */
static bool isLocalDecl(Checker *c, Node *d) {
    for (Scope *s = c->scope; s; s = s->parent) {
        Sym *sym = findIn(s, d->name);
        if (sym && sym->decl == d) return true;
        if (s->function) break;
    }
    return false;
}

/* The variable a place belongs to: x for x, x.f, x[i] and x.f[i]. */
static Node *placeRoot(Node *e) {
    for (;;) {
        if (e->kind == N_FIELD_ACCESS || e->kind == N_INDEX) {
            if (e->a->type && e->a->type->kind == TY_PTR) return NULL; /* through a pointer: elsewhere */
            if (e->a->type && e->a->type->kind == TY_SLICE) return NULL;
            e = e->a;
        } else if (e->kind == N_IDENT) {
            return e->ref;
        } else {
            return NULL;
        }
    }
}

/* A returned pointer or slice into the function's own variables, which are
 * gone once it returns. */
static void checkEscape(Checker *c, Node *e) {
    Node *place = NULL;
    if (e->kind == N_CONVERT && e->a->type && e->a->type->kind == TY_ARRAY && e->type && e->type->kind == TY_SLICE) {
        place = e->a;               /* a local array given back as a slice */
    } else if (e->kind == N_UNARY && e->op == P_AMP) {
        place = e->a;
    } else if (e->kind == N_SLICE && e->a->type && e->a->type->kind == TY_ARRAY) {
        place = e->a;
    }
    if (!place) return;
    Node *root = placeRoot(place);
    if (root && (root->kind == N_VAR || root->kind == N_PARAM || root->kind == N_IDENT) && isLocalDecl(c, root)) {
        error(c, e, "this points into %s, which is gone when %s returns; return a copy, or memory from alloc",
              root->name, c->func->name);
    }
}

/* A struct in a function's signature. One whose fields are being resolved
 * now (List[Node] inside Node: List's methods take a Node) is left to
 * finish: the signature only needs it complete by the time code is written. */
static void resolveSignatureType(Checker *c, Type *t) {
    if (t->kind == TY_STRUCT && t->state == 1) {
        return;
    }
    resolveStruct(c, t);
}

static void resolveStruct(Checker *c, Type *t) {
    if (t->kind != TY_STRUCT || t->state == 2) {
        return;
    }
    Node *d = t->decl;
    if (t->state == 1) {
        error(c, d, "%s contains itself; use a pointer (%s *) for the inner one", d->name, d->name);
        t->state = 2;
        return;
    }
    t->state = 1;
    Scope *saved = c->scope;
    const char *savedModule = c->module;
    c->scope = t->scope ? (Scope *)t->scope : (Scope *)d->scope;
    c->module = d->module;
    if (d->flags & F_PACKED) {
        notYet(c, d, "packed structs", "a later version");
    }
    if (d->a) {
        notYet(c, d->a, "align", "a later version");
    }
    for (int i = 0; i < d->attrs.len; i++) {
        Node *at = d->attrs.items[i];
        if (strcmp(at->name, "json") == 0 && !at->list.len) {
            d->flags |= F_JSON;
        } else if (strcmp(at->name, "json") == 0) {
            error(c, at, "@json on a struct takes nothing; field names go on the fields: @json(\"name\")");
        } else if (strcmp(at->name, "flags") == 0) {
            d->flags |= F_CLI;
            if (at->list.len > 1 || (at->list.len == 1 && at->list.items[0]->kind != N_STRING)) {
                error(c, at, "@flags on a struct takes nothing, or what the program does in quotes: @flags(\"copies files\")");
            }
        } else if (strcmp(at->name, "env") == 0) {
            d->flags |= F_ENV;
            if (at->list.len > 1 || (at->list.len == 1 && at->list.items[0]->kind != N_STRING)) {
                error(c, at, "@env on a struct takes nothing, or a prefix for the names in quotes: @env(\"APP\")");
            }
        } else {
            notYet(c, at, "attributes", "a later version");
        }
    }
    for (int i = 0; i < d->list.len; i++) {
        Node *f = d->list.items[i];
        Type *ft = resolveType(c, f->a);
        if (ft == &tyVoid) {
            error(c, f, "a field can't be void");
            ft = &tyError;
        }
        /* a struct inside (not behind a pointer) must be complete first */
        completeInside(c, ft);
        f->type = ft;
        if (f->kind == N_EMBED) {
            if (ft->kind != TY_STRUCT && !isErr(ft)) {
                error(c, f, "only a struct can be embedded, not %s", ft->name);
                ft = &tyError;
                f->type = ft;
            }
            f->name = isErr(ft) ? "?" : ft->name;
        }
        if (f->c) {
            notYet(c, f->c, "bitfields", "a later version");
        }
        for (int k = 0; k < f->attrs.len; k++) {
            checkFieldAttr(c, d, f, f->attrs.items[k]);
        }
        for (int j = 0; j < i; j++) {
            Node *g = d->list.items[j];
            if (g->name && f->name && strcmp(g->name, f->name) == 0) {
                error(c, f, "%s already has a field named %s", d->name, f->name);
            }
        }
    }
    c->scope = saved;
    c->module = savedModule;
    t->state = 2;
    if (d->flags & F_JSON) {
        checkJsonFields(c, t, d);
    }
    if (d->flags & F_CLI) {
        bool haveArgs = false;
        for (int i = 0; i < d->list.len; i++) {
            Node *f = d->list.items[i];
            Type *ft = f->type;
            if (!ft || isErr(ft)) continue;
            bool args = false;
            for (int k = 0; k < f->attrs.len; k++) {
                Node *at = f->attrs.items[k];
                if (strcmp(at->name, "flag") != 0) continue;
                for (int j = 0; j < at->list.len; j++) {
                    Node *a = at->list.items[j];
                    if (a->kind == N_IDENT && strcmp(a->name, "args") == 0) args = true;
                }
            }
            if (args) {
                if (ft->kind != TY_SLICE || !isStr(ft->elem)) {
                    error(c, f, "@flag(args) gets the other arguments, so its field must be str[], not %s", ft->name);
                } else if (haveArgs) {
                    error(c, f, "only one field can have @flag(args)");
                }
                haveArgs = true;
                continue;
            }
            if (f->name && strcmp(f->name, "help") == 0) {
                error(c, f, "a field named help would be --help, which every @flags struct has already: it shows the help made from the struct; give the field another name");
                continue;
            }
            bool ok = ft->kind == TY_INT || ft->kind == TY_FLOAT || ft->kind == TY_BOOL || isStr(ft) ||
                      ft->kind == TY_STRING || (ft->kind == TY_ENUM && !ft->hasData);
            if (!ok || f->kind == N_EMBED) {
                error(c, f, "a @flags field can be a number, a Duration, bool, str, String or an enum, or str[] with @flag(args); not %s", ft->name);
            }
        }
    }
    if (d->flags & F_ENV) {
        for (int i = 0; i < d->list.len; i++) {
            Node *f = d->list.items[i];
            Type *ft = f->type;
            if (!ft || isErr(ft)) continue;
            bool ok = ft->kind == TY_INT || ft->kind == TY_FLOAT || ft->kind == TY_BOOL || isStr(ft) ||
                      ft->kind == TY_STRING || (ft->kind == TY_ENUM && !ft->hasData);
            if (!ok || f->kind == N_EMBED) {
                error(c, f, "an @env field can be a number, a Duration, bool, str, String or an enum, not %s", ft->name);
            }
        }
    }
}

/* Finds a field by name, also inside embedded structs (breadth first).
 * path gets the field nodes to go through, the last being the field. */
static int findField(Type *t, const char *name, Node **path, int max) {
    if (t->kind != TY_STRUCT || max < 1) {
        return 0;
    }
    NodeList *fields = &t->decl->list;
    for (int i = 0; i < fields->len; i++) {
        Node *f = fields->items[i];
        if (f->name && strcmp(f->name, name) == 0) {
            path[0] = f;
            return 1;
        }
    }
    for (int i = 0; i < fields->len; i++) {
        Node *f = fields->items[i];
        if (f->kind == N_EMBED && f->type && f->type->kind == TY_STRUCT) {
            int n = findField(f->type, name, path + 1, max - 1);
            if (n) {
                path[0] = f;
                return n + 1;
            }
        }
    }
    return 0;
}

static Checker *gChecker;           /* for methods checked when first used */
static Node *gUseSite;              /* the method call being checked, for its errors */
static Node *gBodySite;             /* the site of the generic body being checked */
static void queueBody(Checker *c, Node *fn, Scope *scope, Node *site);

/* A method of a generic struct's instance is checked, and compiled, only
 * once something uses it: List[T].contains needs T to have ==, and a List
 * of structs that never calls it still works. */
static Node *usedMethod(Node *m) {
    if (m && (m->flags & F_LAZY) && gChecker) {
        m->flags &= ~F_LAZY;
        Type *rt = m->list.len ? m->list.items[0]->type : NULL;
        Node *site = gUseSite ? gUseSite : rt && rt->kind == TY_PTR && rt->elem->decl ? rt->elem->decl : m;
        if (gBodySite && site && site->src && strncmp(site->src->path, "std/", 4) == 0) {
            site = gBodySite;   /* used inside the library: the program's own call is the one to show */
        }
        queueBody(gChecker, m, m->scope, site);
        listPush(gChecker->arena, gChecker->instOut, m);
    }
    return m;
}

static Node *findMethodIn(Type *t, const char *name) {
    for (int i = 0; i < t->methods.len; i++) {
        if (strcmp(t->methods.items[i]->name, name) == 0) {
            return usedMethod(t->methods.items[i]);
        }
    }
    return NULL;
}

/* A method, also of embedded structs. path gets the embedded fields to go
 * through to reach the receiver. */
static Node *findMethod(Type *t, const char *name, Node **path, int *n, int max) {
    Node *m = findMethodIn(t, name);
    if (m) {
        *n = 0;
        return m;
    }
    if (t->kind != TY_STRUCT || max < 1) {
        return NULL;
    }
    NodeList *fields = &t->decl->list;
    for (int i = 0; i < fields->len; i++) {
        Node *f = fields->items[i];
        if (f->kind == N_EMBED && f->type && f->type->kind == TY_STRUCT) {
            int k;
            m = findMethod(f->type, name, path + 1, &k, max - 1);
            if (m) {
                path[0] = f;
                *n = k + 1;
                return m;
            }
        }
    }
    return NULL;
}

static int fieldIndex(Node *structDecl, Node *field) {
    for (int i = 0; i < structDecl->list.len; i++) {
        if (structDecl->list.items[i] == field) {
            return i;
        }
    }
    return -1;
}

/* A field access node for one step of a path. */
static Node *fieldStep(Checker *c, Node *base, Node *field, Node *at) {
    Node *f = like(c, N_FIELD_ACCESS, at);
    f->a = base;
    f->name = field->name;
    f->ref = field;
    Type *bt = base->type->kind == TY_PTR ? base->type->elem : base->type;
    f->ival = (uint64_t)fieldIndex(bt->decl, field);
    f->type = field->type;
    return f;
}

/* ---------------------------------------------------------------- generics */

static void checkSignature(Checker *c, Node *f, Type *recv);

/* "List" and [i64, str] -> "List[i64, str]" */
static const char *instanceName(Checker *c, const char *name, Type **args, int n) {
    StrBuf b = {0};
    sbPrintf(&b, "%s[", name);
    for (int i = 0; i < n; i++) {
        sbPrintf(&b, "%s%s", i ? ", " : "", args[i]->name);
    }
    sbPutc(&b, ']');
    return sbFinish(&b, c->arena);
}

/* A scope where the type parameters stand for the given types. */
static Scope *paramScope(Checker *c, Node *generic, NodeList *params, Type **args) {
    Scope *s = arenaAlloc(c->arena, sizeof(Scope));
    s->parent = generic->scope;
    for (int i = 0; i < params->len; i++) {
        Node *tp = like(c, N_TYPE_PARAM, params->items[i]);
        tp->name = params->items[i]->name;
        tp->type = args[i];
        addSym(c, s, tp->name, tp);
    }
    return s;
}

static Instance *findInstance(Checker *c, Node *generic, Type **args, int n) {
    for (Instance *in = c->instances; in; in = in->next) {
        if (in->generic != generic || in->nargs != n) continue;
        bool same = true;
        for (int i = 0; i < n && same; i++) {
            same = sameTypeAs(in->args[i], args[i], true);
        }
        if (same) return in;
    }
    return NULL;
}

static Instance *newInstance(Checker *c, Node *generic, Type **args, int n) {
    Instance *in = arenaAlloc(c->arena, sizeof(Instance));
    in->generic = generic;
    in->nargs = n;
    in->args = arenaAlloc(c->arena, sizeof(Type *) * (size_t)(n ? n : 1));
    memcpy(in->args, args, sizeof(Type *) * (size_t)n);
    in->next = c->instances;
    c->instances = in;
    return in;
}

static void queueBody(Checker *c, Node *fn, Scope *scope, Node *site) {
    if (c->pendFns.len == c->pendCap) {
        int cap = c->pendCap ? c->pendCap * 2 : 16;
        Scope **ss = arenaAlloc(c->arena, sizeof(Scope *) * (size_t)cap);
        if (c->pendFns.len) {
            memcpy(ss, c->pendScopes, sizeof(Scope *) * (size_t)c->pendFns.len);
        }
        c->pendScopes = ss;
        c->pendCap = cap;
    }
    c->pendScopes[c->pendFns.len] = scope;
    fn->cval = c->instGen + 1;
    listPush(c->arena, &c->pendFns, fn);
    listPush(c->arena, &c->pendSites, site);
}

/* Whether t has every method the interface lists, with the same types.
 * Self in the interface stands for t. */
static bool conforms(Checker *c, Type *t, Node *iface, Node *at, bool report) {
    if (t->kind != TY_STRUCT) {
        if (report) {
            error(c, at, "%s has no methods, so it doesn't satisfy %s", t->name, iface->name);
        }
        return false;
    }
    Scope *s = arenaAlloc(c->arena, sizeof(Scope));
    s->parent = iface->scope;
    Node *self = like(c, N_TYPE_PARAM, iface);
    self->name = "Self";
    self->type = t;
    addSym(c, s, "Self", self);
    Scope *saved = c->scope;
    c->scope = s;
    bool ok = true;
    for (int i = 0; i < iface->list.len && ok; i++) {
        Node *want = iface->list.items[i];
        Node *m = findMethodIn(t, want->name);
        if (!m) {
            if (report) {
                error(c, at, "%s doesn't satisfy %s: it has no method %s", t->name, iface->name, want->name);
            }
            ok = false;
            break;
        }
        if (!m->type) {
            checkSignature(c, m, NULL);
        }
        bool same = m->list.len - 1 == want->list.len;
        int before = gErrors;
        Type *ret = resolveType(c, want->a);
        same = same && m->type && sameType(ret, m->type);
        for (int j = 0; same && j < want->list.len; j++) {
            Type *pt = resolveType(c, want->list.items[j]->a);
            same = sameType(pt, m->list.items[j + 1]->type);
        }
        if (gErrors != before) {
            ok = false;
        } else if (!same) {
            if (report) {
                error(c, at, "%s doesn't satisfy %s: its method %s has different types", t->name, iface->name,
                      want->name);
            }
            ok = false;
        }
    }
    c->scope = saved;
    return ok;
}

/* Checks the T: Interface bounds of type parameters. */
static bool checkBounds(Checker *c, NodeList *params, Type **args, Node *at) {
    bool ok = true;
    for (int i = 0; i < params->len; i++) {
        Node *tp = params->items[i];
        if (!tp->a) continue;
        Node *d = tp->a->kind == N_T_NAME ? lookup(c, tp->a->name) : NULL;
        if (!d || d->kind != N_INTERFACE) {
            error(c, tp->a, "a type parameter's bound must be an interface");
            ok = false;
            continue;
        }
        if (!conforms(c, args[i], d, at, true)) {
            ok = false;
        }
    }
    return ok;
}

static void instantiateMethods(Checker *c, Type *t);

/* List[i64]: a copy of the generic struct with T = i64. */
static Type *instantiateStruct(Checker *c, Node *decl, Type **args, int n, Node *at) {
    if (n != decl->list2.len) {
        error(c, at, "%s takes %d type%s, not %d", decl->name, decl->list2.len, decl->list2.len == 1 ? "" : "s", n);
        return &tyError;
    }
    Instance *in = findInstance(c, decl, args, n);
    if (in) {
        return in->type;
    }
    if (c->instDepth > 64 || c->instGen > 64) {
        error(c, at, "generic types are nested too deeply");
        return &tyError;
    }
    if (!checkBounds(c, &decl->list2, args, at)) {
        return &tyError;
    }
    in = newInstance(c, decl, args, n);
    Node *copy = cloneNode(c->arena, decl);
    copy->list2 = (NodeList){0};
    copy->name = instanceName(c, decl->name, args, n);
    Type *t = structType(c, copy);
    t->generic = decl;
    t->args = in->args;
    t->nargs = n;
    t->scope = paramScope(c, decl, &decl->list2, args);
    in->type = t;
    listPush(c->arena, c->instOut, copy);
    c->instDepth++;
    resolveStruct(c, t);
    instantiateMethods(c, t);
    c->instDepth--;
    return t;
}

/* The methods of a generic struct, for one instance of it. */
static void instantiateMethods(Checker *c, Type *t) {
    for (int i = 0; i < c->genericMethods.len; i++) {
        Node *g = c->genericMethods.items[i];
        const char *gm = g->module ? g->module : "main", *tm = t->generic->module ? t->generic->module : "main";
        if (strcmp(g->b->a->name, t->generic->name) != 0 || strcmp(gm, tm) != 0) {
            continue;               /* a struct of the same name in another module */
        }
        if (g->b->list.len != t->nargs) {
            continue;               /* reported where the method is declared */
        }
        Node *copy = cloneNode(c->arena, g);
        copy->b = NULL;
        copy->flags |= F_INSTANCE;
        Scope *s = paramScope(c, g, &g->b->list, t->args);
        Scope *saved = c->scope;
        const char *savedModule = c->module;
        c->scope = s;
        c->module = g->module;
        copy->scope = s;
        checkSignature(c, copy, t);
        c->scope = saved;
        c->module = savedModule;
        copy->flags |= F_LAZY;      /* the body waits until the method is used */
    }
}

/* max[i64]: a copy of the generic function with T = i64. */
static Node *instantiateFunc(Checker *c, Node *decl, Type **args, int n, Node *at) {
    Instance *in = findInstance(c, decl, args, n);
    if (in) {
        return in->fn;
    }
    if (c->instDepth > 64 || c->instGen > 64) {
        error(c, at, "generic functions call each other with ever new types");
        return NULL;
    }
    if (!checkBounds(c, &decl->list2, args, at)) {
        return NULL;
    }
    in = newInstance(c, decl, args, n);
    Node *copy = cloneNode(c->arena, decl);
    copy->list2 = (NodeList){0};
    copy->name = instanceName(c, decl->name, args, n);
    copy->flags |= F_INSTANCE;
    in->fn = copy;
    Scope *s = paramScope(c, decl, &decl->list2, args);
    Scope *saved = c->scope;
    const char *savedModule = c->module;
    c->scope = s;
    c->module = decl->module;
    copy->scope = s;
    c->instDepth++;
    checkSignature(c, copy, NULL);
    c->instDepth--;
    c->scope = saved;
    c->module = savedModule;
    queueBody(c, copy, s, at);
    listPush(c->arena, c->instOut, copy);
    return copy;
}

/* Works out type parameters by matching a parameter's type with an
 * argument's type. */
static void infer(Node *tn, Type *actual, NodeList *params, Type **out) {
    if (!tn || !actual || isErr(actual)) {
        return;
    }
    switch (tn->kind) {
    case N_T_NAME:
        for (int i = 0; i < params->len; i++) {
            if (strcmp(params->items[i]->name, tn->name) == 0 && !out[i] && actual->kind != TY_NULL) {
                out[i] = actual;
            }
        }
        return;
    case N_T_PTR:
        if (actual->kind == TY_PTR) infer(tn->a, actual->elem, params, out);
        return;
    case N_T_SLICE:
        if (actual->kind == TY_SLICE || actual->kind == TY_ARRAY) infer(tn->a, actual->elem, params, out);
        return;
    case N_T_ARRAY:
        if (actual->kind == TY_ARRAY) infer(tn->a, actual->elem, params, out);
        return;
    case N_T_GENERIC:
        if (actual->kind == TY_STRUCT && actual->generic && tn->a->kind == N_T_NAME &&
            strcmp(actual->generic->name, tn->a->name) == 0) {
            for (int i = 0; i < tn->list.len && i < actual->nargs; i++) {
                infer(tn->list.items[i], actual->args[i], params, out);
            }
        }
        return;
    default:
        return;
    }
}

/* The dyn type of an interface. Its methods' types are worked out once;
 * Self has no meaning behind dyn. */
static Type *dynType(Checker *c, Node *iface) {
    if (iface->type) {
        return iface->type;
    }
    Type *t = arenaAlloc(c->arena, sizeof(Type));
    t->kind = TY_DYN;
    t->decl = iface;
    StrBuf b = {0};
    sbPrintf(&b, "dyn %s", iface->name);
    t->name = sbFinish(&b, c->arena);
    iface->type = t;
    Scope *saved = c->scope;
    c->scope = iface->scope;
    for (int i = 0; i < iface->list.len; i++) {
        Node *m = iface->list.items[i];
        m->type = resolveType(c, m->a);
        for (int j = 0; j < m->list.len; j++) {
            m->list.items[j]->type = resolveType(c, m->list.items[j]->a);
        }
    }
    c->scope = saved;
    return t;
}

/* A type written as an expression: List[i64] in List[i64]{...}, the first
 * argument of alloc, the [i64] of max[i64](...). */
static Type *typeFromExpr(Checker *c, Node *e, bool report) {
    if (e->kind == N_IDENT) {
        return typeFromName(c, e->name, e, report);
    }
    if (e->kind == N_INDEX && e->a->kind == N_IDENT) {
        Node *d = lookup(c, e->a->name);
        if (d && d->kind == N_STRUCT && d->list2.len) {
            Type *args[16];
            if (e->list.len > 16) return &tyError;
            for (int i = 0; i < e->list.len; i++) {
                args[i] = typeFromExpr(c, e->list.items[i], true);
                if (!args[i] || isErr(args[i])) return &tyError;
            }
            return instantiateStruct(c, d, args, e->list.len, e);
        }
    }
    if (report) {
        error(c, e, "this is not a type");
        return &tyError;
    }
    return NULL;
}

/* ---------------------------------------------------------------- constants */

static Type *checkExpr(Checker *c, Node **slot, Type *expected);
static bool checkStarted(Checker *c, Node **callp, const char *word, const char *what);
static void checkArgs(Checker *c, Node *e, Node *fn, NodeList *given, int first, NodeList *out, bool checked);
static Node *whenBranch(Checker *c, Node *w);
static void checkTempResult(Checker *c, Node *e);
static void checkConstDecl(Checker *c, Node *decl);

static void constError(Checker *c, Node *e, const char *msg) {
    if (!c->quiet) {
        error(c, e, "%s", msg);
    }
}

/* Works out an integer or bool value known at compile time. Reports
 * overflow and division by zero; returns false if the value isn't known. */
static bool evalConst(Checker *c, Node *e, int64_t *out, bool *isBool) {
    *isBool = false;
    switch (e->kind) {
    case N_INT: {
        if (e->ival > (uint64_t)INT64_MAX) {
            return false;
        }
        int64_t v = (int64_t)e->ival;
        if (e->name) {
            /* units: durations in nanoseconds, sizes in bytes */
            static const struct { const char *unit; int64_t times; } units[] = {
                {"ns", 1}, {"us", 1000}, {"ms", 1000000}, {"s", 1000000000LL}, {"m", 60000000000LL},
                {"h", 3600000000000LL}, {"KiB", 1024}, {"MiB", 1024 * 1024}, {"GiB", 1024LL * 1024 * 1024},
            };
            for (size_t i = 0; i < sizeof units / sizeof units[0]; i++) {
                if (strcmp(units[i].unit, e->name) == 0) {
                    long long r;
                    if (__builtin_mul_overflow(v, units[i].times, &r)) {
                        constError(c, e, "this constant is too large");
                        return false;
                    }
                    v = r;
                }
            }
        }
        *out = v;
        return true;
    }
    case N_CHAR:
        *out = (int64_t)e->ival;
        return true;
    case N_BOOL:
        *out = (int64_t)e->ival;
        *isBool = true;
        return true;
    case N_IDENT: {
        Node *d = lookup(c, e->name);
        if (!d || d->kind != N_CONST) {
            return false;
        }
        checkConstDecl(c, d);
        if (!d->isConst || (d->type && isFloat(d->type))) {
            return false;
        }
        *out = d->cval;
        *isBool = d->type == &tyBool;
        return true;
    }
    case N_UNARY: {
        int64_t v;
        bool b;
        if (e->op != P_MINUS && e->op != P_BANG && e->op != P_TILDE) {
            return false;
        }
        if (!evalConst(c, e->a, &v, &b)) {
            return false;
        }
        switch (e->op) {
        case P_MINUS:
            if (b) return false;
            if (v == INT64_MIN) {
                constError(c, e, "this constant is too large");
                return false;
            }
            *out = -v;
            return true;
        case P_BANG:
            if (!b) return false;
            *out = !v;
            *isBool = true;
            return true;
        default:
            if (b) return false;
            *out = ~v;
            return true;
        }
    }
    case N_BINARY: {
        int64_t x, y;
        bool bx, by;
        if (!evalConst(c, e->a, &x, &bx) || !evalConst(c, e->b, &y, &by)) {
            return false;
        }
        if (bx != by) {
            return false;
        }
        if (bx) {
            switch (e->op) {
            case P_ANDAND: *out = x && y; break;
            case P_OROR: *out = x || y; break;
            case P_EQ: *out = x == y; break;
            case P_NE: *out = x != y; break;
            default: return false;
            }
            *isBool = true;
            return true;
        }
        long long r;
        switch (e->op) {
        case P_PLUS:
            if (__builtin_add_overflow(x, y, &r)) goto overflow;
            *out = r;
            return true;
        case P_MINUS:
            if (__builtin_sub_overflow(x, y, &r)) goto overflow;
            *out = r;
            return true;
        case P_STAR:
            if (__builtin_mul_overflow(x, y, &r)) goto overflow;
            *out = r;
            return true;
        case P_SLASH:
        case P_PERCENT:
            if (y == 0) {
                constError(c, e, "division by zero in a constant");
                return false;
            }
            if (x == INT64_MIN && y == -1) goto overflow;
            *out = e->op == P_SLASH ? x / y : x % y;
            return true;
        case P_AMP: *out = x & y; return true;
        case P_PIPE: *out = x | y; return true;
        case P_CARET: *out = x ^ y; return true;
        case P_SHL:
            if (y < 0 || y > 62 || (x != 0 && (x > (INT64_MAX >> y) || x < (INT64_MIN >> y)))) goto overflow;
            *out = (int64_t)((uint64_t)x << y);
            return true;
        case P_SHR:
            if (y < 0 || y > 63) goto overflow;
            *out = x >> y;
            return true;
        case P_EQ: case P_NE: case P_LT: case P_LE: case P_GT: case P_GE:
            *isBool = true;
            *out = e->op == P_EQ ? x == y : e->op == P_NE ? x != y : e->op == P_LT ? x < y
                 : e->op == P_LE ? x <= y : e->op == P_GT ? x > y : x >= y;
            return true;
        default:
            return false;
        }
    overflow:
        constError(c, e, "this constant is too large");
        return false;
    }
    default:
        return false;
    }
}

/* A constant: its value is worked out once, in dependency order. */
/* const X = comptime f(): the helper program gives the value. While the
 * helper itself is checked, X is a global with no value yet. True when d
 * became a read-only global (an array or a struct) and is done. */
static void checkGlobalValue(Checker *c, Node **slot, Type *t);

static bool checkComptime(Checker *c, Node *d) {
    Node *ct = d->b;
    Type *t = checkExpr(c, &ct->a, NULL);
    if (isErr(t)) {
        d->type = &tyError;
        d->state = 2;
        return true;
    }
    if (t->kind == TY_STRING) {
        t = &tyStr;     /* a String's text becomes a str constant */
    }
    bool scalar = t->kind == TY_INT || t->kind == TY_FLOAT || t->kind == TY_BOOL || isStr(t) ||
                  (t->kind == TY_ENUM && !t->hasData);
    bool aggregate = t->kind == TY_ARRAY || t->kind == TY_STRUCT;
    if (!scalar && !aggregate) {
        error(c, ct, "comptime can give numbers, bool, text, enums, and arrays and structs of those, not %s", t->name);
        d->type = &tyError;
        d->state = 2;
        return true;
    }
    if (t->kind == TY_STRUCT && !isJsonStruct(t)) {
        error(c, ct, "a struct that comptime gives needs @json, which carries it from the helper: %s", t->name);
        d->type = &tyError;
        d->state = 2;
        return true;
    }
    if (gComptime == 1 && scalar) {
        /* the helper: the value isn't known yet, so a zero stands in */
        const char *zero = t->kind == TY_BOOL ? "false" : isStr(t) ? "\"\"" : "0";
        char name[300];
        if (t->kind == TY_ENUM) {
            snprintf(name, sizeof name, "\"%s\"", t->decl->list.len ? t->decl->list.items[0]->name : "");
            zero = name;
        }
        d->b = comptimeLiteral(c->arena, zero, t, ct);
        d->type = t;
        d->flags |= F_COMPTIMED;
        return false;
    }
    if (gComptime == 1) {
        /* the helper: the value isn't known yet */
        d->kind = N_VAR;
        d->type = t;
        d->b = NULL;
        d->flags |= F_READONLY;
        d->state = 2;
        return true;
    }
    const char *json = gComptime == 2 ? comptimeResult(comptimeKey(c->arena, ct)) : NULL;
    if (!json) {
        error(c, ct, "comptime only works in a constant at the top of the program's own files");
        d->type = &tyError;
        d->state = 2;
        return true;
    }
    Node *v = comptimeLiteral(c->arena, json, t, ct);
    if (!v) {
        error(c, ct, "comptime's value can't be made a constant of type %s (a NaN, a slice or a pointer?)", t->name);
        d->type = &tyError;
        d->state = 2;
        return true;
    }
    if (scalar) {
        d->b = v;
        d->type = t;
        d->flags |= F_COMPTIMED;
        return false;   /* checked as the constant it now is */
    }
    d->kind = N_VAR;
    d->type = t;
    d->b = v;
    d->flags |= F_READONLY;
    checkGlobalValue(c, &d->b, t);
    d->state = 2;
    return true;
}

/* A float as the program writes it into text: the fewest digits that read
 * back as the same value, as iris_write_float does. */
static void constFloatText(StrBuf *out, double v, int bits) {
    if (v != v) {
        sbPuts(out, "nan");
        return;
    }
    if (v == 1.0 / 0.0 || v == -1.0 / 0.0) {
        sbPuts(out, v > 0 ? "inf" : "-inf");
        return;
    }
    if (v == 0) {
        sbPuts(out, signbit(v) ? "-0" : "0");
        return;
    }
    char buf[64];
    for (int prec = 0; prec < 17; prec++) {
        snprintf(buf, sizeof buf, "%.*e", prec, v);
        double back = strtod(buf, NULL);
        if (bits == 32 ? (float)back == (float)v : back == v) {
            break;
        }
    }
    char *ep = strchr(buf, 'e');
    int exp10 = atoi(ep + 1);
    if (exp10 < -6 || exp10 >= 21) {
        sbPutn(out, buf, (size_t)(ep - buf));
        sbPrintf(out, "e%c%d", exp10 < 0 ? '-' : '+', exp10 < 0 ? -exp10 : exp10);
        return;
    }
    char digits[32];
    int nd = 0;
    bool neg = buf[0] == '-';
    for (char *q = buf + (neg ? 1 : 0); q < ep; q++) {
        if (*q != '.') digits[nd++] = *q;
    }
    if (neg) sbPutc(out, '-');
    if (exp10 < 0) {
        sbPuts(out, "0.");
        for (int i = 0; i < -exp10 - 1; i++) sbPutc(out, '0');
        sbPutn(out, digits, (size_t)nd);
    } else {
        for (int i = 0; i <= exp10; i++) sbPutc(out, i < nd ? digits[i] : '0');
        if (nd > exp10 + 1) {
            sbPutc(out, '.');
            sbPutn(out, digits + exp10 + 1, (size_t)(nd - exp10 - 1));
        }
    }
}

/* A constant made of text with {} values, bare or given to format or
 * str.format: the text is put together now, from the constants in the
 * braces, and the constant becomes a str. */
static void foldConstText(Checker *c, Node *d) {
    Node *v = d->b;
    if (v->kind == N_CALL && v->list.len == 1 && v->list.items[0]->kind != N_NAMED_ARG) {
        Node *f = v->a;
        Node *strFormat = f->kind == N_IDENT && strcmp(f->name, "str.format") == 0 ? lookup(c, f->name) : NULL;
        if ((f->kind == N_IDENT && strcmp(f->name, "format") == 0) ||
            (strFormat && strFormat->module && strcmp(strFormat->module, "str") == 0)) {
            v = v->list.items[0];
            if (v->kind == N_STRING) {
                d->b = v;
                return;
            }
        }
    }
    if (v->kind == N_IDENT) {
        /* another str constant, by name */
        Node *k = lookup(c, v->name);
        if (k && k->kind == N_CONST) {
            checkConstDecl(c, k);
            if (k->type && isStr(k->type) && k->b && k->b->kind == N_STRING) {
                v->ref = k;
                d->b = k->b;
            }
        }
        return;
    }
    if (v->kind != N_INTERP) {
        return;
    }
    StrBuf out = {0};
    bool ok = true;
    for (int i = 0; i < v->list.len; i++) {
        Node **part = &v->list.items[i];
        if ((*part)->kind == N_STRING) {
            sbPutn(&out, (*part)->text, (*part)->textLen);
            continue;
        }
        Type *t = checkExpr(c, part, NULL);
        if (isErr(t)) {
            ok = false;
            continue;
        }
        Node *p = *part;
        int64_t n;
        bool b;
        if (isStr(t) && p->kind == N_STRING) {
            sbPutn(&out, p->text, p->textLen);
        } else if (isStr(t) && p->kind == N_IDENT && p->ref && p->ref->kind == N_CONST && p->ref->b &&
                   p->ref->b->kind == N_STRING) {
            sbPutn(&out, p->ref->b->text, p->ref->b->textLen);
        } else if (isFloat(t) && p->isConst) {
            constFloatText(&out, p->fval, t->bits);
        } else if ((isInt(t) || t == &tyBool) && t != &tyDuration && evalConst(c, p, &n, &b)) {
            if (b) {
                sbPuts(&out, n ? "true" : "false");
            } else {
                sbPrintf(&out, "%lld", (long long)n);
            }
        } else if (t == &tyDuration || t->kind == TY_ENUM || t->kind == TY_ERRCODE || t->kind == TY_STRUCT) {
            error(c, p, "a constant's text can have numbers, bool and text in its braces, not %s", t->name);
            ok = false;
        } else {
            error(c, p, "the values in a constant's text must be known when compiling");
            ok = false;
        }
    }
    if (!ok) {
        sbFree(&out);
        d->type = &tyError;
        return;
    }
    Node *s = like(c, N_STRING, v);
    s->textLen = out.len;
    if (out.len) {
        s->text = sbFinish(&out, c->arena);
    } else {
        sbFree(&out);
        s->text = "";
    }
    d->b = s;
}

static void checkConstDecl(Checker *c, Node *d) {
    if (d->state == 2) {
        return;
    }
    if (d->state == 1) {
        error(c, d, "the constant %s depends on itself", d->name);
        d->type = &tyError;
        d->state = 2;
        return;
    }
    d->state = 1;
    Scope *saved = c->scope;
    const char *savedModule = c->module;
    c->scope = d->scope;
    c->module = d->module;
    if (d->b && d->b->kind == N_COMPTIME) {
        bool done = checkComptime(c, d);
        c->scope = saved;
        c->module = savedModule;
        if (done) {
            return;
        }
        d->state = 1;
        c->scope = d->scope;
        c->module = d->module;
    }
    foldConstText(c, d);
    if (d->type == &tyError) {
        c->scope = saved;
        c->module = savedModule;
        d->state = 2;
        return;
    }
    Type *declared = d->a ? resolveType(c, d->a) : (d->flags & F_COMPTIMED) ? d->type : NULL;
    int64_t v;
    bool b;
    int before = gErrors;
    bool negFloat = d->b->kind == N_UNARY && d->b->op == P_MINUS && d->b->a->kind == N_FLOAT;
    if (d->b->kind == N_FLOAT || negFloat) {
        Type *ft = declared && isFloat(declared) ? declared : &tyF64;
        if (declared && !isErr(declared) && !isFloat(declared)) {
            error(c, d->b, "the value is a floating point number, but %s is needed", declared->name);
        }
        checkExpr(c, &d->b, ft);
        d->isConst = true;
        d->fval = d->b->fval;
        d->type = ft;
    } else if (d->b->kind == N_STRING) {
        checkExpr(c, &d->b, NULL);
        d->type = &tyStr;
        if (declared && !isStr(declared) && !isErr(declared)) {
            error(c, d->b, "the value is str, but %s is needed", declared->name);
        }
    } else if (evalConst(c, d->b, &v, &b)) {
        d->isConst = true;
        d->cval = v;
        if (b) {
            d->type = &tyBool;
        } else if (declared && isInt(declared)) {
            if (!fitsIn(v, declared)) {
                error(c, d->b, "%lld doesn't fit in %s", (long long)v, declared->name);
            }
            d->type = declared;
            d->flags |= F_EXPLICIT;     /* typed: doesn't adapt */
        } else {
            d->type = &tyI64;           /* untyped: adapts where it's used */
        }
        if (declared && !isErr(declared) && !isInt(declared) && declared != d->type) {
            error(c, d->b, "the value is %s, but %s is needed", d->type->name, declared->name);
        }
    } else if (gErrors == before) {
        checkExpr(c, &d->b, NULL);
        if (!isErr(d->b->type)) {
            error(c, d->b, "a constant's value must be known when compiling");
        }
        d->type = &tyError;
    } else {
        d->type = &tyError;
    }
    c->scope = saved;
    c->module = savedModule;
    d->state = 2;
}

/* ---------------------------------------------------------------- expressions */

static Type *checkBlockValue(Checker *c, Node *block, Type *expected);
static void checkBlock(Checker *c, Node *block, bool newScope);
static Type *checkMatch(Checker *c, Node *m, Type *expected, bool wantValue);
static Type *checkIf(Checker *c, Node *n, Type *expected, bool wantValue);
static Type *checkCall(Checker *c, Node *e, Type *expected);

static Type *setType(Node *e, Type *t) {
    e->type = t;
    return t;
}

/* Literals and constants take the type the context expects, when the
 * value fits. */
static bool adaptable(Checker *c, Node *e) {
    int64_t v;
    bool b;
    if (e->kind == N_IDENT) {
        Node *d = lookup(c, e->name);
        if (d && d->kind == N_CONST && (d->flags & F_EXPLICIT)) {
            return false;
        }
    }
    if (e->kind == N_BINARY && (e->op == P_EQ || e->op == P_NE || e->op == P_LT || e->op == P_LE ||
                                e->op == P_GT || e->op == P_GE || e->op == P_ANDAND || e->op == P_OROR)) {
        return false;
    }
    bool saved = c->quiet;
    c->quiet = true;            /* errors are reported when it is checked for real */
    bool ok = e->kind != N_STRING && evalConst(c, e, &v, &b) && !b;
    c->quiet = saved;
    return ok;
}

/* Whether an expression names a place in memory: a variable, a field of
 * one, an element, or what a pointer points at. */
static bool addressable(Node *e) {
    switch (e->kind) {
    case N_IDENT:
        return e->ref && (e->ref->kind == N_VAR || e->ref->kind == N_PARAM || e->ref->kind == N_IDENT);
    case N_FIELD_ACCESS:
        /* a real field: .len and .name are values, not places */
        return e->ref && e->ref->kind != N_ENUM_MEMBER && e->a->type &&
               (e->a->type->kind == TY_PTR || addressable(e->a));
    case N_INDEX:
        return e->a->type && (e->a->type->kind == TY_SLICE || addressable(e->a));
    case N_UNARY:
        return e->op == P_STAR;
    default:
        return false;
    }
}

static bool isComparable(Type *t) {
    switch (t->kind) {
    case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_PTR: case TY_NULL: case TY_ERRCODE: case TY_FUNC:
        return true;
    case TY_ENUM:
        return !t->hasData;
    case TY_SLICE:
        return isStr(t);
    case TY_STRUCT:
        for (int i = 0; i < t->decl->list.len; i++) {
            if (!isComparable(t->decl->list.items[i]->type)) {
                return false;
            }
        }
        return true;
    default:
        return false;
    }
}

static Type *checkBinary(Checker *c, Node *e, Type *expected) {
    TokKind op = e->op;
    /* the operator as it was written: a is b is a == b */
    const char *opName = e->ival == 1 ? "is" : e->ival == 2 ? "is not" : tokName(op);
    if (op == P_DOTDOT) {
        error(c, e, "a range can only be used in 'for', 'match' and slicing");
        return setType(e, &tyError);
    }
    if (op == P_LARROW) {
        /* ch <- value: a statement */
        Type *tc = checkExpr(c, &e->a, NULL);
        if (isErr(tc)) {
            checkExpr(c, &e->b, NULL);
            return setType(e, &tyVoid);
        }
        if (tc->kind != TY_CHAN) {
            error(c, e->a, "<- sends to a channel, not to %s", tc->name);
            checkExpr(c, &e->b, NULL);
            return setType(e, &tyVoid);
        }
        checkExpr(c, &e->b, tc->elem);
        coerce(c, &e->b, tc->elem, "the value sent");
        return setType(e, &tyVoid);
    }
    if (op == P_ANDAND || op == P_OROR) {
        checkExpr(c, &e->a, &tyBool);
        checkExpr(c, &e->b, &tyBool);
        coerce(c, &e->a, &tyBool, "the left side");
        coerce(c, &e->b, &tyBool, "the right side");
        return setType(e, &tyBool);
    }
    bool compare = op == P_EQ || op == P_NE || op == P_LT || op == P_LE || op == P_GT || op == P_GE;
    bool shift = op == P_SHL || op == P_SHR;
    bool bitOp = op == P_AMP || op == P_PIPE || op == P_CARET;
    Type *want = compare ? NULL : expected;
    if (want && !isNumber(want) && !(bitOp && want->kind == TY_ENUM && want->isFlags)) {
        want = NULL;
    }

    Type *ta, *tb;
    if (shift) {
        ta = checkExpr(c, &e->a, want);
        tb = checkExpr(c, &e->b, NULL);
        if (!isErr(ta) && !isInt(ta)) {
            error(c, e->a, "'%s' needs an integer, not %s", opName, ta->name);
            return setType(e, &tyError);
        }
        if (!isErr(tb) && !isInt(tb)) {
            error(c, e->b, "a shift amount must be an integer, not %s", tb->name);
        }
        return setType(e, ta);
    }
    if (adaptable(c, e->a) && !adaptable(c, e->b)) {
        tb = checkExpr(c, &e->b, want);
        ta = checkExpr(c, &e->a, isInt(tb) ? tb : want);
    } else {
        ta = checkExpr(c, &e->a, want);
        tb = checkExpr(c, &e->b, isNumber(ta) || ta->kind == TY_ENUM ? ta : want);
    }
    if (isErr(ta) || isErr(tb)) {
        return setType(e, compare ? &tyBool : &tyError);
    }

    /* pointer arithmetic: p + n, p - n */
    if ((op == P_PLUS || op == P_MINUS) && ta->kind == TY_PTR && isInt(tb)) {
        return setType(e, ta);
    }

    /* an enum value compares with an error code */
    if (ta->kind == TY_ERRCODE && tb->kind == TY_ENUM) {
        coerce(c, &e->b, &tyErrCode, "the right side");
        tb = &tyErrCode;
    } else if (tb->kind == TY_ERRCODE && ta->kind == TY_ENUM) {
        coerce(c, &e->a, &tyErrCode, "the left side");
        ta = &tyErrCode;
    }

    /* a String compares as its text */
    if (ta->kind == TY_STRING) {
        coerce(c, &e->a, &tyStr, "the left side");
        ta = &tyStr;
    }
    if (tb->kind == TY_STRING) {
        coerce(c, &e->b, &tyStr, "the right side");
        tb = &tyStr;
    }

    /* bring both sides to one type */
    Type *t = ta;
    if (!sameType(ta, tb)) {
        if (e->a->isConst && isNumber(ta) && isFloat(tb) && (isInt(ta) || isFloat(ta))) {
            coerce(c, &e->a, tb, "the left side");
            t = tb;
        } else if (e->b->isConst && isFloat(ta) && isNumber(tb)) {
            coerce(c, &e->b, ta, "the right side");
        } else if (e->a->isConst && isInt(ta) && isInt(tb)) {
            coerce(c, &e->a, tb, "the left side");
            t = tb;
        } else if (e->b->isConst && isInt(ta) && isInt(tb)) {
            coerce(c, &e->b, ta, "the right side");
        } else if (widens(ta, tb)) {
            coerce(c, &e->a, tb, "the left side");
            t = tb;
        } else if (widens(tb, ta)) {
            coerce(c, &e->b, ta, "the right side");
        } else if (ta->kind == TY_NULL && (tb->kind == TY_PTR || tb->kind == TY_FUNC)) {
            e->a->type = tb;
            t = tb;
        } else if (tb->kind == TY_NULL && (ta->kind == TY_PTR || ta->kind == TY_FUNC)) {
            e->b->type = ta;
        } else {
            if (ta->kind == TY_ATOMIC || tb->kind == TY_ATOMIC) {
                error(c, e, "an Atomic's value is read with .load()");
            } else if (isNumber(ta) && isNumber(tb)) {
                error(c, e, "'%s' can't mix %s and %s; convert one side, for example %s(...)", opName,
                      ta->name, tb->name, isFloat(tb) && !isFloat(ta) ? tb->name : ta->name);
            } else {
                error(c, e, "'%s' can't mix %s and %s", opName, ta->name, tb->name);
            }
            return setType(e, compare ? &tyBool : &tyError);
        }
    }

    if (compare) {
        if (t->kind == TY_NULL) {
            return setType(e, &tyBool);
        }
        if (op == P_EQ || op == P_NE) {
            if (!isComparable(t)) {
                error(c, e, "'%s' can't compare %s values", opName, t->name);
            }
        } else if (!isNumber(t)) {
            error(c, e, "'%s' can't compare %s values", opName, t->name);
        }
        return setType(e, &tyBool);
    }
    if (bitOp && t->kind == TY_ENUM && t->isFlags) {
        return setType(e, t);
    }
    if (isFloat(t) && (op == P_PLUS || op == P_MINUS || op == P_STAR || op == P_SLASH || op == P_PERCENT)) {
        return setType(e, t);
    }
    if (!isInt(t)) {
        error(c, e, "'%s' needs integers, not %s", opName, t->name);
        return setType(e, &tyError);
    }
    if (op == P_SLASH && ta == &tyDuration && tb == &tyDuration && (!e->b->isConst || hasDuration(e->b))) {
        return setType(e, &tyI64);      /* timeout / 1s: how many seconds */
    }
    return setType(e, t);
}

/* The struct type behind a value or a pointer to one. */
static Type *structOf(Type *t) {
    if (t->kind == TY_PTR) {
        t = t->elem;
    }
    return t->kind == TY_STRUCT ? t : NULL;
}

static Type *checkFieldAccess(Checker *c, Node **slot, Type *expected) {
    Node *e = *slot;
    if (e->a->kind == N_IDENT && !lookup(c, e->a->name)) {
        /* i64.max, u8.min: the limits of an integer type */
        Type *it = intTypeNamed(e->a->name);
        if (it && isInt(it) && (strcmp(e->name, "min") == 0 || strcmp(e->name, "max") == 0)) {
            int bits = it->bits;
            bool max = e->name[1] == 'a';
            int64_t v;
            if (it->isSigned) {
                v = max ? (bits == 64 ? INT64_MAX : (INT64_C(1) << (bits - 1)) - 1)
                        : (bits == 64 ? INT64_MIN : -(INT64_C(1) << (bits - 1)));
            } else {
                v = max ? (bits == 64 ? -1 : (int64_t)((UINT64_C(1) << bits) - 1)) : 0;
            }
            e->isConst = true;
            e->cval = v;
            e->kind = N_INT;
            e->ival = (uint64_t)v;
            return setType(e, it);
        }
    }
    if (e->a->kind == N_IDENT) {
        Node *d = lookup(c, e->a->name);
        if (d && d->kind == N_ENUM) {
            Type *et = enumType(c, d);
            Node *m = enumMember(et, e->name);
            if (!m) {
                error(c, e, "%s has no value named %s", d->name, e->name);
                return setType(e, &tyError);
            }
            if (memberData(m)) {
                error(c, e, "%s.%s carries data; write %s.%s(...)", d->name, m->name, d->name, m->name);
                return setType(e, &tyError);
            }
            e->a->ref = d;
            e->ref = m;
            e->isConst = !et->hasData;
            e->cval = m->cval;
            return setType(e, et);
        }
    }
    Type *t = checkExpr(c, &e->a, NULL);
    if (t->kind == TY_ENUM && !t->hasData && strcmp(e->name, "value") == 0) {
        e->builtin = 3;                 /* the value's number: Status.notFound.value is 404 */
        return setType(e, &tyI64);
    }
    if (t->kind == TY_ENUM && strcmp(e->name, "name") == 0) {
        e->builtin = 1;                 /* the value's name, from the runtime's table */
        return setType(e, &tyStr);
    }
    if (isErr(t)) return setType(e, t);
    if (e->a->kind == N_IDENT && e->a->ref && (e->a->ref->kind == N_STRUCT || e->a->ref->kind == N_TYPEDEF)) {
        error(c, e, "%s is a type; methods are called on values", e->a->name);
        return setType(e, &tyError);
    }
    if (t->kind == TY_PTR && (t->elem->kind == TY_SLICE || t->elem->kind == TY_STRING) &&
        (strcmp(e->name, "len") == 0 || strcmp(e->name, "ptr") == 0)) {
        /* s.len through a String * or a slice pointer: (*s).len */
        Node *d = like(c, N_UNARY, e->a);
        d->op = P_STAR;
        d->a = e->a;
        d->type = t->elem;
        e->a = d;
        t = t->elem;
    }
    if (strcmp(e->name, "ptr") == 0 && (t->kind == TY_SLICE || t->kind == TY_STRING)) {
        e->builtin = 2;                 /* the pointer to the first item */
        return setType(e, ptrTo(t->kind == TY_STRING ? &tyU8 : t->elem));
    }
    if (strcmp(e->name, "len") == 0) {
        Type *base = t->kind == TY_PTR && t->elem->kind == TY_ARRAY ? t->elem : t;
        if (base->kind == TY_SLICE || base->kind == TY_STRING) {
            return setType(e, &tyI64);
        }
        if (base->kind == TY_ARRAY) {
            e->isConst = true;
            e->cval = base->count;
            return setType(e, &tyI64);
        }
    }
    if (t->kind == TY_ERRVAL) {
        if (strcmp(e->name, "code") == 0) {
            return setType(e, &tyErrCode);
        }
        if (strcmp(e->name, "message") == 0) {
            return setType(e, &tyStr);
        }
        error(c, e, "an Error has a code and a message, not %s", e->name);
        return setType(e, &tyError);
    }
    Type *st = structOf(t);
    if (!st) {
        if (t->kind == TY_PTR && t->elem->kind == TY_PTR) {
            error(c, e, "%s is a pointer to a pointer; use * to reach the struct", t->name);
        } else {
            error(c, e, "%s has no field %s", t->name, e->name);
        }
        return setType(e, &tyError);
    }
    resolveStruct(c, st);
    Node *path[16];
    int n = findField(st, e->name, path, 16);
    if (!n) {
        Node *mpath[16];
        int mn;
        if (findMethod(st, e->name, mpath, &mn, 16)) {
            error(c, e, "%s is a method; call it with ()", e->name);
        } else {
            error(c, e, "%s has no field %s", st->name, e->name);
        }
        return setType(e, &tyError);
    }
    /* reached through embedded structs: add the steps */
    Node *base = e->a;
    for (int i = 0; i < n - 1; i++) {
        base = fieldStep(c, base, path[i], e);
    }
    e->a = base;
    e->ref = path[n - 1];
    e->ival = (uint64_t)fieldIndex(structOf(base->type)->decl, path[n - 1]);
    Node *owner = structOf(base->type)->decl;
    if (owner->module && c->module && strcmp(owner->module, c->module) != 0 && !(path[n - 1]->flags & F_PUB)) {
        error(c, e, "the field %s of %s isn't pub, so only its own module can use it", e->name, owner->name);
    }
    (void)expected;
    return setType(e, path[n - 1]->type);
}

static Type *checkIndex(Checker *c, Node *e) {
    Type *t = checkExpr(c, &e->a, NULL);
    if (e->list.len != 1) {
        notYet(c, e, "generic types", "0.6");
        return setType(e, &tyError);
    }
    Type *ti = checkExpr(c, &e->list.items[0], NULL);
    if (isErr(t)) return setType(e, t);
    if (t->kind == TY_STRING) {
        coerce(c, &e->a, &tyStr, "the text");
        t = &tyStr;
    }
    if (t->kind != TY_SLICE && t->kind != TY_ARRAY) {
        if (t->kind == TY_PTR) {
            error(c, e, "a pointer can't be indexed; use a slice");
        } else {
            error(c, e, "%s can't be indexed", t->name);
        }
        return setType(e, &tyError);
    }
    if (!isErr(ti) && !isInt(ti)) {
        error(c, e->list.items[0], "an index must be an integer, not %s", ti->name);
    }
    Node *ix = e->list.items[0];
    if (ix->isConst && (ix->cval < 0 || (t->kind == TY_ARRAY && ix->cval >= t->count))) {
        error(c, ix, "index %lld is out of range for %s", (long long)ix->cval, t->name);
    }
    return setType(e, t->elem);
}

static Type *checkSlice(Checker *c, Node *e) {
    Type *t = checkExpr(c, &e->a, NULL);
    if (e->b) {
        Type *tb = checkExpr(c, &e->b, NULL);
        if (!isErr(tb) && !isInt(tb)) {
            error(c, e->b, "the start of a slice must be an integer, not %s", tb->name);
        }
    }
    if (e->c) {
        Type *tc = checkExpr(c, &e->c, NULL);
        if (!isErr(tc) && !isInt(tc)) {
            error(c, e->c, "the end of a slice must be an integer, not %s", tc->name);
        }
    }
    if (isErr(t)) return setType(e, t);
    if (t->kind == TY_STRING) {
        coerce(c, &e->a, &tyStr, "the text");
        t = &tyStr;
    }
    if (t->kind == TY_ARRAY) {
        if (!addressable(e->a)) {
            error(c, e->a, "only an array in a variable can be sliced");
            return setType(e, &tyError);
        }
        return setType(e, sliceOf(t->elem));
    }
    if (t->kind == TY_PTR) {
        /* p[a:b]: the items a pointer points at, for C's memory; not checked */
        if (!e->c) {
            error(c, e, "slicing a pointer needs an end: p[0:n]");
            return setType(e, &tyError);
        }
        return setType(e, sliceOf(t->elem));
    }
    if (t->kind != TY_SLICE) {
        error(c, e, "%s can't be sliced", t->name);
        return setType(e, &tyError);
    }
    return setType(e, t);
}

static Type *structForLiteral(Checker *c, Node *e, Type *expected) {
    if (!e->a) {
        Type *t = expected ? expected : NULL;
        if (!t || t->kind != TY_STRUCT) {
            error(c, e, ".{...} needs a known struct type here; write the type's name before the {");
            return NULL;
        }
        return t;
    }
    if (e->a->kind == N_INDEX) {
        Type *t = typeFromExpr(c, e->a, true);
        if (isErr(t)) return NULL;
        if (t->kind != TY_STRUCT) {
            error(c, e->a, "%s is not a struct", t->name);
            return NULL;
        }
        return t;
    }
    if (e->a->kind != N_IDENT) {
        error(c, e->a, "a struct literal starts with the struct's name");
        return NULL;
    }
    Node *d = lookup(c, e->a->name);
    if (!d || (d->kind != N_STRUCT && d->kind != N_TYPEDEF)) {
        error(c, e->a, "%s is not a struct", e->a->name);
        return NULL;
    }
    Type *t = typeFromName(c, e->a->name, e->a, true);
    if (t->kind != TY_STRUCT) {
        if (!isErr(t)) error(c, e->a, "%s is not a struct", e->a->name);
        return NULL;
    }
    return t;
}

static Type *checkStructLit(Checker *c, Node *e, Type *expected) {
    Type *t = structForLiteral(c, e, expected);
    if (!t) {
        for (int i = 0; i < e->list.len; i++) {
            checkExpr(c, &e->list.items[i]->a, NULL);
        }
        return setType(e, &tyError);
    }
    resolveStruct(c, t);
    Node *d = t->decl;
    for (int i = 0; i < e->list.len; i++) {
        Node *init = e->list.items[i];
        Node *field = NULL;
        for (int j = 0; j < d->list.len; j++) {
            Node *f = d->list.items[j];
            if (f->name && strcmp(f->name, init->name) == 0) {
                field = f;
                init->ival = (uint64_t)j;
            }
        }
        for (int j = 0; j < i; j++) {
            if (strcmp(e->list.items[j]->name, init->name) == 0) {
                error(c, init, "%s is set twice", init->name);
            }
        }
        if (!field) {
            error(c, init, "%s has no field %s", t->name, init->name);
            checkExpr(c, &init->a, NULL);
            continue;
        }
        init->ref = field;
        if (d->module && c->module && strcmp(d->module, c->module) != 0 && !(field->flags & F_PUB)) {
            error(c, init, "the field %s of %s isn't pub, so only its own module can set it", init->name, t->name);
        }
        checkExpr(c, &init->a, field->type);
        char what[96];
        snprintf(what, sizeof what, "the value for %s", init->name);
        coerce(c, &init->a, field->type, what);
    }
    return setType(e, t);
}

static Type *checkArrayLit(Checker *c, Node *e, Type *expected) {
    Type *elem = NULL;
    if (expected && expected->kind == TY_ARRAY) {
        elem = expected->elem;
        if (e->list.len != expected->count) {
            error(c, e, "%s needs %lld values, not %d", expected->name, (long long)expected->count, e->list.len);
        }
    } else if (expected && expected->kind == TY_SLICE) {
        elem = expected->elem;
    }
    if (e->list.len == 0) {
        if (!elem) {
            error(c, e, "the type of [] can't be worked out here");
            return setType(e, &tyError);
        }
        if (expected && expected->kind == TY_SLICE) {
            return setType(e, expected);    /* [] where a slice goes: an empty one */
        }
        error(c, e, "an array needs at least one value");
        return setType(e, &tyError);
    }
    for (int i = 0; i < e->list.len; i++) {
        Type *t = checkExpr(c, &e->list.items[i], elem);
        if (!elem && !isErr(t)) {
            elem = t;
        }
        if (elem) {
            coerce(c, &e->list.items[i], elem, "the value");
        }
    }
    if (!elem) {
        return setType(e, &tyError);
    }
    return setType(e, arrayOf(elem, e->list.len));
}

static Type *checkExprInner(Checker *c, Node **slot, Type *expected);

/* Checks an expression. A !T call is only allowed where try, catch or
 * _ = deal with its error; they set allowResult. */
static Type *checkExpr(Checker *c, Node **slot, Type *expected) {
    bool allow = c->allowResult;
    c->allowResult = false;
    Type *t = checkExprInner(c, slot, expected);
    if (t->kind == TY_RESULT && !allow) {
        Node *e = *slot;
        const char *fn = e->kind == N_CALL && e->ref ? e->ref->name : "this call";
        error(c, e, "the error from %s is not handled; use try, catch or _ =", fn);
        return setType(e, &tyError);
    }
    return t;
}

static Type *checkTry(Checker *c, Node *e);
static Type *checkCatch(Checker *c, Node *e, Type *expected);
static Type *checkIndirectCall(Checker *c, Node *e, Type *ft);

/* Whether a constant expression is made of durations: 2h + 3m. */
static bool hasDuration(Node *e) {
    if (!e) return false;
    if (e->kind == N_INT) return e->name && !strchr("KMG", e->name[0]);
    if (e->kind == N_UNARY) return hasDuration(e->a);
    if (e->kind == N_BINARY) {
        if (e->op == P_SLASH && hasDuration(e->a) && hasDuration(e->b)) return false; /* 90s / 1s is 90 */
        return hasDuration(e->a) || hasDuration(e->b);
    }
    return false;
}

static Type *checkExprInner(Checker *c, Node **slot, Type *expected) {
    Node *e = *slot;

    /* values known when compiling */
    if (e->kind != N_STRING && e->kind != N_CALL) {
        int64_t v;
        bool b;
        int before = gErrors;
        if (evalConst(c, e, &v, &b)) {
            e->isConst = true;
            e->cval = v;
            if (b) {
                return setType(e, &tyBool);
            }
            if (e->kind == N_IDENT) {
                Node *d = lookup(c, e->name);
                e->ref = d;
                if (d->flags & F_EXPLICIT) {
                    return setType(e, d->type);
                }
            }
            if (e->kind == N_CHAR && !(expected && isInt(expected) && fitsIn(v, expected))) {
                return setType(e, &tyU8);
            }
            if (hasDuration(e) && (!expected || expected == &tyDuration || !isInt(expected))) {
                return setType(e, &tyDuration);     /* 2s, 2h + 3m, -1500ms */
            }
            if (expected && isInt(expected) && fitsIn(v, expected)) {
                return setType(e, expected);
            }
            return setType(e, &tyI64);
        }
        if (gErrors != before) {
            return setType(e, &tyError);
        }
    }

    switch (e->kind) {
    case N_INT:
        /* only literals above the i64 range get here */
        if (expected == &tyU64) {
            e->isConst = true;
            e->cval = (int64_t)e->ival;
            return setType(e, &tyU64);
        }
        error(c, e, "%s is too large for i64", e->text);
        return setType(e, &tyError);
    case N_FLOAT:
        e->isConst = true;
        return setType(e, expected == &tyF32 ? &tyF32 : &tyF64);
    case N_STRING:
        return setType(e, &tyStr);
    case N_INTERP:
        error(c, e, "text with {} values can be passed straight to a function that takes str, or to format to keep it");
        return setType(e, &tyError);
    case N_NULL:
        return setType(e, &tyNull);
    case N_IDENT: {
        if (strcmp(e->name, "_") == 0) {
            error(c, e, "'_' can only be assigned to");
            return setType(e, &tyError);
        }
        Node *d = lookup(c, e->name);
        if (!d) {
            if (strcmp(e->name, "print") == 0 || strcmp(e->name, "panic") == 0 ||
                strcmp(e->name, "format") == 0 || strcmp(e->name, "alloc") == 0 ||
                strcmp(e->name, "free") == 0) {
                error(c, e, "%s can only be called", e->name);
            } else if (typeFromName(c, e->name, e, false)) {
                error(c, e, "%s is a type, not a value", e->name);
            } else if (!(e->flags & F_REPORTED)) {
                notDeclared(c, e, e->name);
            }
            return setType(e, &tyError);
        }
        e->ref = d;
        switch (d->kind) {
        case N_FUNC: {
            /* a function used as a value: its address */
            if (d->list2.len) {
                error(c, e, "%s is generic; only a function with fixed types can be used as a value", e->name);
                return setType(e, &tyError);
            }
            if (!d->type) {
                Scope *saved = c->scope;
                const char *savedModule = c->module;
                c->scope = d->scope;
                c->module = d->module;
                checkSignature(c, d, NULL);
                c->scope = saved;
                c->module = savedModule;
            }
            if (isErr(d->type)) return setType(e, &tyError);
            if (d->flags & F_VARIADIC) {
                error(c, e, "%s takes C's ..., so it can't be used as a value", e->name);
                return setType(e, &tyError);
            }
            Type *params[64];
            if (d->list.len > 64) return setType(e, &tyError);
            for (int i = 0; i < d->list.len; i++) {
                params[i] = d->list.items[i]->type ? d->list.items[i]->type : &tyError;
                if (isErr(params[i])) return setType(e, &tyError);
            }
            return setType(e, funcOf(d->type, params, d->list.len));
        }
        case N_STRUCT: case N_TYPEDEF:
            error(c, e, "%s is a type, not a value", e->name);
            return setType(e, &tyError);
        case N_CONST:
            checkConstDecl(c, d);
            if (d->type && isFloat(d->type)) {
                e->isConst = true;
                e->fval = d->fval;
                return setType(e, expected == &tyF32 ? &tyF32 : d->type);
            }
            return setType(e, d->type);
        default:
            if (!d->type && d->kind == N_VAR && d->a) {
                /* a global declared further down, not checked yet: its
                 * declared type is enough here */
                Scope *saved = c->scope;
                if (d->scope) c->scope = d->scope;
                d->type = resolveType(c, d->a);
                c->scope = saved;
            }
            return setType(e, d->type ? d->type : &tyError);
        }
    }
    case N_UNARY: {
        switch (e->op) {
        case P_MINUS: {
            Type *t = checkExpr(c, &e->a, expected && isNumber(expected) ? expected : NULL);
            if (isErr(t)) return setType(e, t);
            if (isFloat(t)) {
                if (e->a->isConst) {
                    e->isConst = true;
                    e->fval = -e->a->fval;
                }
                return setType(e, t);
            }
            if (!isInt(t)) {
                error(c, e, "'-' needs a number, not %s", t->name);
                return setType(e, &tyError);
            }
            if (!t->isSigned) {
                error(c, e, "'-' can't be used on %s, which has no negative values", t->name);
                return setType(e, &tyError);
            }
            return setType(e, t);
        }
        case P_BANG:
            checkExpr(c, &e->a, &tyBool);
            coerce(c, &e->a, &tyBool, e->ival ? "the value after 'not'" : "the value after '!'");
            return setType(e, &tyBool);
        case P_TILDE: {
            Type *t = checkExpr(c, &e->a, expected && (isInt(expected) || expected->isFlags) ? expected : NULL);
            if (t->kind == TY_ENUM && t->isFlags) {
                return setType(e, t);
            }
            if (!isErr(t) && !isInt(t)) {
                error(c, e, "'~' needs an integer, not %s", t->name);
                return setType(e, &tyError);
            }
            return setType(e, t);
        }
        case P_AMP: {
            Type *t = checkExpr(c, &e->a, NULL);
            if (isErr(t)) return setType(e, t);
            if (!addressable(e->a)) {
                if (e->a->kind == N_IDENT && e->a->ref && e->a->ref->kind == N_CONST) {
                    error(c, e, "a constant has no address");
                } else {
                    error(c, e, "only a variable, a field or an element has an address");
                }
                return setType(e, &tyError);
            }
            return setType(e, ptrTo(t));
        }
        case P_STAR: {
            Type *t = checkExpr(c, &e->a, NULL);
            if (isErr(t)) return setType(e, t);
            if (t->kind != TY_PTR) {
                error(c, e, "'*' needs a pointer, not %s", t->name);
                return setType(e, &tyError);
            }
            return setType(e, t->elem);
        }
        default: {
            /* <-ch: the next value from a channel, waiting for one */
            Type *tc = checkExpr(c, &e->a, NULL);
            if (isErr(tc)) return setType(e, tc);
            if (tc->kind != TY_CHAN) {
                error(c, e, "<- takes from a channel, not from %s", tc->name);
                return setType(e, &tyError);
            }
            return setType(e, tc->elem);
        }
        }
    }
    case N_CHAN_MAKE: {
        Type *t = resolveType(c, e->a);
        Type *n = checkExpr(c, &e->b, &tyI64);
        if (!isErr(n)) {
            if (!isInt(n)) {
                error(c, e->b, "a channel's size is an integer, not %s", n->name);
            } else {
                coerce(c, &e->b, &tyI64, "the size");
                if (e->b->isConst && e->b->cval < 0) {
                    error(c, e->b, "a channel's room can't be below 0; 0 is a channel with no buffer");
                }
            }
        }
        if (isErr(t)) return setType(e, t);
        if (t == &tyVoid) {
            error(c, e->a, "a channel of void carries nothing");
            return setType(e, &tyError);
        }
        if (t->kind == TY_STRUCT) resolveStruct(c, t);
        return setType(e, chanOf(t));
    }
    case N_COMPTIME:
        error(c, e, "comptime goes in a constant at the top of a file: const TABLE = comptime makeTable()");
        checkExpr(c, &e->a, expected);
        return setType(e, &tyError);
    case N_THREAD: {
        if (!checkStarted(c, &e->a, "thread", "a thread")) {
            return setType(e, &tyError);
        }
        return setType(e, &tyThread);
    }
    case N_BINARY:
        return checkBinary(c, e, expected);
    case N_CALL: {
        Type *t = checkCall(c, e, expected);
        checkTempResult(c, e);
        if (e->ref && e->ref->kind == N_FUNC && (e->ref->flags & F_NORETURN) && !isErr(t)) {
            return setType(e, &tyNoreturn);     /* like panic: the code after it is never reached */
        }
        return t;
    }
    case N_IF:
        return setType(e, checkIf(c, e, expected, true));
    case N_MATCH:
        return setType(e, checkMatch(c, e, expected, true));
    case N_FIELD_ACCESS:
        return checkFieldAccess(c, slot, expected);
    case N_INDEX:
        return checkIndex(c, e);
    case N_SLICE:
        return checkSlice(c, e);
    case N_STRUCT_LIT:
        return checkStructLit(c, e, expected);
    case N_ARRAY_LIT:
        return checkArrayLit(c, e, expected);
    case N_TRY:
        return checkTry(c, e);
    case N_CATCH:
        return checkCatch(c, e, expected);
    case N_ENUM_DOT: {
        if (!expected || expected->kind != TY_ENUM) {
            error(c, e, "the enum of .%s can't be worked out here; write the enum's name before the dot", e->name);
            return setType(e, &tyError);
        }
        Node *m = enumMember(expected, e->name);
        if (!m) {
            error(c, e, "%s has no value named %s", expected->name, e->name);
            return setType(e, &tyError);
        }
        if (memberData(m)) {
            error(c, e, "%s.%s carries data; write .%s(...)", expected->name, m->name, m->name);
            return setType(e, &tyError);
        }
        e->ref = m;
        e->isConst = !expected->hasData;
        e->cval = m->cval;
        return setType(e, expected);
    }
    case N_TUPLE: {
        Type *elems[32];
        if (e->list.len > 32) {
            error(c, e, "a tuple can have at most 32 values");
            return setType(e, &tyError);
        }
        bool bad = false;
        for (int i = 0; i < e->list.len; i++) {
            Type *want = expected && expected->kind == TY_TUPLE && expected->nelems == e->list.len
                             ? expected->elems[i] : NULL;
            elems[i] = checkExpr(c, &e->list.items[i], want);
            if (want) {
                coerce(c, &e->list.items[i], want, "the value");
                elems[i] = isErr(e->list.items[i]->type) ? want : want;
            }
            if (isErr(elems[i])) bad = true;
            if (elems[i]->kind == TY_VOID || elems[i]->kind == TY_NORETURN) {
                error(c, e->list.items[i], "this gives no value to put in a tuple");
                bad = true;
            } else if (elems[i]->kind == TY_NULL) {
                error(c, e->list.items[i], "the type of null can't be worked out here");
                bad = true;
            }
        }
        if (bad) return setType(e, &tyError);
        return setType(e, tupleOf(elems, e->list.len));
    }
    default:
        if (e->kind == N_T_SLICE || e->kind == N_T_ARRAY || e->kind == N_T_NAME || e->kind == N_T_GENERIC) {
            error(c, e, "this is a type, not a value");
            return setType(e, &tyError);
        }
        notYet(c, e, "this expression", "a later version");
        return setType(e, &tyError);
    }
}

static Type *intTypeNamed(const char *name) {
    for (size_t i = 0; i < sizeof builtinTypes / sizeof builtinTypes[0]; i++) {
        if (isNumber(builtinTypes[i]) && strcmp(builtinTypes[i]->name, name) == 0) {
            return builtinTypes[i];
        }
    }
    return NULL;
}

static bool isBuiltinName(const char *name) {
    return strcmp(name, "print") == 0 || strcmp(name, "panic") == 0 || strcmp(name, "format") == 0 ||
           strcmp(name, "alloc") == 0 || strcmp(name, "free") == 0 || strcmp(name, "error") == 0 ||
           strcmp(name, "sleep") == 0 || strcmp(name, "after") == 0 || strcmp(name, "expect") == 0;
}

static bool printable(Type *t) {
    return isNumber(t) || t == &tyBool || isStr(t) || t->kind == TY_STRING || t->kind == TY_ENUM ||
           t->kind == TY_ERRCODE;
}

static void checkText(Checker *c, Node **slot, const char *fn);

/* Text with {} values passed straight to a str parameter: built in a
 * String that lives until the end of the statement. */
static bool checkTempText(Checker *c, Node **slot, Type *param) {
    Node *arg = *slot;
    if (arg->kind != N_INTERP || !isStr(param)) {
        return false;
    }
    checkText(c, slot, "the call");
    arg->flags |= F_TEMPTEXT;
    arg->type = &tyStr;
    return true;
}

/* A String that a call made, passed straight to a str parameter: nothing
 * else can free it, so it is freed when the statement ends. */
static void markTempString(Node **slot, Type *param) {
    Node *x = *slot;
    if (!isStr(param) || x->kind != N_CONVERT || !x->a->type || x->a->type->kind != TY_STRING) {
        return;
    }
    Node *v = x->a;
    Node *call = v->kind == N_TRY ? v->a : v;
    if (!call || call->kind != N_CALL) {
        return;
    }
    /* a generic's T is a value stored somewhere: List[String].at gives the
     * list's String, not a new one */
    Node *fn = call->ref ? call->ref : call->a ? call->a->ref : NULL;
    if (fn && fn->kind == N_FUNC && (fn->flags & F_INSTANCE)) {
        return;
    }
    v->flags |= F_TEMPSTR;
}

/* Whether a value of type t can point into other memory. */
static bool mayPoint(Type *t) {
    switch (t->kind) {
    case TY_PTR: case TY_SLICE: case TY_DYN: case TY_NULL:
        return true;
    case TY_ARRAY: case TY_RESULT:
        return mayPoint(t->elem);
    case TY_TUPLE:
        for (int i = 0; i < t->nelems; i++) {
            if (mayPoint(t->elems[i])) return true;
        }
        return false;
    case TY_STRUCT:
        if (!t->decl) return false;
        for (int i = 0; i < t->decl->list.len; i++) {
            Type *ft = t->decl->list.items[i]->type;
            if (ft && mayPoint(ft)) return true;
        }
        return false;
    default:
        return false;
    }
}

static Node *tempArg(Node *a) {
    if (a->kind == N_NAMED_ARG) a = a->a;
    if (!a) return NULL;
    if (a->flags & (F_TEMPTEXT | F_TEMPSTR)) return a;
    if (a->kind == N_CONVERT && a->a && (a->a->flags & F_TEMPSTR)) return a->a;
    return NULL;
}

/* A temporary String is freed when the statement ends, so a call that
 * gets one can't give back something that may point into it. */
static void checkTempResult(Checker *c, Node *e) {
    if (!e->type || isErr(e->type) || !mayPoint(e->type)) return;
    /* env.loadWith's struct points into the environment, never its path */
    if (e->builtin == BI_ENV_LOAD) return;
    for (int i = 0; i < e->list.len; i++) {
        Node *a = tempArg(e->list.items[i]);
        if (!a) continue;
        const char *what = a->flags & F_TEMPTEXT ? "this text" : "this String";
        error(c, a, "%s is freed when the statement ends, and the call gives back a %s that may point into it; "
                    "keep it in a variable first, with format(...) for text",
              what, e->type->name);
        return;
    }
}

/* print, panic and format: one argument, the text, which may have {}
 * values in it. */
static void checkTextArg(Checker *c, Node *call, const char *fn) {
    if (call->list.len != 1 || call->list.items[0]->kind == N_NAMED_ARG) {
        error(c, call, "%s takes one argument, the text", fn);
        return;
    }
    checkText(c, &call->list.items[0], fn);
}

static void checkText(Checker *c, Node **slot, const char *fn) {
    Node *arg = *slot;
    if (arg->kind == N_INTERP) {
        arg->type = &tyStr;
        for (int i = 0; i < arg->list.len; i++) {
            Node **part = &arg->list.items[i];
            if ((*part)->kind == N_STRING) {
                (*part)->type = &tyStr;
                continue;
            }
            Type *t = checkExpr(c, part, NULL);
            Type *st = t->kind == TY_PTR ? t->elem : t;
            Node *fm = st->kind == TY_STRUCT ? findMethodIn(st, "format") : NULL;
            if (!isErr(t) && !printable(t) && fm) {
                /* a struct with String format(): its text, freed after the statement */
                if (!fm->type) checkSignature(c, fm, st);
                /* format() with no arguments: parameters after self need defaults */
                bool noArgs = fm->list.len >= 1;
                for (int k = 1; k < fm->list.len; k++) {
                    if (!fm->list.items[k]->b) noArgs = false;
                }
                if (fm->type != &tyString || !noArgs) {
                    error(c, *part, "%s.format must take nothing and give a String to put a %s into text",
                          st->name, st->name);
                    continue;
                }
                /* built as checkMethodCall leaves a call, since the value is
                 * checked already: checking a method call twice breaks it */
                Node *recv = *part;
                Node *access = like(c, N_FIELD_ACCESS, recv);
                access->a = recv;
                access->name = "format";
                access->ref = fm;
                Node *self = recv;
                if (t->kind != TY_PTR) {
                    if (!addressable(recv)) {
                        recv->flags |= F_EXPLICIT;
                    }
                    self = like(c, N_UNARY, recv);
                    self->op = P_AMP;
                    self->a = recv;
                    self->type = ptrTo(recv->type);
                }
                Node *call = like(c, N_CALL, recv);
                call->a = access;
                NodeList args = {0};
                listPush(c->arena, &args, self);
                checkArgs(c, call, fm, &call->list, 1, &args, false);
                call->list = args;
                call->ref = fm;
                setType(call, fm->type);
                *part = call;
                coerce(c, part, &tyStr, "the value");
                markTempString(part, &tyStr);
            } else if (!isErr(t) && !printable(t)) {
                if (st->kind == TY_STRUCT) {
                    error(c, *part, "%s can't be put into text; give it a method String %s.format()", t->name,
                          st->name);
                } else {
                    error(c, *part, "a value of type %s can't be put into text", t->name);
                }
            } else if (t->kind == TY_STRING) {
                coerce(c, part, &tyStr, "the value");
            }
        }
        return;
    }
    Type *t = checkExpr(c, slot, &tyStr);
    if (isErr(t)) {
        return;
    }
    if (t->kind == TY_STRING) {
        coerce(c, slot, &tyStr, "the text");
        markTempString(slot, &tyStr);
    } else if (!isStr(t)) {
        error(c, arg, "%s takes text, not %s; write %s(\"{...}\") to use a value", fn, t->name, fn);
    }
}

/* Lines arguments up with parameters: positional first, then named, then
 * defaults. params starts at `first` (1 for a method: 0 is self). */
static void checkArgs(Checker *c, Node *e, Node *fn, NodeList *given, int first, NodeList *out, bool checked) {
    int np = fn->list.len;
    Node **args = arenaAlloc(c->arena, sizeof(Node *) * (size_t)(np ? np : 1));
    bool named = false;
    int before = gErrors;
    int pos = first;
    /* a T... parameter: the arguments left over go into an array literal,
     * which becomes the T[]; by name it takes a slice as it is */
    int restAt = np > first && (fn->list.items[np - 1]->flags & F_REST) ? np - 1 : -1;
    Node *rest = NULL;
    for (int i = 0; i < given->len; i++) {
        Node *arg = given->items[i];
        if (arg->kind != N_NAMED_ARG && !named && pos == restAt) {
            if (!rest) {
                rest = like(c, N_ARRAY_LIT, arg);
                args[pos] = rest;
            }
            listPush(c->arena, &rest->list, arg);
            continue;
        }
        if (arg->kind == N_NAMED_ARG) {
            named = true;
            int k = -1;
            for (int j = first; j < np; j++) {
                if (strcmp(fn->list.items[j]->name, arg->name) == 0) {
                    k = j;
                }
            }
            if (k < 0) {
                error(c, arg, "%s has no parameter named %s", fn->name, arg->name);
                continue;
            }
            if (args[k]) {
                error(c, arg, "%s is given twice", arg->name);
                continue;
            }
            args[k] = arg->a;
            continue;
        }
        if (named) {
            error(c, arg, "an argument without a name can't follow a named one");
            continue;
        }
        if (pos >= np) {
            if (fn->flags & F_VARIADIC) {
                pos++;
                continue;           /* C's ..., checked below */
            }
            int n = np - first;
            error(c, arg, "%s takes %d argument%s, but more are given", fn->name, n, n == 1 ? "" : "s");
            break;
        }
        args[pos++] = arg;
    }
    if (restAt >= 0 && !args[restAt]) {
        args[restAt] = like(c, N_ARRAY_LIT, e);     /* nothing left over: an empty slice */
    }
    for (int j = first; j < np; j++) {
        Node *param = fn->list.items[j];
        if (!args[j]) {
            if (param->b) {
                args[j] = param->b;     /* the default: checked with the function */
            } else {
                if (gErrors == before) {
                    error(c, e, "%s needs a value for %s", fn->name, param->name);
                }
                continue;
            }
        } else if (!checked && checkTempText(c, &args[j], param->type)) {
            /* built when the call is made */
        } else {
            if (j == restAt && args[j] == rest) {
                /* each value left over is an argument for the T... */
                Type *elem = param->type->elem;
                char what[96];
                snprintf(what, sizeof what, "an argument for %s", param->name);
                for (int k = 0; k < rest->list.len; k++) {
                    Node **slot = &rest->list.items[k];
                    if (!checked && checkTempText(c, slot, elem)) {
                        continue;
                    }
                    if (!checked) {
                        checkExpr(c, slot, elem);
                    }
                    coerce(c, slot, elem, what);
                    markTempString(slot, elem);
                }
                setType(rest, arrayOf(elem, rest->list.len));
            } else if (!checked || (j == restAt && args[j]->kind == N_ARRAY_LIT && !args[j]->list.len)) {
                checkExpr(c, &args[j], param->type);
            }
            char what[96];
            snprintf(what, sizeof what, "the argument for %s", param->name);
            coerce(c, &args[j], param->type, what);
            markTempString(&args[j], param->type);
        }
        listPush(c->arena, out, args[j]);
    }
    if (fn->flags & F_VARIADIC) {
        /* the extra arguments of a C function like printf: integers and pointers */
        int extra = 0;
        for (int i = 0; i < given->len; i++) {
            Node **slot = &given->items[i];
            if ((*slot)->kind == N_NAMED_ARG) continue;
            if (extra++ < np - first) continue;
            Type *t = checked ? (*slot)->type : checkExpr(c, slot, NULL);
            if (isErr(t)) continue;
            if (isStr(t) || t->kind == TY_STRING) {
                error(c, *slot, "C takes text as a pointer; pass .ptr, ending the text with \\0");
            } else if (!isNumber(t) && t->kind != TY_PTR && t->kind != TY_BOOL && t->kind != TY_NULL) {
                error(c, *slot, "an extra argument to a C function must be a number or a pointer, not %s", t->name);
            }
            if (t->kind == TY_NULL) {
                (*slot)->type = ptrTo(&tyU8);
            }
            listPush(c->arena, out, *slot);
        }
    }
}

/* The call after spawn or thread: a function of the program, worked out
 * now and run later, whose result nobody receives. */
static bool checkStarted(Checker *c, Node **callp, const char *word, const char *what) {
    Node *call = *callp;
    if (call->kind != N_CALL) {
        error(c, call, "%s starts a function: %s handle(conn)", word, word);
        checkExpr(c, callp, NULL);
        return false;
    }
    c->allowResult = true;
    Type *t = checkExpr(c, callp, NULL);
    call = *callp;
    if (isErr(t)) return false;
    if (call->builtin && call->builtin != BI_INDIRECT) {
        error(c, call, "%s starts a function of the program, not a built-in", word);
        return false;
    }
    if (t->kind == TY_RESULT) {
        error(c, call, "%s can't return an error, since nothing waits for it; handle the error inside", what);
        return false;
    }
    if (c->inDefer) {
        error(c, call, "a deferred statement can't %s", word);
        return false;
    }
    return true;
}

/* Methods of Atomic[T], Mutex and Thread. */
static Type *checkSyncMethod(Checker *c, Node *e, Type *t, const char *name) {
    Node *recv = e->a->a;
    Type *base = t->kind == TY_PTR ? t->elem : t;
    if (t->kind != TY_PTR && base->kind != TY_THREAD && !addressable(recv)) {
        error(c, recv, "%s must be called on a variable, a field or through a pointer", name);
        return setType(e, &tyError);
    }
    struct { int kind; const char *name; int bi; int args; } methods[] = {
        {TY_ATOMIC, "load", BI_ATOMIC_LOAD, 0}, {TY_ATOMIC, "store", BI_ATOMIC_STORE, 1},
        {TY_ATOMIC, "add", BI_ATOMIC_ADD, 1}, {TY_ATOMIC, "sub", BI_ATOMIC_SUB, 1},
        {TY_ATOMIC, "swap", BI_ATOMIC_SWAP, 1}, {TY_ATOMIC, "cas", BI_ATOMIC_CAS, 2},
        {TY_MUTEX, "lock", BI_MUTEX_LOCK, 0}, {TY_MUTEX, "unlock", BI_MUTEX_UNLOCK, 0},
        {TY_MUTEX, "tryLock", BI_MUTEX_TRY_LOCK, 0}, {TY_THREAD, "join", BI_THREAD_JOIN, 0},
    };
    for (size_t i = 0; i < sizeof methods / sizeof methods[0]; i++) {
        if ((int)base->kind != methods[i].kind || strcmp(methods[i].name, name) != 0) continue;
        int bi = methods[i].bi;
        if (base->kind == TY_ATOMIC && base->elem->kind == TY_BOOL && (bi == BI_ATOMIC_ADD || bi == BI_ATOMIC_SUB)) {
            error(c, e, "%s is for numbers; an Atomic[bool] has load, store, swap and cas", name);
            return setType(e, &tyError);
        }
        if (e->list.len != methods[i].args) {
            if (methods[i].args == 0) {
                error(c, e, "%s takes no arguments", name);
            } else if (methods[i].args == 1) {
                error(c, e, "%s takes one argument, the %s value", name, base->elem->name);
            } else {
                error(c, e, "cas takes two arguments: the value expected and the new one");
            }
            return setType(e, &tyError);
        }
        for (int k = 0; k < e->list.len; k++) {
            Node **a = &e->list.items[k];
            if ((*a)->kind == N_NAMED_ARG) {
                error(c, *a, "%s takes no named arguments", name);
                return setType(e, &tyError);
            }
            Type *at = checkExpr(c, a, base->elem);
            if (isErr(at)) return setType(e, &tyError);
            coerce(c, a, base->elem, "the value");
        }
        e->builtin = bi;
        switch (bi) {
        case BI_ATOMIC_LOAD: case BI_ATOMIC_ADD: case BI_ATOMIC_SUB: case BI_ATOMIC_SWAP:
            return setType(e, base->elem);
        case BI_ATOMIC_CAS: case BI_MUTEX_TRY_LOCK:
            return setType(e, &tyBool);
        default:
            return setType(e, &tyVoid);
        }
    }
    error(c, e->a, "%s has no method %s", base->name, name);
    return setType(e, &tyError);
}

/* The function of std/str that s.name(...) calls on a str or String: a
 * pub one whose first parameter is the text, or NULL. */
Node *textMethod(Program *prog, const char *name) {
    Module *m = prog ? prog->text : NULL;
    for (int f = 0; m && f < m->files.len; f++) {
        Node *file = m->files.items[f];
        for (int i = 0; i < file->list.len; i++) {
            Node *d = file->list.items[i];
            if (d->kind != N_FUNC || !(d->flags & F_PUB) || (d->flags & (F_INTRINSIC | F_EXTERN)) || d->b ||
                !d->name || strcmp(d->name, name) != 0) {
                continue;
            }
            if (d->list.len && d->list.items[0]->type && isStr(d->list.items[0]->type)) return d;
        }
    }
    return NULL;
}

/* A method call: v.m(...). The receiver becomes the first argument, as a
 * pointer. */
static Type *checkMethodCall(Checker *c, Node *e) {
    Node *callee = e->a;
    Type *t = checkExpr(c, &callee->a, NULL);
    if (isErr(t)) {
        for (int i = 0; i < e->list.len; i++) {
            Node **a = &e->list.items[i];
            checkExpr(c, (*a)->kind == N_NAMED_ARG ? &(*a)->a : a, NULL);
        }
        return setType(e, &tyError);
    }
    Node *recv = callee->a;
    const char *name = callee->name;

    /* built-in methods of str and String */
    if ((isStr(t) || t->kind == TY_STRING) && strcmp(name, "copy") == 0) {
        if (e->list.len) {
            error(c, e, "copy takes no arguments");
        }
        if (t->kind == TY_STRING) {
            coerce(c, &callee->a, &tyStr, "the text");
        }
        e->builtin = BI_STR_COPY;
        return setType(e, &tyString);
    }
    if (isStr(t) || t->kind == TY_STRING) {
        /* the functions of std/str, as methods: s.startsWith("/api") is str.startsWith(s, "/api") */
        Node *fn = strcmp(name, "append") && strcmp(name, "free") ? textMethod(c->prog, name) : NULL;
        if (fn) {
            if (t->kind == TY_STRING) {
                coerce(c, &callee->a, &tyStr, "the text");
            }
            NodeList args = {0};
            listPush(c->arena, &args, callee->a);
            checkArgs(c, e, fn, &e->list, 1, &args, false);
            e->list = args;
            e->ref = fn;
            callee->ref = fn;
            return setType(e, fn->type);
        }
    }
    if (t->kind == TY_STRING || (t->kind == TY_PTR && t->elem->kind == TY_STRING)) {
        if (strcmp(name, "append") == 0 || strcmp(name, "free") == 0) {
            if (t->kind == TY_STRING && !addressable(recv)) {
                error(c, recv, "%s changes the String, so it must be called on a variable", name);
                return setType(e, &tyError);
            }
            if (name[0] == 'a') {
                if (e->list.len != 1 || e->list.items[0]->kind == N_NAMED_ARG) {
                    error(c, e, "append takes one argument, the text to add");
                    return setType(e, &tyError);
                }
                if (!checkTempText(c, &e->list.items[0], &tyStr)) {
                    Type *at = checkExpr(c, &e->list.items[0], &tyStr);
                    if (!isErr(at)) {
                        coerce(c, &e->list.items[0], &tyStr, "the text to add");
                        markTempString(&e->list.items[0], &tyStr);
                    }
                }
                e->builtin = BI_STRING_APPEND;
            } else {
                if (e->list.len) {
                    error(c, e, "free takes no arguments");
                }
                e->builtin = BI_STRING_FREE;
            }
            return setType(e, &tyVoid);
        }
    }

    {
        Type *base = t->kind == TY_PTR ? t->elem : t;
        if (base->kind == TY_ATOMIC || base->kind == TY_MUTEX || base->kind == TY_THREAD) {
            return checkSyncMethod(c, e, t, name);
        }
        bool generatedFree = isJsonStruct(base) || (base->kind == TY_STRUCT && base->decl && (base->decl->flags & (F_ENV | F_CLI)));
        if (generatedFree && strcmp(name, "free") == 0 && !findMethodIn(base, "free")) {
            /* the free @json gives a struct: its Strings and slices */
            if (t->kind != TY_PTR && !addressable(recv)) {
                error(c, recv, "free changes the value, so it must be called on a variable");
                return setType(e, &tyError);
            }
            if (e->list.len) {
                error(c, e, "free takes no arguments");
            }
            e->builtin = BI_JSON_FREE;
            return setType(e, &tyVoid);
        }
    }

    if (t->kind == TY_DYN) {
        Node *iface = t->decl;
        for (int i = 0; i < iface->list.len; i++) {
            Node *m = iface->list.items[i];
            if (strcmp(m->name, name) != 0) continue;
            NodeList args = {0};
            listPush(c->arena, &args, recv);
            Node fn = {0};
            fn.name = m->name;
            fn.list = m->list;
            checkArgs(c, e, &fn, &e->list, 0, &args, false);
            e->list = args;
            e->builtin = BI_DYN_CALL;
            e->ival = (uint64_t)i;
            e->ref = m;
            return setType(e, m->type);
        }
        error(c, callee, "%s has no method %s", iface->name, name);
        return setType(e, &tyError);
    }
    if (t->kind == TY_ENUM && t->isFlags && strcmp(name, "has") == 0) {
        if (e->list.len != 1 || e->list.items[0]->kind == N_NAMED_ARG) {
            error(c, e, "has takes one value: p.has(.write)");
            return setType(e, &tyError);
        }
        checkExpr(c, &e->list.items[0], t);
        coerce(c, &e->list.items[0], t, "the value");
        e->builtin = BI_FLAGS_HAS;
        return setType(e, &tyBool);
    }
    Type *st = structOf(t);
    if (!st) {
        error(c, callee, "%s has no method %s", t->name, name);
        return setType(e, &tyError);
    }
    resolveStruct(c, st);
    Node *path[16];
    int n;
    Node *savedSite = gUseSite;
    gUseSite = e;
    Node *m = findMethod(st, name, path, &n, 16);
    gUseSite = savedSite;
    if (!m) {
        Node *fpath[16];
        if (findField(st, name, fpath, 16)) {
            Type *ft = checkExpr(c, &e->a, NULL);
            if (!isErr(ft) && ft->kind == TY_FUNC) {
                return checkIndirectCall(c, e, ft);
            }
            if (!isErr(ft)) {
                error(c, callee, "the field %s is %s, not a function", name, ft->name);
            }
        } else {
            error(c, callee, "%s has no method %s", st->name, name);
        }
        return setType(e, &tyError);
    }
    for (int i = 0; i < n; i++) {
        recv = fieldStep(c, recv, path[i], callee);
    }
    if (recv->type->kind != TY_PTR) {
        if (!addressable(recv)) {
            /* a temporary: the IR writer stores it first */
            recv->flags |= F_EXPLICIT;
        }
        Node *addr = like(c, N_UNARY, recv);
        addr->op = P_AMP;
        addr->a = recv;
        addr->type = ptrTo(recv->type);
        recv = addr;
    }
    callee->ref = m;
    if (!visible(c, m)) {
        error(c, callee, "the method %s of %s isn't pub, so only its own module can call it", name, st->name);
    }
    NodeList args = {0};
    listPush(c->arena, &args, recv);
    checkArgs(c, e, m, &e->list, 1, &args, false);
    e->list = args;
    e->ref = m;
    return setType(e, m->type);
}

/* alloc(T, n) gives a T[] of n zeroed items. */
static Type *checkAlloc(Checker *c, Node *e) {
    e->builtin = BI_ALLOC;
    if (e->list.len != 2 || e->list.items[0]->kind == N_NAMED_ARG || e->list.items[1]->kind == N_NAMED_ARG) {
        error(c, e, "alloc takes a type and a count: alloc(u8, 4096)");
        return setType(e, &tyError);
    }
    Node *tn = e->list.items[0];
    if (tn->kind != N_IDENT && tn->kind != N_INDEX) {
        error(c, tn, "alloc's first argument is a type name");
        return setType(e, &tyError);
    }
    Type *t = typeFromExpr(c, tn, true);
    if (isErr(t)) return setType(e, t);
    if (t == &tyVoid) {
        error(c, tn, "void has no size");
        return setType(e, &tyError);
    }
    resolveStruct(c, t);
    tn->type = t;
    Type *tc = checkExpr(c, &e->list.items[1], &tyI64);
    if (!isErr(tc)) {
        if (!isInt(tc)) {
            error(c, e->list.items[1], "the count must be an integer, not %s", tc->name);
        } else {
            coerce(c, &e->list.items[1], &tyI64, "the count");
        }
    }
    return setType(e, sliceOf(t));
}

static Type *checkTry(Checker *c, Node *e) {
    c->allowResult = true;
    Type *t = checkExpr(c, &e->a, NULL);
    if (isErr(t)) return setType(e, t);
    if (t->kind != TY_RESULT) {
        error(c, e, "try needs a call that can fail (one that returns !T), not %s", t->name);
        return setType(e, &tyError);
    }
    if (c->inDefer) {
        error(c, e, "a deferred statement can't use try; handle the error with catch");
    } else if (!c->func || c->func->type->kind != TY_RESULT) {
        error(c, e, "try passes the error to the caller, so %s must return !T; use catch to handle it here",
              c->func ? c->func->name : "this function");
    }
    return setType(e, t->elem);
}

static void coerceValue(Checker *c, Node *body, Type *t);

static Type *checkCatch(Checker *c, Node *e, Type *expected) {
    c->allowResult = true;
    Type *t = checkExpr(c, &e->a, NULL);
    if (!isErr(t) && t->kind != TY_RESULT) {
        error(c, e, "catch needs a call that can fail (one that returns !T), not %s", t->name);
        t = &tyError;
    }
    Type *value = t->kind == TY_RESULT ? t->elem : &tyError;
    if (e->b) {
        if (value == &tyVoid) {
            error(c, e->b, "this call gives no value, so catch can't give one; use catch e { ... }");
            checkExpr(c, &e->b, NULL);
            return setType(e, &tyVoid);
        }
        checkExpr(c, &e->b, isErr(value) ? expected : value);
        if (!isErr(value)) {
            coerce(c, &e->b, value, "the value after catch");
        }
        return setType(e, value);
    }
    /* catch e { ... } or catch e match ... : e is the Error */
    Node *var = like(c, N_IDENT, e);
    var->name = e->name;
    var->type = &tyErrorValue;
    e->d = var;
    pushScope(c, false);
    declareLocal(c, e->name, var);
    Type *res;
    if (e->c->kind == N_MATCH) {
        res = value == &tyVoid || isErr(value) ? checkMatch(c, e->c, NULL, false) : checkMatch(c, e->c, value, true);
        if (value == &tyVoid) res = &tyVoid;
        e->c->type = res;
    } else if (value == &tyVoid || isErr(value)) {
        checkBlock(c, e->c, true);
        res = isErr(value) ? &tyError : &tyVoid;
    } else {
        res = checkBlockValue(c, e->c, value);
        if (!isErr(res) && res->kind != TY_NORETURN) {
            coerceValue(c, e->c, value);
        }
        res = value;
    }
    popScope(c);
    return setType(e, res);
}

/* A call of a generic function: the types come from [...] after its name,
 * or are worked out from the arguments. */
static Type *checkGenericCall(Checker *c, Node *e, Node *d, Node *typeArgs) {
    int n = d->list2.len;
    Type *args[16] = {0};
    if (n > 16) {
        error(c, e, "too many type parameters");
        return setType(e, &tyError);
    }
    if (typeArgs) {
        if (typeArgs->list.len != n) {
            error(c, typeArgs, "%s takes %d type%s, not %d", d->name, n, n == 1 ? "" : "s", typeArgs->list.len);
            return setType(e, &tyError);
        }
        for (int i = 0; i < n; i++) {
            args[i] = typeFromExpr(c, typeArgs->list.items[i], true);
            if (isErr(args[i])) return setType(e, &tyError);
        }
    }
    bool bad = false;
    for (int i = 0; !typeArgs && i < e->list.len; i++) {
        /* the types come from the arguments: look at them first */
        Node *arg = e->list.items[i];
        Node **slot = arg->kind == N_NAMED_ARG ? &arg->a : &e->list.items[i];
        if (checkTempText(c, slot, &tyStr)) {
            continue;                   /* text with values: a str built for the call */
        }
        if (isErr(checkExpr(c, slot, NULL))) bad = true;
    }
    if (bad) return setType(e, &tyError);
    if (!typeArgs) {
        int pos = 0;
        for (int i = 0; i < e->list.len; i++) {
            Node *arg = e->list.items[i];
            Node *param = NULL;
            if (arg->kind == N_NAMED_ARG) {
                for (int j = 0; j < d->list.len; j++) {
                    if (strcmp(d->list.items[j]->name, arg->name) == 0) param = d->list.items[j];
                }
                if (param) infer(param->a, arg->a->type, &d->list2, args);
            } else if (pos < d->list.len) {
                param = d->list.items[pos];
                if (param->flags & F_REST) {
                    infer(param->a->a, arg->type, &d->list2, args);     /* T... takes each one as a T */
                } else {
                    pos++;
                    infer(param->a, arg->type, &d->list2, args);
                }
            }
        }
        for (int i = 0; i < n; i++) {
            if (!args[i]) {
                error(c, e, "the type %s of %s can't be worked out from the arguments; write %s[...](...)",
                      d->list2.items[i]->name, d->name, d->name);
                return setType(e, &tyError);
            }
        }
    }
    int before = gErrors;
    Node *inst = instantiateFunc(c, d, args, n, e);
    if (!inst) return setType(e, &tyError);
    if (gErrors != before) {
        return setType(e, &tyError);
    }
    e->a->ref = inst;
    NodeList out = {0};
    /* with the types written out, the arguments are checked against the
     * parameters, so [.int(1)] and .red know their type */
    checkArgs(c, e, inst, &e->list, 0, &out, !typeArgs);
    e->list = out;
    e->ref = inst;
    return setType(e, inst->type);
}

/* A call through a function value: the arguments by position. */
static Type *checkIndirectCall(Checker *c, Node *e, Type *ft) {
    if (e->list.len != ft->nelems) {
        error(c, e, "this function takes %d argument%s, not %d", ft->nelems, ft->nelems == 1 ? "" : "s",
              e->list.len);
        return setType(e, &tyError);
    }
    for (int i = 0; i < e->list.len; i++) {
        Node **slot = &e->list.items[i];
        if ((*slot)->kind == N_NAMED_ARG) {
            error(c, *slot, "a call through a function value takes its arguments by position");
            continue;
        }
        if (checkTempText(c, slot, ft->elems[i])) {
            continue;
        }
        checkExpr(c, slot, ft->elems[i]);
        coerce(c, slot, ft->elems[i], "the argument");
        markTempString(slot, ft->elems[i]);
    }
    e->builtin = BI_INDIRECT;
    return setType(e, ft->elem);
}

/* Packet.Data(bytes) or .Close(1000, "bye"): a value of a data enum. */
static Type *checkEnumNew(Checker *c, Node *e, Type *et, const char *name) {
    Node *m = enumMember(et, name);
    if (!m) {
        error(c, e->a, "%s has no value named %s", et->name, name);
        return setType(e, &tyError);
    }
    if (!memberData(m)) {
        error(c, e->a, "%s.%s carries no data; write it without ()", et->name, name);
        return setType(e, &tyError);
    }
    Node fn = {0};
    fn.name = m->name;
    fn.list = m->list;
    NodeList args = {0};
    checkArgs(c, e, &fn, &e->list, 0, &args, false);
    e->list = args;
    e->ref = m;
    e->builtin = BI_ENUM_NEW;
    return setType(e, et);
}

/* The type named by a call's argument: json.decode(User, text). */
static Type *typeArgument(Checker *c, Node *a) {
    if (a->kind == N_IDENT) {
        return typeFromName(c, a->name, a, true);
    }
    if (a->kind == N_T_SLICE) {
        return resolveType(c, a);
    }
    if (a->kind == N_INDEX) {
        return typeFromExpr(c, a, true);
    }
    error(c, a, "this should be a type's name: json.decode(User, text)");
    return &tyError;
}

/* Whether json.decode can fill a value of type t: str points into memory
 * it doesn't own, so text must be a String. */
static bool jsonDecodable(Checker *c, Type *t, Node *at) {
    if (isStr(t)) {
        error(c, at, "a str field can't be filled from JSON or a row, since a str can't own its text; use a String");
        return false;
    }
    if (t->kind == TY_SLICE || t->kind == TY_ARRAY) {
        return jsonDecodable(c, t->elem, at);
    }
    if (isJsonList(t)) {
        return jsonDecodable(c, t->args[0], at);
    }
    if (isJsonStruct(t)) {
        for (int i = 0; i < t->decl->list.len; i++) {
            Node *f = t->decl->list.items[i];
            if (f->type && !jsonDecodable(c, f->type, at)) {
                return false;
            }
        }
    }
    return true;
}

/* json.encode(v) and json.decode(T, text). */
static Type *checkIntrinsic(Checker *c, Node *e, Node *d) {
    for (int i = 0; i < e->list.len; i++) {
        if (e->list.items[i]->kind == N_NAMED_ARG) {
            error(c, e->list.items[i], "%s takes its arguments by position", d->name);
            return setType(e, &tyError);
        }
    }
    if (strcmp(d->name, "format") == 0 && d->module && strcmp(d->module, "str") == 0) {
        /* str.format: the builtin format, by the module's name */
        checkTextArg(c, e, "str.format");
        e->builtin = BI_FORMAT;
        return setType(e, &tyString);
    }
    if (strcmp(d->name, "encode") == 0) {
        if (e->list.len != 1) {
            error(c, e, "json.encode takes one value");
            return setType(e, &tyError);
        }
        Type *t = checkExpr(c, &e->list.items[0], NULL);
        if (isErr(t)) return setType(e, &tyError);
        if (t->kind == TY_STRUCT && !isJsonStruct(t) && !isJsonList(t)) {
            error(c, e->list.items[0], "%s needs @json to be turned into JSON", t->name);
            return setType(e, &tyError);
        }
        if (!jsonType(c, t, e->list.items[0], "this value")) return setType(e, &tyError);
        e->builtin = BI_JSON_ENCODE;
        return setType(e, &tyString);
    }
    if ((strcmp(d->name, "parse") == 0 || strcmp(d->name, "parseArgs") == 0) && d->module &&
        strcmp(d->module, "flags") == 0) {
        /* flags.parse(Options), flags.parseArgs(Options, args, name) */
        bool own = strcmp(d->name, "parse") == 0;
        if (own ? e->list.len != 1 : (e->list.len < 2 || e->list.len > 3)) {
            error(c, e, own ? "flags.parse takes the struct's type: flags.parse(Options)"
                            : "flags.parseArgs takes the struct's type, the arguments and the program's name: flags.parseArgs(Options, args, \"tool add\")");
            return setType(e, &tyError);
        }
        Type *t = typeArgument(c, e->list.items[0]);
        if (isErr(t)) return setType(e, &tyError);
        if (t->kind == TY_STRUCT) resolveStruct(c, t);
        if (t->kind != TY_STRUCT || !(t->decl->flags & F_CLI)) {
            error(c, e->list.items[0], "%s needs @flags to be read from the arguments", t->name);
            return setType(e, &tyError);
        }
        Node *into = lookup(c, own ? "flags.parseMain" : "flags.parseInto");
        if (!into) {
            error(c, e, "flags.%s needs the flags module's %s", d->name, own ? "parseMain" : "parseInto");
            return setType(e, &tyError);
        }
        if (!own) {
            checkExpr(c, &e->list.items[1], sliceOf(&tyStr));
            coerce(c, &e->list.items[1], sliceOf(&tyStr), "the arguments");
            if (e->list.len == 3) {
                checkExpr(c, &e->list.items[2], &tyStr);
                coerce(c, &e->list.items[2], &tyStr, "the program's name");
            }
        }
        e->ref = into;
        Node *lit = like(c, N_STRUCT_LIT, e);
        checkExpr(c, &lit, t);
        e->b = lit;
        e->c = like(c, N_T_NAME, e);
        e->c->type = t;
        e->builtin = BI_FLAGS_PARSE;
        return setType(e, own ? t : resultOf(t));
    }
    if ((strcmp(d->name, "load") == 0 || strcmp(d->name, "loadWith") == 0) && d->module &&
        strcmp(d->module, "env") == 0) {
        /* env.load(Settings), env.loadWith(Settings, ".env") */
        bool withFile = strcmp(d->name, "loadWith") == 0;
        if (!withFile && e->list.len != 1) {
            error(c, e, "env.load takes the struct's type: env.load(Settings)");
            return setType(e, &tyError);
        }
        if (withFile && e->list.len != 2) {
            error(c, e, "env.loadWith takes the struct's type and a file: env.loadWith(Settings, \".env\")");
            return setType(e, &tyError);
        }
        Type *t = typeArgument(c, e->list.items[0]);
        if (isErr(t)) return setType(e, &tyError);
        if (t->kind == TY_STRUCT) resolveStruct(c, t);
        if (t->kind != TY_STRUCT || !(t->decl->flags & F_ENV)) {
            error(c, e->list.items[0], "%s needs @env to be read from the environment", t->name);
            return setType(e, &tyError);
        }
        if (withFile) {
            Node **path = &e->list.items[1];
            if (!checkTempText(c, path, &tyStr)) {
                Type *pt = checkExpr(c, path, &tyStr);
                if (isErr(pt)) return setType(e, &tyError);
                coerce(c, path, &tyStr, "the file's path");
                markTempString(path, &tyStr);
            }
        }
        Node *into = lookup(c, withFile ? "env.loadFileInto" : "env.loadInto");
        if (!into) {
            error(c, e, "env.%s needs the env module's %s", d->name, withFile ? "loadFileInto" : "loadInto");
            return setType(e, &tyError);
        }
        e->ref = into;
        Node *lit = like(c, N_STRUCT_LIT, e);
        checkExpr(c, &lit, t);
        e->b = lit;
        e->c = like(c, N_T_NAME, e);
        e->c->type = t;
        e->builtin = BI_ENV_LOAD;
        return setType(e, resultOf(t));
    }
    if (strcmp(d->name, "decode") == 0) {
        if (e->list.len != 2) {
            error(c, e, "json.decode takes a type and the text: json.decode(User, text)");
            return setType(e, &tyError);
        }
        Type *t = typeArgument(c, e->list.items[0]);
        if (isErr(t)) return setType(e, &tyError);
        if (t->kind == TY_STRUCT) resolveStruct(c, t);
        if (t->kind == TY_STRUCT && !isJsonStruct(t) && !isJsonList(t)) {
            error(c, e->list.items[0], "%s needs @json to be read from JSON", t->name);
            return setType(e, &tyError);
        }
        if (!jsonType(c, t, e->list.items[0], "this value") || !jsonDecodable(c, t, e->list.items[0])) {
            return setType(e, &tyError);
        }
        Node **text = &e->list.items[1];
        Type *tt = checkExpr(c, text, &tyStr);
        if (isErr(tt)) return setType(e, &tyError);
        coerce(c, text, &tyStr, "the text");
        markTempString(text, &tyStr);
        Node *into = lookup(c, "json.decodeInto");
        if (!into) {
            error(c, e, "json.decode needs the json module's decodeInto");
            return setType(e, &tyError);
        }
        e->ref = into;
        e->a->ref = d;
        /* the starting value: the defaults of the struct's fields */
        if (t->kind == TY_STRUCT) {
            Node *lit = like(c, N_STRUCT_LIT, e);
            checkExpr(c, &lit, t);
            e->b = lit;
        }
        e->builtin = BI_JSON_DECODE;
        e->ival = 0;
        Type *rt = resultOf(t);
        e->c = like(c, N_T_NAME, e);
        e->c->type = t;
        return setType(e, rt);
    }
    if (strcmp(d->name, "tableOf") == 0) {
        /* the field table of a @json struct, for the runtime to fill it */
        if (e->list.len != 1) {
            error(c, e, "tableOf takes a pointer to a @json struct");
            return setType(e, &tyError);
        }
        Type *t = checkExpr(c, &e->list.items[0], NULL);
        if (isErr(t)) return setType(e, &tyError);
        Type *st = t->kind == TY_PTR ? t->elem : NULL;
        if (!st || st->kind != TY_STRUCT) {
            error(c, e->list.items[0], "this should be a pointer to a struct, not %s", t->name);
            return setType(e, &tyError);
        }
        resolveStruct(c, st);
        if (!isJsonStruct(st)) {
            error(c, e->list.items[0], "%s needs @json to be filled from a row", st->name);
            return setType(e, &tyError);
        }
        if (!jsonDecodable(c, st, e->list.items[0])) return setType(e, &tyError);
        e->builtin = BI_TABLE_OF;
        return setType(e, ptrTo(&tyU8));
    }
    if (strcmp(d->name, "hashOf") == 0 || strcmp(d->name, "keyOwn") == 0 || strcmp(d->name, "keyFree") == 0) {
        /* Map's: the hash of a key, a key the map owns, and giving it back */
        if (e->list.len != 1) {
            error(c, e, "%s takes one key", d->name);
            return setType(e, &tyError);
        }
        Type *t = checkExpr(c, &e->list.items[0], NULL);
        if (isErr(t)) return setType(e, &tyError);
        bool ok = t->kind == TY_INT || t->kind == TY_BOOL || t->kind == TY_PTR || isStr(t) ||
                  (t->kind == TY_ENUM && !t->hasData);
        if (!ok) {
            error(c, e, "a Map's keys can be integers, bool, enums, pointers or str, not %s%s", t->name,
                  t->kind == TY_STRING ? " (use str: the map keeps its own copy)" : "");
            return setType(e, &tyError);
        }
        e->builtin = d->name[0] == 'h' ? BI_HASH : d->name[3] == 'O' ? BI_KEY_OWN : BI_KEY_FREE;
        return setType(e, e->builtin == BI_HASH ? &tyU64 : e->builtin == BI_KEY_OWN ? t : &tyVoid);
    }
    error(c, e, "%s is not a built-in the compiler knows", d->name);
    return setType(e, &tyError);
}

/* The value arguments an intrinsic didn't get to before an error, so the
 * editor still finds what their names stand for. Type names are left. */
static void checkLeftArgs(Checker *c, Node *e) {
    for (int i = 0; i < e->list.len; i++) {
        Node **a = &e->list.items[i];
        if ((*a)->kind == N_NAMED_ARG) a = &(*a)->a;
        if ((*a)->type || (*a)->kind == N_T_SLICE || (*a)->kind == N_INDEX) continue;
        if ((*a)->kind == N_IDENT) {
            Node *d = lookup(c, (*a)->name);
            if (!d || d->kind == N_STRUCT || d->kind == N_ENUM || d->kind == N_INTERFACE ||
                d->kind == N_TYPEDEF || d->kind == N_IMPORT) {
                continue;
            }
        }
        checkExpr(c, a, NULL);
    }
}

static Type *checkCall(Checker *c, Node *e, Type *expected) {
    Node *callee = e->a;
    if (callee->kind == N_FIELD_ACCESS && callee->a->kind == N_IDENT) {
        Node *d = lookup(c, callee->a->name);
        if (d && d->kind == N_ENUM) {
            Type *et = enumType(c, d);
            callee->a->ref = d;
            return checkEnumNew(c, e, et, callee->name);
        }
    }
    if (callee->kind == N_ENUM_DOT) {
        if (!expected || expected->kind != TY_ENUM) {
            error(c, callee, "the enum of .%s can't be worked out here; write the enum's name before the dot",
                  callee->name);
            return setType(e, &tyError);
        }
        return checkEnumNew(c, e, expected, callee->name);
    }
    if (callee->kind == N_FIELD_ACCESS) {
        /* str.upper(...) without import str: str is also a type */
        if (callee->a->kind == N_IDENT && !lookup(c, callee->a->name) &&
            (strcmp(callee->a->name, "str") == 0 || !typeFromName(c, callee->a->name, callee, false))) {
            notDeclared(c, callee->a, callee->a->name);
            return setType(e, &tyError);
        }
        return checkMethodCall(c, e);
    }
    if (callee->kind == N_INDEX && callee->a->kind == N_IDENT) {
        Node *g = lookup(c, callee->a->name);
        if (g && g->kind == N_FUNC && g->list2.len) {
            return checkGenericCall(c, e, g, callee);
        }
    }
    if (callee->kind != N_IDENT) {
        Type *ft = checkExpr(c, &e->a, NULL);
        if (isErr(ft)) return setType(e, &tyError);
        if (ft->kind != TY_FUNC) {
            error(c, callee, "this is %s, not a function", ft->name);
            return setType(e, &tyError);
        }
        return checkIndirectCall(c, e, ft);
    }
    const char *name = callee->name;
    Node *d = lookup(c, name);
    if (d && d->kind == N_FUNC && isBuiltinName(name)) {
        /* a module's own error or format (log.error, term.format) is
         * called with the module's name; a bare one is the builtin */
        d = NULL;
    }
    if (d && d->kind == N_FUNC && (d->flags & F_INTRINSIC)) {
        callee->ref = d;    /* go to definition and hover, also when the call is wrong */
        Type *t = checkIntrinsic(c, e, d);
        if (isErr(t)) checkLeftArgs(c, e);
        return t;
    }

    if (!d && isBuiltinName(name)) {
        if (strcmp(name, "alloc") == 0) {
            return checkAlloc(c, e);
        }
        if (strcmp(name, "expect") == 0) {
            /* expect(cond) or expect(cond, "text"): in a test, a failure is reported and the test goes on */
            e->builtin = BI_EXPECT;
            if (!c->func || !(c->func->flags & F_TESTFN)) {
                error(c, e, "expect is for test blocks: test \"name\" { expect(...) }");
            }
            if (e->list.len < 1 || e->list.len > 2 || e->list.items[0]->kind == N_NAMED_ARG) {
                error(c, e, "expect takes a condition and, if wanted, a message: expect(n == 3, \"n is {n}\")");
                return setType(e, &tyError);
            }
            Type *t = checkExpr(c, &e->list.items[0], &tyBool);
            if (!isErr(t) && t != &tyBool) {
                error(c, e->list.items[0], "expect takes a bool, not %s", t->name);
            }
            if (e->list.len == 2) {
                checkText(c, &e->list.items[1], "expect");
            }
            return setType(e, &tyVoid);
        }
        if (strcmp(name, "sleep") == 0 || strcmp(name, "after") == 0) {
            e->builtin = name[0] == 's' ? BI_SLEEP : BI_AFTER;
            if (e->list.len != 1 || e->list.items[0]->kind == N_NAMED_ARG) {
                error(c, e, "%s takes one Duration: %s(200ms)", name, name);
                return setType(e, &tyError);
            }
            Type *t = checkExpr(c, &e->list.items[0], &tyDuration);
            if (!isErr(t)) {
                if (!isInt(t)) {
                    error(c, e->list.items[0], "%s takes a Duration, not %s", name, t->name);
                } else {
                    coerce(c, &e->list.items[0], &tyI64, "the time");
                }
            }
            if (e->builtin == BI_AFTER) {
                error(c, e, "after is an arm of a select: select { after(5s) => ... }");
            }
            return setType(e, &tyVoid);
        }
        if (strcmp(name, "error") == 0) {
            /* error(code) or error(code, text): an enum value, or the code of
             * another error */
            e->builtin = BI_ERROR;
            if (e->list.len < 1 || e->list.len > 2 || e->list.items[0]->kind == N_NAMED_ARG) {
                error(c, e, "error takes a code and, if wanted, a message: error(ParseError.empty, \"...\")");
                return setType(e, &tyError);
            }
            Type *t = checkExpr(c, &e->list.items[0], NULL);
            if (!isErr(t)) {
                if (t->kind == TY_ENUM || t->kind == TY_ERRCODE) {
                    coerce(c, &e->list.items[0], &tyErrCode, "the code");
                } else {
                    error(c, e->list.items[0], "an error's code is a value from an enum, not %s", t->name);
                }
            }
            if (e->list.len == 2) {
                if (e->list.items[1]->kind == N_NAMED_ARG) {
                    error(c, e->list.items[1], "error's message has no name");
                } else {
                    checkText(c, &e->list.items[1], "error");
                }
            }
            return setType(e, &tyErrorValue);
        }
        if (strcmp(name, "free") == 0) {
            e->builtin = BI_FREE;
            if (e->list.len != 1 || e->list.items[0]->kind == N_NAMED_ARG) {
                error(c, e, "free takes one argument");
                return setType(e, &tyVoid);
            }
            Type *t = checkExpr(c, &e->list.items[0], NULL);
            if (!isErr(t) && t->kind != TY_SLICE && t->kind != TY_PTR) {
                if (t->kind == TY_STRING) {
                    error(c, e->list.items[0], "a String is freed with its free method: s.free()");
                } else {
                    error(c, e->list.items[0], "free takes a slice from alloc, not %s", t->name);
                }
            }
            return setType(e, &tyVoid);
        }
        checkTextArg(c, e, name);
        if (name[0] == 'f') {
            e->builtin = BI_FORMAT;
            return setType(e, &tyString);
        }
        e->builtin = name[1] == 'a' ? BI_PANIC : BI_PRINT;
        return setType(e, name[1] == 'a' ? &tyNoreturn : &tyVoid);
    }

    Type *conv = !d ? intTypeNamed(name) : NULL;
    if (conv) {
        if (e->list.len != 1 || e->list.items[0]->kind == N_NAMED_ARG) {
            error(c, e, "%s(...) converts one value", name);
            return setType(e, &tyError);
        }
        Type *t = checkExpr(c, &e->list.items[0], NULL);
        if (isErr(t)) return setType(e, &tyError);
        if (!isNumber(t)) {
            error(c, e->list.items[0], "%s(...) converts numbers, not %s", name, t->name);
            return setType(e, &tyError);
        }
        Node *arg = e->list.items[0];
        if (arg->isConst && (isFloat(conv) || isFloat(t))) {
            if (isFloat(conv)) {
                coerce(c, &e->list.items[0], conv, "the value");
                *e = *e->list.items[0];
                return conv;
            }
            /* a float constant to an integer: done when the program runs */
        } else if (arg->isConst) {
            if (!fitsIn(arg->cval, conv)) {
                error(c, arg, "%lld doesn't fit in %s", (long long)arg->cval, conv->name);
            }
            arg->type = conv;
            *e = *arg;              /* the call becomes the constant */
            return conv;
        }
        Node *n = convertNode(c, arg, conv, true);
        int line = e->line, col = e->col;
        *e = *n;
        e->line = line;
        e->col = col;
        return conv;
    }

    if (!d) {
        if (typeFromName(c, name, callee, false)) {
            error(c, callee, "%s is a type; a struct is made with %s{...}", name, name);
        } else if (!(callee->flags & F_REPORTED)) {
            error(c, callee, "%s is not declared", name);
        }
        return setType(e, &tyError);
    }
    if (d->kind == N_IMPORT || d->kind == N_IMPORT_ITEM) {
        error(c, callee, "%s is a module, not a function", name);
        return setType(e, &tyError);
    }
    if (d->kind != N_FUNC) {
        Type *ft = checkExpr(c, &e->a, NULL);
        if (isErr(ft)) return setType(e, &tyError);
        if (ft->kind != TY_FUNC) {
            error(c, callee, "%s is not a function", name);
            return setType(e, &tyError);
        }
        return checkIndirectCall(c, e, ft);
    }
    if (d->list2.len) {
        return checkGenericCall(c, e, d, NULL);
    }
    callee->ref = d;
    NodeList args = {0};
    checkArgs(c, e, d, &e->list, 0, &args, false);
    e->list = args;
    e->ref = d;
    (void)expected;
    return setType(e, d->type);
}

/* ---------------------------------------------------------------- control flow */

/* The value of a block used as an expression: its last statement. */
static Type *checkBlockValue(Checker *c, Node *block, Type *expected) {
    if (block->kind != N_BLOCK) {
        /* a match arm body that is a single statement */
        if (block->kind == N_EXPR_STMT) {
            return checkExpr(c, &block->a, expected);
        }
        if (block->kind == N_RETURN || block->kind == N_BREAK || block->kind == N_CONTINUE) {
            checkStmt(c, block);
            return &tyNoreturn;
        }
        error(c, block, "this arm has to give a value");
        checkStmt(c, block);
        return &tyError;
    }
    pushScope(c, false);
    Type *t = &tyVoid;
    for (int i = 0; i < block->list.len; i++) {
        Node *s = block->list.items[i];
        bool last = i == block->list.len - 1;
        if (last && s->kind == N_EXPR_STMT) {
            t = checkExpr(c, &s->a, expected);
            s->type = t;
        } else {
            checkStmt(c, s);
            if (last) {
                t = (s->kind == N_RETURN || s->kind == N_BREAK || s->kind == N_CONTINUE) ? &tyNoreturn : &tyVoid;
            }
        }
    }
    popScope(c);
    block->type = t;
    if (t == &tyVoid) {
        error(c, block, "this block has to end with a value");
        return &tyError;
    }
    return t;
}

/* Brings the values of two branches to one type. */
static Type *unify(Checker *c, Type *ta, Type *tb, Node *at) {
    if (isErr(ta) || isErr(tb)) return &tyError;
    if (ta->kind == TY_NORETURN) return tb;
    if (tb->kind == TY_NORETURN) return ta;
    if (sameType(ta, tb)) return ta;
    if (widens(ta, tb)) return tb;
    if (widens(tb, ta)) return ta;
    if (ta->kind == TY_NULL && tb->kind == TY_PTR) return tb;
    if (tb->kind == TY_NULL && ta->kind == TY_PTR) return ta;
    error(c, at, "the branches give different types: %s and %s", ta->name, tb->name);
    return &tyError;
}

/* The last expression of a value block, so it can be converted. */
static Node **valueSlot(Node *body) {
    if (body->kind == N_EXPR_STMT) {
        return &body->a;
    }
    if (body->kind == N_BLOCK && body->list.len) {
        Node *last = body->list.items[body->list.len - 1];
        if (last->kind == N_EXPR_STMT) {
            return &last->a;
        }
    }
    return NULL;
}

static void coerceValue(Checker *c, Node *body, Type *t) {
    Node **slot = valueSlot(body);
    if (slot && (*slot)->type && (*slot)->type->kind != TY_NORETURN) {
        coerce(c, slot, t, "the value");
    }
}

static void checkCondition(Checker *c, Node **slot) {
    checkExpr(c, slot, &tyBool);
    if (!isErr((*slot)->type) && (*slot)->type != &tyBool) {
        error(c, *slot, "a condition must be bool, not %s", (*slot)->type->name);
    }
}

static Type *checkIf(Checker *c, Node *n, Type *expected, bool wantValue) {
    checkCondition(c, &n->a);
    if (!wantValue) {
        checkBlock(c, n->c, true);
        if (n->d) {
            if (n->d->kind == N_IF) {
                checkIf(c, n->d, NULL, false);
            } else {
                checkBlock(c, n->d, true);
            }
        }
        return &tyVoid;
    }
    if (!n->d) {
        error(c, n, "an 'if' that gives a value needs an 'else'");
        checkBlockValue(c, n->c, expected);
        return &tyError;
    }
    Type *ta = checkBlockValue(c, n->c, expected);
    Type *hint = expected ? expected : (ta->kind == TY_NORETURN || isErr(ta) ? NULL : ta);
    Type *tb = n->d->kind == N_IF ? setType(n->d, checkIf(c, n->d, hint, true)) : checkBlockValue(c, n->d, hint);
    Type *t = expected && !isErr(expected) ? expected : unify(c, ta, tb, n);
    if (!isErr(t)) {
        coerceValue(c, n->c, t);
        if (n->d->kind != N_IF) {
            coerceValue(c, n->d, t);
        } else if (n->d->type && !isErr(n->d->type) && !sameType(n->d->type, t)) {
            n->d->type = t;
        }
    }
    return t;
}

static bool isWildcard(Node *p) { return p->kind == N_IDENT && strcmp(p->name, "_") == 0; }

/* The enum value a pattern names: Ping, .Ping, Packet.Ping, or the callee
 * of Data(bytes). */
static Node *patternMember(Checker *c, Node *p, Type *subject) {
    if (subject->kind != TY_ENUM) {
        return NULL;
    }
    Node *name = p->kind == N_CALL ? p->a : p;
    if (name->kind == N_IDENT) {
        Node *local = lookup(c, name->name);
        if (local && local->kind != N_FUNC && local->kind != N_ENUM) {
            return NULL;
        }
        return enumMember(subject, name->name);
    }
    if (name->kind == N_ENUM_DOT) {
        return enumMember(subject, name->name);
    }
    if (name->kind == N_FIELD_ACCESS && name->a->kind == N_IDENT && strcmp(name->a->name, subject->name) == 0) {
        return enumMember(subject, name->name);
    }
    return NULL;
}

/* A pattern for an enum value; Data(bytes) also declares bytes. */
static bool checkEnumPattern(Checker *c, Node *p, Type *subject, bool single) {
    Node *m = patternMember(c, p, subject);
    if (!m) {
        Node *name = p->kind == N_CALL ? p->a : p;
        if (name->kind == N_IDENT || name->kind == N_ENUM_DOT ||
            (name->kind == N_FIELD_ACCESS && name->a->kind == N_IDENT && strcmp(name->a->name, subject->name) == 0)) {
            error(c, p, "%s has no value named %s", subject->name, name->name);
            p->type = &tyError;
            return true;
        }
        return false;
    }
    p->ref = m;
    p->type = subject;
    p->isConst = !subject->hasData;
    p->cval = m->cval;
    Type *data = memberData(m);
    if (p->kind != N_CALL) {
        if (data) {
            /* Data alone matches any Data value, without naming the fields */
            p->flags |= F_EXPLICIT;
        }
        return true;
    }
    if (!data) {
        error(c, p, "%s.%s carries no data, so it has nothing to name", subject->name, m->name);
        return true;
    }
    if (p->list.len != data->nelems) {
        error(c, p, "%s.%s carries %d value%s, but the pattern names %d", subject->name, m->name, data->nelems,
              data->nelems == 1 ? "" : "s", p->list.len);
        return true;
    }
    if (!single) {
        error(c, p, "a pattern that names values must be the only pattern in its arm");
        return true;
    }
    for (int i = 0; i < p->list.len; i++) {
        Node *b = p->list.items[i];
        if (b->kind != N_IDENT) {
            error(c, b, "a pattern names the values it takes apart; write a name or _");
            continue;
        }
        b->type = data->elems[i];
        if (strcmp(b->name, "_") != 0) {
            declareLocal(c, b->name, b);
        }
    }
    return true;
}

static void checkPattern(Checker *c, Node **slot, Type *subject, bool single) {
    Node *p = *slot;
    if (isWildcard(p)) {
        p->type = subject;
        return;
    }
    if (subject->kind == TY_ENUM && checkEnumPattern(c, p, subject, single)) {
        return;
    }
    if (subject->kind == TY_ENUM && subject->hasData) {
        error(c, p, "a pattern for %s names one of its values", subject->name);
        return;
    }
    if (p->kind == N_BINARY && p->op == P_DOTDOT) {
        if (!isInt(subject) && !isErr(subject)) {
            error(c, p, "a range only matches integers");
            return;
        }
        checkExpr(c, &p->a, subject);
        checkExpr(c, &p->b, subject);
        if (!p->a->isConst || !p->b->isConst) {
            error(c, p, "a range in a match needs constant ends");
            return;
        }
        coerce(c, &p->a, subject, "the start of the range");
        coerce(c, &p->b, subject, "the end of the range");
        if (p->a->cval > p->b->cval) {
            error(c, p, "this range is empty: %lld is above %lld", (long long)p->a->cval, (long long)p->b->cval);
        }
        p->type = subject;
        return;
    }
    Type *t = checkExpr(c, slot, subject);
    p = *slot;
    if (isErr(t) || isErr(subject)) {
        return;
    }
    if (!p->isConst && p->kind != N_STRING && !(p->kind == N_IDENT && p->ref && p->ref->kind == N_CONST)) {
        error(c, p, "a match pattern must be a constant");
        return;
    }
    coerce(c, slot, subject, "the pattern");
}

static Type *checkMatch(Checker *c, Node *m, Type *expected, bool wantValue) {
    Type *subject = checkExpr(c, &m->a, NULL);
    if (subject->kind == TY_STRING) {
        coerce(c, &m->a, &tyStr, "the text");
        subject = &tyStr;
    }
    if (!isErr(subject) && !isInt(subject) && subject != &tyBool && !isStr(subject) &&
        subject->kind != TY_ENUM && subject->kind != TY_ERRCODE) {
        error(c, m->a, "match works on integers, bool, str and enums, not %s", subject->name);
        subject = &tyError;
    }
    int covered = 0;
    bool hasWildcard = false, hasTrue = false, hasFalse = false;
    Type *t = expected && !isErr(expected) ? expected : NULL;
    Type *first = NULL;
    for (int i = 0; i < m->list.len; i++) {
        Node *arm = m->list.items[i];
        pushScope(c, false);            /* for the names a pattern takes apart */
        for (int j = 0; j < arm->list.len; j++) {
            Node **slot = &arm->list.items[j];
            if (hasWildcard) {
                error(c, *slot, "this arm can never run: '_' above it matches everything");
            }
            checkPattern(c, slot, subject, arm->list.len == 1);
            if (isWildcard(*slot)) {
                hasWildcard = true;
            } else if ((*slot)->type == &tyBool && (*slot)->isConst) {
                if ((*slot)->cval) hasTrue = true;
                else hasFalse = true;
            } else if (subject->kind == TY_ENUM && (*slot)->isConst) {
                covered++;
            }
        }
        if (wantValue) {
            Type *at = checkBlockValue(c, arm->c, t ? t : first);
            if (!first && at->kind != TY_NORETURN && !isErr(at)) {
                first = at;
            } else if (first && !t && !isErr(at)) {
                Type *u = unify(c, first, at, arm);
                if (!isErr(u)) first = u;
            }
        } else if (arm->c->kind == N_BLOCK) {
            checkBlock(c, arm->c, true);
        } else {
            checkStmt(c, arm->c);
        }
        popScope(c);
    }
    bool complete = hasWildcard || (hasTrue && hasFalse);
    if (subject->kind == TY_ENUM && !hasWildcard) {
        /* every value of the enum needs an arm */
        StrBuf missing = {0};
        int nmissing = 0;
        for (int k = 0; k < subject->decl->list.len; k++) {
            Node *mem = subject->decl->list.items[k];
            bool found = false;
            for (int i = 0; i < m->list.len && !found; i++) {
                Node *arm = m->list.items[i];
                for (int j = 0; j < arm->list.len; j++) {
                    if (arm->list.items[j]->ref == mem) {
                        found = true;
                    }
                }
            }
            if (!found) {
                sbPrintf(&missing, "%s%s", nmissing ? ", " : "", mem->name);
                nmissing++;
            }
        }
        if (nmissing) {
            error(c, m, "this match has no arm for %s.%s%s%s; add %s or a '_' arm", subject->name,
                  nmissing == 1 ? "" : "{", missing.buf, nmissing == 1 ? "" : "}", nmissing == 1 ? "it" : "them");
        }
        sbFree(&missing);
        complete = true;
    }
    (void)covered;
    if (complete) {
        m->cval = 1;                    /* every value has an arm */
    }
    if (!wantValue) {
        return &tyVoid;
    }
    if (!complete) {
        error(c, m, "a match that gives a value needs a '_' arm for the values not listed");
    }
    Type *res = t ? t : first ? first : &tyNoreturn;
    for (int i = 0; i < m->list.len; i++) {
        coerceValue(c, m->list.items[i]->c, res);
    }
    return res;
}

/* ---------------------------------------------------------------- statements */

static void checkBlock(Checker *c, Node *block, bool newScope) {
    if (newScope) {
        pushScope(c, false);
    }
    for (int i = 0; i < block->list.len; i++) {
        checkStmt(c, block->list.items[i]);
    }
    if (newScope) {
        popScope(c);
    }
}

/* Whether a target can be assigned to; reports why not. */
/* The comptime global that an element or a field belongs to, if any:
 * T[0] and LIMITS.max are parts of T and LIMITS. */
static Node *readonlyRoot(Node *e) {
    for (;;) {
        if (e->kind == N_IDENT) {
            return e->ref && (e->ref->flags & F_READONLY) ? e->ref : NULL;
        }
        if ((e->kind == N_INDEX || e->kind == N_FIELD_ACCESS) && e->a && e->a->type &&
            (e->a->type->kind == TY_ARRAY || e->a->type->kind == TY_STRUCT)) {
            e = e->a;
            continue;
        }
        return NULL;
    }
}

static bool assignable(Checker *c, Node *target) {
    Node *ro = target->kind != N_IDENT ? readonlyRoot(target) : NULL;
    if (ro) {
        error(c, target, "%s is a constant that comptime made, and can't change", ro->name);
        return false;
    }
    if (target->kind == N_IDENT) {
        Node *d = target->ref;
        if (!d) {
            return false;
        }
        if (d->kind == N_CONST) {
            error(c, target, "%s is a constant and can't change", target->name);
            return false;
        }
        if (d->flags & F_READONLY) {
            error(c, target, "%s is a constant that comptime made, and can't change", target->name);
            return false;
        }
        if (d->kind == N_FUNC) {
            error(c, target, "%s is a function", target->name);
            return false;
        }
        if (d->kind == N_PARAM && (d->flags & F_SELF)) {
            error(c, target, "self can't be changed; change the fields through it");
            return false;
        }
        return true;
    }
    if (addressable(target)) {
        return true;
    }
    if (target->kind == N_FIELD_ACCESS && target->ref) {
        error(c, target, "this field belongs to a temporary value; store the value in a variable first");
    } else if (target->kind == N_FIELD_ACCESS || target->kind == N_INDEX) {
        error(c, target, "this can't be assigned to");
    } else {
        error(c, target, "this can't be assigned to");
    }
    return false;
}

static void checkLocalVar(Checker *c, Node *s) {
    Type *t;
    bool checked = false;
    if (!s->a) {
        /* scoped name := value */
        t = checkExpr(c, &s->b, NULL);
        checked = true;
        if (!isErr(t) && !noCopy(c, s->b)) {
            t = &tyError;
        } else if (t == &tyVoid) {
            error(c, s->b, "this gives no value to store");
            t = &tyError;
        } else if (t->kind == TY_NORETURN || t->kind == TY_TUPLE || t->kind == TY_NULL) {
            error(c, s->b, "this can't be stored in a scoped variable");
            t = &tyError;
        }
    } else {
        t = resolveType(c, s->a);
    }
    if (t == &tyVoid) {
        error(c, s->a, "a variable can't be void");
        t = &tyError;
    }
    resolveStruct(c, t);
    s->type = t;
    if (s->flags & F_THREADLOCAL) {
        error(c, s, "threadlocal is for global variables; a local variable belongs to its task already");
    }
    if ((s->flags & F_SCOPED) && !isErr(t)) {
        /* scoped needs a free method to call at the end of the block */
        Node *m = t->kind == TY_STRUCT ? findMethodIn(t, "free") : NULL;
        if (t->kind == TY_STRING || (!m && (isJsonStruct(t) || (t->kind == TY_STRUCT && (t->decl->flags & (F_ENV | F_CLI)))))) {
            s->ref = NULL;              /* freed by the runtime */
        } else if (!m) {
            error(c, s, "scoped needs a type with a free method; %s has none", t->name);
        } else if (m->list.len != 1) {
            error(c, s, "scoped calls free() with no arguments, but %s.free takes some", t->name);
        } else {
            s->ref = m;
        }
    }
    if (s->b && !isErr(t) && !checked) {
        checkExpr(c, &s->b, t);
        coerce(c, &s->b, t, "the value");
    }
    declareLocal(c, s->name, s);
}

static void checkForIn(Checker *c, Node *s) {
    Node *it = s->a;
    if (it->kind == N_BINARY && it->op == P_DOTDOT) {
        if (s->list.len != 1) {
            error(c, s, "a range gives one value per round: for i in a..b");
            return;
        }
        Type *ta, *tb;
        if (adaptable(c, it->a) && !adaptable(c, it->b)) {
            tb = checkExpr(c, &it->b, NULL);
            ta = checkExpr(c, &it->a, isInt(tb) ? tb : NULL);
        } else {
            ta = checkExpr(c, &it->a, NULL);
            tb = checkExpr(c, &it->b, isInt(ta) ? ta : NULL);
        }
        Type *t = &tyError;
        if (!isErr(ta) && !isErr(tb)) {
            if (!isInt(ta) || !isInt(tb)) {
                error(c, it, "a range needs integers");
            } else if (sameType(ta, tb) || widens(tb, ta)) {
                t = ta;
                coerce(c, &it->b, t, "the end of the range");
            } else if (widens(ta, tb)) {
                t = tb;
                coerce(c, &it->a, t, "the start of the range");
            } else {
                error(c, it, "the range mixes %s and %s", ta->name, tb->name);
            }
        }
        it->type = t;
        Node *v = s->list.items[0];
        v->type = t;
        if (strcmp(v->name, "_") != 0) {
            declareLocal(c, v->name, v);
        }
        return;
    }
    Type *t = checkExpr(c, &s->a, NULL);
    if (t->kind == TY_STRING) {
        coerce(c, &s->a, &tyStr, "the text");
        t = &tyStr;
    }
    Type *elem = &tyError;
    Type *st = t->kind == TY_PTR ? t->elem : t;
    if (!isErr(t) && st->kind == TY_STRUCT) {
        /* a struct with next(i64 *at) (T, bool) or (K, V, bool): List, Map, or one of yours */
        resolveStruct(c, st);
        Node *m = findMethodIn(st, "next");
        Type *rt = m ? m->type : NULL;
        bool ok = m && m->list.len == 2 && m->list.items[1]->type && m->list.items[1]->type->kind == TY_PTR &&
                  m->list.items[1]->type->elem == &tyI64 && rt && rt->kind == TY_TUPLE &&
                  (rt->nelems == 2 || rt->nelems == 3) && rt->elems[rt->nelems - 1] == &tyBool;
        if (!ok) {
            error(c, s->a, "for ... in goes over a %s when it has a method next(i64 *at) that gives the next value "
                           "and a bool", st->name);
            return;
        }
        if (s->list.len != rt->nelems - 1) {
            error(c, s, "a %s gives %d value%s per round", st->name, rt->nelems - 1, rt->nelems == 2 ? "" : "s");
            return;
        }
        if (t->kind != TY_PTR && !addressable(s->a)) {
            s->a->flags |= F_EXPLICIT;      /* a temporary: loop over a copy */
        }
        s->ref = m;
        for (int i = 0; i < s->list.len; i++) {
            Node *v = s->list.items[i];
            v->type = rt->elems[i];
            if (strcmp(v->name, "_") != 0) {
                declareLocal(c, v->name, v);
            }
        }
        return;
    }
    if (!isErr(t)) {
        if (t->kind == TY_SLICE || t->kind == TY_ARRAY) {
            elem = t->elem;
            if (t->kind == TY_ARRAY && !addressable(s->a)) {
                /* a temporary array: loop over a copy */
                s->a->flags |= F_EXPLICIT;
            }
        } else {
            error(c, s->a, "for ... in goes over a range, a slice, an array, or a struct with a next method, not %s",
                  t->name);
        }
    }
    Node *item = s->list.items[s->list.len - 1];
    if (s->list.len == 2) {
        Node *idx = s->list.items[0];
        idx->type = &tyI64;
        if (strcmp(idx->name, "_") != 0) {
            declareLocal(c, idx->name, idx);
        }
    }
    item->type = elem;
    if (strcmp(item->name, "_") != 0) {
        declareLocal(c, item->name, item);
    }
}

static void checkStmt(Checker *c, Node *s) {
    switch (s->kind) {
    case N_VAR:
        checkLocalVar(c, s);
        return;
    case N_DEFINE: {
        if (s->list.len != 1) {
            /* q, r := divmod(17, 5): one name per value of a tuple */
            Type *t = checkExpr(c, &s->b, NULL);
            s->type = t;
            if (!isErr(t) && (t->kind != TY_TUPLE || t->nelems != s->list.len)) {
                if (t->kind == TY_TUPLE) {
                    error(c, s->b, "this gives %d values, but %d names are given", t->nelems, s->list.len);
                } else {
                    error(c, s->b, "this gives one value, but %d names are given", s->list.len);
                }
                t = &tyError;
            }
            bool any = false;
            for (int i = 0; i < s->list.len; i++) {
                Node *id = s->list.items[i];
                id->type = isErr(t) ? &tyError : t->elems[i];
                if (strcmp(id->name, "_") != 0) {
                    declareLocal(c, id->name, id);
                    any = true;
                }
            }
            if (!any) {
                error(c, s, "every name here is _; drop the values with _ = instead");
            }
            return;
        }
        Node *id = s->list.items[0];
        Type *t = checkExpr(c, &s->b, NULL);
        if (!isErr(t) && !noCopy(c, s->b)) {
            t = &tyError;
        } else if (t == &tyVoid) {
            error(c, s->b, "this gives no value to store");
            t = &tyError;
        } else if (t->kind == TY_NORETURN) {
            t = &tyError;
        } else if (t->kind == TY_NULL) {
            error(c, s->b, "the type of null can't be worked out here; declare the pointer's type");
            t = &tyError;
        } else if (t->kind == TY_TUPLE) {
            error(c, s->b, "this gives %d values; unpack them into names: a, b := ...", t->nelems);
            t = &tyError;
        }
        id->type = t;
        s->type = t;
        declareLocal(c, id->name, id);
        return;
    }
    case N_ASSIGN: {
        if (s->list.len != 1) {
            /* a, b = b, a: the values are worked out first, then stored */
            Type *ts[32];
            if (s->list.len > 32) {
                error(c, s, "too many names");
                return;
            }
            for (int i = 0; i < s->list.len; i++) {
                Node **target = &s->list.items[i];
                if ((*target)->kind == N_IDENT && strcmp((*target)->name, "_") == 0) {
                    ts[i] = NULL;
                    (*target)->type = &tyError;
                    continue;
                }
                ts[i] = checkExpr(c, target, NULL);
                if (!isErr(ts[i]) && !assignable(c, *target)) {
                    ts[i] = &tyError;
                }
            }
            Type *t;
            if (s->b->kind == N_TUPLE && s->b->list.len == s->list.len) {
                for (int i = 0; i < s->list.len; i++) {
                    checkExpr(c, &s->b->list.items[i], ts[i]);
                    if (ts[i]) coerce(c, &s->b->list.items[i], ts[i], "the value");
                }
                Type *elems[32];
                for (int i = 0; i < s->list.len; i++) {
                    elems[i] = ts[i] && !isErr(ts[i]) ? ts[i] : s->b->list.items[i]->type;
                    if (!elems[i] || isErr(elems[i])) elems[i] = &tyError;
                }
                t = tupleOf(elems, s->list.len);
                s->b->type = t;
            } else {
                t = checkExpr(c, &s->b, NULL);
                if (!isErr(t) && (t->kind != TY_TUPLE || t->nelems != s->list.len)) {
                    error(c, s->b, "this gives %s, but %d places are given",
                          t->kind == TY_TUPLE ? "a different number of values" : "one value", s->list.len);
                    return;
                }
                if (!isErr(t)) {
                    for (int i = 0; i < s->list.len; i++) {
                        if (ts[i] && !isErr(ts[i]) && !sameType(ts[i], t->elems[i])) {
                            error(c, s->list.items[i], "value %d is %s, but %s is needed", i + 1, t->elems[i]->name,
                                  ts[i]->name);
                        }
                    }
                }
            }
            if (s->op != P_ASSIGN) {
                error(c, s, "'%s' takes one target", tokName(s->op));
            }
            return;
        }
        Node **target = &s->list.items[0];
        if ((*target)->kind == N_IDENT && strcmp((*target)->name, "_") == 0) {
            if (s->op != P_ASSIGN) {
                error(c, s, "'_' can only be assigned with '='");
            }
            c->allowResult = true;
            Type *t = checkExpr(c, &s->b, NULL);
            if (t == &tyVoid) {
                error(c, s->b, "this gives no value to drop");
            }
            (*target)->type = &tyError;
            return;
        }
        Type *tt = checkExpr(c, target, NULL);
        if (isErr(tt) || !assignable(c, *target)) {
            checkExpr(c, &s->b, NULL);
            return;
        }
        if (s->op == P_PLUS_ASSIGN || s->op == P_MINUS_ASSIGN) {
            if (tt->kind == TY_PTR) {
                Type *ta = checkExpr(c, &s->b, NULL);
                if (!isErr(ta) && !isInt(ta)) {
                    error(c, s->b, "a pointer moves by an integer, not %s", ta->name);
                }
                return;
            }
        }
        bool arith = s->op == P_PLUS_ASSIGN || s->op == P_MINUS_ASSIGN || s->op == P_STAR_ASSIGN ||
                     s->op == P_SLASH_ASSIGN || s->op == P_PERCENT_ASSIGN;
        bool bits = s->op == P_AMP_ASSIGN || s->op == P_PIPE_ASSIGN || s->op == P_CARET_ASSIGN;
        bool ok = isInt(tt) || (arith && isFloat(tt)) || (bits && tt->kind == TY_ENUM && tt->isFlags);
        if (s->op != P_ASSIGN && !ok) {
            error(c, s, "'%s' needs a number variable, not %s", tokName(s->op), tt->name);
        }
        if (s->op == P_SHL_ASSIGN || s->op == P_SHR_ASSIGN) {
            Type *ta = checkExpr(c, &s->b, NULL);
            if (!isErr(ta) && !isInt(ta)) {
                error(c, s->b, "a shift amount must be an integer, not %s", ta->name);
            }
            return;
        }
        checkExpr(c, &s->b, tt);
        coerce(c, &s->b, tt, "the value");
        return;
    }
    case N_EXPR_STMT: {
        Node *e = s->a;
        if (e->kind == N_IF) {
            checkIf(c, e, NULL, false);
            return;
        }
        if (e->kind == N_MATCH) {
            checkMatch(c, e, NULL, false);
            return;
        }
        checkExpr(c, &s->a, NULL);
        Node *x = s->a;
        if (x->kind == N_THREAD) {
            x->ival = 1;            /* nobody joins it: it frees itself */
        }
        bool used = x->kind == N_CALL || x->kind == N_THREAD || (x->kind == N_BINARY && x->op == P_LARROW) ||
                    ((x->kind == N_TRY || x->kind == N_CATCH) &&
                                          (x->type == &tyVoid || x->type->kind == TY_NORETURN));
        if (!used && (x->kind == N_TRY || x->kind == N_CATCH) && !isErr(x->type)) {
            error(c, x, "the value from this call is not used; assign it, or drop it with _ =");
        } else if (!used && !isErr(x->type)) {
            error(c, x, "this value is not used");
        }
        return;
    }
    case N_IF:
        checkIf(c, s, NULL, false);
        return;
    case N_MATCH:
        checkMatch(c, s, NULL, false);
        return;
    case N_BLOCK:
        checkBlock(c, s, true);
        return;
    case N_RETURN: {
        Type *ret = c->func->type;
        if (c->inDefer) {
            error(c, s, "a deferred statement can't return");
            return;
        }
        if (c->func->flags & F_NORETURN) {
            error(c, s, "%s is @noreturn, so it can't return", c->func->name);
            return;
        }
        if (ret->kind == TY_RESULT) {
            /* return value, return error(...), or return e (an Error) */
            if (!s->a) {
                if (ret->elem != &tyVoid) {
                    error(c, s, "%s must return a value of type %s, or an error", c->func->name, ret->elem->name);
                }
                return;
            }
            Type *t = checkExpr(c, &s->a, ret->elem == &tyVoid ? NULL : ret->elem);
            if (isErr(t) || t->kind == TY_ERRVAL) {
                return;
            }
            if (ret->elem == &tyVoid) {
                error(c, s->a, "%s returns no value, only an error or nothing", c->func->name);
                return;
            }
            coerce(c, &s->a, ret->elem, "the returned value");
            return;
        }
        if (!s->a) {
            if (ret != &tyVoid && !isErr(ret)) {
                error(c, s, "%s must return a value of type %s", c->func->name, ret->name);
            }
            return;
        }
        if (ret == &tyVoid) {
            Type *t = checkExpr(c, &s->a, NULL);
            if (t->kind == TY_ERRVAL) {
                error(c, s->a, "%s can't return an error; its return type would have to be !void", c->func->name);
            } else {
                error(c, s->a, "%s returns nothing, so 'return' takes no value", c->func->name);
            }
            return;
        }
        Type *rt = checkExpr(c, &s->a, ret);
        if (rt->kind == TY_ERRVAL) {
            error(c, s->a, "%s can't return an error; its return type would have to be !%s", c->func->name,
                  ret->name);
            return;
        }
        coerce(c, &s->a, ret, "the returned value");
        checkEscape(c, s->a);
        return;
    }
    case N_BREAK:
    case N_CONTINUE: {
        const char *word = s->kind == N_BREAK ? "break" : "continue";
        if (c->inDefer && !c->loop) {
            error(c, s, "a deferred statement can't use %s", word);
            return;
        }
        if (!c->loop) {
            error(c, s, "'%s' can only be used inside a loop", word);
            return;
        }
        if (s->name) {
            Loop *l = c->loop;
            while (l && !(l->label && strcmp(l->label, s->name) == 0)) {
                l = l->outer;
            }
            if (!l) {
                error(c, s, "there is no loop labelled %s around this '%s'", s->name, word);
            }
        }
        return;
    }
    case N_LABELED: {
        for (Loop *l = c->loop; l; l = l->outer) {
            if (l->label && strcmp(l->label, s->name) == 0) {
                error(c, s, "the label %s is already used by an enclosing loop", s->name);
            }
        }
        c->pendingLabel = s->name;
        checkStmt(c, s->a);
        return;
    }
    case N_FOR: {
        Loop loop = {c->loop, c->pendingLabel};
        c->pendingLabel = NULL;
        pushScope(c, false);
        if (s->flags & F_FOR_IN) {
            checkForIn(c, s);
        } else if (s->a) {
            checkCondition(c, &s->a);
        }
        c->loop = &loop;
        checkBlock(c, s->c, true);
        c->loop = loop.outer;
        popScope(c);
        return;
    }
    case N_DEFER:
    case N_ERRDEFER: {
        if (s->kind == N_ERRDEFER && (!c->func || c->func->type->kind != TY_RESULT)) {
            error(c, s, "errdefer runs when the function returns an error, so %s must return !T",
                  c->func ? c->func->name : "the function");
        }
        if (c->inDefer) {
            error(c, s, "a deferred statement can't defer another one");
            return;
        }
        bool savedDefer = c->inDefer;
        Loop *savedLoop = c->loop;
        c->inDefer = true;
        c->loop = NULL;             /* break and continue can't leave the defer */
        checkStmt(c, s->a);
        c->loop = savedLoop;
        c->inDefer = savedDefer;
        return;
    }
    case N_SPAWN:
        checkStarted(c, &s->a, "spawn", "a task");
        return;
    case N_SELECT: {
        bool hasAfter = false;
        for (int i = 0; i < s->list.len; i++) {
            Node *arm = s->list.items[i];
            Node *x = arm->a;
            pushScope(c, false);
            if (x->kind == N_UNARY && x->op == P_LARROW) {
                Type *t = checkExpr(c, &arm->a, NULL);
                if (arm->name && !isErr(t)) {
                    Node *var = like(c, N_IDENT, arm);
                    var->name = arm->name;
                    var->type = t;
                    arm->d = var;
                    if (strcmp(arm->name, "_") != 0) {
                        declareLocal(c, arm->name, var);
                    }
                }
            } else if (x->kind == N_CALL && x->a->kind == N_IDENT && strcmp(x->a->name, "after") == 0 &&
                       !lookup(c, "after")) {
                if (arm->name) {
                    error(c, arm, "after gives no value to name");
                }
                if (hasAfter) {
                    error(c, x, "a select can have one after arm");
                }
                hasAfter = true;
                x->builtin = BI_AFTER;
                if (x->list.len != 1 || x->list.items[0]->kind == N_NAMED_ARG) {
                    error(c, x, "after takes one Duration: after(5s)");
                } else {
                    Type *t = checkExpr(c, &x->list.items[0], &tyDuration);
                    if (!isErr(t) && !isInt(t)) {
                        error(c, x->list.items[0], "after takes a Duration, not %s", t->name);
                    } else if (!isErr(t)) {
                        coerce(c, &x->list.items[0], &tyI64, "the time");
                    }
                }
            } else {
                error(c, x, "a select arm takes from a channel (<-ch) or waits with after(...)");
            }
            if (arm->c->kind == N_BLOCK) {
                checkBlock(c, arm->c, true);
            } else {
                checkStmt(c, arm->c);
            }
            popScope(c);
        }
        if (s->list.len == 0) {
            error(c, s, "a select needs at least one arm");
        }
        return;
    }
    case N_WHEN: {
        /* only the branch for the target is checked and compiled: it
         * becomes a plain block */
        Node *b = whenBranch(c, s);
        if (b) {
            *s = *b;
        } else {
            s->kind = N_BLOCK;
            s->list = (NodeList){0};
            s->a = s->c = s->d = NULL;
        }
        checkStmt(c, s);
        return;
    }
    case N_ASM:
        notYet(c, s, "assembly", "a later version");
        return;
    default:
        notYet(c, s, "this statement", "a later version");
        return;
    }
}

/* Whether control can reach the end of a statement. */
static bool canFallThrough(Node *s);

static bool blockFallsThrough(Node *b) {
    if (b->kind != N_BLOCK) {
        return canFallThrough(b);
    }
    for (int i = 0; i < b->list.len; i++) {
        if (!canFallThrough(b->list.items[i])) {
            return false;
        }
    }
    return true;
}

static bool hasBreak(Node *s, int depth) {
    if (!s) return false;
    switch (s->kind) {
    case N_BREAK:
        return depth == 0 || s->name;   /* a labelled break may leave this loop */
    case N_FOR:
        return hasBreak(s->c, depth + 1);
    case N_LABELED:
        return hasBreak(s->a, depth);
    case N_BLOCK:
        for (int i = 0; i < s->list.len; i++) {
            if (hasBreak(s->list.items[i], depth)) return true;
        }
        return false;
    case N_IF:
        return hasBreak(s->c, depth) || hasBreak(s->d, depth);
    case N_MATCH:
        for (int i = 0; i < s->list.len; i++) {
            if (hasBreak(s->list.items[i]->c, depth)) return true;
        }
        return false;
    case N_EXPR_STMT:
        return hasBreak(s->a, depth);
    default:
        return false;
    }
}

static bool canFallThrough(Node *s) {
    switch (s->kind) {
    case N_RETURN: case N_BREAK: case N_CONTINUE:
        return false;
    case N_EXPR_STMT:
        if (s->a->kind == N_CALL && s->a->type == &tyNoreturn) {
            return false;
        }
        return s->a->kind == N_IF || s->a->kind == N_MATCH ? canFallThrough(s->a) : true;
    case N_BLOCK:
        return blockFallsThrough(s);
    case N_IF:
        return !s->d || blockFallsThrough(s->c) || blockFallsThrough(s->d);
    case N_FOR:
        return s->a != NULL || hasBreak(s->c, 0);
    case N_LABELED:
        return canFallThrough(s->a);
    case N_MATCH: {
        bool wildcard = false;
        for (int i = 0; i < s->list.len; i++) {
            Node *arm = s->list.items[i];
            for (int j = 0; j < arm->list.len; j++) {
                if (isWildcard(arm->list.items[j])) wildcard = true;
            }
            if (blockFallsThrough(arm->c)) return true;
        }
        return !wildcard && !s->cval;
    }
    default:
        return true;
    }
}

/* ---------------------------------------------------------------- declarations */

static void declareGlobal(Checker *c, Scope *g, Node *d, const char *name) {
    Sym *old = findIn(g, name);
    if (old) {
        if (old->decl->src && d->src && old->decl->src != d->src) {
            error(c, d, "%s is already declared in %s, line %d", name, old->decl->src->path, old->decl->line);
        } else {
            error(c, d, "%s is already declared on line %d", name, old->decl->line);
        }
        return;
    }
    for (size_t i = 0; i < sizeof builtinTypes / sizeof builtinTypes[0]; i++) {
        if (strcmp(builtinTypes[i]->name, name) == 0) {
            error(c, d, "%s is a built-in name", name);
            return;
        }
    }
    bool stdFunc = d->kind == N_FUNC && d->src && strncmp(d->src->path, "std/", 4) == 0;
    if (isBuiltinName(name) && !stdFunc) {
        /* the standard library may: log.error is only reached through its module */
        error(c, d, "%s is a built-in name", name);
        return;
    }
    addSym(c, g, name, d);
}

/* A function's or method's types. recv is the receiver of a generic
 * struct's method, for an instance; otherwise f->b names it. */
static void checkSignature(Checker *c, Node *f, Type *recv) {
    if (f->state == 3) {
        return;                     /* done already */
    }
    f->state = 3;
    for (int i = 0; i < f->attrs.len; i++) {
        Node *at = f->attrs.items[i];
        if (strcmp(at->name, "extern") == 0) {
            /* @extern("c_name"): a C function; the standard library only */
            bool std = f->module && strcmp(f->module, "main") != 0 && f->src &&
                       (strncmp(f->src->path, "std/", 4) == 0 || strncmp(f->src->path, "c-import:", 9) == 0);
            if (!std) {
                error(c, at, "@extern is for the standard library; use import c to call C");
            } else if (at->list.len < 1 || at->list.len > 2 || at->list.items[0]->kind != N_STRING) {
                error(c, at, "@extern takes the C function's name in quotes");
            } else {
                f->flags |= F_EXTERN;
                f->text = at->list.items[0]->text;
                if (at->list.len == 2 && at->list.items[1]->kind == N_IDENT &&
                    strcmp(at->list.items[1]->name, "variadic") == 0) {
                    f->flags |= F_VARIADIC;
                }
            }
        } else if (strcmp(at->name, "export") == 0) {
            /* @export or @export("name"): C can call it by that name */
            if (f->b || f->list2.len) {
                error(c, at, "only a plain function can be exported, not a method or a generic");
            } else if (at->list.len > 1 || (at->list.len == 1 && at->list.items[0]->kind != N_STRING)) {
                error(c, at, "@export takes nothing, or the C name in quotes");
            } else {
                f->flags |= F_EXPORT;
                f->text = at->list.len ? at->list.items[0]->text : f->name;
                bool okName = f->text[0] && !isdigit((unsigned char)f->text[0]);
                for (const char *q = f->text; *q; q++) {
                    okName = okName && (isalnum((unsigned char)*q) || *q == '_');
                }
                if (!okName) {
                    error(c, at, "\"%s\" isn't a C name: letters, digits and _, not starting with a digit", f->text);
                    f->flags &= ~F_EXPORT;
                }
                for (int k = 0; okName && k < c->exports.len; k++) {
                    Node *o = c->exports.items[k];
                    if (strcmp(o->text, f->text) == 0) {
                        error(c, at, "%s is exported already, on line %d", f->text, o->line);
                        f->flags &= ~F_EXPORT;
                        okName = false;
                    }
                }
                if (okName && (strncmp(f->text, "iris_", 5) == 0 || strcmp(f->text, "main") == 0)) {
                    error(c, at, "the names main and iris_... belong to the runtime; export under another name");
                    f->flags &= ~F_EXPORT;
                    okName = false;
                }
                if (okName) {
                    listPush(c->arena, &c->exports, f);
                }
            }
        } else if (strcmp(at->name, "inline") == 0 || strcmp(at->name, "unchecked") == 0) {
            if (at->list.len) {
                error(c, at, "@%s takes nothing", at->name);
            }
            f->flags |= at->name[0] == 'i' ? F_INLINE : F_UNCHECKED;
        } else if (strcmp(at->name, "noreturn") == 0) {
            if (at->list.len) {
                error(c, at, "@noreturn takes nothing");
            }
            f->flags |= F_NORETURN;
        } else if (strcmp(at->name, "builtin") == 0 && f->module && f->src &&
                   strncmp(f->src->path, "std/", 4) == 0) {
            f->flags |= F_INTRINSIC;    /* the compiler makes calls to it */
        } else {
            notYet(c, at, "attributes", "a later version");
        }
    }
    if (!f->c && !f->d && !(f->flags & (F_EXTERN | F_INTRINSIC)) && !f->b) {
        error(c, f, "%s has no body", f->name);
    }
    f->type = resolveType(c, f->a);
    if ((f->flags & F_NORETURN) && f->type != &tyVoid && !isErr(f->type)) {
        error(c, f, "a @noreturn function returns void: its calls never come back with a value");
    }
    resolveSignatureType(c, f->type);
    int first = 0;
    if (f->b || recv) {
        /* a method: self, a pointer to the receiver, comes first */
        if (f->list2.len) {
            notYet(c, f, "methods with type parameters of their own", "a later version");
        }
        Type *rt = recv ? recv : resolveType(c, f->b);
        Node *self = like(c, N_PARAM, f);
        self->name = "self";
        self->flags |= F_SELF;
        if (rt->kind == TY_STRUCT) {
            self->type = ptrTo(rt);
            bool listed = false;
            for (int i = 0; i < rt->methods.len; i++) {
                listed = listed || rt->methods.items[i] == f;
            }
            resolveStruct(c, rt);
            Node *path[16];
            if (findField(rt, f->name, path, 1)) {
                error(c, f, "%s already has a field named %s", rt->name, f->name);
            }
            if (!listed) {
                if (findMethodIn(rt, f->name)) {
                    error(c, f, "%s already has a method named %s", rt->name, f->name);
                } else {
                    listPush(c->arena, &rt->methods, f);
                }
            }
        } else {
            if (!isErr(rt)) {
                error(c, f->b ? f->b : f, "methods can only be added to structs declared in this program, not %s",
                      rt->name);
            }
            self->type = &tyError;
        }
        NodeList params = {0};
        listPush(c->arena, &params, self);
        for (int i = 0; i < f->list.len; i++) {
            listPush(c->arena, &params, f->list.items[i]);
        }
        f->list = params;
        first = 1;
    }
    for (int i = first; i < f->list.len; i++) {
        Node *p = f->list.items[i];
        p->type = resolveType(c, p->a);
        if (p->type == &tyVoid) {
            error(c, p, "a parameter can't be void");
            p->type = &tyError;
        }
        resolveSignatureType(c, p->type);
        for (int j = 0; j < i; j++) {
            if (strcmp(f->list.items[j]->name, p->name) == 0) {
                error(c, p, "%s is already a parameter", p->name);
            }
        }
        if (i > first && !p->b && f->list.items[i - 1]->b) {
            error(c, p, "a parameter without a default can't follow one with a default");
        }
    }
    if (f->flags & F_EXPORT) {
        for (int i = 0; i < f->list.len; i++) {
            Type *t = f->list.items[i]->type;
            if (!isErr(t) && !isNumber(t) && t->kind != TY_PTR && t->kind != TY_FUNC) {
                error(c, f->list.items[i], "an exported function takes numbers and pointers, not %s", t->name);
            }
        }
        Type *r = f->type;
        if (!isErr(r) && r != &tyVoid && !isNumber(r) && r->kind != TY_PTR) {
            error(c, f, "an exported function returns a number, a pointer or nothing, not %s", r->name);
        }
    }
    if (f->flags & F_EXTERN) {
        /* only what C passes the same way: integers and pointers */
        for (int i = 0; i < f->list.len; i++) {
            Type *t = f->list.items[i]->type;
            if (!isErr(t) && !isNumber(t) && t->kind != TY_PTR && t->kind != TY_FUNC) {
                error(c, f->list.items[i], "a C function takes numbers and pointers, not %s", t->name);
            }
        }
        Type *r = f->type;
        if (!isErr(r) && r != &tyVoid && !isNumber(r) && r->kind != TY_PTR) {
            error(c, f, "a C function returns a number, a pointer or nothing, not %s", r->name);
        }
    }
}

static bool isConstValue(Node *e) {
    if (e->isConst || e->kind == N_STRING || e->kind == N_NULL || (e->type && e->type->kind == TY_NULL)) {
        return true;
    }
    if (e->kind == N_IDENT && e->ref && e->ref->kind == N_CONST) {
        return true;
    }
    if (e->kind == N_STRUCT_LIT) {
        for (int i = 0; i < e->list.len; i++) {
            if (!isConstValue(e->list.items[i]->a)) return false;
        }
        return true;
    }
    if (e->kind == N_ARRAY_LIT) {
        for (int i = 0; i < e->list.len; i++) {
            if (!isConstValue(e->list.items[i])) return false;
        }
        return true;
    }
    return false;
}

static void checkFunctionBody(Checker *c, Node *f) {
    if (f->flags & F_EXTERN) {
        return;                     /* a C function: no body */
    }
    c->func = f;
    Scope *outer = c->scope;
    const char *outerModule = c->module;
    c->scope = f->scope;
    c->module = f->module;
    pushScope(c, true);
    for (int i = 0; i < f->list.len; i++) {
        Node *p = f->list.items[i];
        if (p->b) {
            /* defaults are checked like arguments, outside the function */
            Scope *saved = c->scope;
            c->scope = f->scope;
            checkExpr(c, &p->b, p->type);
            coerce(c, &p->b, p->type, "the default value");
            if (!isConstValue(p->b) && !isErr(p->b->type)) {
                error(c, p->b, "a default value must be known when compiling");
            }
            c->scope = saved;
        }
        declareLocal(c, p->name, p);
    }
    if (f->d && f->type->kind == TY_RESULT) {
        Type *t = checkExpr(c, &f->d, f->type->elem == &tyVoid ? NULL : f->type->elem);
        if (!isErr(t) && t->kind != TY_ERRVAL && f->type->elem != &tyVoid) {
            coerce(c, &f->d, f->type->elem, "the returned value");
        }
    } else if (f->d) {
        Type *t = checkExpr(c, &f->d, f->type == &tyVoid ? NULL : f->type);
        if ((f->flags & F_NORETURN) && f->type == &tyVoid && t != &tyNoreturn && !isErr(t)) {
            error(c, f->d, "%s is @noreturn, so its '=>' body must be a call that never comes back", f->name);
        }
        if (f->type == &tyVoid) {
            if (f->d->kind != N_CALL && !isErr(t)) {
                error(c, f->d, "%s returns nothing, so its '=>' body must be a call", f->name);
            }
        } else {
            coerce(c, &f->d, f->type, "the returned value");
            checkEscape(c, f->d);
        }
    } else if (f->c) {
        checkBlock(c, f->c, false);
        bool needsReturn = f->type != &tyVoid && !isErr(f->type) &&
                           !(f->type->kind == TY_RESULT && f->type->elem == &tyVoid);
        if (needsReturn && blockFallsThrough(f->c)) {
            error(c, f, "%s can reach its end without returning a value", f->name);
        }
        if ((f->flags & F_NORETURN) && blockFallsThrough(f->c)) {
            error(c, f, "%s is @noreturn but can reach its end; end it with a call that never comes back, like panic", f->name);
        }
    }
    popScope(c);
    c->func = NULL;
    c->scope = outer;
    c->module = outerModule;
}

static void checkGlobalValue(Checker *c, Node **slot, Type *t) {
    checkExpr(c, slot, t);
    if (t) {
        coerce(c, slot, t, "the value");
    }
    if (!isConstValue(*slot) && !isErr((*slot)->type)) {
        error(c, *slot, "a global's value must be known when compiling");
    }
}

/* Rewrites mod.name into one name, "mod.name", which lookup finds in the
 * module. Reports names that don't exist or aren't pub. */
static void qualify(Checker *c, Node *n) {
    if (!n) {
        return;
    }
    if (n->kind == N_FIELD_ACCESS && n->a && n->a->kind == N_IDENT && !strchr(n->a->name, '.')) {
        Node *imp = lookupPlain(c, n->a->name);
        if (imp && imp->kind == N_IMPORT && imp->scope) {
            Sym *sym = findIn((Scope *)imp->scope, n->name);
            if (!sym) {
                const char *why = NULL;
                for (int i = 0; i < imp->list.len; i++) {
                    if (strcmp(imp->list.items[i]->name, n->name) == 0) why = imp->list.items[i]->text;
                }
                if (why) {
                    error(c, n, "c.%s can't be used from Iris yet: it has %s", n->name, why);
                } else if (imp->flags & F_C_IMPORT) {
                    error(c, n, "the C headers imported here have no %s (only functions, structs, types and integer constants come through)", n->name);
                } else {
                    error(c, n, "the module %s has no %s", imp->name, n->name);
                }
                n->flags |= F_REPORTED;
            } else if (!visible(c, sym->decl)) {
                error(c, n, "%s.%s isn't pub, so only its own module can use it", imp->name, n->name);
                n->flags |= F_REPORTED;
            }
            size_t len = strlen(imp->name) + strlen(n->name) + 2;
            char *full = arenaAlloc(c->arena, len);
            snprintf(full, len, "%s.%s", imp->name, n->name);
            n->kind = N_IDENT;
            n->name = full;
            n->a = NULL;
            return;
        }
    }
    qualify(c, n->a);
    qualify(c, n->b);
    qualify(c, n->c);
    qualify(c, n->d);
    for (int i = 0; i < n->list.len; i++) qualify(c, n->list.items[i]);
    for (int i = 0; i < n->list2.len; i++) qualify(c, n->list2.items[i]);
    for (int i = 0; i < n->attrs.len; i++) qualify(c, n->attrs.items[i]);
}

/* Puts the checker in a top-level declaration's module and file. */
static void enter(Checker *c, Node *d) {
    c->scope = d->scope;
    c->module = d->module;
    c->src = d->src;
}

/* ---------------------------------------------------------------- when */

/* A when condition, worked out for the target: os, arch and mode compared
 * with values, joined with &&, || and !. */
static bool whenValue(Checker *c, Node *e, bool *out) {
    static const struct { const char *name; const char *values[4]; } names[] = {
        {"os", {"linux", "macos"}},
        {"arch", {"x86_64", "arm64"}},
        {"mode", {"debug", "releaseSafe", "releaseFast"}},
    };
    switch (e->kind) {
    case N_BOOL:
        *out = e->ival != 0;
        return true;
    case N_UNARY:
        if (e->op == P_BANG && whenValue(c, e->a, out)) {
            *out = !*out;
            return true;
        }
        break;
    case N_BINARY: {
        if (e->op == P_ANDAND || e->op == P_OROR) {
            bool a, b;
            if (!whenValue(c, e->a, &a) || !whenValue(c, e->b, &b)) return false;
            *out = e->op == P_ANDAND ? a && b : a || b;
            return true;
        }
        if ((e->op == P_EQ || e->op == P_NE) && e->a->kind == N_IDENT && e->b->kind == N_ENUM_DOT) {
            for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
                if (strcmp(names[i].name, e->a->name) != 0) continue;
                const char *now = i == 0 ? gTarget.os : i == 1 ? gTarget.arch
                                : gTarget.mode == MODE_DEBUG ? "debug"
                                : gTarget.mode == MODE_RELEASE_SAFE ? "releaseSafe" : "releaseFast";
                bool known = false;
                StrBuf list = {0};
                int count = 0;
                while (count < 4 && names[i].values[count]) count++;
                for (int k = 0; k < count; k++) {
                    known = known || strcmp(names[i].values[k], e->b->name) == 0;
                    sbPrintf(&list, "%s.%s", k == 0 ? "" : k == count - 1 ? " or " : ", ", names[i].values[k]);
                }
                if (!known) {
                    error(c, e->b, "%s can be %s, not .%s", names[i].name, list.buf, e->b->name);
                    sbFree(&list);
                    return false;
                }
                sbFree(&list);
                *out = (strcmp(now, e->b->name) == 0) == (e->op == P_EQ);
                return true;
            }
            error(c, e->a, "when can test os, arch and mode, not %s", e->a->name);
            return false;
        }
        break;
    }
    default:
        break;
    }
    error(c, e, "when tests os, arch or mode against a value: when os == .linux && arch == .arm64");
    return false;
}

/* The branch of a when that is compiled: its block, or NULL for none. */
static Node *whenBranch(Checker *c, Node *w) {
    bool v;
    if (!whenValue(c, w->a, &v)) {
        return NULL;
    }
    if (v) {
        return w->c;
    }
    if (w->d && w->d->kind == N_WHEN) {
        return whenBranch(c, w->d);
    }
    return w->d;
}

/* Replaces each when among a file's declarations with the declarations of
 * the branch that is compiled. */
static void expandWhens(Checker *c, NodeList *list) {
    NodeList out = {0};
    bool any = false;
    for (int i = 0; i < list->len; i++) {
        Node *d = list->items[i];
        if (d->kind != N_WHEN) {
            listPush(c->arena, &out, d);
            continue;
        }
        any = true;
        Node *b = whenBranch(c, d);
        if (b) {
            expandWhens(c, &b->list);
            for (int k = 0; k < b->list.len; k++) {
                listPush(c->arena, &out, b->list.items[k]);
            }
        }
    }
    if (any) {
        *list = out;
    }
}

/* Test blocks: under iris test, each becomes a !void function that main
 * calls; otherwise they are left out. Only the program's own module's
 * tests run. */
static void expandTests(Checker *c, Program *prog, Module *m, NodeList *list) {
    NodeList out = {0};
    bool any = false;
    for (int i = 0; i < list->len; i++) {
        Node *d = list->items[i];
        if (d->kind != N_TEST) {
            listPush(c->arena, &out, d);
            continue;
        }
        any = true;
        if (!gWithTests || m != prog->main) {
            continue;
        }
        Node *f = like(c, N_FUNC, d);
        char name[32];
        snprintf(name, sizeof name, "test.%d", prog->tests.len + 1);
        f->name = arenaStrndup(c->arena, name, strlen(name));
        f->text = d->text;
        f->flags = F_TESTFN;
        Node *ret = like(c, N_T_ERR, d);
        ret->a = like(c, N_T_NAME, d);
        ret->a->name = "void";
        f->a = ret;
        f->c = d->c;
        listPush(c->arena, &out, f);
        listPush(c->arena, &prog->tests, f);
    }
    if (any) {
        *list = out;
    }
}

bool checkProgram(Arena *a, Program *prog) {
    Checker c = {0};
    c.arena = a;
    gArena = a;
    gChecker = &c;     /* cleared before it returns */
    c.prog = prog;
    gTypes = NULL;
    c.instOut = &prog->instances;
    int before = gErrors;

    /* every module gets a scope for its names, every file one for its imports */
    for (Module *m = prog->modules; m; m = m->next) {
        Scope *g = arenaAlloc(a, sizeof(Scope));
        m->anchor->scope = g;
    }
    /* core's names are below every other module's own */
    if (prog->core) {
        for (Module *m = prog->modules; m; m = m->next) {
            if (m != prog->core) ((Scope *)m->anchor->scope)->parent = prog->core->anchor->scope;
        }
    }
    NodeList all = {0};
    for (Module *m = prog->modules; m; m = m->next) {
        Scope *g = m->anchor->scope;
        for (int f = 0; f < m->files.len; f++) {
            Node *file = m->files.items[f];
            Scope *fs = arenaAlloc(a, sizeof(Scope));
            fs->parent = g;
            c.src = file->src;
            expandWhens(&c, &file->list);
            expandTests(&c, prog, m, &file->list);
            for (int i = 0; i < file->list.len; i++) {
                Node *d = file->list.items[i];
                d->scope = fs;
                d->module = m->path;
                if (d->kind == N_IMPORT) {
                    for (int k = 0; k < d->list.len; k++) {
                        Node *item = d->list.items[k];
                        if (!item->ref) continue;
                        Sym *had = findIn(fs, item->ref->name);
                        if (had && had->decl == item->ref && (item->flags & F_C_IMPORT)) {
                            continue;           /* several C headers: one c */
                        }
                        if (had) {
                            error(&c, item, "%s is imported twice", item->ref->name);
                            continue;
                        }
                        addSym(&c, fs, item->ref->name, item->ref);
                    }
                    continue;
                }
                listPush(a, &all, d);
                switch (d->kind) {
                case N_FUNC:
                    if (!d->b) {
                        declareGlobal(&c, g, d, d->name);
                    }
                    break;
                case N_CONST: case N_VAR: case N_STRUCT: case N_TYPEDEF: case N_ENUM: case N_INTERFACE:
                    declareGlobal(&c, g, d, d->name);
                    break;
                case N_DEFINE:
                    if (d->list.len == 1) {
                        declareGlobal(&c, g, d->list.items[0], d->list.items[0]->name);
                    }
                    break;
                case N_WHEN:
                    notYet(&c, d, "when", "a later version");
                    break;
                default:
                    error(&c, d, "this can't appear outside a function");
                    break;
                }
            }
            /* an import can't share a name with the module's own names */
            for (int k = 0; k < fs->n; k++) {
                Sym *own = findIn(g, fs->syms[k].name);
                if (own) {
                    error(&c, own->decl, "%s is also the name of an imported module", fs->syms[k].name);
                }
            }
        }
    }

    for (int i = 0; i < all.len; i++) {
        enter(&c, all.items[i]);
        qualify(&c, all.items[i]);
    }

    /* methods are listed with their structs early, so interfaces can see them */
    for (int i = 0; i < all.len; i++) {
        Node *d = all.items[i];
        if (d->kind != N_FUNC || !d->b) {
            continue;
        }
        enter(&c, d);
        if (d->b->kind == N_T_GENERIC) {
            Node *g = d->b->a->kind == N_T_NAME ? lookup(&c, d->b->a->name) : NULL;
            if (!g || g->kind != N_STRUCT || !g->list2.len) {
                error(&c, d->b, "%s is not a generic struct", d->b->a->kind == N_T_NAME ? d->b->a->name : "this");
            } else if (g->list2.len != d->b->list.len) {
                error(&c, d->b, "%s takes %d type%s", g->name, g->list2.len, g->list2.len == 1 ? "" : "s");
            } else if (g->module != d->module) {
                error(&c, d->b, "methods can only be added to structs of the same module");
            } else {
                listPush(a, &c.genericMethods, d);
            }
            d->state = 4;               /* generic: checked per instance */
            continue;
        }
        if (d->b->kind != N_T_NAME) {
            continue;
        }
        Node *sd = lookup(&c, d->b->name);
        if (sd && sd->kind == N_STRUCT && sd->list2.len) {
            error(&c, d->b, "%s is generic; write %s[...].%s", sd->name, sd->name, d->name);
            d->state = 4;
            continue;
        }
        if (sd && sd->kind == N_STRUCT) {
            if (sd->module != d->module) {
                error(&c, d->b, "methods can only be added to structs of the same module");
                d->state = 4;
                continue;
            }
            Type *rt = structType(&c, sd);
            if (findMethodIn(rt, d->name)) {
                error(&c, d, "%s already has a method named %s", rt->name, d->name);
                d->state = 4;
            } else {
                listPush(a, &rt->methods, d);
            }
        }
    }

    for (int i = 0; i < all.len; i++) {
        Node *d = all.items[i];
        enter(&c, d);
        if (d->kind == N_STRUCT && !d->list2.len) {
            resolveStruct(&c, structType(&c, d));
        } else if (d->kind == N_ENUM) {
            enumType(&c, d);
        } else if (d->kind == N_TYPEDEF) {
            typeFromName(&c, d->name, d, true);
        }
    }

    for (int i = 0; i < all.len; i++) {
        Node *d = all.items[i];
        if (d->kind == N_FUNC && !d->list2.len && d->state != 4) {
            enter(&c, d);
            checkSignature(&c, d, NULL);
        }
    }

    /* field defaults are checked like constants */
    for (int i = 0; i < all.len; i++) {
        Node *d = all.items[i];
        if (d->kind != N_STRUCT || d->list2.len) {
            continue;
        }
        enter(&c, d);
        for (int j = 0; j < d->list.len; j++) {
            Node *f = d->list.items[j];
            if (f->kind == N_FIELD && f->b && f->type) {
                checkGlobalValue(&c, &f->b, f->type);
            }
        }
    }

    for (int i = 0; i < all.len; i++) {
        Node *d = all.items[i];
        enter(&c, d);
        if (d->kind == N_CONST) {
            checkConstDecl(&c, d);
        } else if (d->kind == N_VAR) {
            Type *t = resolveType(&c, d->a);
            resolveStruct(&c, t);
            d->type = t;
            if (d->b && !isErr(t)) {
                checkGlobalValue(&c, &d->b, t);
            }
        } else if (d->kind == N_DEFINE && d->list.len == 1) {
            Node *id = d->list.items[0];
            checkGlobalValue(&c, &d->b, NULL);
            id->type = d->b->type;
            d->type = d->b->type;
            id->module = d->module;
        }
    }

    Node *mainFn = NULL;
    for (int i = 0; i < all.len; i++) {
        Node *d = all.items[i];
        if (d->kind == N_FUNC && !d->list2.len && d->state != 4) {
            checkFunctionBody(&c, d);
            if (!d->b && strcmp(d->name, "main") == 0 && strcmp(d->module, "main") == 0) {
                mainFn = d;
            }
        }
    }

    /* the bodies of generic instances; checking one can make more */
    for (int i = 0; i < c.pendFns.len; i++) {
        Node *fn = c.pendFns.items[i];
        int errs = gErrors;
        c.instGen = (int)fn->cval;
        fn->scope = c.pendScopes[i];
        Node *site = c.pendSites.items[i];
        gBodySite = site && site->kind == N_CALL ? site : NULL;
        checkFunctionBody(&c, fn);
        gBodySite = NULL;
        if (gErrors != errs && site && site->kind == N_CALL) {
            Type *self = fn->list.len && (fn->list.items[0]->flags & F_SELF) ? fn->list.items[0]->type : NULL;
            if (self && self->kind == TY_PTR) {
                error(&c, site, "%s.%s, made here, has the errors above", self->elem->name, fn->name);
            } else {
                error(&c, site, "%s, made here, has the errors above", fn->name);
            }
        }
    }

    /* field defaults of generic instances */
    for (int i = 0; i < prog->instances.len; i++) {
        Node *d = prog->instances.items[i];
        if (d->kind != N_STRUCT) continue;
        for (int j = 0; j < d->list.len; j++) {
            Node *f = d->list.items[j];
            if (f->kind == N_FIELD && f->b && f->type) {
                c.scope = d->type->scope;
                c.module = d->module;
                checkGlobalValue(&c, &f->b, f->type);
            }
        }
    }

    if (!mainFn && (gTesting || gComptime == 1)) {
        /* iris test and comptime's helper need no main */
    } else if (!mainFn) {
        Node *first = prog->main->files.len ? prog->main->files.items[0] : NULL;
        if (first && first->src) {
            errorAt(first->src, 1, 1, "the program has no main function");
        } else {
            fprintf(stderr, "iris: the program has no main function\n");
            gErrors++;
        }
    } else if (mainFn) {
        c.src = mainFn->src;
        if (mainFn->list.len) {
            error(&c, mainFn, "main takes no parameters");
        }
        Type *mt = mainFn->type;
        if (mt != &tyVoid && mt != &tyI32 && !isErr(mt) && !(mt->kind == TY_RESULT && mt->elem == &tyVoid)) {
            error(&c, mainFn, "main must return void, i32 or !void");
        }
    }
    gChecker = NULL;
    return gErrors == before;
}
