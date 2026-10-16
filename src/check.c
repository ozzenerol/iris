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

#include <limits.h>
#include <stdio.h>
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
Type tyStr = {.kind = TY_SLICE, .name = "str", .elem = &tyU8};
Type tyString = {.kind = TY_STRING, .name = "String", .ir = "%String"};
Type tyErrorValue = {.kind = TY_ERRVAL, .name = "Error"};
Type tyErrCode = {.kind = TY_ERRCODE, .name = "error code"};

static Type *builtinTypes[] = {&tyI8, &tyI16, &tyI32, &tyI64, &tyU8, &tyU16, &tyU32,
                               &tyU64, &tyBool, &tyStr, &tyVoid, &tyString, &tyErrorValue};

/* types that exist in the language but not in this version yet */
static const struct {
    const char *name, *version;
} laterTypes[] = {
    {"f32", "a later version"}, {"f64", "a later version"},
    {"Duration", "a later version"}, {"Arena", "a later version"},
};

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
    Scope *global;
    Scope *scope;
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
    verrorAt(c->src, at->line, at->col, fmt, ap);
    va_end(ap);
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

static Node *lookup(Checker *c, const char *name) {
    for (Scope *s = c->scope; s; s = s->parent) {
        Sym *sym = findIn(s, name);
        if (sym) {
            return sym->decl;
        }
    }
    return NULL;
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
    addSym(c, c->scope, name, decl);
}

/* ---------------------------------------------------------------- types */

static bool isInt(Type *t) { return t->kind == TY_INT; }
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
        if (tc->t->kind == kind && tc->t->count == count && sameType(tc->t->elem, elem)) {
            return tc->t;
        }
    }
    Type *t = arenaAlloc(gArena, sizeof(Type));
    t->kind = kind;
    t->elem = elem;
    t->count = count;
    char buf[256];
    if (kind == TY_PTR) {
        snprintf(buf, sizeof buf, "%s%s", elem->name, elem->kind == TY_PTR ? "*" : " *");
    } else if (kind == TY_RESULT) {
        snprintf(buf, sizeof buf, "!%s", elem->name);
    } else if (kind == TY_SLICE) {
        snprintf(buf, sizeof buf, "%s[]", elem->name);
    } else {
        snprintf(buf, sizeof buf, "%s[%lld]", elem->name, (long long)count);
    }
    t->name = arenaStrndup(gArena, buf, strlen(buf));
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

static bool sameType(Type *a, Type *b) {
    if (a == b) return true;
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case TY_INT: return a->bits == b->bits && a->isSigned == b->isSigned;
    case TY_PTR: case TY_SLICE: case TY_RESULT: return sameType(a->elem, b->elem);
    case TY_ARRAY: return a->count == b->count && sameType(a->elem, b->elem);
    case TY_STRUCT: case TY_ENUM: return a->decl == b->decl;
    case TY_TUPLE:
        if (a->nelems != b->nelems) return false;
        for (int i = 0; i < a->nelems; i++) {
            if (!sameType(a->elems[i], b->elems[i])) return false;
        }
        return true;
    default: return true;
    }
}

static void resolveStruct(Checker *c, Type *t);

static Type *structType(Checker *c, Node *decl) {
    if (!decl->type) {
        Type *t = arenaAlloc(c->arena, sizeof(Type));
        t->kind = TY_STRUCT;
        t->name = decl->name;
        t->decl = decl;
        t->ir = arenaStrndup(c->arena, "", 0);
        char buf[160];
        snprintf(buf, sizeof buf, "%%\"T.%s\"", decl->name);
        t->ir = arenaStrndup(c->arena, buf, strlen(buf));
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
    case TY_INT: return t->bits / 8;
    case TY_PTR: case TY_NULL: return 8;
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
    case TY_INT: return t->bits / 8;
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
        char buf[160];
        snprintf(buf, sizeof buf, "%%\"E.%s\"", decl->name);
        t->ir = arenaStrndup(c->arena, buf, strlen(buf));
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
    c->scope = c->global;
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
            char buf[200];
            snprintf(buf, sizeof buf, "%%\"E.%s.%s\"", decl->name, m->name);
            fields->ir = arenaStrndup(c->arena, buf, strlen(buf));
            fields->name = m->name;
            m->d = newNode(c->arena, N_T_TUPLE, m->line, m->col);   /* holds the data's type */
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
            c->scope = c->global;
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
        if (strchr(t->name, '.')) {
            notYet(c, t, "types from modules", "0.7");
            return &tyError;
        }
        return typeFromName(c, t->name, t, true);
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
            if (elems[i]->kind == TY_STRUCT) resolveStruct(c, elems[i]);
        }
        return tupleOf(elems, t->list.len);
    }
    case N_T_GENERIC: {
        Node *base = t->a;
        Node *d = base->kind == N_T_NAME ? lookup(c, base->name) : NULL;
        if (!d || d->kind != N_STRUCT || !d->list2.len) {
            if (d && d->kind == N_STRUCT) {
                error(c, t, "%s is not generic", base->name);
            } else if (base->kind == N_T_NAME && strchr(base->name, '.')) {
                notYet(c, t, "types from modules", "0.7");
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
        return instantiateStruct(c, d, args, t->list.len, t);
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
    case N_T_FUNC:
        notYet(c, t, "function types", "a later version");
        return &tyError;
    default:
        notYet(c, t, "this kind of type", "a later version");
        return &tyError;
    }
}

/* Every value of `from` fits in `to`. */
static bool widens(Type *from, Type *to) {
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
    Node *n = newNode(c->arena, N_CONVERT, e->line, e->col);
    n->a = e;
    n->type = to;
    if (explicit) {
        n->flags |= F_EXPLICIT;
    }
    return n;
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
    if (widens(from, to)) {
        *slot = convertNode(c, e, to, false);
        return true;
    }
    if (from->kind == TY_NULL && to->kind == TY_PTR) {
        e->type = to;
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
    if (isInt(from) && isInt(to)) {
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
    c->scope = t->scope ? (Scope *)t->scope : c->global;
    if (d->flags & F_PACKED) {
        notYet(c, d, "packed structs", "a later version");
    }
    if (d->a) {
        notYet(c, d->a, "align", "a later version");
    }
    if (d->attrs.len) {
        notYet(c, d->attrs.items[0], "attributes", "a later version");
    }
    for (int i = 0; i < d->list.len; i++) {
        Node *f = d->list.items[i];
        Type *ft = resolveType(c, f->a);
        if (ft == &tyVoid) {
            error(c, f, "a field can't be void");
            ft = &tyError;
        }
        /* a struct inside (not behind a pointer) must be complete first */
        Type *inner = ft;
        while (inner->kind == TY_ARRAY) {
            inner = inner->elem;
        }
        if (inner->kind == TY_STRUCT) {
            resolveStruct(c, inner);
        }
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
        if (f->attrs.len) {
            notYet(c, f->attrs.items[0], "attributes", "a later version");
        }
        for (int j = 0; j < i; j++) {
            Node *g = d->list.items[j];
            if (g->name && f->name && strcmp(g->name, f->name) == 0) {
                error(c, f, "%s already has a field named %s", d->name, f->name);
            }
        }
    }
    c->scope = saved;
    t->state = 2;
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

static Node *findMethodIn(Type *t, const char *name) {
    for (int i = 0; i < t->methods.len; i++) {
        if (strcmp(t->methods.items[i]->name, name) == 0) {
            return t->methods.items[i];
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
    Node *f = newNode(c->arena, N_FIELD_ACCESS, at->line, at->col);
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
static Scope *paramScope(Checker *c, NodeList *params, Type **args) {
    Scope *s = arenaAlloc(c->arena, sizeof(Scope));
    s->parent = c->global;
    for (int i = 0; i < params->len; i++) {
        Node *tp = newNode(c->arena, N_TYPE_PARAM, params->items[i]->line, params->items[i]->col);
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
            same = sameType(in->args[i], args[i]);
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
    s->parent = c->global;
    Node *self = newNode(c->arena, N_TYPE_PARAM, iface->line, iface->col);
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
    t->scope = paramScope(c, &decl->list2, args);
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
        if (strcmp(g->b->a->name, t->generic->name) != 0) {
            continue;
        }
        if (g->b->list.len != t->nargs) {
            continue;               /* reported where the method is declared */
        }
        Node *copy = cloneNode(c->arena, g);
        copy->b = NULL;
        Scope *s = paramScope(c, &g->b->list, t->args);
        Scope *saved = c->scope;
        c->scope = s;
        checkSignature(c, copy, t);
        c->scope = saved;
        queueBody(c, copy, s, t->decl);
        listPush(c->arena, c->instOut, copy);
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
    in->fn = copy;
    Scope *s = paramScope(c, &decl->list2, args);
    Scope *saved = c->scope;
    c->scope = s;
    c->instDepth++;
    checkSignature(c, copy, NULL);
    c->instDepth--;
    c->scope = saved;
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
    char buf[160];
    snprintf(buf, sizeof buf, "dyn %s", iface->name);
    t->name = arenaStrndup(c->arena, buf, strlen(buf));
    iface->type = t;
    Scope *saved = c->scope;
    c->scope = c->global;
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
    case N_INT:
        if (e->ival > (uint64_t)INT64_MAX) {
            return false;
        }
        *out = (int64_t)e->ival;
        return true;
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
        if (!d->isConst) {
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
    c->scope = c->global;
    Type *declared = d->a ? resolveType(c, d->a) : NULL;
    int64_t v;
    bool b;
    int before = gErrors;
    if (d->b->kind == N_STRING) {
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
    case TY_INT: case TY_BOOL: case TY_PTR: case TY_NULL: case TY_ERRCODE:
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
    if (op == P_DOTDOT) {
        error(c, e, "a range can only be used in 'for', 'match' and slicing");
        return setType(e, &tyError);
    }
    if (op == P_LARROW) {
        notYet(c, e, "channels", "a later version");
        return setType(e, &tyError);
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
    if (want && !isInt(want) && !(bitOp && want->kind == TY_ENUM && want->isFlags)) {
        want = NULL;
    }

    Type *ta, *tb;
    if (shift) {
        ta = checkExpr(c, &e->a, want);
        tb = checkExpr(c, &e->b, NULL);
        if (!isErr(ta) && !isInt(ta)) {
            error(c, e->a, "'%s' needs an integer, not %s", tokName(op), ta->name);
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
        tb = checkExpr(c, &e->b, isInt(ta) || ta->kind == TY_ENUM ? ta : want);
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
        if (e->a->isConst && isInt(ta) && isInt(tb)) {
            coerce(c, &e->a, tb, "the left side");
            t = tb;
        } else if (e->b->isConst && isInt(ta) && isInt(tb)) {
            coerce(c, &e->b, ta, "the right side");
        } else if (widens(ta, tb)) {
            coerce(c, &e->a, tb, "the left side");
            t = tb;
        } else if (widens(tb, ta)) {
            coerce(c, &e->b, ta, "the right side");
        } else if (ta->kind == TY_NULL && tb->kind == TY_PTR) {
            e->a->type = tb;
            t = tb;
        } else if (tb->kind == TY_NULL && ta->kind == TY_PTR) {
            e->b->type = ta;
        } else {
            if (isInt(ta) && isInt(tb)) {
                error(c, e, "'%s' can't mix %s and %s; convert one side, for example %s(...)", tokName(op),
                      ta->name, tb->name, ta->name);
            } else {
                error(c, e, "'%s' can't mix %s and %s", tokName(op), ta->name, tb->name);
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
                error(c, e, "'%s' can't compare %s values", tokName(op), t->name);
            }
        } else if (!isInt(t)) {
            error(c, e, "'%s' can't compare %s values", tokName(op), t->name);
        }
        return setType(e, &tyBool);
    }
    if (bitOp && t->kind == TY_ENUM && t->isFlags) {
        return setType(e, t);
    }
    if (!isInt(t)) {
        error(c, e, "'%s' needs integers, not %s", tokName(op), t->name);
        return setType(e, &tyError);
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
    if (t->kind == TY_ENUM && strcmp(e->name, "name") == 0) {
        e->builtin = 1;                 /* the value's name, from the runtime's table */
        return setType(e, &tyStr);
    }
    if (isErr(t)) return setType(e, t);
    if (e->a->kind == N_IDENT && e->a->ref && (e->a->ref->kind == N_STRUCT || e->a->ref->kind == N_TYPEDEF)) {
        error(c, e, "%s is a type; methods are called on values", e->a->name);
        return setType(e, &tyError);
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
        notYet(c, e->a, "types from modules", "0.7");
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
        notYet(c, e, "floating point numbers", "a later version");
        return setType(e, &tyError);
    case N_STRING:
        return setType(e, &tyStr);
    case N_INTERP:
        error(c, e, "text with {} values can be passed to print, panic and format");
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
            } else {
                error(c, e, "%s is not declared", e->name);
            }
            return setType(e, &tyError);
        }
        e->ref = d;
        switch (d->kind) {
        case N_FUNC:
            notYet(c, e, "functions as values", "a later version");
            return setType(e, &tyError);
        case N_STRUCT: case N_TYPEDEF:
            error(c, e, "%s is a type, not a value", e->name);
            return setType(e, &tyError);
        case N_CONST:
            checkConstDecl(c, d);
            return setType(e, d->type);
        default:
            return setType(e, d->type ? d->type : &tyError);
        }
    }
    case N_UNARY: {
        switch (e->op) {
        case P_MINUS: {
            Type *t = checkExpr(c, &e->a, expected && isInt(expected) ? expected : NULL);
            if (isErr(t)) return setType(e, t);
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
            coerce(c, &e->a, &tyBool, "the value after '!'");
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
        default:
            notYet(c, e, "channels", "a later version");
            return setType(e, &tyError);
        }
    }
    case N_BINARY:
        return checkBinary(c, e, expected);
    case N_CALL:
        return checkCall(c, e, expected);
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
        notYet(c, e, "this expression", "a later version");
        return setType(e, &tyError);
    }
}

static Type *intTypeNamed(const char *name) {
    for (size_t i = 0; i < sizeof builtinTypes / sizeof builtinTypes[0]; i++) {
        if (isInt(builtinTypes[i]) && strcmp(builtinTypes[i]->name, name) == 0) {
            return builtinTypes[i];
        }
    }
    return NULL;
}

static bool isBuiltinName(const char *name) {
    return strcmp(name, "print") == 0 || strcmp(name, "panic") == 0 || strcmp(name, "format") == 0 ||
           strcmp(name, "alloc") == 0 || strcmp(name, "free") == 0 || strcmp(name, "error") == 0;
}

static bool printable(Type *t) {
    return isInt(t) || t == &tyBool || isStr(t) || t->kind == TY_STRING || t->kind == TY_ENUM ||
           t->kind == TY_ERRCODE;
}

static void checkText(Checker *c, Node **slot, const char *fn);

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
            if (!isErr(t) && !printable(t)) {
                error(c, *part, "a %s value can't be put into text", t->name);
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
    for (int i = 0; i < given->len; i++) {
        Node *arg = given->items[i];
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
            int n = np - first;
            error(c, arg, "%s takes %d argument%s, but more are given", fn->name, n, n == 1 ? "" : "s");
            break;
        }
        args[pos++] = arg;
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
        } else {
            if (!checked) {
                checkExpr(c, &args[j], param->type);
            }
            char what[96];
            snprintf(what, sizeof what, "the argument for %s", param->name);
            coerce(c, &args[j], param->type, what);
        }
        listPush(c->arena, out, args[j]);
    }
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
                Type *at = checkExpr(c, &e->list.items[0], &tyStr);
                if (!isErr(at)) {
                    coerce(c, &e->list.items[0], &tyStr, "the text to add");
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
    Node *m = findMethod(st, name, path, &n, 16);
    if (!m) {
        Node *fpath[16];
        if (findField(st, name, fpath, 16)) {
            notYet(c, callee, "calling a function stored in a field", "a later version");
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
        Node *addr = newNode(c->arena, N_UNARY, recv->line, recv->col);
        addr->op = P_AMP;
        addr->a = recv;
        addr->type = ptrTo(recv->type);
        recv = addr;
    }
    callee->ref = m;
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
    Node *var = newNode(c->arena, N_IDENT, e->line, e->col);
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
    for (int i = 0; i < e->list.len; i++) {
        Node *arg = e->list.items[i];
        Node **slot = arg->kind == N_NAMED_ARG ? &arg->a : &e->list.items[i];
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
                param = d->list.items[pos++];
                infer(param->a, arg->type, &d->list2, args);
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
    checkArgs(c, e, inst, &e->list, 0, &out, true);
    e->list = out;
    e->ref = inst;
    return setType(e, inst->type);
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
        if (callee->a->kind == N_IDENT && !lookup(c, callee->a->name) && !typeFromName(c, callee->a->name, callee, false)) {
            notYet(c, callee, "modules", "0.7");
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
        notYet(c, callee, "calling this kind of expression", "a later version");
        return setType(e, &tyError);
    }
    const char *name = callee->name;
    Node *d = lookup(c, name);

    if (!d && isBuiltinName(name)) {
        if (strcmp(name, "alloc") == 0) {
            return checkAlloc(c, e);
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
        if (!isInt(t)) {
            error(c, e->list.items[0], "%s(...) converts integers, not %s", name, t->name);
            return setType(e, &tyError);
        }
        Node *arg = e->list.items[0];
        if (arg->isConst) {
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
        } else {
            error(c, callee, "%s is not declared", name);
        }
        return setType(e, &tyError);
    }
    if (d->kind != N_FUNC) {
        error(c, callee, "%s is not a function", name);
        return setType(e, &tyError);
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
static bool assignable(Checker *c, Node *target) {
    if (target->kind == N_IDENT) {
        Node *d = target->ref;
        if (!d) {
            return false;
        }
        if (d->kind == N_CONST) {
            error(c, target, "%s is a constant and can't change", target->name);
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
    Type *t = resolveType(c, s->a);
    if (t == &tyVoid) {
        error(c, s->a, "a variable can't be void");
        t = &tyError;
    }
    resolveStruct(c, t);
    s->type = t;
    if (s->flags & (F_THREADLOCAL | F_ATOMIC)) {
        notYet(c, s, "threadlocal and atomic", "a later version");
    }
    if ((s->flags & F_SCOPED) && !isErr(t)) {
        /* scoped needs a free method to call at the end of the block */
        Node *m = t->kind == TY_STRUCT ? findMethodIn(t, "free") : NULL;
        if (t->kind == TY_STRING) {
            s->ref = NULL;
        } else if (!m) {
            error(c, s, "scoped needs a type with a free method; %s has none", t->name);
        } else if (m->list.len != 1) {
            error(c, s, "scoped calls free() with no arguments, but %s.free takes some", t->name);
        } else {
            s->ref = m;
        }
    }
    if (s->b && !isErr(t)) {
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
    if (!isErr(t)) {
        if (t->kind == TY_SLICE || t->kind == TY_ARRAY) {
            elem = t->elem;
            if (t->kind == TY_ARRAY && !addressable(s->a)) {
                /* a temporary array: loop over a copy */
                s->a->flags |= F_EXPLICIT;
            }
        } else {
            error(c, s->a, "for ... in goes over a range, a slice or an array, not %s", t->name);
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
        if (t == &tyVoid) {
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
        if (s->op != P_ASSIGN && !isInt(tt)) {
            error(c, s, "'%s' needs an integer variable, not %s", tokName(s->op), tt->name);
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
        bool used = x->kind == N_CALL || ((x->kind == N_TRY || x->kind == N_CATCH) &&
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
    case N_SELECT:
        notYet(c, s, "tasks and channels", "a later version");
        return;
    case N_WHEN:
        notYet(c, s, "when", "a later version");
        return;
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

static void declareGlobal(Checker *c, Node *d, const char *name) {
    Sym *old = findIn(c->global, name);
    if (old) {
        error(c, d, "%s is already declared on line %d", name, old->decl->line);
        return;
    }
    if (isBuiltinName(name) || typeFromName(c, name, d, false)) {
        error(c, d, "%s is a built-in name", name);
        return;
    }
    addSym(c, c->global, name, d);
}

/* A function's or method's types. recv is the receiver of a generic
 * struct's method, for an instance; otherwise f->b names it. */
static void checkSignature(Checker *c, Node *f, Type *recv) {
    if (f->state == 3) {
        return;                     /* done already */
    }
    f->state = 3;
    if (f->attrs.len) {
        notYet(c, f->attrs.items[0], "attributes", "a later version");
    }
    f->type = resolveType(c, f->a);
    resolveStruct(c, f->type);
    int first = 0;
    if (f->b || recv) {
        /* a method: self, a pointer to the receiver, comes first */
        if (f->list2.len) {
            notYet(c, f, "methods with type parameters of their own", "a later version");
        }
        Type *rt = recv ? recv : resolveType(c, f->b);
        Node *self = newNode(c->arena, N_PARAM, f->line, f->col);
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
        resolveStruct(c, p->type);
        for (int j = 0; j < i; j++) {
            if (strcmp(f->list.items[j]->name, p->name) == 0) {
                error(c, p, "%s is already a parameter", p->name);
            }
        }
        if (i > first && !p->b && f->list.items[i - 1]->b) {
            error(c, p, "a parameter without a default can't follow one with a default");
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
    c->func = f;
    pushScope(c, true);
    for (int i = 0; i < f->list.len; i++) {
        Node *p = f->list.items[i];
        if (p->b) {
            /* defaults are checked like arguments, in the global scope */
            Scope *saved = c->scope;
            c->scope = c->global;
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
        if (f->type == &tyVoid) {
            if (f->d->kind != N_CALL && !isErr(t)) {
                error(c, f->d, "%s returns nothing, so its '=>' body must be a call", f->name);
            }
        } else {
            coerce(c, &f->d, f->type, "the returned value");
        }
    } else if (f->c) {
        checkBlock(c, f->c, false);
        bool needsReturn = f->type != &tyVoid && !isErr(f->type) &&
                           !(f->type->kind == TY_RESULT && f->type->elem == &tyVoid);
        if (needsReturn && blockFallsThrough(f->c)) {
            error(c, f, "%s can reach its end without returning a value", f->name);
        }
    }
    popScope(c);
    c->func = NULL;
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

bool checkProgram(Arena *a, Source *src, Node *file) {
    Checker c = {0};
    c.arena = a;
    c.src = src;
    c.global = pushScope(&c, false);
    gArena = a;
    gTypes = NULL;
    c.instOut = &file->list2;
    int before = gErrors;

    /* names first, so declarations can come in any order */
    for (int i = 0; i < file->list.len; i++) {
        Node *d = file->list.items[i];
        switch (d->kind) {
        case N_FUNC:
            if (!d->b) {
                declareGlobal(&c, d, d->name);
            }
            break;
        case N_CONST: case N_VAR: case N_STRUCT: case N_TYPEDEF: case N_ENUM: case N_INTERFACE:
            declareGlobal(&c, d, d->name);
            break;
        case N_DEFINE:
            if (d->list.len == 1) {
                declareGlobal(&c, d->list.items[0], d->list.items[0]->name);
            }
            break;
        case N_IMPORT:
            notYet(&c, d, "imports", "0.7");
            break;
        case N_TEST:
            notYet(&c, d, "tests", "a later version");
            break;
        case N_WHEN:
            notYet(&c, d, "when", "a later version");
            break;
        default:
            error(&c, d, "this can't appear outside a function");
            break;
        }
    }

    /* methods are listed with their structs early, so interfaces can see them */
    for (int i = 0; i < file->list.len; i++) {
        Node *d = file->list.items[i];
        if (d->kind != N_FUNC || !d->b) {
            continue;
        }
        if (d->b->kind == N_T_GENERIC) {
            Node *g = d->b->a->kind == N_T_NAME ? lookup(&c, d->b->a->name) : NULL;
            if (!g || g->kind != N_STRUCT || !g->list2.len) {
                error(&c, d->b, "%s is not a generic struct", d->b->a->kind == N_T_NAME ? d->b->a->name : "this");
            } else if (g->list2.len != d->b->list.len) {
                error(&c, d->b, "%s takes %d type%s", g->name, g->list2.len, g->list2.len == 1 ? "" : "s");
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
            Type *rt = structType(&c, sd);
            if (findMethodIn(rt, d->name)) {
                error(&c, d, "%s already has a method named %s", rt->name, d->name);
                d->state = 4;
            } else {
                listPush(a, &rt->methods, d);
            }
        }
    }

    for (int i = 0; i < file->list.len; i++) {
        Node *d = file->list.items[i];
        if (d->kind == N_STRUCT && !d->list2.len) {
            resolveStruct(&c, structType(&c, d));
        } else if (d->kind == N_ENUM) {
            enumType(&c, d);
        } else if (d->kind == N_TYPEDEF) {
            typeFromName(&c, d->name, d, true);
        }
    }

    for (int i = 0; i < file->list.len; i++) {
        Node *d = file->list.items[i];
        if (d->kind == N_FUNC && !d->list2.len && d->state != 4) {
            checkSignature(&c, d, NULL);
        }
    }

    /* field defaults are checked like constants */
    for (int i = 0; i < file->list.len; i++) {
        Node *d = file->list.items[i];
        if (d->kind != N_STRUCT || d->list2.len) {
            continue;
        }
        for (int j = 0; j < d->list.len; j++) {
            Node *f = d->list.items[j];
            if (f->kind == N_FIELD && f->b && f->type) {
                checkGlobalValue(&c, &f->b, f->type);
            }
        }
    }

    for (int i = 0; i < file->list.len; i++) {
        Node *d = file->list.items[i];
        if (d->kind == N_CONST) {
            checkConstDecl(&c, d);
        } else if (d->kind == N_VAR) {
            Type *t = resolveType(&c, d->a);
            resolveStruct(&c, t);
            d->type = t;
            if (d->flags & (F_THREADLOCAL | F_ATOMIC)) {
                notYet(&c, d, "threadlocal and atomic", "a later version");
            }
            if (d->b && !isErr(t)) {
                checkGlobalValue(&c, &d->b, t);
            }
        } else if (d->kind == N_DEFINE && d->list.len == 1) {
            Node *id = d->list.items[0];
            checkGlobalValue(&c, &d->b, NULL);
            id->type = d->b->type;
            d->type = d->b->type;
        }
    }

    Node *mainFn = NULL;
    for (int i = 0; i < file->list.len; i++) {
        Node *d = file->list.items[i];
        if (d->kind == N_FUNC && !d->list2.len && d->state != 4) {
            checkFunctionBody(&c, d);
            if (!d->b && strcmp(d->name, "main") == 0) {
                mainFn = d;
            }
        }
    }

    /* the bodies of generic instances; checking one can make more */
    for (int i = 0; i < c.pendFns.len; i++) {
        Scope *saved = c.scope;
        c.scope = c.pendScopes[i];
        int errs = gErrors;
        Node *fn = c.pendFns.items[i];
        c.instGen = (int)fn->cval;
        checkFunctionBody(&c, fn);
        c.scope = saved;
        Node *site = c.pendSites.items[i];
        if (gErrors != errs && site && site->kind == N_CALL) {
            error(&c, site, "%s, made here, has the errors above", fn->name);
        }
    }

    /* field defaults of generic instances */
    for (int i = 0; i < file->list2.len; i++) {
        Node *d = file->list2.items[i];
        if (d->kind != N_STRUCT) continue;
        for (int j = 0; j < d->list.len; j++) {
            Node *f = d->list.items[j];
            if (f->kind == N_FIELD && f->b && f->type) {
                Scope *saved = c.scope;
                c.scope = d->type->scope;
                checkGlobalValue(&c, &f->b, f->type);
                c.scope = saved;
            }
        }
    }

    if (!mainFn) {
        errorAt(src, 1, 1, "the program has no main function");
    } else {
        if (mainFn->list.len) {
            error(&c, mainFn, "main takes no parameters");
        }
        Type *mt = mainFn->type;
        if (mt != &tyVoid && mt != &tyI32 && !isErr(mt) && !(mt->kind == TY_RESULT && mt->elem == &tyVoid)) {
            error(&c, mainFn, "main must return void, i32 or !void");
        }
    }
    return gErrors == before;
}
