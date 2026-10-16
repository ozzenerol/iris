/* The IR writer: the checked tree to LLVM IR, as text.
 *
 * Every variable gets a stack slot (alloca) in the function's entry block;
 * clang's optimiser turns them into registers. Values in registers are
 * named %t1, %t2, ...; labels L1, L2, ...
 *
 * In debug builds, arithmetic that overflows, division by zero, shifts by
 * too much and conversions that lose a value call iris_check_failed. */
#include "iris.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *s;          /* an operand: %t3, 42, true, { ptr @s1, i64 5 } */
    Type *t;
} Val;

typedef struct Loop Loop;
struct Loop {
    Loop *outer;
    const char *label;
    int breakTo, continueTo;
};

typedef struct {
    Arena *arena;
    Source *src;
    BuildMode mode;
    StrBuf head;            /* string constants and globals */
    StrBuf allocas;         /* the current function's entry block */
    StrBuf body;            /* the current function's code */
    int tmp, label, strings, vars;
    bool terminated;        /* the current block has ended */
    Node *func;
    Loop *loop;
    const char *pendingLabel;
    const char *fileStr;    /* the source path, as a C string constant */
} IR;

static bool checks(IR *ir) { return ir->mode == MODE_DEBUG; }

static const char *fmt(IR *ir, const char *f, ...) {
    va_list ap;
    va_start(ap, f);
    char buf[512];
    vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return arenaStrndup(ir->arena, buf, strlen(buf));
}

static const char *llType(Type *t) {
    switch (t->kind) {
    case TY_BOOL: return "i1";
    case TY_INT:
        switch (t->bits) {
        case 8: return "i8";
        case 16: return "i16";
        case 32: return "i32";
        default: return "i64";
        }
    case TY_STR: return "{ ptr, i64 }";
    default: return "void";
    }
}

/* Starts a new block if the current one has ended, so code after a return
 * still forms valid IR. */
static void ensureBlock(IR *ir) {
    if (ir->terminated) {
        sbPrintf(&ir->body, "L%d:\n", ++ir->label);
        ir->terminated = false;
    }
}

static void ins(IR *ir, const char *f, ...) {
    ensureBlock(ir);
    va_list ap;
    va_start(ap, f);
    char buf[1024];
    vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    sbPrintf(&ir->body, "  %s\n", buf);
}

static const char *tmp(IR *ir) { return fmt(ir, "%%t%d", ++ir->tmp); }

static int newLabel(IR *ir) { return ++ir->label; }

static void br(IR *ir, int l) {
    if (!ir->terminated) {
        ins(ir, "br label %%L%d", l);
        ir->terminated = true;
    }
}

static void condBr(IR *ir, const char *cond, int yes, int no) {
    ins(ir, "br i1 %s, label %%L%d, label %%L%d", cond, yes, no);
    ir->terminated = true;
}

static void placeLabel(IR *ir, int l) {
    br(ir, l);
    sbPrintf(&ir->body, "L%d:\n", l);
    ir->terminated = false;
}

static void putEscaped(StrBuf *b, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 32 || c >= 127 || c == '"' || c == '\\') {
            sbPrintf(b, "\\%02X", c);
        } else {
            sbPutc(b, (char)c);
        }
    }
}

/* A byte string in the program, NUL-terminated so C can read it too. */
static const char *stringConst(IR *ir, const char *s, size_t n) {
    int id = ++ir->strings;
    sbPrintf(&ir->head, "@s%d = private unnamed_addr constant [%zu x i8] c\"", id, n + 1);
    putEscaped(&ir->head, s, n);
    sbPuts(&ir->head, "\\00\"\n");
    return fmt(ir, "@s%d", id);
}

static const char *cString(IR *ir, const char *s) { return stringConst(ir, s, strlen(s)); }

static const char *allocaFor(IR *ir, Type *t, const char *hint) {
    const char *name = fmt(ir, "%%v.%s.%d", hint, ++ir->vars);
    sbPrintf(&ir->allocas, "  %s = alloca %s\n", name, llType(t));
    return name;
}

/* A constant integer as LLVM writes it: the value sign-extended from the
 * type's width. */
static const char *intConst(IR *ir, int64_t v, Type *t) {
    if (t->bits < 64) {
        uint64_t mask = ((uint64_t)1 << t->bits) - 1;
        uint64_t u = (uint64_t)v & mask;
        if (u >> (t->bits - 1)) {
            u |= ~mask;
        }
        v = (int64_t)u;
    }
    return fmt(ir, "%lld", (long long)v);
}

static void checkFailed(IR *ir, Node *at, const char *msg) {
    const char *m = cString(ir, msg);
    const char *fn = cString(ir, ir->func ? ir->func->name : "?");
    ins(ir, "call void @iris_check_failed(ptr %s, ptr %s, i32 %d, i32 %d, ptr %s)", m, ir->fileStr,
        at->line, at->col, fn);
    ins(ir, "unreachable");
    ir->terminated = true;
}

/* Branches to a failure if cond is true. */
static void failIf(IR *ir, const char *cond, Node *at, const char *msg) {
    int bad = newLabel(ir), ok = newLabel(ir);
    ins(ir, "br i1 %s, label %%L%d, label %%L%d, !prof !0", cond, bad, ok);
    ir->terminated = true;
    sbPrintf(&ir->body, "L%d:\n", bad);
    ir->terminated = false;
    checkFailed(ir, at, msg);
    sbPrintf(&ir->body, "L%d:\n", ok);
    ir->terminated = false;
}

/* ---------------------------------------------------------------- expressions */

static Val emitExpr(IR *ir, Node *e);
static void emitStmt(IR *ir, Node *s);
static void emitBlock(IR *ir, Node *b);
static Val emitIf(IR *ir, Node *n, bool wantValue);
static Val emitMatch(IR *ir, Node *m, bool wantValue);

static Val val(const char *s, Type *t) { return (Val){s, t}; }

/* Every value of from fits in to, so no check is needed. */
static bool widensSafely(Type *from, Type *to) {
    if (from->isSigned == to->isSigned) {
        return from->bits <= to->bits;
    }
    return !from->isSigned && to->isSigned && from->bits < to->bits;
}

static Val emitConvert(IR *ir, Node *e) {
    Val v = emitExpr(ir, e->a);
    Type *from = v.t, *to = e->type;
    if (from->bits == to->bits && from->isSigned == to->isSigned) {
        return val(v.s, to);
    }
    const char *r = v.s;
    if (to->bits < from->bits) {
        r = tmp(ir);
        ins(ir, "%s = trunc %s %s to %s", r, llType(from), v.s, llType(to));
    } else if (to->bits > from->bits) {
        r = tmp(ir);
        ins(ir, "%s = %s %s %s to %s", r, from->isSigned ? "sext" : "zext", llType(from), v.s, llType(to));
    }
    if ((e->flags & F_EXPLICIT) && checks(ir) && !widensSafely(from, to)) {
        /* convert back and compare; also the sign must survive */
        const char *back = r;
        if (to->bits < from->bits) {
            back = tmp(ir);
            ins(ir, "%s = %s %s %s to %s", back, to->isSigned ? "sext" : "zext", llType(to), r, llType(from));
        }
        const char *same = tmp(ir);
        ins(ir, "%s = icmp ne %s %s, %s", same, llType(from), back, v.s);
        failIf(ir, same, e, "the value doesn't fit in the type it is converted to");
        if (from->isSigned != to->isSigned) {
            const char *neg = tmp(ir);
            if (from->isSigned) {
                ins(ir, "%s = icmp slt %s %s, 0", neg, llType(from), v.s);
            } else {
                ins(ir, "%s = icmp slt %s %s, 0", neg, llType(to), r);
            }
            failIf(ir, neg, e, "the value doesn't fit in the type it is converted to");
        }
    }
    return val(r, to);
}

static const char *overflowIntrinsic(TokKind op, Type *t) {
    const char *name = op == P_PLUS ? "add" : op == P_MINUS ? "sub" : "mul";
    return t->isSigned ? (op == P_PLUS ? "sadd" : op == P_MINUS ? "ssub" : "smul")
                       : (op == P_PLUS ? "uadd" : op == P_MINUS ? "usub" : "umul");
    (void)name;
}

/* + - * with an overflow check in debug builds */
static Val emitArith(IR *ir, Node *at, TokKind op, Val a, Val b) {
    Type *t = a.t;
    const char *ty = llType(t);
    const char *r = tmp(ir);
    if (checks(ir)) {
        const char *pair = tmp(ir);
        ins(ir, "%s = call { %s, i1 } @llvm.%s.with.overflow.%s(%s %s, %s %s)", pair, ty,
            overflowIntrinsic(op, t), ty, ty, a.s, ty, b.s);
        ins(ir, "%s = extractvalue { %s, i1 } %s, 0", r, ty, pair);
        const char *of = tmp(ir);
        ins(ir, "%s = extractvalue { %s, i1 } %s, 1", of, ty, pair);
        failIf(ir, of, at, "integer overflow");
        return val(r, t);
    }
    ins(ir, "%s = %s %s %s, %s", r, op == P_PLUS ? "add" : op == P_MINUS ? "sub" : "mul", ty, a.s, b.s);
    return val(r, t);
}

static Val emitDivide(IR *ir, Node *at, TokKind op, Val a, Val b) {
    Type *t = a.t;
    const char *ty = llType(t);
    if (ir->mode != MODE_RELEASE_FAST) {
        const char *zero = tmp(ir);
        ins(ir, "%s = icmp eq %s %s, 0", zero, ty, b.s);
        failIf(ir, zero, at, "division by zero");
        if (t->isSigned) {
            /* the smallest value divided by -1 doesn't fit */
            const char *m1 = tmp(ir), *mn = tmp(ir), *both = tmp(ir);
            ins(ir, "%s = icmp eq %s %s, -1", m1, ty, b.s);
            ins(ir, "%s = icmp eq %s %s, %s", mn, ty, a.s, intConst(ir, (int64_t)((uint64_t)1 << 63) >> (64 - t->bits), t));
            ins(ir, "%s = and i1 %s, %s", both, m1, mn);
            failIf(ir, both, at, "integer overflow");
        }
    }
    const char *r = tmp(ir);
    const char *inst = op == P_SLASH ? (t->isSigned ? "sdiv" : "udiv") : (t->isSigned ? "srem" : "urem");
    ins(ir, "%s = %s %s %s, %s", r, inst, ty, a.s, b.s);
    return val(r, t);
}

static Val emitShift(IR *ir, Node *at, TokKind op, Val a, Val b) {
    Type *t = a.t;
    const char *ty = llType(t);
    const char *amt = b.s;
    if (b.t->bits != t->bits) {
        const char *x = tmp(ir);
        ins(ir, "%s = %s %s %s to %s", x, b.t->bits > t->bits ? "trunc" : "zext", llType(b.t), b.s, ty);
        if (b.t->bits > t->bits && checks(ir)) {
            const char *big = tmp(ir);
            ins(ir, "%s = icmp uge %s %s, %d", big, llType(b.t), b.s, t->bits);
            failIf(ir, big, at, "shift amount too large");
        }
        amt = x;
    }
    if (checks(ir)) {
        const char *big = tmp(ir);
        ins(ir, "%s = icmp uge %s %s, %d", big, ty, amt, t->bits);
        failIf(ir, big, at, "shift amount too large");
    } else {
        const char *m = tmp(ir);
        ins(ir, "%s = and %s %s, %d", m, ty, amt, t->bits - 1);
        amt = m;
    }
    const char *r = tmp(ir);
    ins(ir, "%s = %s %s %s, %s", r, op == P_SHL ? "shl" : t->isSigned ? "ashr" : "lshr", ty, a.s, amt);
    return val(r, t);
}

static const char *strEq(IR *ir, Val a, Val b) {
    const char *pa = tmp(ir), *la = tmp(ir), *pb = tmp(ir), *lb = tmp(ir), *r = tmp(ir), *c = tmp(ir);
    ins(ir, "%s = extractvalue { ptr, i64 } %s, 0", pa, a.s);
    ins(ir, "%s = extractvalue { ptr, i64 } %s, 1", la, a.s);
    ins(ir, "%s = extractvalue { ptr, i64 } %s, 0", pb, b.s);
    ins(ir, "%s = extractvalue { ptr, i64 } %s, 1", lb, b.s);
    ins(ir, "%s = call i32 @iris_str_eq(ptr %s, i64 %s, ptr %s, i64 %s)", r, pa, la, pb, lb);
    ins(ir, "%s = icmp ne i32 %s, 0", c, r);
    return c;
}

static Val emitCompare(IR *ir, TokKind op, Val a, Val b) {
    if (a.t->kind == TY_STR) {
        const char *eq = strEq(ir, a, b);
        if (op == P_NE) {
            const char *r = tmp(ir);
            ins(ir, "%s = xor i1 %s, true", r, eq);
            return val(r, &tyBool);
        }
        return val(eq, &tyBool);
    }
    const char *cc;
    bool s = a.t->isSigned;
    switch (op) {
    case P_EQ: cc = "eq"; break;
    case P_NE: cc = "ne"; break;
    case P_LT: cc = s ? "slt" : "ult"; break;
    case P_LE: cc = s ? "sle" : "ule"; break;
    case P_GT: cc = s ? "sgt" : "ugt"; break;
    default: cc = s ? "sge" : "uge"; break;
    }
    const char *r = tmp(ir);
    ins(ir, "%s = icmp %s %s %s, %s", r, cc, llType(a.t), a.s, b.s);
    return val(r, &tyBool);
}

static Val emitLogical(IR *ir, Node *e) {
    const char *slot = allocaFor(ir, &tyBool, "logic");
    Val a = emitExpr(ir, e->a);
    ins(ir, "store i1 %s, ptr %s", a.s, slot);
    int rhs = newLabel(ir), end = newLabel(ir);
    if (e->op == P_ANDAND) {
        condBr(ir, a.s, rhs, end);
    } else {
        condBr(ir, a.s, end, rhs);
    }
    placeLabel(ir, rhs);
    Val b = emitExpr(ir, e->b);
    ins(ir, "store i1 %s, ptr %s", b.s, slot);
    placeLabel(ir, end);
    const char *r = tmp(ir);
    ins(ir, "%s = load i1, ptr %s", r, slot);
    return val(r, &tyBool);
}

static Val emitBinary(IR *ir, Node *e) {
    TokKind op = e->op;
    if (op == P_ANDAND || op == P_OROR) {
        return emitLogical(ir, e);
    }
    Val a = emitExpr(ir, e->a);
    Val b = emitExpr(ir, e->b);
    switch (op) {
    case P_PLUS: case P_MINUS: case P_STAR:
        return emitArith(ir, e, op, a, b);
    case P_SLASH: case P_PERCENT:
        return emitDivide(ir, e, op, a, b);
    case P_SHL: case P_SHR:
        return emitShift(ir, e, op, a, b);
    case P_AMP: case P_PIPE: case P_CARET: {
        const char *r = tmp(ir);
        ins(ir, "%s = %s %s %s, %s", r, op == P_AMP ? "and" : op == P_PIPE ? "or" : "xor", llType(a.t), a.s, b.s);
        return val(r, a.t);
    }
    default:
        return emitCompare(ir, op, a, b);
    }
}

/* Writes the pieces of print or panic text to a runtime stream. */
static void emitText(IR *ir, Node *arg, int stream) {
    Node *parts[1] = {arg};
    int n = 1;
    Node **list = parts;
    if (arg->kind == N_INTERP) {
        list = arg->list.items;
        n = arg->list.len;
    }
    for (int i = 0; i < n; i++) {
        Node *p = list[i];
        Val v = emitExpr(ir, p);
        if (v.t->kind == TY_STR) {
            const char *ptr = tmp(ir), *len = tmp(ir);
            ins(ir, "%s = extractvalue { ptr, i64 } %s, 0", ptr, v.s);
            ins(ir, "%s = extractvalue { ptr, i64 } %s, 1", len, v.s);
            ins(ir, "call void @iris_write_str(i32 %d, ptr %s, i64 %s)", stream, ptr, len);
        } else if (v.t->kind == TY_BOOL) {
            const char *x = tmp(ir);
            ins(ir, "%s = zext i1 %s to i32", x, v.s);
            ins(ir, "call void @iris_write_bool(i32 %d, i32 %s)", stream, x);
        } else {
            const char *x = v.s;
            if (v.t->bits < 64) {
                x = tmp(ir);
                ins(ir, "%s = %s %s %s to i64", x, v.t->isSigned ? "sext" : "zext", llType(v.t), v.s);
            }
            ins(ir, "call void @iris_write_%s(i32 %d, i64 %s)", v.t->isSigned ? "i64" : "u64", stream, x);
        }
    }
}

static Val emitCall(IR *ir, Node *e) {
    Node *callee = e->a;
    Node *d = callee->ref;
    if (!d) {
        if (strcmp(callee->name, "print") == 0) {
            emitText(ir, e->list.items[0], 1);
            ins(ir, "call void @iris_print_end()");
            return val("", &tyVoid);
        }
        /* panic */
        emitText(ir, e->list.items[0], 2);
        const char *fn = cString(ir, ir->func->name);
        ins(ir, "call void @iris_panic(ptr %s, i32 %d, i32 %d, ptr %s)", ir->fileStr, e->line, e->col, fn);
        ins(ir, "unreachable");
        ir->terminated = true;
        return val("", &tyNoreturn);
    }
    StrBuf args = {0};
    for (int i = 0; i < e->list.len; i++) {
        Val v = emitExpr(ir, e->list.items[i]);
        sbPrintf(&args, "%s%s %s", i ? ", " : "", llType(d->list.items[i]->type), v.s);
    }
    const char *a = args.buf ? args.buf : "";
    Val r;
    if (d->type->kind == TY_VOID) {
        ins(ir, "call void %s(%s)", d->ir, a);
        r = val("", &tyVoid);
    } else {
        const char *t = tmp(ir);
        ins(ir, "%s = call %s %s(%s)", t, llType(d->type), d->ir, a);
        r = val(t, d->type);
    }
    sbFree(&args);
    return r;
}

static Val emitExpr(IR *ir, Node *e) {
    if (e->isConst && e->type) {
        if (e->type->kind == TY_BOOL) {
            return val(e->cval ? "true" : "false", &tyBool);
        }
        if (e->type->kind == TY_INT) {
            return val(intConst(ir, e->cval, e->type), e->type);
        }
    }
    switch (e->kind) {
    case N_STRING: {
        const char *g = stringConst(ir, e->text, e->textLen);
        return val(fmt(ir, "{ ptr %s, i64 %zu }", g, e->textLen), &tyStr);
    }
    case N_IDENT: {
        Node *d = e->ref;
        if (d->kind == N_CONST) {
            /* a str constant */
            return emitExpr(ir, d->b);
        }
        const char *r = tmp(ir);
        ins(ir, "%s = load %s, ptr %s", r, llType(d->type), d->ir);
        return val(r, d->type);
    }
    case N_CONVERT:
        return emitConvert(ir, e);
    case N_UNARY: {
        Val a = emitExpr(ir, e->a);
        const char *r = tmp(ir);
        if (e->op == P_BANG) {
            ins(ir, "%s = xor i1 %s, true", r, a.s);
            return val(r, &tyBool);
        }
        if (e->op == P_TILDE) {
            ins(ir, "%s = xor %s %s, -1", r, llType(a.t), a.s);
            return val(r, a.t);
        }
        ir->tmp--;              /* r unused: - is 0 - x */
        return emitArith(ir, e, P_MINUS, val("0", a.t), a);
    }
    case N_BINARY:
        return emitBinary(ir, e);
    case N_CALL:
        return emitCall(ir, e);
    case N_FIELD_ACCESS: {
        Val a = emitExpr(ir, e->a);
        const char *r = tmp(ir);
        ins(ir, "%s = extractvalue { ptr, i64 } %s, 1", r, a.s);
        return val(r, &tyI64);
    }
    case N_IF:
        return emitIf(ir, e, true);
    case N_MATCH:
        return emitMatch(ir, e, true);
    default:
        return val("undef", e->type ? e->type : &tyVoid);
    }
}

/* ---------------------------------------------------------------- control flow */

/* Runs a value block and stores its value in slot (unless it leaves). */
static void emitValueBlock(IR *ir, Node *b, const char *slot, Type *t) {
    if (b->kind != N_BLOCK) {
        if (b->kind == N_EXPR_STMT) {
            Val v = emitExpr(ir, b->a);
            if (!ir->terminated && v.t->kind != TY_NORETURN) {
                ins(ir, "store %s %s, ptr %s", llType(t), v.s, slot);
            }
        } else {
            emitStmt(ir, b);
        }
        return;
    }
    for (int i = 0; i < b->list.len; i++) {
        Node *s = b->list.items[i];
        if (i == b->list.len - 1 && s->kind == N_EXPR_STMT) {
            Val v = emitExpr(ir, s->a);
            if (!ir->terminated && v.t->kind != TY_NORETURN) {
                ins(ir, "store %s %s, ptr %s", llType(t), v.s, slot);
            }
        } else {
            emitStmt(ir, s);
        }
    }
}

static Val emitIf(IR *ir, Node *n, bool wantValue) {
    Type *t = n->type;
    bool value = wantValue && t && (t->kind == TY_INT || t->kind == TY_BOOL || t->kind == TY_STR);
    const char *slot = value ? allocaFor(ir, t, "if") : NULL;
    Val c = emitExpr(ir, n->a);
    int yes = newLabel(ir), no = newLabel(ir), end = newLabel(ir);
    condBr(ir, c.s, yes, n->d ? no : end);
    placeLabel(ir, yes);
    if (value) {
        emitValueBlock(ir, n->c, slot, t);
    } else {
        emitBlock(ir, n->c);
    }
    br(ir, end);
    if (n->d) {
        placeLabel(ir, no);
        if (n->d->kind == N_IF) {
            Val v = emitIf(ir, n->d, value);
            if (value && !ir->terminated) {
                ins(ir, "store %s %s, ptr %s", llType(t), v.s, slot);
            }
        } else if (value) {
            emitValueBlock(ir, n->d, slot, t);
        } else {
            emitBlock(ir, n->d);
        }
        br(ir, end);
    }
    placeLabel(ir, end);
    if (!value) {
        return val("", &tyVoid);
    }
    const char *r = tmp(ir);
    ins(ir, "%s = load %s, ptr %s", r, llType(t), slot);
    return val(r, t);
}

static bool wildcard(Node *p) { return p->kind == N_IDENT && strcmp(p->name, "_") == 0; }

static Val emitMatch(IR *ir, Node *m, bool wantValue) {
    Type *t = m->type;
    bool value = wantValue && t && (t->kind == TY_INT || t->kind == TY_BOOL || t->kind == TY_STR);
    const char *slot = value ? allocaFor(ir, t, "match") : NULL;
    Val subject = emitExpr(ir, m->a);
    int end = newLabel(ir);
    for (int i = 0; i < m->list.len; i++) {
        Node *arm = m->list.items[i];
        int body = newLabel(ir), next = newLabel(ir);
        for (int j = 0; j < arm->list.len; j++) {
            Node *p = arm->list.items[j];
            int tryNext = j + 1 < arm->list.len ? newLabel(ir) : next;
            if (wildcard(p)) {
                br(ir, body);
            } else if (p->kind == N_BINARY && p->op == P_DOTDOT) {
                const char *lo = tmp(ir), *hi = tmp(ir), *both = tmp(ir);
                bool s = subject.t->isSigned;
                const char *ty = llType(subject.t);
                ins(ir, "%s = icmp %s %s %s, %s", lo, s ? "sge" : "uge", ty, subject.s, intConst(ir, p->a->cval, subject.t));
                ins(ir, "%s = icmp %s %s %s, %s", hi, s ? "sle" : "ule", ty, subject.s, intConst(ir, p->b->cval, subject.t));
                ins(ir, "%s = and i1 %s, %s", both, lo, hi);
                condBr(ir, both, body, tryNext);
            } else {
                Val pv = emitExpr(ir, p);
                Val eq = emitCompare(ir, P_EQ, subject, pv);
                condBr(ir, eq.s, body, tryNext);
            }
            if (tryNext != next) {
                placeLabel(ir, tryNext);
            }
        }
        placeLabel(ir, body);
        if (value) {
            emitValueBlock(ir, arm->c, slot, t);
        } else if (arm->c->kind == N_BLOCK) {
            emitBlock(ir, arm->c);
        } else {
            emitStmt(ir, arm->c);
        }
        br(ir, end);
        placeLabel(ir, next);
    }
    br(ir, end);
    placeLabel(ir, end);
    if (!value) {
        return val("", &tyVoid);
    }
    const char *r = tmp(ir);
    ins(ir, "%s = load %s, ptr %s", r, llType(t), slot);
    return val(r, t);
}

/* ---------------------------------------------------------------- statements */

static void emitBlock(IR *ir, Node *b) {
    for (int i = 0; i < b->list.len; i++) {
        emitStmt(ir, b->list.items[i]);
    }
}

static void emitFor(IR *ir, Node *s) {
    Loop loop = {ir->loop, ir->pendingLabel, 0, 0};
    ir->pendingLabel = NULL;
    int cond = newLabel(ir), body = newLabel(ir), post = newLabel(ir), end = newLabel(ir);
    loop.breakTo = end;
    loop.continueTo = post;

    if (s->flags & F_FOR_IN) {
        Node *range = s->a, *v = s->list.items[0];
        Type *t = range->type;
        v->ir = allocaFor(ir, t, v->name);
        const char *endSlot = allocaFor(ir, t, "end");
        Val lo = emitExpr(ir, range->a);
        ins(ir, "store %s %s, ptr %s", llType(t), lo.s, v->ir);
        Val hi = emitExpr(ir, range->b);
        ins(ir, "store %s %s, ptr %s", llType(t), hi.s, endSlot);
        placeLabel(ir, cond);
        const char *i = tmp(ir), *e = tmp(ir), *more = tmp(ir);
        ins(ir, "%s = load %s, ptr %s", i, llType(t), v->ir);
        ins(ir, "%s = load %s, ptr %s", e, llType(t), endSlot);
        ins(ir, "%s = icmp %s %s %s, %s", more, t->isSigned ? "slt" : "ult", llType(t), i, e);
        condBr(ir, more, body, end);
        placeLabel(ir, body);
        ir->loop = &loop;
        emitBlock(ir, s->c);
        ir->loop = loop.outer;
        placeLabel(ir, post);
        const char *cur = tmp(ir), *nx = tmp(ir);
        ins(ir, "%s = load %s, ptr %s", cur, llType(t), v->ir);
        ins(ir, "%s = add %s %s, 1", nx, llType(t), cur);
        ins(ir, "store %s %s, ptr %s", llType(t), nx, v->ir);
        br(ir, cond);
        placeLabel(ir, end);
        return;
    }

    placeLabel(ir, cond);
    if (s->a) {
        Val c = emitExpr(ir, s->a);
        condBr(ir, c.s, body, end);
    } else {
        br(ir, body);
    }
    placeLabel(ir, body);
    ir->loop = &loop;
    emitBlock(ir, s->c);
    ir->loop = loop.outer;
    placeLabel(ir, post);
    br(ir, cond);
    placeLabel(ir, end);
}

static Loop *findLoop(IR *ir, const char *label) {
    for (Loop *l = ir->loop; l; l = l->outer) {
        if (!label || (l->label && strcmp(l->label, label) == 0)) {
            return l;
        }
    }
    return ir->loop;
}

static void emitStmt(IR *ir, Node *s) {
    switch (s->kind) {
    case N_VAR: {
        s->ir = allocaFor(ir, s->type, s->name);
        if (s->b) {
            Val v = emitExpr(ir, s->b);
            if (!ir->terminated) {
                ins(ir, "store %s %s, ptr %s", llType(s->type), v.s, s->ir);
            }
        } else {
            ins(ir, "store %s zeroinitializer, ptr %s", llType(s->type), s->ir);
        }
        return;
    }
    case N_DEFINE: {
        Node *id = s->list.items[0];
        id->ir = allocaFor(ir, id->type, id->name);
        Val v = emitExpr(ir, s->b);
        if (!ir->terminated) {
            ins(ir, "store %s %s, ptr %s", llType(id->type), v.s, id->ir);
        }
        return;
    }
    case N_ASSIGN: {
        Node *target = s->list.items[0];
        if (target->kind == N_IDENT && strcmp(target->name, "_") == 0) {
            emitExpr(ir, s->b);
            return;
        }
        Node *d = target->ref;
        Type *t = d->type;
        if (s->op == P_ASSIGN) {
            Val v = emitExpr(ir, s->b);
            ins(ir, "store %s %s, ptr %s", llType(t), v.s, d->ir);
            return;
        }
        const char *cur = tmp(ir);
        ins(ir, "%s = load %s, ptr %s", cur, llType(t), d->ir);
        Val a = val(cur, t);
        Val b = emitExpr(ir, s->b);
        Val r;
        switch (s->op) {
        case P_PLUS_ASSIGN: r = emitArith(ir, s, P_PLUS, a, b); break;
        case P_MINUS_ASSIGN: r = emitArith(ir, s, P_MINUS, a, b); break;
        case P_STAR_ASSIGN: r = emitArith(ir, s, P_STAR, a, b); break;
        case P_SLASH_ASSIGN: r = emitDivide(ir, s, P_SLASH, a, b); break;
        case P_PERCENT_ASSIGN: r = emitDivide(ir, s, P_PERCENT, a, b); break;
        case P_SHL_ASSIGN: r = emitShift(ir, s, P_SHL, a, b); break;
        case P_SHR_ASSIGN: r = emitShift(ir, s, P_SHR, a, b); break;
        default: {
            const char *x = tmp(ir);
            ins(ir, "%s = %s %s %s, %s", x, s->op == P_AMP_ASSIGN ? "and" : s->op == P_PIPE_ASSIGN ? "or" : "xor",
                llType(t), cur, b.s);
            r = val(x, t);
        }
        }
        ins(ir, "store %s %s, ptr %s", llType(t), r.s, d->ir);
        return;
    }
    case N_EXPR_STMT:
        if (s->a->kind == N_IF) {
            emitIf(ir, s->a, false);
        } else if (s->a->kind == N_MATCH) {
            emitMatch(ir, s->a, false);
        } else {
            emitExpr(ir, s->a);
        }
        return;
    case N_IF:
        emitIf(ir, s, false);
        return;
    case N_MATCH:
        emitMatch(ir, s, false);
        return;
    case N_BLOCK:
        emitBlock(ir, s);
        return;
    case N_RETURN:
        if (s->a) {
            Val v = emitExpr(ir, s->a);
            if (!ir->terminated) {
                ins(ir, "ret %s %s", llType(ir->func->type), v.s);
            }
        } else {
            ins(ir, "ret void");
        }
        ir->terminated = true;
        return;
    case N_BREAK:
        br(ir, findLoop(ir, s->name)->breakTo);
        return;
    case N_CONTINUE:
        br(ir, findLoop(ir, s->name)->continueTo);
        return;
    case N_LABELED:
        ir->pendingLabel = s->name;
        emitStmt(ir, s->a);
        return;
    case N_FOR:
        emitFor(ir, s);
        return;
    default:
        return;
    }
}

/* ---------------------------------------------------------------- functions */

static void emitFunction(IR *ir, Node *f, StrBuf *out) {
    ir->func = f;
    ir->tmp = 0;
    ir->label = 0;
    ir->vars = 0;
    ir->terminated = false;
    ir->allocas = (StrBuf){0};
    ir->body = (StrBuf){0};

    sbPrintf(out, "define internal %s %s(", llType(f->type), f->ir);
    for (int i = 0; i < f->list.len; i++) {
        Node *p = f->list.items[i];
        sbPrintf(out, "%s%s %%p.%s", i ? ", " : "", llType(p->type), p->name);
        p->ir = allocaFor(ir, p->type, p->name);
        sbPrintf(&ir->allocas, "  store %s %%p.%s, ptr %s\n", llType(p->type), p->name, p->ir);
    }
    sbPuts(out, ") {\n");

    if (f->d) {
        Val v = emitExpr(ir, f->d);
        if (!ir->terminated) {
            if (f->type->kind == TY_VOID) {
                ins(ir, "ret void");
            } else {
                ins(ir, "ret %s %s", llType(f->type), v.s);
            }
        }
    } else {
        emitBlock(ir, f->c);
        if (!ir->terminated) {
            if (f->type->kind == TY_VOID) {
                ins(ir, "ret void");
            } else {
                ins(ir, "unreachable");
            }
        }
    }
    sbPuts(out, "entry:\n");
    if (ir->allocas.buf) {
        sbPuts(out, ir->allocas.buf);
    }
    sbPuts(out, "  br label %L0\nL0:\n");
    if (ir->body.buf) {
        sbPuts(out, ir->body.buf);
    }
    sbPuts(out, "}\n\n");
    sbFree(&ir->allocas);
    sbFree(&ir->body);
    ir->func = NULL;
}

static const char *globalInit(IR *ir, Node *value, Type *t) {
    if (!value) {
        return "zeroinitializer";
    }
    if (t->kind == TY_STR) {
        Node *v = value;
        if (v->kind == N_IDENT && v->ref && v->ref->kind == N_CONST) {
            v = v->ref->b;
        }
        const char *g = stringConst(ir, v->text, v->textLen);
        return fmt(ir, "{ ptr %s, i64 %zu }", g, v->textLen);
    }
    if (t->kind == TY_BOOL) {
        return value->cval ? "true" : "false";
    }
    return intConst(ir, value->cval, t);
}

char *writeIR(Arena *a, Source *src, Node *file, BuildMode mode) {
    IR ir = {0};
    ir.arena = a;
    ir.src = src;
    ir.mode = mode;
    ir.fileStr = cString(&ir, src->path);

    StrBuf funcs = {0};
    Node *mainFn = NULL;

    for (int i = 0; i < file->list.len; i++) {
        Node *d = file->list.items[i];
        if (d->kind == N_FUNC) {
            d->ir = fmt(&ir, "@f.%s", d->name);
            if (strcmp(d->name, "main") == 0) {
                mainFn = d;
            }
        } else if (d->kind == N_VAR) {
            d->ir = fmt(&ir, "@g.%s", d->name);
            sbPrintf(&ir.head, "%s = internal global %s %s\n", d->ir, llType(d->type), globalInit(&ir, d->b, d->type));
        } else if (d->kind == N_DEFINE) {
            Node *id = d->list.items[0];
            id->ir = fmt(&ir, "@g.%s", id->name);
            sbPrintf(&ir.head, "%s = internal global %s %s\n", id->ir, llType(id->type),
                     globalInit(&ir, d->b, id->type));
        }
    }
    for (int i = 0; i < file->list.len; i++) {
        Node *d = file->list.items[i];
        if (d->kind == N_FUNC) {
            emitFunction(&ir, d, &funcs);
        }
    }

    StrBuf out = {0};
    sbPrintf(&out, "; generated by iris %s from %s\n\n", IRIS_VERSION, src->path);
    if (ir.head.buf) {
        sbPuts(&out, ir.head.buf);
    }
    sbPuts(&out, "\n");
    if (funcs.buf) {
        sbPuts(&out, funcs.buf);
    }
    sbPuts(&out, "define i32 @main(i32 %argc, ptr %argv) {\n");
    sbPuts(&out, "  call void @iris_init(i32 %argc, ptr %argv)\n");
    if (mainFn && mainFn->type->kind == TY_INT) {
        sbPrintf(&out, "  %%r = call i32 %s()\n  ret i32 %%r\n}\n\n", mainFn->ir);
    } else {
        sbPrintf(&out, "  call void %s()\n  ret i32 0\n}\n\n", mainFn ? mainFn->ir : "@f.main");
    }
    sbPuts(&out,
           "declare void @iris_init(i32, ptr)\n"
           "declare void @iris_write_str(i32, ptr, i64)\n"
           "declare void @iris_write_i64(i32, i64)\n"
           "declare void @iris_write_u64(i32, i64)\n"
           "declare void @iris_write_bool(i32, i32)\n"
           "declare void @iris_print_end()\n"
           "declare i32 @iris_str_eq(ptr, i64, ptr, i64)\n"
           "declare void @iris_panic(ptr, i32, i32, ptr) noreturn\n"
           "declare void @iris_check_failed(ptr, ptr, i32, i32, ptr) noreturn\n"
           "\n!0 = !{!\"branch_weights\", i32 1, i32 1048575}\n");
    sbFree(&funcs);
    sbFree(&ir.head);
    return sbFinish(&out, a);
}
