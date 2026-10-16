/* The checker: looks up names, works out the type of every expression and
 * checks the rules the parser can't see. It fills in node->type, node->ref
 * and, for values known at compile time, node->isConst and node->cval.
 *
 * Errors don't stop it. An expression with an error gets tyError, which
 * fits anywhere, so one mistake gives one message. */
#include "iris.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

Type tyError = {TY_ERROR, 0, false, "error"};
Type tyVoid = {TY_VOID, 0, false, "void"};
Type tyNoreturn = {TY_NORETURN, 0, false, "noreturn"};
Type tyBool = {TY_BOOL, 1, false, "bool"};
Type tyStr = {TY_STR, 0, false, "str"};
Type tyI8 = {TY_INT, 8, true, "i8"};
Type tyI16 = {TY_INT, 16, true, "i16"};
Type tyI32 = {TY_INT, 32, true, "i32"};
Type tyI64 = {TY_INT, 64, true, "i64"};
Type tyU8 = {TY_INT, 8, false, "u8"};
Type tyU16 = {TY_INT, 16, false, "u16"};
Type tyU32 = {TY_INT, 32, false, "u32"};
Type tyU64 = {TY_INT, 64, false, "u64"};

static Type *builtinTypes[] = {&tyI8, &tyI16, &tyI32, &tyI64, &tyU8, &tyU16, &tyU32,
                               &tyU64, &tyBool, &tyStr, &tyVoid};

/* types that exist in the language but not in this version yet */
static const struct {
    const char *name, *version;
} laterTypes[] = {
    {"f32", "a later version"}, {"f64", "a later version"}, {"String", "0.3"},
    {"Duration", "a later version"}, {"Error", "0.4"},
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
} Checker;

static void checkStmt(Checker *c, Node *s);

static void error(Checker *c, Node *at, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    verrorAt(c->src, at->line, at->col, fmt, ap);
    va_end(ap);
}

static void notYet(Checker *c, Node *at, const char *what, const char *version) {
    bool plural = what[strlen(what) - 1] == 's' || strstr(what, " and ");
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

static Type *resolveType(Checker *c, Node *t) {
    if (!t) {
        return &tyError;
    }
    switch (t->kind) {
    case N_T_NAME:
        for (size_t i = 0; i < sizeof builtinTypes / sizeof builtinTypes[0]; i++) {
            if (strcmp(builtinTypes[i]->name, t->name) == 0) {
                return builtinTypes[i];
            }
        }
        for (size_t i = 0; i < sizeof laterTypes / sizeof laterTypes[0]; i++) {
            if (strcmp(laterTypes[i].name, t->name) == 0) {
                error(c, t, "the type %s is not supported yet (planned for %s)", t->name, laterTypes[i].version);
                return &tyError;
            }
        }
        error(c, t, "unknown type %s", t->name);
        return &tyError;
    case N_T_PTR:
        notYet(c, t, "pointers", "0.3");
        return &tyError;
    case N_T_SLICE:
    case N_T_ARRAY:
        notYet(c, t, "arrays and slices", "0.3");
        return &tyError;
    case N_T_ERR:
        notYet(c, t, "error results (!T)", "0.4");
        return &tyError;
    case N_T_TUPLE:
        notYet(c, t, "several return values", "0.5");
        return &tyError;
    case N_T_GENERIC:
        notYet(c, t, "generic types", "0.6");
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

static bool sameType(Type *a, Type *b) {
    return a == b || (a->kind == b->kind && a->kind != TY_INT) ||
           (isInt(a) && isInt(b) && a->bits == b->bits && a->isSigned == b->isSigned);
}

static Node *convertNode(Checker *c, Node *e, Type *to, bool explicit) {
    Node *n = newNode(c->arena, N_CONVERT, e->line, e->col);
    n->a = e;
    n->type = to;
    if (explicit) {
        n->flags |= F_EXPLICIT;
    }
    return n;
}

/* Makes the expression in *slot fit `to`, adding a widening conversion if
 * needed. what describes the place for the error message. */
static void coerce(Checker *c, Node **slot, Type *to, const char *what) {
    Node *e = *slot;
    Type *from = e->type;
    if (isErr(from) || isErr(to) || from->kind == TY_NORETURN) {
        return;
    }
    if (sameType(from, to)) {
        return;
    }
    if (e->isConst && isInt(from) && isInt(to)) {
        if (fitsIn(e->cval, to)) {
            e->type = to;
            return;
        }
        error(c, e, "%lld doesn't fit in %s", (long long)e->cval, to->name);
        return;
    }
    if (widens(from, to)) {
        *slot = convertNode(c, e, to, false);
        return;
    }
    if (isInt(from) && isInt(to)) {
        error(c, e, "%s is %s, which doesn't always fit in %s; convert it with %s(...)", what,
              from->name, to->name, to->name);
        return;
    }
    error(c, e, "%s is %s, but %s is needed", what, from->name, to->name);
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
        case P_TILDE:
            if (b) return false;
            *out = ~v;
            return true;
        default:
            return false;
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
        if (declared && declared != &tyStr && !isErr(declared)) {
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

static Type *checkCall(Checker *c, Node *e, Type *expected);

static Type *checkBinary(Checker *c, Node *e, Type *expected) {
    TokKind op = e->op;
    if (op == P_DOTDOT) {
        error(c, e, "a range can only be used in 'for' and 'match'");
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
    Type *want = compare ? NULL : expected;
    if (want && !isInt(want)) {
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
        tb = checkExpr(c, &e->b, isInt(ta) ? ta : want);
    }
    if (isErr(ta) || isErr(tb)) {
        return setType(e, compare ? &tyBool : &tyError);
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
        } else {
            error(c, e, "'%s' can't mix %s and %s; convert one side, for example %s(...)", tokName(op),
                  ta->name, tb->name, ta->name);
            return setType(e, compare ? &tyBool : &tyError);
        }
    }

    if (compare) {
        if (t == &tyStr || t == &tyBool) {
            if (op != P_EQ && op != P_NE) {
                error(c, e, "'%s' can't compare %s values", tokName(op), t->name);
            }
        } else if (!isInt(t)) {
            error(c, e, "'%s' can't compare %s values", tokName(op), t->name);
        }
        return setType(e, &tyBool);
    }
    if (!isInt(t)) {
        error(c, e, "'%s' needs integers, not %s", tokName(op), t->name);
        return setType(e, &tyError);
    }
    return setType(e, t);
}

static Type *checkExpr(Checker *c, Node **slot, Type *expected) {
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
        error(c, e, "text with {} values can only be passed to print for now (format comes in 0.3)");
        return setType(e, &tyError);
    case N_NULL:
        notYet(c, e, "pointers", "0.3");
        return setType(e, &tyError);
    case N_IDENT: {
        if (strcmp(e->name, "_") == 0) {
            error(c, e, "'_' can only be assigned to");
            return setType(e, &tyError);
        }
        Node *d = lookup(c, e->name);
        if (!d) {
            if (strcmp(e->name, "print") == 0 || strcmp(e->name, "panic") == 0) {
                error(c, e, "%s can only be called", e->name);
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
        case P_BANG: {
            Type *t = checkExpr(c, &e->a, &tyBool);
            coerce(c, &e->a, &tyBool, "the value after '!'");
            (void)t;
            return setType(e, &tyBool);
        }
        case P_TILDE: {
            Type *t = checkExpr(c, &e->a, expected && isInt(expected) ? expected : NULL);
            if (!isErr(t) && !isInt(t)) {
                error(c, e, "'~' needs an integer, not %s", t->name);
                return setType(e, &tyError);
            }
            return setType(e, t);
        }
        case P_AMP:
        case P_STAR:
            notYet(c, e, "pointers", "0.3");
            return setType(e, &tyError);
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
    case N_FIELD_ACCESS: {
        Type *t = checkExpr(c, &e->a, NULL);
        if (isErr(t)) return setType(e, t);
        if (t == &tyStr && strcmp(e->name, "len") == 0) {
            return setType(e, &tyI64);
        }
        if (e->a->kind == N_IDENT && !e->a->ref) {
            notYet(c, e, "modules", "0.7");
        } else if (t == &tyStr) {
            error(c, e, "str has no field %s", e->name);
        } else {
            notYet(c, e, "fields and methods", "0.3");
        }
        return setType(e, &tyError);
    }
    case N_STRUCT_LIT:
    case N_FIELD_INIT:
        notYet(c, e, "structs", "0.3");
        return setType(e, &tyError);
    case N_INDEX:
    case N_SLICE:
    case N_ARRAY_LIT:
        notYet(c, e, "arrays and slices", "0.3");
        return setType(e, &tyError);
    case N_TRY:
    case N_CATCH:
        notYet(c, e, "errors (try and catch)", "0.4");
        return setType(e, &tyError);
    case N_ENUM_DOT:
        notYet(c, e, "enums", "0.5");
        return setType(e, &tyError);
    case N_TUPLE:
        notYet(c, e, "several return values", "0.5");
        return setType(e, &tyError);
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

/* print(text) and panic(text): one str argument, or text with {} values. */
static void checkTextArg(Checker *c, Node *call, const char *fn) {
    if (call->list.len != 1 || call->list.items[0]->kind == N_NAMED_ARG) {
        error(c, call, "%s takes one argument, the text", fn);
        return;
    }
    Node *arg = call->list.items[0];
    if (arg->kind == N_INTERP) {
        arg->type = &tyStr;
        for (int i = 0; i < arg->list.len; i++) {
            Node **part = &arg->list.items[i];
            if ((*part)->kind == N_STRING) {
                (*part)->type = &tyStr;
                continue;
            }
            Type *t = checkExpr(c, part, NULL);
            if (!isErr(t) && !isInt(t) && t != &tyBool && t != &tyStr) {
                error(c, *part, "a %s value can't be put into text", t->name);
            }
        }
        return;
    }
    Type *t = checkExpr(c, &call->list.items[0], &tyStr);
    if (!isErr(t) && t != &tyStr) {
        error(c, arg, "%s takes text, not %s; write %s(\"{...}\") to print a value", fn, t->name, fn);
    }
}

static Type *checkCall(Checker *c, Node *e, Type *expected) {
    Node *callee = e->a;
    if (callee->kind != N_IDENT) {
        if (callee->kind == N_FIELD_ACCESS) {
            notYet(c, callee, "methods and modules", "0.3");
        } else {
            notYet(c, callee, "calling this kind of expression", "a later version");
        }
        return setType(e, &tyError);
    }
    const char *name = callee->name;
    Node *d = lookup(c, name);

    if (!d && (strcmp(name, "print") == 0 || strcmp(name, "panic") == 0)) {
        checkTextArg(c, e, name);
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
        if (strcmp(name, "alloc") == 0 || strcmp(name, "free") == 0) {
            notYet(c, callee, "memory allocation", "0.3");
        } else if (strcmp(name, "error") == 0) {
            notYet(c, callee, "errors", "0.4");
        } else if (strcmp(name, "format") == 0) {
            notYet(c, callee, "format", "0.3");
        } else {
            error(c, callee, "%s is not declared", name);
        }
        return setType(e, &tyError);
    }
    if (d->kind != N_FUNC) {
        error(c, callee, "%s is not a function", name);
        return setType(e, &tyError);
    }
    callee->ref = d;

    /* line the arguments up with the parameters: positional first, then
     * named, then defaults */
    int np = d->list.len;
    Node **args = arenaAlloc(c->arena, sizeof(Node *) * (size_t)(np ? np : 1));
    bool named = false;
    int before = gErrors;
    for (int i = 0; i < e->list.len; i++) {
        Node *arg = e->list.items[i];
        if (arg->kind == N_NAMED_ARG) {
            named = true;
            int k = -1;
            for (int j = 0; j < np; j++) {
                if (strcmp(d->list.items[j]->name, arg->name) == 0) {
                    k = j;
                }
            }
            if (k < 0) {
                error(c, arg, "%s has no parameter named %s", name, arg->name);
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
        if (i >= np) {
            error(c, arg, "%s takes %d argument%s, but more are given", name, np, np == 1 ? "" : "s");
            break;
        }
        args[i] = arg;
    }
    NodeList list = {0};
    for (int j = 0; j < np; j++) {
        Node *param = d->list.items[j];
        if (!args[j]) {
            if (param->b) {
                args[j] = param->b;     /* the default: checked with the function */
            } else {
                if (gErrors == before) {
                    error(c, e, "%s needs a value for %s", name, param->name);
                }
                continue;
            }
        } else {
            checkExpr(c, &args[j], param->type);
            char what[96];
            snprintf(what, sizeof what, "the argument for %s", param->name);
            coerce(c, &args[j], param->type, what);
        }
        listPush(c->arena, &list, args[j]);
    }
    e->list = list;
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
            Node wrap = {0};
            wrap.kind = N_BLOCK;
            listPush(c->arena, &wrap.list, block);
            return checkBlockValue(c, &wrap, expected);
        }
        error(c, block, "this arm has to give a value");
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
static Type *unify(Checker *c, Node **a, Node **b, Type *ta, Type *tb, Node *at) {
    if (isErr(ta) || isErr(tb)) return &tyError;
    if (ta->kind == TY_NORETURN) return tb;
    if (tb->kind == TY_NORETURN) return ta;
    if (sameType(ta, tb)) return ta;
    if (widens(ta, tb)) {
        (void)a;
        return tb;
    }
    if (widens(tb, ta)) {
        (void)b;
        return ta;
    }
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

static Type *checkIf(Checker *c, Node *n, Type *expected, bool wantValue) {
    checkExpr(c, &n->a, &tyBool);
    if (!isErr(n->a->type) && n->a->type != &tyBool) {
        error(c, n->a, "a condition must be bool, not %s", n->a->type->name);
    }
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
    Type *tb = n->d->kind == N_IF ? setType(n->d, checkIf(c, n->d, expected ? expected : ta, true))
                                  : checkBlockValue(c, n->d, expected ? expected : (ta->kind == TY_NORETURN ? NULL : ta));
    Type *t = expected && !isErr(expected) ? expected : unify(c, NULL, NULL, ta, tb, n);
    if (!isErr(t)) {
        coerceValue(c, n->c, t);
        if (n->d->kind != N_IF) {
            coerceValue(c, n->d, t);
        }
    }
    return t;
}

static void checkPattern(Checker *c, Node **slot, Type *subject) {
    Node *p = *slot;
    if (p->kind == N_IDENT && strcmp(p->name, "_") == 0) {
        p->type = subject;
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
    if (!p->isConst && p->kind != N_STRING) {
        error(c, p, "a match pattern must be a constant");
        return;
    }
    coerce(c, slot, subject, "the pattern");
}

static bool isWildcard(Node *p) { return p->kind == N_IDENT && strcmp(p->name, "_") == 0; }

static Type *checkMatch(Checker *c, Node *m, Type *expected, bool wantValue) {
    Type *subject = checkExpr(c, &m->a, NULL);
    if (!isErr(subject) && !isInt(subject) && subject != &tyBool && subject != &tyStr) {
        error(c, m->a, "match works on integers, bool and str, not %s", subject->name);
        subject = &tyError;
    }
    bool hasWildcard = false, hasTrue = false, hasFalse = false;
    Type *t = expected && !isErr(expected) ? expected : NULL;
    Type *first = NULL;
    for (int i = 0; i < m->list.len; i++) {
        Node *arm = m->list.items[i];
        for (int j = 0; j < arm->list.len; j++) {
            Node **slot = &arm->list.items[j];
            if (hasWildcard) {
                error(c, *slot, "this arm can never run: '_' above it matches everything");
            }
            checkPattern(c, slot, subject);
            if (isWildcard(*slot)) {
                hasWildcard = true;
            } else if ((*slot)->type == &tyBool && (*slot)->isConst) {
                if ((*slot)->cval) hasTrue = true;
                else hasFalse = true;
            }
        }
        if (wantValue) {
            Type *at = checkBlockValue(c, arm->c, t ? t : first);
            if (!first && at->kind != TY_NORETURN && !isErr(at)) {
                first = at;
            } else if (first && !t && !isErr(at)) {
                Type *u = unify(c, NULL, NULL, first, at, arm);
                if (!isErr(u)) first = u;
            }
        } else if (arm->c->kind == N_BLOCK) {
            checkBlock(c, arm->c, true);
        } else {
            checkStmt(c, arm->c);
        }
    }
    if (!wantValue) {
        return &tyVoid;
    }
    if (!hasWildcard && !(hasTrue && hasFalse)) {
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

static bool isAssignable(Checker *c, Node *target) {
    if (target->kind != N_IDENT) {
        if (target->kind == N_FIELD_ACCESS || target->kind == N_INDEX || target->kind == N_UNARY) {
            notYet(c, target, "assigning to fields, elements and pointers", "0.3");
        } else {
            error(c, target, "this can't be assigned to");
        }
        return false;
    }
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
    return true;
}

static void checkLocalVar(Checker *c, Node *s) {
    Type *t = resolveType(c, s->a);
    if (t == &tyVoid) {
        error(c, s->a, "a variable can't be void");
        t = &tyError;
    }
    s->type = t;
    if (s->flags & (F_SCOPED | F_THREADLOCAL | F_ATOMIC)) {
        notYet(c, s, (s->flags & F_SCOPED) ? "scoped" : "threadlocal and atomic", (s->flags & F_SCOPED) ? "0.4" : "a later version");
    }
    if (s->b && !isErr(t)) {
        checkExpr(c, &s->b, t);
        coerce(c, &s->b, t, "the value");
    }
    declareLocal(c, s->name, s);
}

static void checkStmt(Checker *c, Node *s) {
    switch (s->kind) {
    case N_VAR:
        checkLocalVar(c, s);
        return;
    case N_DEFINE: {
        if (s->list.len != 1) {
            notYet(c, s, "several return values", "0.5");
            return;
        }
        Node *id = s->list.items[0];
        Type *t = checkExpr(c, &s->b, NULL);
        if (t == &tyVoid) {
            error(c, s->b, "this gives no value to store");
            t = &tyError;
        } else if (t->kind == TY_NORETURN) {
            t = &tyError;
        }
        id->type = t;
        s->type = t;
        declareLocal(c, id->name, id);
        return;
    }
    case N_ASSIGN: {
        if (s->list.len != 1) {
            notYet(c, s, "several return values", "0.5");
            return;
        }
        Node **target = &s->list.items[0];
        if ((*target)->kind == N_IDENT && strcmp((*target)->name, "_") == 0) {
            if (s->op != P_ASSIGN) {
                error(c, s, "'_' can only be assigned with '='");
            }
            Type *t = checkExpr(c, &s->b, NULL);
            if (t == &tyVoid) {
                error(c, s->b, "this gives no value to drop");
            }
            (*target)->type = &tyError;
            return;
        }
        Type *tt = checkExpr(c, target, NULL);
        if (isErr(tt) || !isAssignable(c, *target)) {
            checkExpr(c, &s->b, NULL);
            return;
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
        if (s->a->kind != N_CALL && !isErr(s->a->type)) {
            error(c, s->a, "this value is not used");
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
        if (!s->a) {
            if (ret != &tyVoid && !isErr(ret)) {
                error(c, s, "%s must return a value of type %s", c->func->name, ret->name);
            }
            return;
        }
        if (ret == &tyVoid) {
            error(c, s->a, "%s returns nothing, so 'return' takes no value", c->func->name);
            checkExpr(c, &s->a, NULL);
            return;
        }
        checkExpr(c, &s->a, ret);
        coerce(c, &s->a, ret, "the returned value");
        return;
    }
    case N_BREAK:
    case N_CONTINUE: {
        const char *word = s->kind == N_BREAK ? "break" : "continue";
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
            Node *it = s->a;
            if (s->list.len != 1) {
                notYet(c, s, "looping with an index", "0.3");
            } else if (it->kind != N_BINARY || it->op != P_DOTDOT) {
                notYet(c, it, "looping over slices and maps", "0.3");
            } else {
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
            }
        } else if (s->a) {
            checkExpr(c, &s->a, &tyBool);
            if (!isErr(s->a->type) && s->a->type != &tyBool) {
                error(c, s->a, "a condition must be bool, not %s", s->a->type->name);
            }
        }
        c->loop = &loop;
        checkBlock(c, s->c, true);
        c->loop = loop.outer;
        popScope(c);
        return;
    }
    case N_DEFER:
    case N_ERRDEFER:
        notYet(c, s, "defer and errdefer", "0.4");
        return;
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
        return !wildcard;
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
    if (strcmp(name, "print") == 0 || strcmp(name, "panic") == 0 || intTypeNamed(name)) {
        error(c, d, "%s is a built-in name", name);
        return;
    }
    addSym(c, c->global, name, d);
}

static void checkSignature(Checker *c, Node *f) {
    if (f->b) {
        notYet(c, f, "methods", "0.3");
    }
    if (f->list2.len) {
        notYet(c, f, "generic functions", "0.6");
    }
    if (f->attrs.len) {
        notYet(c, f->attrs.items[0], "attributes", "a later version");
    }
    f->type = resolveType(c, f->a);
    for (int i = 0; i < f->list.len; i++) {
        Node *p = f->list.items[i];
        p->type = resolveType(c, p->a);
        if (p->type == &tyVoid) {
            error(c, p, "a parameter can't be void");
            p->type = &tyError;
        }
        for (int j = 0; j < i; j++) {
            if (strcmp(f->list.items[j]->name, p->name) == 0) {
                error(c, p, "%s is already a parameter", p->name);
            }
        }
        if (i > 0 && !p->b && f->list.items[i - 1]->b) {
            error(c, p, "a parameter without a default can't follow one with a default");
        }
    }
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
            if (!p->b->isConst && p->b->kind != N_STRING && !isErr(p->b->type)) {
                error(c, p->b, "a default value must be known when compiling");
            }
            c->scope = saved;
        }
        declareLocal(c, p->name, p);
    }
    if (f->d) {
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
        if (f->type != &tyVoid && !isErr(f->type) && blockFallsThrough(f->c)) {
            error(c, f, "%s can reach its end without returning a value", f->name);
        }
    }
    popScope(c);
    c->func = NULL;
}

bool checkProgram(Arena *a, Source *src, Node *file) {
    Checker c = {0};
    c.arena = a;
    c.src = src;
    c.global = pushScope(&c, false);
    int before = gErrors;

    /* names first, so declarations can come in any order */
    for (int i = 0; i < file->list.len; i++) {
        Node *d = file->list.items[i];
        switch (d->kind) {
        case N_FUNC: case N_CONST: case N_VAR:
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
        case N_STRUCT:
            notYet(&c, d, "structs", "0.3");
            break;
        case N_ENUM:
            notYet(&c, d, "enums", "0.5");
            break;
        case N_INTERFACE:
            notYet(&c, d, "interfaces", "0.6");
            break;
        case N_TYPEDEF:
            notYet(&c, d, "type aliases", "0.3");
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

    for (int i = 0; i < file->list.len; i++) {
        Node *d = file->list.items[i];
        if (d->kind == N_FUNC) {
            checkSignature(&c, d);
        }
    }

    for (int i = 0; i < file->list.len; i++) {
        Node *d = file->list.items[i];
        if (d->kind == N_CONST) {
            checkConstDecl(&c, d);
        } else if (d->kind == N_VAR) {
            Type *t = resolveType(&c, d->a);
            d->type = t;
            if (d->flags & (F_THREADLOCAL | F_ATOMIC)) {
                notYet(&c, d, "threadlocal and atomic", "a later version");
            }
            if (d->b) {
                checkExpr(&c, &d->b, t);
                coerce(&c, &d->b, t, "the value");
                if (!d->b->isConst && d->b->kind != N_STRING && !isErr(d->b->type)) {
                    error(&c, d->b, "a global's value must be known when compiling");
                }
            }
        } else if (d->kind == N_DEFINE && d->list.len == 1) {
            Node *id = d->list.items[0];
            Type *t = checkExpr(&c, &d->b, NULL);
            if (!d->b->isConst && d->b->kind != N_STRING && !isErr(t)) {
                error(&c, d->b, "a global's value must be known when compiling");
            }
            id->type = t;
            d->type = t;
        }
    }

    Node *mainFn = NULL;
    for (int i = 0; i < file->list.len; i++) {
        Node *d = file->list.items[i];
        if (d->kind == N_FUNC) {
            checkFunctionBody(&c, d);
            if (strcmp(d->name, "main") == 0) {
                mainFn = d;
            }
        }
    }

    if (!mainFn) {
        errorAt(src, 1, 1, "the program has no main function");
    } else {
        if (mainFn->list.len) {
            error(&c, mainFn, "main takes no parameters");
        }
        if (mainFn->type != &tyVoid && mainFn->type != &tyI32 && !isErr(mainFn->type)) {
            error(&c, mainFn, "main must return void or i32");
        }
    }
    return gErrors == before;
}
