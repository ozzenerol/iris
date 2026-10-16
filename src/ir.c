/* The IR writer: the checked tree to LLVM IR, as text.
 *
 * Every variable gets a stack slot (alloca) in the function's entry block;
 * clang's optimiser turns them into registers. Values in registers are
 * named %t1, %t2, ...; labels L1, L2, ...
 *
 * An expression is either read as a value (emitExpr) or, when it names a
 * place in memory, as an address (emitAddr). Assignments, & and method
 * receivers use addresses.
 *
 * In debug builds, arithmetic that overflows, division by zero, shifts by
 * too much, conversions that lose a value and null pointers call
 * iris_check_failed. Index and slice bounds are checked in debug and
 * release-safe builds. Debug builds also use AddressSanitizer, which
 * catches use after free. */
#define _XOPEN_SOURCE 700         /* realpath, for debug info paths */
#include "iris.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *s;          /* an operand: %t3, 42, true, { ptr @s1, i64 5 } */
    Type *t;
} Val;

/* The deferred statements of one block, run in reverse when it ends.
 * Items are N_DEFER and N_ERRDEFER statements, and scoped variables. */
typedef struct DeferFrame DeferFrame;
struct DeferFrame {
    DeferFrame *outer;
    NodeList items;
};

typedef struct Loop Loop;
struct Loop {
    Loop *outer;
    const char *label;
    int breakTo, continueTo;
    DeferFrame *frame;      /* the frame outside the loop: exits run the ones above it */
    int temps;              /* temporary Strings made before the loop: kept on break */
};

typedef struct {
    Arena *arena;
    Source *src;
    BuildMode mode;
    StrBuf head;            /* types, string constants and globals */
    StrBuf allocas;         /* the current function's entry block */
    StrBuf body;            /* the current function's code */
    int tmp, label, strings, vars;
    bool terminated;        /* the current block has ended */
    Node *func;
    Loop *loop;
    DeferFrame *defers;
    const char *pendingLabel;
    const char *fileStr;    /* the source path, as a C string constant */
    NodeList vtables;       /* the names of the method tables written so far */
    StrBuf extra;           /* functions made along the way: the thunks of spawn */
    int spawns;
    const char **temps;     /* the slots of temporary Strings of the statements being written */
    int ntemps, capTemps;
    StrBuf meta;            /* debug info: files, functions */
    int metaId;
    const char **dbgPaths;  /* the files that have a DIFile, and its id */
    int *dbgIds;
    int ndbg, capDbg;
    int dbgScope;           /* the current function's DISubprogram, 0 for none */
    int dbgLine, dbgCol;    /* where the code being written comes from */
    Type **dbgTypes;        /* the types that have a debug type, and its id */
    int *dbgTypeIds;
    int ndbgT, capDbgT;
    Type **jsonTypes;       /* the types whose JSON tables are written, and their names */
    const char **jsonNames;
    int njson, capJson;
} IR;

static bool checks(IR *ir) { return ir->mode == MODE_DEBUG; }
static bool boundsChecks(IR *ir) { return ir->mode != MODE_RELEASE_FAST; }

static const char *fmt(IR *ir, const char *f, ...) {
    va_list ap;
    va_start(ap, f);
    int n = vsnprintf(NULL, 0, f, ap);
    va_end(ap);
    char *buf = arenaAlloc(ir->arena, (size_t)n + 1);
    va_start(ap, f);
    vsnprintf(buf, (size_t)n + 1, f, ap);
    va_end(ap);
    return buf;
}

static const char *llType(IR *ir, Type *t) {
    switch (t->kind) {
    case TY_BOOL: return "i1";
    case TY_INT:
        switch (t->bits) {
        case 8: return "i8";
        case 16: return "i16";
        case 32: return "i32";
        default: return "i64";
        }
    case TY_PTR: case TY_NULL: case TY_FUNC: case TY_CHAN: case TY_THREAD: return "ptr";
    case TY_ATOMIC: return t->elem->kind == TY_BOOL ? "i8" : llType(ir, t->elem);
    case TY_MUTEX: return "[3 x i64]";
    case TY_FLOAT: return t->bits == 32 ? "float" : "double";
    case TY_DYN: return "{ ptr, ptr }";
    case TY_SLICE: return "{ ptr, i64 }";
    case TY_ARRAY: return fmt(ir, "[%lld x %s]", (long long)t->count, llType(ir, t->elem));
    case TY_STRUCT: case TY_STRING: return t->ir;
    case TY_ENUM: return t->hasData ? t->ir : "i32";
    case TY_TUPLE: {
        StrBuf b = {0};
        sbPuts(&b, "{ ");
        for (int i = 0; i < t->nelems; i++) {
            sbPrintf(&b, "%s%s", i ? ", " : "", llType(ir, t->elems[i]));
        }
        sbPuts(&b, " }");
        return sbFinish(&b, ir->arena);
    }
    case TY_ERRVAL: case TY_ERRCODE: return "i64";
    case TY_RESULT:
        return t->elem->kind == TY_VOID ? "i64" : fmt(ir, "{ %s, i64 }", llType(ir, t->elem));
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

static void putEscaped(StrBuf *b, const char *s, size_t n);

static void ins(IR *ir, const char *f, ...) {
    ensureBlock(ir);
    va_list ap;
    va_start(ap, f);
    int n = vsnprintf(NULL, 0, f, ap);
    va_end(ap);
    char *buf = malloc((size_t)n + 1);
    va_start(ap, f);
    vsnprintf(buf, (size_t)n + 1, f, ap);
    va_end(ap);
    if (ir->dbgScope) {
        sbPrintf(&ir->body, "  %s, !dbg !DILocation(line: %d, column: %d, scope: !%d)\n", buf, ir->dbgLine,
                 ir->dbgCol, ir->dbgScope);
    } else {
        sbPrintf(&ir->body, "  %s\n", buf);
    }
    free(buf);
}

/* The DIFile of a source path, written the first time it is needed. */
static int dbgFile(IR *ir, const char *path) {
    for (int i = 0; i < ir->ndbg; i++) {
        if (strcmp(ir->dbgPaths[i], path) == 0) return ir->dbgIds[i];
    }
    if (ir->ndbg == ir->capDbg) {
        int cap = ir->capDbg ? ir->capDbg * 2 : 8;
        const char **ps = arenaAlloc(ir->arena, sizeof(char *) * (size_t)cap);
        int *ids = arenaAlloc(ir->arena, sizeof(int) * (size_t)cap);
        if (ir->ndbg) {
            memcpy(ps, ir->dbgPaths, sizeof(char *) * (size_t)ir->ndbg);
            memcpy(ids, ir->dbgIds, sizeof(int) * (size_t)ir->ndbg);
        }
        ir->dbgPaths = ps;
        ir->dbgIds = ids;
        ir->capDbg = cap;
    }
    /* the standard library is inside the compiler: its paths stay as they are */
    char full[4096];
    const char *name = path, *dir = ".";
    if (strncmp(path, "std/", 4) != 0 && realpath(path, full)) {
        char *slash = strrchr(full, '/');
        *slash = 0;
        dir = full;
        name = slash + 1;
    }
    int id = ir->metaId++;
    sbPrintf(&ir->meta, "!%d = !DIFile(filename: \"", id);
    putEscaped(&ir->meta, name, strlen(name));
    sbPuts(&ir->meta, "\", directory: \"");
    putEscaped(&ir->meta, dir, strlen(dir));
    sbPuts(&ir->meta, "\")\n");
    ir->dbgPaths[ir->ndbg] = arenaStrndup(ir->arena, path, strlen(path));
    ir->dbgIds[ir->ndbg++] = id;
    return id;
}

/* Where the code written next comes from. */
static void dbgAt(IR *ir, Node *n) {
    if (n && n->line > 0) {
        ir->dbgLine = n->line;
        ir->dbgCol = n->col;
    }
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
    sbPrintf(&ir->allocas, "  %s = alloca %s\n", name, llType(ir, t));
    return name;
}

static int dbgType(IR *ir, Type *t);

/* A struct-like debug type: name, size and members (name, type, offset). */
static void dbgMembers(IR *ir, StrBuf *b, const char **names, Type **types, int64_t *offs, int n) {
    for (int i = 0; i < n; i++) {
        int mt = dbgType(ir, types[i]);
        int m = ir->metaId++;
        sbPrintf(&ir->meta, "!%d = !DIDerivedType(tag: DW_TAG_member, name: \"%s\", baseType: !%d, size: %lld, offset: %lld)\n",
                 m, names[i], mt, (long long)typeSizeOf(types[i]) * 8, (long long)offs[i] * 8);
        sbPrintf(b, "%s!%d", i ? ", " : "", m);
    }
}

/* The debug type of an Iris type, so debuggers can show values. */
static int dbgType(IR *ir, Type *t) {
    for (int i = 0; i < ir->ndbgT; i++) {
        if (ir->dbgTypes[i] == t) return ir->dbgTypeIds[i];
    }
    if (ir->ndbgT == ir->capDbgT) {
        int cap = ir->capDbgT ? ir->capDbgT * 2 : 32;
        Type **ts = arenaAlloc(ir->arena, sizeof(Type *) * (size_t)cap);
        int *ids = arenaAlloc(ir->arena, sizeof(int) * (size_t)cap);
        if (ir->ndbgT) {
            memcpy(ts, ir->dbgTypes, sizeof(Type *) * (size_t)ir->ndbgT);
            memcpy(ids, ir->dbgTypeIds, sizeof(int) * (size_t)ir->ndbgT);
        }
        ir->dbgTypes = ts;
        ir->dbgTypeIds = ids;
        ir->capDbgT = cap;
    }
    int id = ir->metaId++;
    ir->dbgTypes[ir->ndbgT] = t;
    ir->dbgTypeIds[ir->ndbgT++] = id;
    long long bits = (long long)typeSizeOf(t) * 8;
    const char *name = t->name ? t->name : "?";
    switch (t->kind) {
    case TY_INT:
        sbPrintf(&ir->meta, "!%d = !DIBasicType(name: \"%s\", size: %d, encoding: %s)\n", id, name, t->bits,
                 t->isSigned ? "DW_ATE_signed" : (t->bits == 8 ? "DW_ATE_unsigned_char" : "DW_ATE_unsigned"));
        break;
    case TY_FLOAT:
        sbPrintf(&ir->meta, "!%d = !DIBasicType(name: \"%s\", size: %d, encoding: DW_ATE_float)\n", id, name, t->bits);
        break;
    case TY_BOOL:
        sbPrintf(&ir->meta, "!%d = !DIBasicType(name: \"bool\", size: 8, encoding: DW_ATE_boolean)\n", id);
        break;
    case TY_PTR: {
        int e = t->elem->kind == TY_VOID ? 0 : dbgType(ir, t->elem);
        if (e) {
            sbPrintf(&ir->meta, "!%d = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !%d, size: 64)\n", id, e);
        } else {
            sbPrintf(&ir->meta, "!%d = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: null, size: 64)\n", id);
        }
        break;
    }
    case TY_SLICE: case TY_STRING: {
        /* { ptr, len } and String's { ptr, len, cap } */
        Type elemPtr = {.kind = TY_PTR, .elem = t->kind == TY_STRING ? &tyU8 : t->elem, .name = "ptr"};
        Type *ep = &elemPtr;
        Type *pt = NULL;
        for (int i = 0; i < ir->ndbgT; i++) {
            if (ir->dbgTypes[i]->kind == TY_PTR && ir->dbgTypes[i]->elem == ep->elem) pt = ir->dbgTypes[i];
        }
        if (!pt) {
            pt = arenaAlloc(ir->arena, sizeof(Type));
            *pt = elemPtr;
        }
        const char *names[3] = {"ptr", "len", "cap"};
        Type *types[3] = {pt, &tyI64, &tyI64};
        int64_t offs[3] = {0, 8, 16};
        StrBuf b = {0};
        dbgMembers(ir, &b, names, types, offs, t->kind == TY_STRING ? 3 : 2);
        sbPrintf(&ir->meta, "!%d = !DICompositeType(tag: DW_TAG_structure_type, name: \"%s\", size: %lld, elements: !{%s})\n",
                 id, name, bits, b.buf);
        sbFree(&b);
        break;
    }
    case TY_STRUCT: {
        NodeList *fs = &t->decl->list;
        int n = fs->len;
        const char **names = arenaAlloc(ir->arena, sizeof(char *) * (size_t)(n ? n : 1));
        Type **types = arenaAlloc(ir->arena, sizeof(Type *) * (size_t)(n ? n : 1));
        int64_t *offs = arenaAlloc(ir->arena, sizeof(int64_t) * (size_t)(n ? n : 1));
        for (int i = 0; i < n; i++) {
            names[i] = fs->items[i]->name;
            types[i] = fs->items[i]->type;
            offs[i] = fieldOffset(t, i);
        }
        StrBuf b = {0};
        dbgMembers(ir, &b, names, types, offs, n);
        sbPrintf(&ir->meta, "!%d = !DICompositeType(tag: DW_TAG_structure_type, name: \"%s\", size: %lld, elements: !{%s})\n",
                 id, name, bits, b.buf ? b.buf : "");
        sbFree(&b);
        break;
    }
    case TY_ARRAY: {
        int e = dbgType(ir, t->elem);
        sbPrintf(&ir->meta, "!%d = !DICompositeType(tag: DW_TAG_array_type, baseType: !%d, size: %lld, "
                            "elements: !{!DISubrange(count: %lld)})\n",
                 id, e, bits, (long long)t->count);
        break;
    }
    case TY_ENUM:
        if (!t->hasData) {
            StrBuf b = {0};
            for (int i = 0; i < t->decl->list.len; i++) {
                Node *m = t->decl->list.items[i];
                sbPrintf(&b, "%s!DIEnumerator(name: \"%s\", value: %lld)", i ? ", " : "", m->name, (long long)m->cval);
            }
            sbPrintf(&ir->meta, "!%d = !DICompositeType(tag: DW_TAG_enumeration_type, name: \"%s\", size: 32, "
                                "elements: !{%s})\n",
                     id, name, b.buf ? b.buf : "");
            sbFree(&b);
            break;
        }
        /* an enum with data shows as its bytes */
        sbPrintf(&ir->meta, "!%d = !DICompositeType(tag: DW_TAG_structure_type, name: \"%s\", size: %lld, elements: !{})\n",
                 id, name, bits);
        break;
    default:
        sbPrintf(&ir->meta, "!%d = !DICompositeType(tag: DW_TAG_structure_type, name: \"%s\", size: %lld, elements: !{})\n",
                 id, name, bits > 0 ? bits : 64);
        break;
    }
    return id;
}

/* A named variable's stack slot, with what a debugger needs to find it.
 * arg is the parameter's position from 1, or 0 for a local. */
static const char *varSlot(IR *ir, Type *t, Node *decl, int arg) {
    const char *slot = allocaFor(ir, t, decl->name);
    if (ir->dbgScope && decl->name && strcmp(decl->name, "_") != 0) {
        int file = dbgFile(ir, ir->func->src->path);
        int ty = dbgType(ir, t);
        int v = ir->metaId++;
        sbPrintf(&ir->meta, "!%d = !DILocalVariable(name: \"%s\", %sscope: !%d, file: !%d, line: %d, type: !%d)\n", v,
                 decl->name, arg ? fmt(ir, "arg: %d, ", arg) : "", ir->dbgScope, file, decl->line, ty);
        sbPrintf(&ir->allocas,
                 "  call void @llvm.dbg.declare(metadata ptr %s, metadata !%d, metadata !DIExpression()), "
                 "!dbg !DILocation(line: %d, column: %d, scope: !%d)\n",
                 slot, v, decl->line, decl->col, ir->dbgScope);
    }
    return slot;
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

/* A floating point constant: LLVM wants the bits of the double, in hex;
 * for f32 the value rounded to float first. */
static const char *floatConst(IR *ir, double v, Type *t) {
    if (t->bits == 32) {
        v = (double)(float)v;
    }
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    return fmt(ir, "0x%016llX", (unsigned long long)bits);
}

static const char *fnName(IR *ir) {
    if (ir->func && (ir->func->flags & F_TESTFN)) {
        return cString(ir, fmt(ir, "test \"%s\"", ir->func->text));
    }
    return cString(ir, ir->func ? ir->func->name : "?");
}

static void checkFailed(IR *ir, Node *at, const char *msg) {
    const char *m = cString(ir, msg);
    ins(ir, "call void @iris_check_failed(ptr %s, ptr %s, i32 %d, i32 %d, ptr %s)", m, ir->fileStr,
        at->line, at->col, fnName(ir));
    ins(ir, "unreachable");
    ir->terminated = true;
}

/* Branches to the failure code if cond is true. Returns the label of the
 * failure block, which the caller fills (or NULL-like 0 when it used
 * checkFailed itself). */
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

static Val val(const char *s, Type *t) { return (Val){s, t}; }

/* The size of a type in bytes, as an i64 value. */
static const char *sizeOf(IR *ir, Type *t) {
    const char *p = tmp(ir), *n = tmp(ir);
    ins(ir, "%s = getelementptr %s, ptr null, i64 1", p, llType(ir, t));
    ins(ir, "%s = ptrtoint ptr %s to i64", n, p);
    return n;
}

static void nullCheck(IR *ir, const char *p, Node *at) {
    if (!checks(ir)) {
        return;
    }
    const char *z = tmp(ir);
    ins(ir, "%s = icmp eq ptr %s, null", z, p);
    failIf(ir, z, at, "null pointer used");
}

static void indexCheck(IR *ir, const char *idx, const char *len, Node *at) {
    if (!boundsChecks(ir)) {
        return;
    }
    const char *bad = tmp(ir);
    ins(ir, "%s = icmp uge i64 %s, %s", bad, idx, len);
    int fail = newLabel(ir), ok = newLabel(ir);
    ins(ir, "br i1 %s, label %%L%d, label %%L%d, !prof !0", bad, fail, ok);
    ir->terminated = true;
    sbPrintf(&ir->body, "L%d:\n", fail);
    ir->terminated = false;
    ins(ir, "call void @iris_index_failed(i64 %s, i64 %s, ptr %s, i32 %d, i32 %d, ptr %s)", idx, len,
        ir->fileStr, at->line, at->col, fnName(ir));
    ins(ir, "unreachable");
    ir->terminated = true;
    sbPrintf(&ir->body, "L%d:\n", ok);
    ir->terminated = false;
}

/* ---------------------------------------------------------------- expressions */

static Val emitExpr(IR *ir, Node *e);
static const char *emitSpawn(IR *ir, Node *call, bool thread);
static const char *emitAddr(IR *ir, Node *e);
static void emitStmt(IR *ir, Node *s);
static void emitBlock(IR *ir, Node *b);
static Val emitIf(IR *ir, Node *n, bool wantValue);
static Val emitMatch(IR *ir, Node *m, bool wantValue);
static bool irAddressable(Node *e);
static Val load(IR *ir, const char *addr, Type *t);
static const char *jsonType(IR *ir, Type *t);
static const char *enumTag(IR *ir, Val v);

/* Every value of from fits in to, so no check is needed. */
static bool widensSafely(Type *from, Type *to) {
    if (from->isSigned == to->isSigned) {
        return from->bits <= to->bits;
    }
    return !from->isSigned && to->isSigned && from->bits < to->bits;
}

/* An integer of any width as an i64 index. */
static const char *toI64(IR *ir, Val v) {
    if (v.t->bits == 64) {
        return v.s;
    }
    const char *r = tmp(ir);
    ins(ir, "%s = %s %s %s to i64", r, v.t->isSigned ? "sext" : "zext", llType(ir, v.t), v.s);
    return r;
}

/* A value stored in a new stack slot, for things that need an address. */
static const char *spill(IR *ir, Val v) {
    const char *slot = allocaFor(ir, v.t, "tmp");
    ins(ir, "store %s %s, ptr %s", llType(ir, v.t), v.s, slot);
    return slot;
}

static Val makeSlice(IR *ir, const char *ptr, const char *len, Type *t) {
    const char *a = tmp(ir), *b = tmp(ir);
    ins(ir, "%s = insertvalue { ptr, i64 } undef, ptr %s, 0", a, ptr);
    ins(ir, "%s = insertvalue { ptr, i64 } %s, i64 %s, 1", b, a, len);
    return val(b, t);
}

static void sliceParts(IR *ir, Val s, const char **ptr, const char **len) {
    *ptr = tmp(ir);
    *len = tmp(ir);
    ins(ir, "%s = extractvalue { ptr, i64 } %s, 0", *ptr, s.s);
    ins(ir, "%s = extractvalue { ptr, i64 } %s, 1", *len, s.s);
}

/* The address of an array: from its variable, or a temporary copy. */
static const char *arrayAddr(IR *ir, Node *e) {
    if (irAddressable(e)) {
        return emitAddr(ir, e);
    }
    return spill(ir, emitExpr(ir, e));
}

/* The table of a struct's methods for an interface, in the interface's
 * order: what a dyn value points to. Written once per pair. */
static const char *vtable(IR *ir, Type *st, Node *iface) {
    char name[400];
    snprintf(name, sizeof name, "@\"vt.%s.%s.%s.%s\"", st->decl->module ? st->decl->module : "main", st->name,
             iface->module ? iface->module : "main", iface->name);
    for (int i = 0; i < ir->vtables.len; i++) {
        if (strcmp(ir->vtables.items[i]->name, name) == 0) {
            return ir->vtables.items[i]->name;
        }
    }
    Node *rec = newNode(ir->arena, N_IDENT, 0, 0);
    rec->name = arenaStrndup(ir->arena, name, strlen(name));
    listPush(ir->arena, &ir->vtables, rec);
    sbPrintf(&ir->head, "%s = private constant [%d x ptr] [", rec->name, iface->list.len);
    for (int i = 0; i < iface->list.len; i++) {
        Node *m = NULL;
        for (int j = 0; j < st->methods.len; j++) {
            if (strcmp(st->methods.items[j]->name, iface->list.items[i]->name) == 0) {
                m = st->methods.items[j];
            }
        }
        sbPrintf(&ir->head, "%sptr %s", i ? ", " : "", m && m->ir ? m->ir : "null");
    }
    sbPuts(&ir->head, "]\n");
    return rec->name;
}

static Val emitConvert(IR *ir, Node *e) {
    Type *to = e->type, *from = e->a->type;
    if (to->kind == TY_PTR && from->kind == TY_PTR) {
        Val p = emitExpr(ir, e->a);
        return val(p.s, to);        /* all pointers are ptr in LLVM */
    }
    if (to->kind == TY_DYN) {
        Val p = emitExpr(ir, e->a);
        const char *vt = vtable(ir, from->elem, to->decl);
        const char *a = tmp(ir), *b = tmp(ir);
        ins(ir, "%s = insertvalue { ptr, ptr } undef, ptr %s, 0", a, p.s);
        ins(ir, "%s = insertvalue { ptr, ptr } %s, ptr %s, 1", b, a, vt);
        return val(b, to);
    }
    if (from->kind == TY_ENUM) {
        /* an error code: the enum's number in the high half */
        Val v = emitExpr(ir, e->a);
        const char *w = tmp(ir), *r = tmp(ir);
        ins(ir, "%s = zext i32 %s to i64", w, v.s);
        ins(ir, "%s = or i64 %s, %lld", r, w, (long long)((uint64_t)from->count << 32));
        return val(r, to);
    }
    if (from->kind == TY_ARRAY) {
        const char *addr = arrayAddr(ir, e->a);
        return makeSlice(ir, addr, fmt(ir, "%lld", (long long)from->count), to);
    }
    if (from->kind == TY_STRING) {
        Val v = emitExpr(ir, e->a);
        const char *p = tmp(ir), *n = tmp(ir);
        ins(ir, "%s = extractvalue %%String %s, 0", p, v.s);
        ins(ir, "%s = extractvalue %%String %s, 1", n, v.s);
        return makeSlice(ir, p, n, to);
    }
    Val v = emitExpr(ir, e->a);
    from = v.t;
    if (from->kind == TY_FLOAT || to->kind == TY_FLOAT) {
        const char *r = tmp(ir);
        if (from->kind == TY_FLOAT && to->kind == TY_FLOAT) {
            if (from->bits == to->bits) return val(v.s, to);
            ins(ir, "%s = %s %s %s to %s", r, to->bits > from->bits ? "fpext" : "fptrunc", llType(ir, from), v.s,
                llType(ir, to));
            return val(r, to);
        }
        if (to->kind == TY_FLOAT) {
            ins(ir, "%s = %s %s %s to %s", r, from->isSigned ? "sitofp" : "uitofp", llType(ir, from), v.s,
                llType(ir, to));
            return val(r, to);
        }
        /* a float to an integer: in debug builds the value must fit (and not be NaN) */
        if (checks(ir)) {
            double lo, hi;          /* the integer must be in [lo, hi) */
            if (to->isSigned) {
                lo = -ldexp(1.0, to->bits - 1);
                hi = ldexp(1.0, to->bits - 1);
            } else {
                lo = 0;
                hi = ldexp(1.0, to->bits);
            }
            const char *ft = llType(ir, from);
            const char *a = tmp(ir), *b = tmp(ir), *ok = tmp(ir), *bad = tmp(ir);
            /* truncation goes toward zero, so lo - 1 < v < hi is the range */
            ins(ir, "%s = fcmp ogt %s %s, %s", a, ft, v.s, floatConst(ir, lo - 1, &tyF64));
            ins(ir, "%s = fcmp olt %s %s, %s", b, ft, v.s, floatConst(ir, hi, &tyF64));
            ins(ir, "%s = and i1 %s, %s", ok, a, b);
            ins(ir, "%s = xor i1 %s, true", bad, ok);
            failIf(ir, bad, e, "the value doesn't fit in the type it is converted to");
        }
        ins(ir, "%s = %s %s %s to %s", r, to->isSigned ? "fptosi" : "fptoui", llType(ir, from), v.s, llType(ir, to));
        return val(r, to);
    }
    if (from->bits == to->bits && from->isSigned == to->isSigned) {
        return val(v.s, to);
    }
    const char *r = v.s;
    if (to->bits < from->bits) {
        r = tmp(ir);
        ins(ir, "%s = trunc %s %s to %s", r, llType(ir, from), v.s, llType(ir, to));
    } else if (to->bits > from->bits) {
        r = tmp(ir);
        ins(ir, "%s = %s %s %s to %s", r, from->isSigned ? "sext" : "zext", llType(ir, from), v.s, llType(ir, to));
    }
    if ((e->flags & F_EXPLICIT) && checks(ir) && !widensSafely(from, to)) {
        /* convert back and compare; also the sign must survive */
        const char *back = r;
        if (to->bits < from->bits) {
            back = tmp(ir);
            ins(ir, "%s = %s %s %s to %s", back, to->isSigned ? "sext" : "zext", llType(ir, to), r, llType(ir, from));
        }
        const char *same = tmp(ir);
        ins(ir, "%s = icmp ne %s %s, %s", same, llType(ir, from), back, v.s);
        failIf(ir, same, e, "the value doesn't fit in the type it is converted to");
        if (from->isSigned != to->isSigned) {
            const char *neg = tmp(ir);
            if (from->isSigned) {
                ins(ir, "%s = icmp slt %s %s, 0", neg, llType(ir, from), v.s);
            } else {
                ins(ir, "%s = icmp slt %s %s, 0", neg, llType(ir, to), r);
            }
            failIf(ir, neg, e, "the value doesn't fit in the type it is converted to");
        }
    }
    return val(r, to);
}

static const char *overflowIntrinsic(TokKind op, Type *t) {
    if (t->isSigned) {
        return op == P_PLUS ? "sadd" : op == P_MINUS ? "ssub" : "smul";
    }
    return op == P_PLUS ? "uadd" : op == P_MINUS ? "usub" : "umul";
}

/* + - * with an overflow check in debug builds */
static Val emitArith(IR *ir, Node *at, TokKind op, Val a, Val b) {
    Type *t = a.t;
    if (t->kind == TY_PTR) {
        /* pointer arithmetic: move by whole items */
        const char *n = toI64(ir, b);
        if (op == P_MINUS) {
            const char *neg = tmp(ir);
            ins(ir, "%s = sub i64 0, %s", neg, n);
            n = neg;
        }
        const char *r = tmp(ir);
        ins(ir, "%s = getelementptr %s, ptr %s, i64 %s", r, llType(ir, t->elem), a.s, n);
        return val(r, t);
    }
    const char *ty = llType(ir, t);
    const char *r = tmp(ir);
    if (t->kind == TY_FLOAT) {
        ins(ir, "%s = %s %s %s, %s", r, op == P_PLUS ? "fadd" : op == P_MINUS ? "fsub" : "fmul", ty, a.s, b.s);
        return val(r, t);
    }
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
    const char *ty = llType(ir, t);
    if (t->kind == TY_FLOAT) {
        const char *r = tmp(ir);
        ins(ir, "%s = %s %s %s, %s", r, op == P_SLASH ? "fdiv" : "frem", ty, a.s, b.s);
        return val(r, t);
    }
    if (ir->mode != MODE_RELEASE_FAST) {
        const char *zero = tmp(ir);
        ins(ir, "%s = icmp eq %s %s, 0", zero, ty, b.s);
        failIf(ir, zero, at, "division by zero");
        if (t->isSigned) {
            /* the smallest value divided by -1 doesn't fit */
            const char *m1 = tmp(ir), *mn = tmp(ir), *both = tmp(ir);
            ins(ir, "%s = icmp eq %s %s, -1", m1, ty, b.s);
            ins(ir, "%s = icmp eq %s %s, %s", mn, ty, a.s,
                intConst(ir, (int64_t)((uint64_t)1 << 63) >> (64 - t->bits), t));
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
    const char *ty = llType(ir, t);
    const char *amt = b.s;
    if (b.t->bits != t->bits) {
        if (b.t->bits > t->bits && checks(ir)) {
            const char *big = tmp(ir);
            ins(ir, "%s = icmp uge %s %s, %d", big, llType(ir, b.t), b.s, t->bits);
            failIf(ir, big, at, "shift amount too large");
        }
        const char *x = tmp(ir);
        ins(ir, "%s = %s %s %s to %s", x, b.t->bits > t->bits ? "trunc" : "zext", llType(ir, b.t), b.s, ty);
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
    const char *pa, *la, *pb, *lb;
    sliceParts(ir, a, &pa, &la);
    sliceParts(ir, b, &pb, &lb);
    const char *r = tmp(ir), *c = tmp(ir);
    ins(ir, "%s = call i32 @iris_str_eq(ptr %s, i64 %s, ptr %s, i64 %s)", r, pa, la, pb, lb);
    ins(ir, "%s = icmp ne i32 %s, 0", c, r);
    return c;
}

/* a == b for any comparable type, as an i1. */
static const char *emitEq(IR *ir, Val a, Val b) {
    Type *t = a.t;
    if (t->kind == TY_SLICE) {
        return strEq(ir, a, b);
    }
    if (t->kind == TY_STRUCT) {
        const char *acc = "true";
        NodeList *fields = &t->decl->list;
        for (int i = 0; i < fields->len; i++) {
            Type *ft = fields->items[i]->type;
            const char *fa = tmp(ir), *fb = tmp(ir);
            ins(ir, "%s = extractvalue %s %s, %d", fa, t->ir, a.s, i);
            ins(ir, "%s = extractvalue %s %s, %d", fb, t->ir, b.s, i);
            const char *eq = emitEq(ir, val(fa, ft), val(fb, ft));
            const char *both = tmp(ir);
            ins(ir, "%s = and i1 %s, %s", both, acc, eq);
            acc = both;
        }
        return acc;
    }
    const char *r = tmp(ir);
    if (t->kind == TY_FLOAT) {
        ins(ir, "%s = fcmp oeq %s %s, %s", r, llType(ir, t), a.s, b.s);
        return r;
    }
    ins(ir, "%s = icmp eq %s %s, %s", r, llType(ir, t), a.s, b.s);
    return r;
}

static Val emitCompare(IR *ir, TokKind op, Val a, Val b) {
    if (a.t->kind == TY_FLOAT) {
        const char *cc = op == P_EQ ? "oeq" : op == P_NE ? "une" : op == P_LT ? "olt" : op == P_LE ? "ole"
                       : op == P_GT ? "ogt" : "oge";
        const char *r = tmp(ir);
        ins(ir, "%s = fcmp %s %s %s, %s", r, cc, llType(ir, a.t), a.s, b.s);
        return val(r, &tyBool);
    }
    if (op == P_EQ || op == P_NE) {
        if (a.t->kind == TY_NULL) {
            a.t = b.t;
        }
        const char *eq = emitEq(ir, a, b);
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
    case P_LT: cc = s ? "slt" : "ult"; break;
    case P_LE: cc = s ? "sle" : "ule"; break;
    case P_GT: cc = s ? "sgt" : "ugt"; break;
    default: cc = s ? "sge" : "uge"; break;
    }
    const char *r = tmp(ir);
    ins(ir, "%s = icmp %s %s %s, %s", r, cc, llType(ir, a.t), a.s, b.s);
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
    if (op == P_LARROW) {
        /* ch <- v: the value goes through memory */
        Val ch = emitExpr(ir, e->a);
        Val v = emitExpr(ir, e->b);
        const char *slot = spill(ir, v);
        ins(ir, "call void @iris_chan_send(ptr %s, ptr %s)", ch.s, slot);
        return val("", &tyVoid);
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
        ins(ir, "%s = %s %s %s, %s", r, op == P_AMP ? "and" : op == P_PIPE ? "or" : "xor", llType(ir, a.t), a.s, b.s);
        return val(r, a.t);
    }
    default:
        return emitCompare(ir, op, a, b);
    }
}

/* Writes the pieces of text to a runtime sink: 1 stdout, 2 the panic
 * message, 3 the String at dst. */
static void emitText(IR *ir, Node *arg, int sink, const char *dst) {
    Node *single[1] = {arg};
    int n = 1;
    Node **list = single;
    if (arg->kind == N_INTERP) {
        list = arg->list.items;
        n = arg->list.len;
    }
    for (int i = 0; i < n; i++) {
        Val v = emitExpr(ir, list[i]);
        if (v.t->kind == TY_SLICE) {
            const char *ptr, *len;
            sliceParts(ir, v, &ptr, &len);
            ins(ir, "call void @iris_write_str(i32 %d, ptr %s, ptr %s, i64 %s)", sink, dst, ptr, len);
        } else if (v.t->kind == TY_BOOL) {
            const char *x = tmp(ir);
            ins(ir, "%s = zext i1 %s to i32", x, v.s);
            ins(ir, "call void @iris_write_bool(i32 %d, ptr %s, i32 %s)", sink, dst, x);
        } else if (v.t->kind == TY_ENUM && v.t->isFlags) {
            ins(ir, "call void @iris_write_flags(i32 %d, ptr %s, i64 %lld, i32 %s)", sink, dst, (long long)v.t->count, v.s);
        } else if (v.t->kind == TY_ENUM) {
            ins(ir, "call void @iris_write_enum(i32 %d, ptr %s, i64 %lld, i32 %s)", sink, dst, (long long)v.t->count,
                enumTag(ir, v));
        } else if (v.t->kind == TY_ERRCODE) {
            ins(ir, "call void @iris_write_errcode(i32 %d, ptr %s, i64 %s)", sink, dst, v.s);
        } else if (v.t->kind == TY_FLOAT) {
            const char *x = v.s;
            if (v.t->bits == 32) {
                x = tmp(ir);
                ins(ir, "%s = fpext float %s to double", x, v.s);
            }
            ins(ir, "call void @iris_write_float(i32 %d, ptr %s, double %s, i32 %d)", sink, dst, x, v.t->bits);
        } else if (v.t == &tyDuration) {
            ins(ir, "call void @iris_write_duration(i32 %d, ptr %s, i64 %s)", sink, dst, v.s);
        } else {
            ins(ir, "call void @iris_write_%s(i32 %d, ptr %s, i64 %s)", v.t->isSigned ? "i64" : "u64", sink, dst,
                toI64(ir, v));
        }
    }
}

/* The data of an enum value: its member's fields, as a struct. */
static Type *memberData(Node *m) { return m->d ? m->d->type : NULL; }

/* A value of a data enum: the tag, then the member's fields in the room
 * after it. */
static Val emitEnumValue(IR *ir, Type *et, Node *m, NodeList *args) {
    const char *slot = allocaFor(ir, et, "enum");
    ins(ir, "store %s zeroinitializer, ptr %s", et->ir, slot);
    const char *tag = tmp(ir);
    ins(ir, "%s = getelementptr %s, ptr %s, i32 0, i32 0", tag, et->ir, slot);
    ins(ir, "store i32 %lld, ptr %s", (long long)m->cval, tag);
    Type *data = memberData(m);
    if (data && args) {
        const char *room = tmp(ir);
        ins(ir, "%s = getelementptr %s, ptr %s, i32 0, i32 1", room, et->ir, slot);
        for (int i = 0; i < args->len; i++) {
            Val v = emitExpr(ir, args->items[i]);
            const char *f = tmp(ir);
            ins(ir, "%s = getelementptr %s, ptr %s, i32 0, i32 %d", f, data->ir, room, i);
            ins(ir, "store %s %s, ptr %s", llType(ir, data->elems[i]), v.s, f);
        }
    }
    return load(ir, slot, et);
}

/* The tag of an enum value: the value itself for a plain enum. */
static const char *enumTag(IR *ir, Val v) {
    if (!v.t->hasData) {
        return v.s;
    }
    const char *t = tmp(ir);
    ins(ir, "%s = extractvalue %s %s, 0", t, v.t->ir, v.s);
    return t;
}

/* A temporary String, freed when the statement ends. */
static void addTemp(IR *ir, const char *slot) {
    if (ir->ntemps == ir->capTemps) {
        int cap = ir->capTemps ? ir->capTemps * 2 : 16;
        const char **t = arenaAlloc(ir->arena, sizeof(char *) * (size_t)cap);
        if (ir->ntemps) memcpy(t, ir->temps, sizeof(char *) * (size_t)ir->ntemps);
        ir->temps = t;
        ir->capTemps = cap;
    }
    ir->temps[ir->ntemps++] = slot;
}

/* ---------------------------------------------------------------- JSON
 *
 * Each type that is turned into JSON or read from it gets a table the
 * runtime works from (rt.c, JsonType):
 *   { i32 kind, i32 size, i64 count, ptr items }
 * A struct's items are its fields, { ptr name, i64 nameLen, i64 offset,
 * ptr type, i64 required }, with embedded structs' fields in line. A slice
 * or array points at its element's table; an enum at its values' names,
 * { ptr name, i64 len, i64 value }. */

enum { J_INT = 1, J_UINT, J_FLOAT, J_BOOL, J_STR, J_STRING, J_STRUCT, J_SLICE, J_ARRAY, J_ENUM };

static void jsonFields(IR *ir, Type *t, int64_t base, StrBuf *out, int *n) {
    NodeList *fields = &t->decl->list;
    for (int i = 0; i < fields->len; i++) {
        Node *f = fields->items[i];
        int64_t off = base + fieldOffset(t, i);
        if (f->kind == N_EMBED) {
            jsonFields(ir, f->type, off, out, n);
            continue;
        }
        const char *key = f->text ? f->text : f->name;
        size_t keyLen = f->text ? f->textLen : strlen(f->name);
        const char *name = stringConst(ir, key, keyLen);
        const char *ft = jsonType(ir, f->type);
        sbPrintf(out, "%s{ ptr, i64, i64, ptr, i64 } { ptr %s, i64 %zu, i64 %lld, ptr %s, i64 %d }", *n ? ", " : "",
                 name, keyLen, (long long)off, ft, (f->flags & F_REQUIRED) ? 1 : 0);
        (*n)++;
    }
}

static const char *jsonType(IR *ir, Type *t) {
    for (int i = 0; i < ir->njson; i++) {
        if (ir->jsonTypes[i] == t) return ir->jsonNames[i];
    }
    if (ir->njson == ir->capJson) {
        int cap = ir->capJson ? ir->capJson * 2 : 16;
        Type **ts = arenaAlloc(ir->arena, sizeof(Type *) * (size_t)cap);
        const char **ns = arenaAlloc(ir->arena, sizeof(char *) * (size_t)cap);
        if (ir->njson) {
            memcpy(ts, ir->jsonTypes, sizeof(Type *) * (size_t)ir->njson);
            memcpy(ns, ir->jsonNames, sizeof(char *) * (size_t)ir->njson);
        }
        ir->jsonTypes = ts;
        ir->jsonNames = ns;
        ir->capJson = cap;
    }
    int id = ir->njson;
    const char *name = fmt(ir, "@json.%d", id);
    ir->jsonTypes[id] = t;
    ir->jsonNames[id] = name;
    ir->njson++;
    int kind = 0;
    int64_t size = typeSizeOf(t), count = 0;
    const char *items = "null";
    switch (t->kind) {
    case TY_INT: kind = t->isSigned ? J_INT : J_UINT; break;
    case TY_FLOAT: kind = J_FLOAT; break;
    case TY_BOOL: kind = J_BOOL; break;
    case TY_STRING: kind = J_STRING; break;
    case TY_SLICE:
        if (t->elem->kind == TY_INT && t->elem->bits == 8 && !t->elem->isSigned) {
            kind = J_STR;
        } else {
            kind = J_SLICE;
            items = jsonType(ir, t->elem);
        }
        break;
    case TY_ARRAY:
        kind = J_ARRAY;
        count = t->count;
        items = jsonType(ir, t->elem);
        break;
    case TY_ENUM: {
        kind = J_ENUM;
        NodeList *ms = &t->decl->list;
        StrBuf b = {0};
        for (int i = 0; i < ms->len; i++) {
            Node *m = ms->items[i];
            const char *s = stringConst(ir, m->name, strlen(m->name));
            sbPrintf(&b, "%s{ ptr, i64, i64 } { ptr %s, i64 %zu, i64 %lld }", i ? ", " : "", s, strlen(m->name),
                     (long long)m->cval);
        }
        count = ms->len;
        items = fmt(ir, "@json.%d.items", id);
        sbPrintf(&ir->head, "%s = private constant [%lld x { ptr, i64, i64 }] [%s]\n", items, (long long)count,
                 b.buf ? b.buf : "");
        sbFree(&b);
        break;
    }
    case TY_STRUCT: {
        kind = J_STRUCT;
        StrBuf b = {0};
        int n = 0;
        jsonFields(ir, t, 0, &b, &n);
        count = n;
        items = fmt(ir, "@json.%d.items", id);
        sbPrintf(&ir->head, "%s = private constant [%d x { ptr, i64, i64, ptr, i64 }] [%s]\n", items, n,
                 b.buf ? b.buf : "");
        sbFree(&b);
        break;
    }
    default:
        break;
    }
    sbPrintf(&ir->head, "%s = private constant { i32, i32, i64, ptr } { i32 %d, i32 %lld, i64 %lld, ptr %s }\n", name,
             kind, (long long)size, (long long)count, items);
    return name;
}

static Val emitJson(IR *ir, Node *e) {
    switch (e->builtin) {
    case BI_JSON_ENCODE: {
        Node *arg = e->list.items[0];
        Val v = emitExpr(ir, arg);
        const char *vs = allocaFor(ir, arg->type, "value");
        ins(ir, "store %s %s, ptr %s", llType(ir, arg->type), v.s, vs);
        const char *out = allocaFor(ir, &tyString, "json");
        ins(ir, "store %%String zeroinitializer, ptr %s", out);
        ins(ir, "call void @iris_json_encode(ptr %s, ptr %s, ptr %s)", jsonType(ir, arg->type), vs, out);
        return load(ir, out, &tyString);
    }
    case BI_JSON_FREE: {
        Node *recv = e->a->a;
        Type *t = recv->type->kind == TY_PTR ? recv->type->elem : recv->type;
        const char *addr = recv->type->kind == TY_PTR ? emitExpr(ir, recv).s : emitAddr(ir, recv);
        ins(ir, "call void @iris_json_free(ptr %s, ptr %s)", jsonType(ir, t), addr);
        return val("", &tyVoid);
    }
    default: {
        /* decode: the defaults, then decodeInto fills in what the text has */
        Type *t = e->c->type;
        const char *slot = allocaFor(ir, t, "decoded");
        if (e->b) {
            Val init = emitExpr(ir, e->b);
            ins(ir, "store %s %s, ptr %s", llType(ir, t), init.s, slot);
        } else {
            ins(ir, "store %s zeroinitializer, ptr %s", llType(ir, t), slot);
        }
        Val text = emitExpr(ir, e->list.items[1]);
        const char *err = tmp(ir);
        ins(ir, "%s = call i64 %s(ptr %s, { ptr, i64 } %s, ptr %s)", err, e->ref->ir, jsonType(ir, t), text.s, slot);
        Val v = load(ir, slot, t);
        const char *rt = llType(ir, e->type);
        const char *r1 = tmp(ir), *r2 = tmp(ir);
        ins(ir, "%s = insertvalue %s undef, %s %s, 0", r1, rt, llType(ir, t), v.s);
        ins(ir, "%s = insertvalue %s %s, i64 %s, 1", r2, rt, r1, err);
        return val(r2, e->type);
    }
    }
}

/* Methods of Atomic[T], Mutex and Thread. Atomics are LLVM's, all
 * sequentially consistent; an Atomic[bool] is kept in a byte. */
static Val emitSync(IR *ir, Node *e) {
    Node *recv = e->a->a;
    Type *rt = recv->type;
    Type *base = rt->kind == TY_PTR ? rt->elem : rt;
    if (e->builtin == BI_THREAD_JOIN) {
        Val h = emitExpr(ir, recv);
        if (rt->kind == TY_PTR) {
            const char *l = tmp(ir);
            ins(ir, "%s = load ptr, ptr %s", l, h.s);
            h.s = l;
        }
        ins(ir, "call void @iris_thread_join(ptr %s)", h.s);
        return val("", &tyVoid);
    }
    const char *addr = rt->kind == TY_PTR ? emitExpr(ir, recv).s : emitAddr(ir, recv);
    switch (e->builtin) {
    case BI_MUTEX_LOCK:
        ins(ir, "call void @iris_mutex_lock(ptr %s)", addr);
        return val("", &tyVoid);
    case BI_MUTEX_UNLOCK:
        ins(ir, "call void @iris_mutex_unlock(ptr %s, ptr %s, i32 %d, i32 %d, ptr %s)", addr, ir->fileStr, e->line,
            e->col, fnName(ir));
        return val("", &tyVoid);
    case BI_MUTEX_TRY_LOCK: {
        const char *r = tmp(ir);
        ins(ir, "%s = call i1 @iris_mutex_try_lock(ptr %s)", r, addr);
        return val(r, &tyBool);
    }
    default:
        break;
    }
    Type *et = base->elem;
    bool isBool = et->kind == TY_BOOL;
    const char *lt = llType(ir, base);
    int64_t align = isBool ? 1 : et->bits / 8;
    const char *args[2];
    for (int i = 0; i < e->list.len && i < 2; i++) {
        Val v = emitExpr(ir, e->list.items[i]);
        if (isBool) {
            const char *z = tmp(ir);
            ins(ir, "%s = zext i1 %s to i8", z, v.s);
            args[i] = z;
        } else {
            args[i] = v.s;
        }
    }
    const char *r = tmp(ir);
    switch (e->builtin) {
    case BI_ATOMIC_LOAD:
        ins(ir, "%s = load atomic %s, ptr %s seq_cst, align %lld", r, lt, addr, (long long)align);
        break;
    case BI_ATOMIC_STORE:
        ins(ir, "store atomic %s %s, ptr %s seq_cst, align %lld", lt, args[0], addr, (long long)align);
        return val("", &tyVoid);
    case BI_ATOMIC_ADD: case BI_ATOMIC_SUB: {
        /* gives the new value */
        const char *old = tmp(ir);
        const char *op = e->builtin == BI_ATOMIC_ADD ? "add" : "sub";
        ins(ir, "%s = atomicrmw %s ptr %s, %s %s seq_cst, align %lld", old, op, addr, lt, args[0], (long long)align);
        ins(ir, "%s = %s %s %s, %s", r, op, lt, old, args[0]);
        return val(r, et);
    }
    case BI_ATOMIC_SWAP:
        ins(ir, "%s = atomicrmw xchg ptr %s, %s %s seq_cst, align %lld", r, addr, lt, args[0], (long long)align);
        break;
    case BI_ATOMIC_CAS: {
        const char *pair = tmp(ir);
        ins(ir, "%s = cmpxchg ptr %s, %s %s, %s %s seq_cst seq_cst, align %lld", pair, addr, lt, args[0], lt, args[1],
            (long long)align);
        ins(ir, "%s = extractvalue { %s, i1 } %s, 1", r, lt, pair);
        return val(r, &tyBool);
    }
    default:
        break;
    }
    if (isBool) {
        const char *b = tmp(ir);
        ins(ir, "%s = icmp ne i8 %s, 0", b, r);
        return val(b, &tyBool);
    }
    return val(r, et);
}

static Val emitBuiltin(IR *ir, Node *e) {
    switch (e->builtin) {
    case BI_PRINT:
        emitText(ir, e->list.items[0], 1, "null");
        ins(ir, "call void @iris_print_end()");
        return val("", &tyVoid);
    case BI_PANIC:
        emitText(ir, e->list.items[0], 2, "null");
        ins(ir, "call void @iris_panic(ptr %s, i32 %d, i32 %d, ptr %s)", ir->fileStr, e->line, e->col, fnName(ir));
        ins(ir, "unreachable");
        ir->terminated = true;
        return val("", &tyNoreturn);
    case BI_FORMAT: {
        const char *slot = allocaFor(ir, &tyString, "fmt");
        ins(ir, "store %%String zeroinitializer, ptr %s", slot);
        emitText(ir, e->list.items[0], 3, slot);
        const char *r = tmp(ir);
        ins(ir, "%s = load %%String, ptr %s", r, slot);
        return val(r, &tyString);
    }
    case BI_ALLOC: {
        Type *t = e->list.items[0]->type;
        const char *size = sizeOf(ir, t);
        Val n = emitExpr(ir, e->list.items[1]);
        const char *p = tmp(ir);
        ins(ir, "%s = call ptr @iris_alloc(i64 %s, i64 %s, ptr %s, i32 %d, i32 %d, ptr %s)", p, size, n.s,
            ir->fileStr, e->line, e->col, fnName(ir));
        return makeSlice(ir, p, n.s, e->type);
    }
    case BI_FREE: {
        Val v = emitExpr(ir, e->list.items[0]);
        const char *p = v.s;
        if (v.t->kind == TY_SLICE) {
            p = tmp(ir);
            ins(ir, "%s = extractvalue { ptr, i64 } %s, 0", p, v.s);
        }
        ins(ir, "call void @free(ptr %s)", p);
        return val("", &tyVoid);
    }
    case BI_ENUM_NEW:
        return emitEnumValue(ir, e->type, e->ref, &e->list);
    case BI_SLEEP: {
        Val d = emitExpr(ir, e->list.items[0]);
        ins(ir, "call void @iris_sleep(i64 %s)", d.s);
        return val("", &tyVoid);
    }
    case BI_INDIRECT: {
        Type *ft = e->a->type;
        Val fp = emitExpr(ir, e->a);
        nullCheck(ir, fp.s, e);
        StrBuf args = {0}, fty = {0};
        sbPrintf(&fty, "%s (", llType(ir, ft->elem));
        for (int i = 0; i < e->list.len; i++) {
            Val v = emitExpr(ir, e->list.items[i]);
            sbPrintf(&args, "%s%s %s", i ? ", " : "", llType(ir, ft->elems[i]), v.s);
            sbPrintf(&fty, "%s%s", i ? ", " : "", llType(ir, ft->elems[i]));
        }
        sbPutc(&fty, ')');
        Val r;
        if (ft->elem->kind == TY_VOID) {
            ins(ir, "call %s %s(%s)", fty.buf, fp.s, args.buf ? args.buf : "");
            r = val("", &tyVoid);
        } else {
            const char *t = tmp(ir);
            ins(ir, "%s = call %s %s(%s)", t, fty.buf, fp.s, args.buf ? args.buf : "");
            r = val(t, ft->elem);
        }
        sbFree(&args);
        sbFree(&fty);
        return r;
    }
    case BI_DYN_CALL: {
        /* the function comes from the table, self is the data pointer */
        Node *m = e->ref;
        Val w = emitExpr(ir, e->list.items[0]);
        const char *data = tmp(ir), *tbl = tmp(ir), *slot = tmp(ir), *fp = tmp(ir);
        ins(ir, "%s = extractvalue { ptr, ptr } %s, 0", data, w.s);
        ins(ir, "%s = extractvalue { ptr, ptr } %s, 1", tbl, w.s);
        ins(ir, "%s = getelementptr ptr, ptr %s, i64 %d", slot, tbl, (int)e->ival);
        ins(ir, "%s = load ptr, ptr %s", fp, slot);
        StrBuf args = {0};
        sbPrintf(&args, "ptr %s", data);
        for (int i = 1; i < e->list.len; i++) {
            Val v = emitExpr(ir, e->list.items[i]);
            sbPrintf(&args, ", %s %s", llType(ir, m->list.items[i - 1]->type), v.s);
        }
        Val r;
        if (m->type->kind == TY_VOID) {
            ins(ir, "call void %s(%s)", fp, args.buf);
            r = val("", &tyVoid);
        } else {
            const char *t = tmp(ir);
            ins(ir, "%s = call %s %s(%s)", t, llType(ir, m->type), fp, args.buf);
            r = val(t, m->type);
        }
        sbFree(&args);
        return r;
    }
    case BI_FLAGS_HAS: {
        Val a = emitExpr(ir, e->a->a);
        Val b = emitExpr(ir, e->list.items[0]);
        const char *x = tmp(ir), *r = tmp(ir);
        ins(ir, "%s = and i32 %s, %s", x, a.s, b.s);
        ins(ir, "%s = icmp eq i32 %s, %s", r, x, b.s);
        return val(r, &tyBool);
    }
    case BI_ERROR: {
        Val code = emitExpr(ir, e->list.items[0]);
        ins(ir, "call void @iris_error_begin()");
        if (e->list.len == 2) {
            emitText(ir, e->list.items[1], 4, "null");
        }
        const char *r = tmp(ir);
        ins(ir, "%s = call i64 @iris_error_finish(i64 %s, ptr %s, i32 %d, i32 %d, ptr %s)", r, code.s, ir->fileStr,
            e->line, e->col, fnName(ir));
        return val(r, &tyErrorValue);
    }
    case BI_STR_COPY: {
        Val v = emitExpr(ir, e->a->a);
        const char *ptr, *len;
        sliceParts(ir, v, &ptr, &len);
        const char *slot = allocaFor(ir, &tyString, "copy");
        ins(ir, "store %%String zeroinitializer, ptr %s", slot);
        ins(ir, "call void @iris_string_append(ptr %s, ptr %s, i64 %s)", slot, ptr, len);
        const char *r = tmp(ir);
        ins(ir, "%s = load %%String, ptr %s", r, slot);
        return val(r, &tyString);
    }
    case BI_ATOMIC_LOAD: case BI_ATOMIC_STORE: case BI_ATOMIC_ADD: case BI_ATOMIC_SUB:
    case BI_ATOMIC_SWAP: case BI_ATOMIC_CAS:
    case BI_MUTEX_LOCK: case BI_MUTEX_UNLOCK: case BI_MUTEX_TRY_LOCK:
    case BI_THREAD_JOIN:
        return emitSync(ir, e);
    case BI_JSON_ENCODE: case BI_JSON_DECODE: case BI_JSON_FREE:
        return emitJson(ir, e);
    case BI_TABLE_OF: {
        Type *t = e->list.items[0]->type->elem;
        emitExpr(ir, e->list.items[0]);
        return val(jsonType(ir, t), e->type);
    }
    case BI_HASH: case BI_KEY_OWN: case BI_KEY_FREE: {
        Node *arg = e->list.items[0];
        Type *t = arg->type;
        Val v = emitExpr(ir, arg);
        if (t->kind == TY_SLICE) {
            const char *ptr, *len;
            sliceParts(ir, v, &ptr, &len);
            const char *r = tmp(ir);
            if (e->builtin == BI_HASH) {
                ins(ir, "%s = call i64 @iris_hash_bytes(ptr %s, i64 %s)", r, ptr, len);
                return val(r, &tyU64);
            }
            if (e->builtin == BI_KEY_FREE) {
                ins(ir, "call void @free(ptr %s)", ptr);
                return val("", &tyVoid);
            }
            ins(ir, "%s = call ptr @iris_bytes_dup(ptr %s, i64 %s)", r, ptr, len);
            return makeSlice(ir, r, len, t);
        }
        if (e->builtin == BI_KEY_OWN) return v;
        if (e->builtin == BI_KEY_FREE) return val("", &tyVoid);
        const char *x = tmp(ir), *r = tmp(ir);
        if (t->kind == TY_PTR) {
            ins(ir, "%s = ptrtoint ptr %s to i64", x, v.s);
        } else {
            const char *lt = llType(ir, t);
            if (strcmp(lt, "i64") == 0) {
                x = v.s;
            } else {
                ins(ir, "%s = zext %s %s to i64", x, lt, v.s);
            }
        }
        ins(ir, "%s = call i64 @iris_hash_u64(i64 %s)", r, x);
        return val(r, &tyU64);
    }
    case BI_EXPECT: {
        Val cond = emitExpr(ir, e->list.items[0]);
        int fail = newLabel(ir), ok = newLabel(ir);
        condBr(ir, cond.s, ok, fail);
        placeLabel(ir, fail);
        const char *msg = "null";
        if (e->list.len == 2) {
            msg = allocaFor(ir, &tyString, "expect");
            ins(ir, "store %%String zeroinitializer, ptr %s", msg);
            emitText(ir, e->list.items[1], 3, msg);
        }
        ins(ir, "call void @iris_expect_failed(ptr %s, i32 %d, i32 %d, ptr %s)", ir->fileStr, e->line, e->col, msg);
        if (e->list.len == 2) {
            ins(ir, "call void @iris_string_free(ptr %s)", msg);
        }
        br(ir, ok);
        placeLabel(ir, ok);
        return val("", &tyVoid);
    }
    case BI_STRING_APPEND:
    case BI_STRING_FREE: {
        Node *recv = e->a->a;
        const char *addr = recv->type->kind == TY_PTR ? emitExpr(ir, recv).s : emitAddr(ir, recv);
        if (e->builtin == BI_STRING_FREE) {
            ins(ir, "call void @iris_string_free(ptr %s)", addr);
        } else {
            Val v = emitExpr(ir, e->list.items[0]);
            const char *ptr, *len;
            sliceParts(ir, v, &ptr, &len);
            ins(ir, "call void @iris_string_append(ptr %s, ptr %s, i64 %s)", addr, ptr, len);
        }
        return val("", &tyVoid);
    }
    default:
        return val("undef", &tyVoid);
    }
}

static Val emitCall(IR *ir, Node *e) {
    if (e->builtin) {
        return emitBuiltin(ir, e);
    }
    Node *d = e->ref;
    StrBuf args = {0};
    for (int i = 0; i < e->list.len; i++) {
        Val v = emitExpr(ir, e->list.items[i]);
        if (i < d->list.len) {
            sbPrintf(&args, "%s%s %s", i ? ", " : "", llType(ir, d->list.items[i]->type), v.s);
            continue;
        }
        /* an extra argument to C's ...: small integers and bool become int */
        const char *x = v.s;
        Type *t = v.t;
        if (t->kind == TY_FLOAT && t->bits == 32) {
            const char *w = tmp(ir);
            ins(ir, "%s = fpext float %s to double", w, x);
            x = w;
            t = &tyF64;
        } else if (t->kind == TY_BOOL || (t->kind == TY_INT && t->bits < 32)) {
            const char *w = tmp(ir);
            ins(ir, "%s = %s %s %s to i32", w, t->kind == TY_INT && t->isSigned ? "sext" : "zext", llType(ir, t), x);
            x = w;
            t = &tyI32;
        }
        sbPrintf(&args, "%s%s %s", i ? ", " : "", llType(ir, t), x);
    }
    const char *a = args.buf ? args.buf : "";
    /* a variadic call names the function's type */
    StrBuf fty = {0};
    if (d->flags & F_VARIADIC) {
        sbPrintf(&fty, "%s (", llType(ir, d->type));
        for (int i = 0; i < d->list.len; i++) {
            sbPrintf(&fty, "%s%s", i ? ", " : "", llType(ir, d->list.items[i]->type));
        }
        sbPuts(&fty, d->list.len ? ", ...)" : "...)");
    } else {
        sbPuts(&fty, llType(ir, d->type));
    }
    Val r;
    if (d->type->kind == TY_VOID) {
        ins(ir, "call %s %s(%s)", fty.buf, d->ir, a);
        r = val("", &tyVoid);
    } else {
        const char *t = tmp(ir);
        ins(ir, "%s = call %s %s(%s)", t, fty.buf, d->ir, a);
        r = val(t, d->type);
    }
    sbFree(&fty);
    sbFree(&args);
    return r;
}

/* Mirrors the checker's idea of a place in memory. */
static bool irAddressable(Node *e) {
    switch (e->kind) {
    case N_IDENT:
        return e->ref && (e->ref->kind == N_VAR || e->ref->kind == N_PARAM || e->ref->kind == N_IDENT);
    case N_FIELD_ACCESS:
        return e->ref && e->a->type && (e->a->type->kind == TY_PTR || irAddressable(e->a));
    case N_INDEX:
        return e->a->type && (e->a->type->kind == TY_SLICE || irAddressable(e->a));
    case N_UNARY:
        return e->op == P_STAR;
    default:
        return false;
    }
}

static const char *emitAddr(IR *ir, Node *e) {
    switch (e->kind) {
    case N_IDENT:
        return e->ref->ir;
    case N_FIELD_ACCESS: {
        Type *bt = e->a->type;
        const char *base;
        Type *st = bt;
        if (bt->kind == TY_PTR) {
            base = emitExpr(ir, e->a).s;
            nullCheck(ir, base, e);
            st = bt->elem;
        } else if (irAddressable(e->a)) {
            base = emitAddr(ir, e->a);
        } else {
            base = spill(ir, emitExpr(ir, e->a));
        }
        const char *r = tmp(ir);
        ins(ir, "%s = getelementptr %s, ptr %s, i32 0, i32 %d", r, st->ir, base, (int)e->ival);
        return r;
    }
    case N_INDEX: {
        Type *bt = e->a->type;
        Val ix = emitExpr(ir, e->list.items[0]);
        if (bt->kind == TY_ARRAY) {
            const char *base = arrayAddr(ir, e->a);
            const char *i = toI64(ir, ix);
            indexCheck(ir, i, fmt(ir, "%lld", (long long)bt->count), e);
            const char *r = tmp(ir);
            ins(ir, "%s = getelementptr %s, ptr %s, i64 0, i64 %s", r, llType(ir, bt), base, i);
            return r;
        }
        Val s = emitExpr(ir, e->a);
        const char *i = toI64(ir, ix);
        const char *ptr, *len;
        sliceParts(ir, s, &ptr, &len);
        indexCheck(ir, i, len, e);
        const char *r = tmp(ir);
        ins(ir, "%s = getelementptr %s, ptr %s, i64 %s", r, llType(ir, bt->elem), ptr, i);
        return r;
    }
    case N_UNARY: {
        Val p = emitExpr(ir, e->a);
        nullCheck(ir, p.s, e);
        return p.s;
    }
    default:
        return spill(ir, emitExpr(ir, e));
    }
}

static Val load(IR *ir, const char *addr, Type *t) {
    const char *r = tmp(ir);
    ins(ir, "%s = load %s, ptr %s", r, llType(ir, t), addr);
    return val(r, t);
}

/* Frees the temporary Strings made since mark. */
static void freeTemps(IR *ir, int mark) {
    for (int i = ir->ntemps - 1; i >= mark; i--) {
        ins(ir, "call void @iris_string_free(ptr %s)", ir->temps[i]);
    }
}

/* ---------------------------------------------------------------- defer and errors */

static void pushFrame(IR *ir, DeferFrame *f) {
    f->outer = ir->defers;
    f->items = (NodeList){0};
    ir->defers = f;
}

static void popFrame(IR *ir) { ir->defers = ir->defers->outer; }

/* Writes out one frame's deferred statements, last first. errdefer ones
 * only on the way out with an error. */
static void runFrame(IR *ir, DeferFrame *f, bool isError) {
    for (int i = f->items.len - 1; i >= 0; i--) {
        Node *d = f->items.items[i];
        if (d->kind == N_VAR) {
            /* a scoped variable: call its free */
            if (d->type->kind == TY_STRING) {
                ins(ir, "call void @iris_string_free(ptr %s)", d->ir);
            } else if (!d->ref) {
                ins(ir, "call void @iris_json_free(ptr %s, ptr %s)", jsonType(ir, d->type), d->ir);
            } else {
                ins(ir, "call void %s(ptr %s)", d->ref->ir, d->ir);
            }
            continue;
        }
        if (d->kind == N_ERRDEFER && !isError) {
            continue;
        }
        emitStmt(ir, d->a);
    }
}

/* Runs the frames from the innermost up to (not including) stop. */
static void runDefers(IR *ir, DeferFrame *stop, bool isError) {
    for (DeferFrame *f = ir->defers; f && f != stop; f = f->outer) {
        runFrame(ir, f, isError);
    }
}

/* The error part of a !T value. */
static const char *errorOf(IR *ir, Val r, Type *rt) {
    if (rt->elem->kind == TY_VOID) {
        return r.s;
    }
    const char *e = tmp(ir);
    ins(ir, "%s = extractvalue %s %s, 1", e, llType(ir, rt), r.s);
    return e;
}

static void traceAdd(IR *ir, Node *at) {
    ins(ir, "call void @iris_trace_add(ptr %s, i32 %d, i32 %d, ptr %s)", ir->fileStr, at->line, at->col, fnName(ir));
}

/* Leaves the function with an error: deferred code first, errdefer too. */
static void returnError(IR *ir, const char *err) {
    freeTemps(ir, 0);
    runDefers(ir, NULL, true);
    Type *rt = ir->func->type;
    if (rt->elem->kind == TY_VOID) {
        ins(ir, "ret i64 %s", err);
    } else {
        const char *r = tmp(ir);
        ins(ir, "%s = insertvalue %s zeroinitializer, i64 %s, 1", r, llType(ir, rt), err);
        ins(ir, "ret %s %s", llType(ir, rt), r);
    }
    ir->terminated = true;
}

/* Leaves a function that returns !T with a value (or nothing for !void). */
static void returnValue(IR *ir, Val v) {
    freeTemps(ir, 0);
    runDefers(ir, NULL, false);
    Type *rt = ir->func->type;
    if (rt->elem->kind == TY_VOID) {
        ins(ir, "ret i64 0");
    } else {
        const char *r = tmp(ir);
        ins(ir, "%s = insertvalue %s zeroinitializer, %s %s, 0", r, llType(ir, rt), llType(ir, rt->elem), v.s);
        ins(ir, "ret %s %s", llType(ir, rt), r);
    }
    ir->terminated = true;
}

static Val emitTry(IR *ir, Node *e) {
    Type *rt = e->a->type;
    Val r = emitExpr(ir, e->a);
    const char *err = errorOf(ir, r, rt);
    const char *bad = tmp(ir);
    ins(ir, "%s = icmp ne i64 %s, 0", bad, err);
    int fail = newLabel(ir), ok = newLabel(ir);
    ins(ir, "br i1 %s, label %%L%d, label %%L%d, !prof !0", bad, fail, ok);
    ir->terminated = true;
    sbPrintf(&ir->body, "L%d:\n", fail);
    ir->terminated = false;
    traceAdd(ir, e);
    returnError(ir, err);
    sbPrintf(&ir->body, "L%d:\n", ok);
    ir->terminated = false;
    if (rt->elem->kind == TY_VOID) {
        return val("", &tyVoid);
    }
    const char *v = tmp(ir);
    ins(ir, "%s = extractvalue %s %s, 0", v, llType(ir, rt), r.s);
    return val(v, rt->elem);
}

static bool hasValue(Type *t);
static void emitValueBlock(IR *ir, Node *b, const char *slot, Type *t);

static Val emitCatch(IR *ir, Node *e) {
    Type *rt = e->a->type, *t = rt->elem;
    bool value = t->kind != TY_VOID && hasValue(e->type);
    const char *slot = value ? allocaFor(ir, t, "catch") : NULL;
    Val r = emitExpr(ir, e->a);
    const char *err = errorOf(ir, r, rt);
    const char *bad = tmp(ir);
    ins(ir, "%s = icmp ne i64 %s, 0", bad, err);
    int onErr = newLabel(ir), ok = newLabel(ir), end = newLabel(ir);
    condBr(ir, bad, onErr, ok);
    placeLabel(ir, ok);
    if (value) {
        const char *v = tmp(ir);
        ins(ir, "%s = extractvalue %s %s, 0", v, llType(ir, rt), r.s);
        ins(ir, "store %s %s, ptr %s", llType(ir, t), v, slot);
    }
    br(ir, end);
    placeLabel(ir, onErr);
    if (e->b) {
        Val v = emitExpr(ir, e->b);
        if (!ir->terminated) {
            ins(ir, "store %s %s, ptr %s", llType(ir, t), v.s, slot);
        }
    } else {
        Node *var = e->d;
        var->ir = varSlot(ir, &tyErrorValue, var, 0);
        ins(ir, "store i64 %s, ptr %s", err, var->ir);
        if (e->c->kind == N_MATCH) {
            Val v = emitMatch(ir, e->c, value);
            if (value && !ir->terminated) {
                ins(ir, "store %s %s, ptr %s", llType(ir, t), v.s, slot);
            }
        } else if (value) {
            emitValueBlock(ir, e->c, slot, t);
        } else {
            emitBlock(ir, e->c);
        }
    }
    br(ir, end);
    placeLabel(ir, end);
    if (!value) {
        return val("", &tyVoid);
    }
    return load(ir, slot, t);
}

static Val emitSlice(IR *ir, Node *e) {
    Type *bt = e->a->type;
    const char *ptr, *len;
    bool unchecked = false;
    if (bt->kind == TY_ARRAY) {
        ptr = emitAddr(ir, e->a);
        len = fmt(ir, "%lld", (long long)bt->count);
    } else if (bt->kind == TY_PTR) {
        ptr = emitExpr(ir, e->a).s;
        len = "0";
        unchecked = true;           /* a pointer has no length to check against */
    } else {
        sliceParts(ir, emitExpr(ir, e->a), &ptr, &len);
    }
    const char *lo = e->b ? toI64(ir, emitExpr(ir, e->b)) : "0";
    const char *hi = e->c ? toI64(ir, emitExpr(ir, e->c)) : len;
    if (boundsChecks(ir) && (e->b || e->c) && !unchecked) {
        const char *a = tmp(ir), *b = tmp(ir), *bad = tmp(ir);
        ins(ir, "%s = icmp ugt i64 %s, %s", a, lo, hi);
        ins(ir, "%s = icmp ugt i64 %s, %s", b, hi, len);
        ins(ir, "%s = or i1 %s, %s", bad, a, b);
        int fail = newLabel(ir), ok = newLabel(ir);
        ins(ir, "br i1 %s, label %%L%d, label %%L%d, !prof !0", bad, fail, ok);
        ir->terminated = true;
        sbPrintf(&ir->body, "L%d:\n", fail);
        ir->terminated = false;
        ins(ir, "call void @iris_slice_failed(i64 %s, i64 %s, i64 %s, ptr %s, i32 %d, i32 %d, ptr %s)", lo, hi, len,
            ir->fileStr, e->line, e->col, fnName(ir));
        ins(ir, "unreachable");
        ir->terminated = true;
        sbPrintf(&ir->body, "L%d:\n", ok);
        ir->terminated = false;
    }
    const char *start = tmp(ir), *n = tmp(ir);
    ins(ir, "%s = getelementptr %s, ptr %s, i64 %s", start, llType(ir, e->type->elem), ptr, lo);
    ins(ir, "%s = sub i64 %s, %s", n, hi, lo);
    return makeSlice(ir, start, n, e->type);
}

static Val emitStructLit(IR *ir, Node *e) {
    Type *t = e->type;
    Node *d = t->decl;
    const char *acc = "zeroinitializer";
    for (int i = 0; i < d->list.len; i++) {
        Node *f = d->list.items[i];
        Node *value = NULL;
        for (int j = 0; j < e->list.len; j++) {
            if (e->list.items[j]->ref == f) {
                value = e->list.items[j]->a;
            }
        }
        if (!value && f->kind == N_FIELD) {
            value = f->b;
        }
        if (!value) {
            continue;
        }
        Val v = emitExpr(ir, value);
        const char *r = tmp(ir);
        ins(ir, "%s = insertvalue %s %s, %s %s, %d", r, t->ir, acc, llType(ir, f->type), v.s, i);
        acc = r;
    }
    return val(acc, t);
}

static Val emitArrayLit(IR *ir, Node *e) {
    Type *t = e->type;
    const char *ty = llType(ir, t);
    const char *acc = "zeroinitializer";
    for (int i = 0; i < e->list.len; i++) {
        Val v = emitExpr(ir, e->list.items[i]);
        const char *r = tmp(ir);
        ins(ir, "%s = insertvalue %s %s, %s %s, %d", r, ty, acc, llType(ir, t->elem), v.s, i);
        acc = r;
    }
    return val(acc, t);
}

static Val emitExpr(IR *ir, Node *e) {
    dbgAt(ir, e);
    if (e->isConst && e->type) {
        if (e->type->kind == TY_BOOL) {
            return val(e->cval ? "true" : "false", &tyBool);
        }
        if (e->type->kind == TY_INT) {
            return val(intConst(ir, e->cval, e->type), e->type);
        }
        if (e->type->kind == TY_ENUM) {
            return val(fmt(ir, "%lld", (long long)(int32_t)e->cval), e->type);
        }
        if (e->type->kind == TY_FLOAT) {
            return val(floatConst(ir, e->fval, e->type), e->type);
        }
        if (e->type->kind == TY_ERRCODE) {
            return val(fmt(ir, "%lld", (long long)e->cval), e->type);
        }
    }
    if (e->type && e->type->kind == TY_NULL) {
        return val("null", e->type);
    }
    if ((e->kind == N_FIELD_ACCESS || e->kind == N_ENUM_DOT) && e->type && e->type->kind == TY_ENUM &&
        e->type->hasData && e->ref && e->ref->kind == N_ENUM_MEMBER) {
        return emitEnumValue(ir, e->type, e->ref, NULL);
    }
    if (e->flags & F_TEMPSTR) {
        /* a String only this statement holds: freed when it ends */
        e->flags &= ~F_TEMPSTR;
        Val v = emitExpr(ir, e);
        e->flags |= F_TEMPSTR;
        if (ir->terminated) {
            return v;
        }
        const char *slot = allocaFor(ir, &tyString, "temp");
        ins(ir, "store %%String %s, ptr %s", v.s, slot);
        addTemp(ir, slot);
        return v;
    }
    if (e->kind == N_INTERP && (e->flags & F_TEMPTEXT)) {
        const char *slot = allocaFor(ir, &tyString, "text");
        ins(ir, "store %%String zeroinitializer, ptr %s", slot);
        emitText(ir, e, 3, slot);
        addTemp(ir, slot);
        Val str = load(ir, slot, &tyString);
        const char *p = tmp(ir), *n = tmp(ir);
        ins(ir, "%s = extractvalue %%String %s, 0", p, str.s);
        ins(ir, "%s = extractvalue %%String %s, 1", n, str.s);
        return makeSlice(ir, p, n, &tyStr);
    }
    switch (e->kind) {
    case N_TUPLE: {
        const char *ty = llType(ir, e->type);
        const char *acc = "undef";
        for (int i = 0; i < e->list.len; i++) {
            Val v = emitExpr(ir, e->list.items[i]);
            const char *r = tmp(ir);
            ins(ir, "%s = insertvalue %s %s, %s %s, %d", r, ty, acc, llType(ir, e->type->elems[i]), v.s, i);
            acc = r;
        }
        return val(acc, e->type);
    }
    case N_STRING: {
        const char *g = stringConst(ir, e->text, e->textLen);
        return val(fmt(ir, "{ ptr %s, i64 %zu }", g, e->textLen), &tyStr);
    }
    case N_NULL:
        return val("null", e->type);
    case N_IDENT: {
        Node *d = e->ref;
        if (d->kind == N_FUNC) {
            return val(d->ir, e->type);     /* a function as a value: its address */
        }
        if (d->kind == N_CONST) {
            return emitExpr(ir, d->b);  /* a str constant */
        }
        return load(ir, d->ir, d->type);
    }
    case N_CONVERT:
        return emitConvert(ir, e);
    case N_UNARY: {
        if (e->op == P_AMP) {
            return val(emitAddr(ir, e->a), e->type);
        }
        if (e->op == P_STAR) {
            return load(ir, emitAddr(ir, e), e->type);
        }
        if (e->op == P_LARROW) {
            Val ch = emitExpr(ir, e->a);
            const char *slot = allocaFor(ir, e->type, "recv");
            ins(ir, "call void @iris_chan_recv(ptr %s, ptr %s)", ch.s, slot);
            return load(ir, slot, e->type);
        }
        Val a = emitExpr(ir, e->a);
        if (e->op == P_BANG) {
            const char *r = tmp(ir);
            ins(ir, "%s = xor i1 %s, true", r, a.s);
            return val(r, &tyBool);
        }
        if (e->op == P_TILDE) {
            const char *r = tmp(ir);
            ins(ir, "%s = xor %s %s, -1", r, llType(ir, a.t), a.s);
            return val(r, a.t);
        }
        if (a.t->kind == TY_FLOAT) {
            const char *r = tmp(ir);
            ins(ir, "%s = fneg %s %s", r, llType(ir, a.t), a.s);
            return val(r, a.t);
        }
        return emitArith(ir, e, P_MINUS, val("0", a.t), a);
    }
    case N_BINARY:
        return emitBinary(ir, e);
    case N_CALL:
        return emitCall(ir, e);
    case N_FIELD_ACCESS: {
        if (e->a->type && e->a->type->kind == TY_ENUM && e->builtin) {
            Val v = emitExpr(ir, e->a);
            const char *p = tmp(ir), *n = tmp(ir);
            ins(ir, "%s = call ptr @iris_enum_name(i64 %lld, i32 %s)", p, (long long)v.t->count, enumTag(ir, v));
            ins(ir, "%s = call i64 @strlen(ptr %s)", n, p);
            return makeSlice(ir, p, n, &tyStr);
        }
        if (e->a->type && e->a->type->kind == TY_ERRVAL) {
            Val err = emitExpr(ir, e->a);
            if (strcmp(e->name, "code") == 0) {
                return val(err.s, &tyErrCode);
            }
            const char *p = tmp(ir), *n = tmp(ir);
            ins(ir, "%s = call ptr @iris_error_message_ptr()", p);
            ins(ir, "%s = call i64 @iris_error_message_len()", n);
            return makeSlice(ir, p, n, &tyStr);
        }
        if (!e->ref) {
            /* .len of a slice or a String (an array's is a constant), or .ptr */
            Val a = emitExpr(ir, e->a);
            const char *r = tmp(ir);
            ins(ir, "%s = extractvalue %s %s, %d", r, llType(ir, a.t), a.s, e->builtin == 2 ? 0 : 1);
            return val(r, e->builtin == 2 ? e->type : &tyI64);
        }
        if (irAddressable(e)) {
            return load(ir, emitAddr(ir, e), e->type);
        }
        Val a = emitExpr(ir, e->a);
        const char *r = tmp(ir);
        ins(ir, "%s = extractvalue %s %s, %d", r, a.t->ir, a.s, (int)e->ival);
        return val(r, e->type);
    }
    case N_INDEX:
        return load(ir, emitAddr(ir, e), e->type);
    case N_SLICE:
        return emitSlice(ir, e);
    case N_STRUCT_LIT:
        return emitStructLit(ir, e);
    case N_ARRAY_LIT:
        if (e->list.len == 0 && e->type->kind == TY_SLICE) {
            return val("zeroinitializer", e->type);     /* an empty slice */
        }
        return emitArrayLit(ir, e);
    case N_IF:
        return emitIf(ir, e, true);
    case N_MATCH:
        return emitMatch(ir, e, true);
    case N_TRY:
        return emitTry(ir, e);
    case N_CATCH:
        return emitCatch(ir, e);
    case N_THREAD: {
        const char *h = emitSpawn(ir, e->a, true);
        if (e->ival) {
            ins(ir, "call void @iris_thread_detach(ptr %s)", h);
        }
        return val(h, &tyThread);
    }
    case N_CHAN_MAKE: {
        Val n = emitExpr(ir, e->b);
        const char *size = sizeOf(ir, e->type->elem);
        const char *r = tmp(ir);
        ins(ir, "%s = call ptr @iris_chan_new(i64 %s, i64 %s, ptr %s, i32 %d, i32 %d, ptr %s)", r, size, n.s,
            ir->fileStr, e->line, e->col, fnName(ir));
        return val(r, e->type);
    }
    default:
        return val("undef", e->type ? e->type : &tyVoid);
    }
}

/* ---------------------------------------------------------------- control flow */

static bool hasValue(Type *t) {
    return t && t->kind != TY_VOID && t->kind != TY_NORETURN && t->kind != TY_ERROR;
}

/* Runs a value block and stores its value in slot (unless it leaves). */
static void emitValueBlock(IR *ir, Node *b, const char *slot, Type *t) {
    if (b->kind != N_BLOCK) {
        if (b->kind == N_EXPR_STMT) {
            Val v = emitExpr(ir, b->a);
            if (!ir->terminated && v.t->kind != TY_NORETURN) {
                ins(ir, "store %s %s, ptr %s", llType(ir, t), v.s, slot);
            }
        } else {
            emitStmt(ir, b);
        }
        return;
    }
    DeferFrame frame;
    pushFrame(ir, &frame);
    for (int i = 0; i < b->list.len; i++) {
        Node *s = b->list.items[i];
        if (i == b->list.len - 1 && s->kind == N_EXPR_STMT) {
            Val v = emitExpr(ir, s->a);
            if (!ir->terminated && v.t->kind != TY_NORETURN) {
                ins(ir, "store %s %s, ptr %s", llType(ir, t), v.s, slot);
            }
        } else {
            emitStmt(ir, s);
        }
    }
    if (!ir->terminated) {
        runFrame(ir, &frame, false);
    }
    popFrame(ir);
}

static Val emitIf(IR *ir, Node *n, bool wantValue) {
    Type *t = n->type;
    bool value = wantValue && hasValue(t);
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
                ins(ir, "store %s %s, ptr %s", llType(ir, t), v.s, slot);
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
    return load(ir, slot, t);
}

static bool wildcard(Node *p) { return p->kind == N_IDENT && strcmp(p->name, "_") == 0; }

static Val emitMatch(IR *ir, Node *m, bool wantValue) {
    Type *t = m->type;
    bool value = wantValue && hasValue(t);
    const char *slot = value ? allocaFor(ir, t, "match") : NULL;
    Val subject = emitExpr(ir, m->a);
    bool dataEnum = subject.t->kind == TY_ENUM && subject.t->hasData;
    const char *subjAddr = NULL, *tag = NULL;
    if (dataEnum) {
        subjAddr = spill(ir, subject);
        tag = enumTag(ir, subject);
    }
    int end = newLabel(ir);
    for (int i = 0; i < m->list.len; i++) {
        Node *arm = m->list.items[i];
        int body = newLabel(ir), next = newLabel(ir);
        for (int j = 0; j < arm->list.len; j++) {
            Node *p = arm->list.items[j];
            int tryNext = j + 1 < arm->list.len ? newLabel(ir) : next;
            if (wildcard(p)) {
                br(ir, body);
            } else if (dataEnum) {
                const char *eq = tmp(ir);
                ins(ir, "%s = icmp eq i32 %s, %lld", eq, tag, (long long)p->ref->cval);
                condBr(ir, eq, body, tryNext);
            } else if (p->kind == N_BINARY && p->op == P_DOTDOT) {
                const char *lo = tmp(ir), *hi = tmp(ir), *both = tmp(ir);
                bool s = subject.t->isSigned;
                const char *ty = llType(ir, subject.t);
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
        if (dataEnum && arm->list.len == 1 && arm->list.items[0]->kind == N_CALL) {
            /* take the value apart into the names the pattern gives */
            Node *p = arm->list.items[0];
            Type *data = memberData(p->ref);
            const char *room = tmp(ir);
            ins(ir, "%s = getelementptr %s, ptr %s, i32 0, i32 1", room, subject.t->ir, subjAddr);
            for (int k = 0; k < p->list.len; k++) {
                Node *b = p->list.items[k];
                if (strcmp(b->name, "_") == 0) {
                    continue;
                }
                const char *f = tmp(ir);
                ins(ir, "%s = getelementptr %s, ptr %s, i32 0, i32 %d", f, data->ir, room, k);
                Val v = load(ir, f, data->elems[k]);
                b->ir = varSlot(ir, data->elems[k], b, 0);
                ins(ir, "store %s %s, ptr %s", llType(ir, data->elems[k]), v.s, b->ir);
            }
        }
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
    return load(ir, slot, t);
}

/* ---------------------------------------------------------------- statements */

static void emitBlock(IR *ir, Node *b) {
    DeferFrame frame;
    pushFrame(ir, &frame);
    for (int i = 0; i < b->list.len; i++) {
        emitStmt(ir, b->list.items[i]);
    }
    if (!ir->terminated) {
        runFrame(ir, &frame, false);
    }
    popFrame(ir);
}

static void emitFor(IR *ir, Node *s) {
    Loop loop = {ir->loop, ir->pendingLabel, 0, 0, ir->defers, ir->ntemps};
    ir->pendingLabel = NULL;
    int cond = newLabel(ir), body = newLabel(ir), post = newLabel(ir), end = newLabel(ir);
    loop.breakTo = end;
    loop.continueTo = post;

    if ((s->flags & F_FOR_IN) && s->a->kind == N_BINARY && s->a->op == P_DOTDOT) {
        Node *range = s->a, *v = s->list.items[0];
        Type *t = range->type;
        v->ir = varSlot(ir, t, v, 0);
        const char *endSlot = allocaFor(ir, t, "end");
        Val lo = emitExpr(ir, range->a);
        ins(ir, "store %s %s, ptr %s", llType(ir, t), lo.s, v->ir);
        Val hi = emitExpr(ir, range->b);
        ins(ir, "store %s %s, ptr %s", llType(ir, t), hi.s, endSlot);
        placeLabel(ir, cond);
        Val i = load(ir, v->ir, t), e = load(ir, endSlot, t);
        const char *more = tmp(ir);
        ins(ir, "%s = icmp %s %s %s, %s", more, t->isSigned ? "slt" : "ult", llType(ir, t), i.s, e.s);
        condBr(ir, more, body, end);
        placeLabel(ir, body);
        ir->loop = &loop;
        emitBlock(ir, s->c);
        ir->loop = loop.outer;
        placeLabel(ir, post);
        Val cur = load(ir, v->ir, t);
        const char *nx = tmp(ir);
        ins(ir, "%s = add %s %s, 1", nx, llType(ir, t), cur.s);
        ins(ir, "store %s %s, ptr %s", llType(ir, t), nx, v->ir);
        br(ir, cond);
        placeLabel(ir, end);
        return;
    }

    if ((s->flags & F_FOR_IN) && s->ref && s->ref->kind == N_FUNC) {
        /* over a struct with next(i64 *at): until it gives false */
        Node *m = s->ref;
        Type *t = s->a->type, *rt = m->type;
        const char *recv;
        if (t->kind == TY_PTR) {
            recv = emitExpr(ir, s->a).s;
        } else if (s->a->flags & F_EXPLICIT) {
            Val v = emitExpr(ir, s->a);
            recv = allocaFor(ir, t, "iterated");
            ins(ir, "store %s %s, ptr %s", llType(ir, t), v.s, recv);
        } else {
            recv = emitAddr(ir, s->a);
        }
        const char *at = allocaFor(ir, &tyI64, "at");
        ins(ir, "store i64 0, ptr %s", at);
        for (int i = 0; i < s->list.len; i++) {
            Node *v = s->list.items[i];
            v->ir = varSlot(ir, v->type, v, 0);
        }
        placeLabel(ir, cond);
        const char *r = tmp(ir), *ok = tmp(ir);
        ins(ir, "%s = call %s %s(ptr %s, ptr %s)", r, llType(ir, rt), m->ir, recv, at);
        ins(ir, "%s = extractvalue %s %s, %d", ok, llType(ir, rt), r, rt->nelems - 1);
        condBr(ir, ok, body, end);
        placeLabel(ir, body);
        for (int i = 0; i < s->list.len; i++) {
            Node *v = s->list.items[i];
            const char *x = tmp(ir);
            ins(ir, "%s = extractvalue %s %s, %d", x, llType(ir, rt), r, i);
            ins(ir, "store %s %s, ptr %s", llType(ir, v->type), x, v->ir);
        }
        ir->loop = &loop;
        emitBlock(ir, s->c);
        ir->loop = loop.outer;
        placeLabel(ir, post);
        br(ir, cond);
        placeLabel(ir, end);
        return;
    }

    if (s->flags & F_FOR_IN) {
        /* over the items of a slice or an array */
        Type *ct = s->a->type;
        const char *ptr, *len;
        if (ct->kind == TY_ARRAY) {
            ptr = arrayAddr(ir, s->a);
            len = fmt(ir, "%lld", (long long)ct->count);
        } else {
            sliceParts(ir, emitExpr(ir, s->a), &ptr, &len);
        }
        Node *item = s->list.items[s->list.len - 1];
        Node *idx = s->list.len == 2 ? s->list.items[0] : NULL;
        const char *iSlot = allocaFor(ir, &tyI64, idx ? idx->name : "i");
        if (idx) {
            idx->ir = iSlot;
        }
        item->ir = varSlot(ir, ct->elem, item, 0);
        ins(ir, "store i64 0, ptr %s", iSlot);
        placeLabel(ir, cond);
        Val i = load(ir, iSlot, &tyI64);
        const char *more = tmp(ir);
        ins(ir, "%s = icmp ult i64 %s, %s", more, i.s, len);
        condBr(ir, more, body, end);
        placeLabel(ir, body);
        const char *at = tmp(ir);
        if (ct->kind == TY_ARRAY) {
            ins(ir, "%s = getelementptr %s, ptr %s, i64 0, i64 %s", at, llType(ir, ct), ptr, i.s);
        } else {
            ins(ir, "%s = getelementptr %s, ptr %s, i64 %s", at, llType(ir, ct->elem), ptr, i.s);
        }
        Val x = load(ir, at, ct->elem);
        ins(ir, "store %s %s, ptr %s", llType(ir, ct->elem), x.s, item->ir);
        ir->loop = &loop;
        emitBlock(ir, s->c);
        ir->loop = loop.outer;
        placeLabel(ir, post);
        Val cur = load(ir, iSlot, &tyI64);
        const char *nx = tmp(ir);
        ins(ir, "%s = add i64 %s, 1", nx, cur.s);
        ins(ir, "store i64 %s, ptr %s", nx, iSlot);
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

static void emitAssign(IR *ir, Node *s) {
    if (s->list.len > 1) {
        /* the values first, then the places, so a, b = b, a swaps */
        Val v = emitExpr(ir, s->b);
        const char *vals[32];
        for (int i = 0; i < s->list.len; i++) {
            const char *x = tmp(ir);
            ins(ir, "%s = extractvalue %s %s, %d", x, llType(ir, v.t), v.s, i);
            vals[i] = x;
        }
        for (int i = 0; i < s->list.len; i++) {
            Node *t = s->list.items[i];
            if (t->kind == N_IDENT && strcmp(t->name, "_") == 0) {
                continue;
            }
            const char *addr = emitAddr(ir, t);
            ins(ir, "store %s %s, ptr %s", llType(ir, t->type), vals[i], addr);
        }
        return;
    }
    Node *target = s->list.items[0];
    if (target->kind == N_IDENT && strcmp(target->name, "_") == 0) {
        emitExpr(ir, s->b);
        return;
    }
    Type *t = target->type;
    if (s->op == P_ASSIGN) {
        Val v = emitExpr(ir, s->b);
        const char *addr = emitAddr(ir, target);
        ins(ir, "store %s %s, ptr %s", llType(ir, t), v.s, addr);
        return;
    }
    const char *addr = emitAddr(ir, target);
    Val a = load(ir, addr, t);
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
            llType(ir, t), a.s, b.s);
        r = val(x, t);
    }
    }
    ins(ir, "store %s %s, ptr %s", llType(ir, t), r.s, addr);
}

/* spawn f(a, b): the arguments are worked out now and kept on the heap; a
 * small function (the thunk) the new task runs makes the call and frees
 * them. */
static const char *emitSpawn(IR *ir, Node *call, bool thread) {
    bool indirect = call->builtin == BI_INDIRECT;
    Node *d = call->ref;
    int n = call->list.len;
    Val *vals = arenaAlloc(ir->arena, sizeof(Val) * (size_t)(n + 1));
    StrBuf env = {0};
    sbPuts(&env, "{ ");
    int first = 0;
    Val fp = {0};
    if (indirect) {
        fp = emitExpr(ir, call->a);
        sbPuts(&env, "ptr");
        first = 1;
    }
    for (int i = 0; i < n; i++) {
        vals[i] = emitExpr(ir, call->list.items[i]);
        Type *pt = indirect ? call->a->type->elems[i] : (i < d->list.len ? d->list.items[i]->type : vals[i].t);
        vals[i].t = pt;
        sbPrintf(&env, "%s%s", (i + first) ? ", " : "", llType(ir, pt));
    }
    if (n + first == 0) {
        sbPuts(&env, "i8");
    }
    sbPuts(&env, " }");
    const char *envTy = sbFinish(&env, ir->arena);
    const char *szp = tmp(ir), *sz = tmp(ir), *mem = tmp(ir);
    ins(ir, "%s = getelementptr %s, ptr null, i64 1", szp, envTy);
    ins(ir, "%s = ptrtoint ptr %s to i64", sz, szp);
    ins(ir, "%s = call ptr @iris_alloc(i64 %s, i64 1, ptr %s, i32 %d, i32 %d, ptr %s)", mem, sz, ir->fileStr,
        call->line, call->col, fnName(ir));
    if (indirect) {
        const char *f0 = tmp(ir);
        ins(ir, "%s = getelementptr %s, ptr %s, i32 0, i32 0", f0, envTy, mem);
        ins(ir, "store ptr %s, ptr %s", fp.s, f0);
    }
    for (int i = 0; i < n; i++) {
        const char *f = tmp(ir);
        ins(ir, "%s = getelementptr %s, ptr %s, i32 0, i32 %d", f, envTy, mem, i + first);
        ins(ir, "store %s %s, ptr %s", llType(ir, vals[i].t), vals[i].s, f);
    }
    int id = ++ir->spawns;
    const char *thunk = fmt(ir, "@\"spawn.%d\"", id);
    const char *handle = NULL;
    if (thread) {
        handle = tmp(ir);
        ins(ir, "%s = call ptr @iris_thread_new(ptr %s, ptr %s)", handle, thunk, mem);
    } else {
        ins(ir, "call void @iris_spawn(ptr %s, ptr %s)", thunk, mem);
    }

    /* the thunk */
    StrBuf *x = &ir->extra;
    sbPrintf(x, "define internal void %s(ptr %%env)%s {\n", thunk, ir->mode == MODE_DEBUG ? " #0" : "");
    StrBuf args = {0};
    for (int i = 0; i < n; i++) {
        sbPrintf(x, "  %%a%d.p = getelementptr %s, ptr %%env, i32 0, i32 %d\n", i, envTy, i + first);
        sbPrintf(x, "  %%a%d = load %s, ptr %%a%d.p\n", i, llType(ir, vals[i].t), i);
        sbPrintf(&args, "%s%s %%a%d", i ? ", " : "", llType(ir, vals[i].t), i);
    }
    const char *callee;
    Type *ret;
    if (indirect) {
        sbPrintf(x, "  %%fp.p = getelementptr %s, ptr %%env, i32 0, i32 0\n", envTy);
        sbPuts(x, "  %fp = load ptr, ptr %fp.p\n");
        callee = "%fp";
        ret = call->a->type->elem;
    } else {
        callee = d->ir;
        ret = d->type;
    }
    StrBuf fty = {0};
    if (indirect) {
        sbPrintf(&fty, "%s (", llType(ir, ret));
        for (int i = 0; i < n; i++) sbPrintf(&fty, "%s%s", i ? ", " : "", llType(ir, vals[i].t));
        sbPutc(&fty, ')');
    } else {
        sbPuts(&fty, llType(ir, ret));
    }
    if (ret->kind == TY_VOID) {
        sbPrintf(x, "  call %s %s(%s)\n", fty.buf, callee, args.buf ? args.buf : "");
    } else {
        sbPrintf(x, "  %%r = call %s %s(%s)\n", fty.buf, callee, args.buf ? args.buf : "");
    }
    sbPuts(x, "  call void @free(ptr %env)\n  ret void\n}\n\n");
    sbFree(&args);
    sbFree(&fty);
    return handle;
}

/* select: the channels and where each value goes, then a jump to the arm
 * whose channel had a value (or the after arm). */
static void emitSelect(IR *ir, Node *s) {
    int n = 0;
    Node *afterArm = NULL;
    for (int i = 0; i < s->list.len; i++) {
        if (s->list.items[i]->a->builtin == BI_AFTER) {
            afterArm = s->list.items[i];
        } else {
            n++;
        }
    }
    const char *chans = fmt(ir, "%%v.selchans.%d", ++ir->vars);
    const char *outs = fmt(ir, "%%v.selouts.%d", ++ir->vars);
    sbPrintf(&ir->allocas, "  %s = alloca [%d x ptr]\n  %s = alloca [%d x ptr]\n", chans, n ? n : 1, outs, n ? n : 1);
    Node **recvArms = arenaAlloc(ir->arena, sizeof(Node *) * (size_t)(n ? n : 1));
    const char **slots = arenaAlloc(ir->arena, sizeof(char *) * (size_t)(n ? n : 1));
    int k = 0;
    for (int i = 0; i < s->list.len; i++) {
        Node *arm = s->list.items[i];
        if (arm == afterArm) continue;
        Node *recv = arm->a;
        Val ch = emitExpr(ir, recv->a);
        slots[k] = allocaFor(ir, recv->type, "sel");
        const char *pc = tmp(ir), *po = tmp(ir);
        ins(ir, "%s = getelementptr [%d x ptr], ptr %s, i64 0, i64 %d", pc, n, chans, k);
        ins(ir, "store ptr %s, ptr %s", ch.s, pc);
        ins(ir, "%s = getelementptr [%d x ptr], ptr %s, i64 0, i64 %d", po, n, outs, k);
        ins(ir, "store ptr %s, ptr %s", slots[k], po);
        recvArms[k++] = arm;
    }
    const char *timeout = "-1";
    if (afterArm) {
        timeout = emitExpr(ir, afterArm->a->list.items[0]).s;
    }
    const char *which = tmp(ir);
    ins(ir, "%s = call i64 @iris_select(i64 %d, ptr %s, ptr %s, i64 %s)", which, n, chans, outs, timeout);
    int end = newLabel(ir);
    for (int i = 0; i <= n; i++) {
        Node *arm = i < n ? recvArms[i] : afterArm;
        if (!arm) continue;
        int body = newLabel(ir), next = newLabel(ir);
        const char *is = tmp(ir);
        ins(ir, "%s = icmp eq i64 %s, %d", is, which, i);
        condBr(ir, is, body, next);
        placeLabel(ir, body);
        if (i < n && arm->d && strcmp(arm->d->name, "_") != 0) {
            arm->d->ir = varSlot(ir, arm->d->type, arm->d, 0);
            Val v = load(ir, slots[i], arm->d->type);
            ins(ir, "store %s %s, ptr %s", llType(ir, arm->d->type), v.s, arm->d->ir);
        }
        if (arm->c->kind == N_BLOCK) {
            emitBlock(ir, arm->c);
        } else {
            emitStmt(ir, arm->c);
        }
        br(ir, end);
        placeLabel(ir, next);
    }
    br(ir, end);
    placeLabel(ir, end);
}

static void emitStmtInner(IR *ir, Node *s);

/* A statement, then the temporary Strings its expressions made. */
static void emitStmt(IR *ir, Node *s) {
    dbgAt(ir, s);
    int mark = ir->ntemps;
    emitStmtInner(ir, s);
    if (!ir->terminated) {
        freeTemps(ir, mark);
    }
    ir->ntemps = mark;
}

static void emitStmtInner(IR *ir, Node *s) {
    switch (s->kind) {
    case N_VAR: {
        s->ir = varSlot(ir, s->type, s, 0);
        if (s->b) {
            Val v = emitExpr(ir, s->b);
            if (!ir->terminated) {
                ins(ir, "store %s %s, ptr %s", llType(ir, s->type), v.s, s->ir);
            }
        } else {
            ins(ir, "store %s zeroinitializer, ptr %s", llType(ir, s->type), s->ir);
        }
        if ((s->flags & F_SCOPED) && ir->defers) {
            listPush(ir->arena, &ir->defers->items, s);
        }
        return;
    }
    case N_DEFER:
    case N_ERRDEFER:
        if (ir->defers) {
            listPush(ir->arena, &ir->defers->items, s);
        }
        return;
    case N_DEFINE: {
        if (s->list.len > 1) {
            Val v = emitExpr(ir, s->b);
            if (ir->terminated) {
                return;
            }
            for (int i = 0; i < s->list.len; i++) {
                Node *id = s->list.items[i];
                if (strcmp(id->name, "_") == 0) {
                    continue;
                }
                id->ir = varSlot(ir, id->type, id, 0);
                const char *x = tmp(ir);
                ins(ir, "%s = extractvalue %s %s, %d", x, llType(ir, v.t), v.s, i);
                ins(ir, "store %s %s, ptr %s", llType(ir, id->type), x, id->ir);
            }
            return;
        }
        Node *id = s->list.items[0];
        id->ir = varSlot(ir, id->type, id, 0);
        Val v = emitExpr(ir, s->b);
        if (!ir->terminated) {
            ins(ir, "store %s %s, ptr %s", llType(ir, id->type), v.s, id->ir);
        }
        return;
    }
    case N_ASSIGN:
        emitAssign(ir, s);
        return;
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
    case N_RETURN: {
        Type *rt = ir->func->type;
        if (rt->kind == TY_RESULT) {
            if (!s->a) {
                returnValue(ir, val("", &tyVoid));
                return;
            }
            Val v = emitExpr(ir, s->a);
            if (ir->terminated) {
                return;
            }
            if (s->a->type->kind == TY_ERRVAL) {
                if (!(s->a->kind == N_CALL && s->a->builtin == BI_ERROR)) {
                    traceAdd(ir, s);    /* passing on an error it caught */
                }
                returnError(ir, v.s);
            } else {
                returnValue(ir, v);
            }
            return;
        }
        if (s->a) {
            Val v = emitExpr(ir, s->a);
            if (!ir->terminated) {
                freeTemps(ir, 0);
                runDefers(ir, NULL, false);
                ins(ir, "ret %s %s", llType(ir, rt), v.s);
            }
        } else {
            freeTemps(ir, 0);
            runDefers(ir, NULL, false);
            ins(ir, "ret void");
        }
        ir->terminated = true;
        return;
    }
    case N_BREAK: {
        Loop *l = findLoop(ir, s->name);
        freeTemps(ir, l->temps);
        runDefers(ir, l->frame, false);
        br(ir, l->breakTo);
        return;
    }
    case N_CONTINUE: {
        Loop *l = findLoop(ir, s->name);
        freeTemps(ir, l->temps);
        runDefers(ir, l->frame, false);
        br(ir, l->continueTo);
        return;
    }
    case N_LABELED:
        ir->pendingLabel = s->name;
        emitStmt(ir, s->a);
        return;
    case N_FOR:
        emitFor(ir, s);
        return;
    case N_SPAWN:
        emitSpawn(ir, s->a, false);
        return;
    case N_SELECT:
        emitSelect(ir, s);
        return;
    default:
        return;
    }
}

/* ---------------------------------------------------------------- functions */

static void emitFunction(IR *ir, Node *f, StrBuf *out) {
    ir->func = f;
    if (f->src) {
        ir->fileStr = cString(ir, f->src->path);
    }
    ir->tmp = 0;
    ir->label = 0;
    ir->vars = 0;
    ir->terminated = false;
    ir->allocas = (StrBuf){0};
    ir->body = (StrBuf){0};

    ir->dbgScope = 0;
    if (f->src) {
        /* the function's DISubprogram: debuggers and AddressSanitizer show Iris lines */
        int file = dbgFile(ir, f->src->path);
        int sp = ir->metaId++;
        const char *link = f->ir[1] == '"' ? f->ir + 2 : f->ir + 1;
        size_t linkLen = strlen(link) - (f->ir[1] == '"' ? 1 : 0);
        sbPrintf(&ir->meta, "!%d = distinct !DISubprogram(name: \"%s\", linkageName: \"", sp, f->name);
        putEscaped(&ir->meta, link, linkLen);
        sbPrintf(&ir->meta, "\", scope: !%d, file: !%d, line: %d, type: !11, scopeLine: %d, "
                            "spFlags: DISPFlagDefinition%s, unit: !10)\n",
                 file, file, f->line, f->line, (f->flags & F_EXPORT) ? "" : " | DISPFlagLocalToUnit");
        ir->dbgScope = sp;
        ir->dbgLine = f->line;
        ir->dbgCol = f->col;
    }
    sbPrintf(out, "define %s%s %s(", (f->flags & F_EXPORT) ? "" : "internal ", llType(ir, f->type), f->ir);
    for (int i = 0; i < f->list.len; i++) {
        Node *p = f->list.items[i];
        sbPrintf(out, "%s%s %%p.%s", i ? ", " : "", llType(ir, p->type), p->name);
        p->ir = varSlot(ir, p->type, p, i + 1);
        sbPrintf(&ir->allocas, "  store %s %%p.%s, ptr %s\n", llType(ir, p->type), p->name, p->ir);
    }
    sbPuts(out, ir->mode == MODE_DEBUG ? ") #0" : ")");
    if (ir->dbgScope) {
        sbPrintf(out, " !dbg !%d", ir->dbgScope);
    }
    sbPuts(out, " {\n");

    ir->defers = NULL;
    if (f->d) {
        Val v = emitExpr(ir, f->d);
        if (!ir->terminated) {
            if (f->type->kind == TY_RESULT) {
                if (f->d->type->kind == TY_ERRVAL) {
                    returnError(ir, v.s);
                } else {
                    returnValue(ir, v);
                }
            } else if (f->type->kind == TY_VOID) {
                ins(ir, "ret void");
            } else {
                ins(ir, "ret %s %s", llType(ir, f->type), v.s);
            }
        }
    } else {
        emitBlock(ir, f->c);
        if (!ir->terminated) {
            if (f->type->kind == TY_VOID) {
                ins(ir, "ret void");
            } else if (f->type->kind == TY_RESULT && f->type->elem->kind == TY_VOID) {
                ins(ir, "ret i64 0");
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

/* A global's starting value, as an LLVM constant. */
static const char *constValue(IR *ir, Node *v, Type *t) {
    if (!v) {
        return "zeroinitializer";
    }
    if (v->kind == N_IDENT && v->ref && v->ref->kind == N_CONST && !v->isConst) {
        v = v->ref->b;
    }
    switch (t->kind) {
    case TY_SLICE: {
        const char *g = stringConst(ir, v->text, v->textLen);
        return fmt(ir, "{ ptr %s, i64 %zu }", g, v->textLen);
    }
    case TY_BOOL:
        return v->cval ? "true" : "false";
    case TY_INT:
        return intConst(ir, v->cval, t);
    case TY_FLOAT:
        return floatConst(ir, v->fval, t);
    case TY_PTR:
        return "null";
    case TY_STRUCT: {
        StrBuf b = {0};
        sbPuts(&b, "{ ");
        NodeList *fields = &t->decl->list;
        for (int i = 0; i < fields->len; i++) {
            Node *f = fields->items[i];
            Node *fv = f->kind == N_FIELD ? f->b : NULL;
            if (v->kind == N_STRUCT_LIT) {
                for (int j = 0; j < v->list.len; j++) {
                    if (v->list.items[j]->ref == f) {
                        fv = v->list.items[j]->a;
                    }
                }
            }
            sbPrintf(&b, "%s%s %s", i ? ", " : "", llType(ir, f->type), constValue(ir, fv, f->type));
        }
        sbPuts(&b, " }");
        char *s = sbFinish(&b, ir->arena);
        return s;
    }
    case TY_ARRAY: {
        if (v->kind != N_ARRAY_LIT) {
            return "zeroinitializer";
        }
        StrBuf b = {0};
        sbPuts(&b, "[ ");
        for (int i = 0; i < v->list.len; i++) {
            sbPrintf(&b, "%s%s %s", i ? ", " : "", llType(ir, t->elem), constValue(ir, v->list.items[i], t->elem));
        }
        sbPuts(&b, " ]");
        return sbFinish(&b, ir->arena);
    }
    default:
        return "zeroinitializer";
    }
}

char *writeIR(Arena *a, Program *prog, BuildMode mode) {
    IR ir = {0};
    ir.arena = a;
    ir.mode = mode;
    Node *firstFile = prog->main->files.items[0];
    ir.src = firstFile->src;
    ir.fileStr = cString(&ir, ir.src->path);
    ir.metaId = 12;                 /* !10 is the compile unit, !11 the function type */
    int mainFile = dbgFile(&ir, ir.src->path);

    StrBuf types = {0}, funcs = {0};
    Node *mainFn = NULL;
    sbPuts(&types, "%String = type { ptr, i64, i64 }\n");

    /* every module's declarations, in one list */
    Node fileNode = {0};
    Node *file = &fileNode;
    for (Module *m = prog->modules; m; m = m->next) {
        for (int f = 0; f < m->files.len; f++) {
            Node *fn = m->files.items[f];
            for (int i = 0; i < fn->list.len; i++) {
                if (fn->list.items[i]->kind != N_IMPORT) {
                    listPush(a, &file->list, fn->list.items[i]);
                }
            }
        }
    }
    file->list2 = prog->instances;

    for (int i = 0; i < file->list.len; i++) {
        Node *d = file->list.items[i];
        if (d->kind == N_ENUM && d->type && d->type->hasData) {
            Type *et = d->type;
            sbPrintf(&types, "%s = type { i32, [%lld x i64] }\n", et->ir, (long long)((et->payload + 7) / 8));
            for (int j = 0; j < d->list.len; j++) {
                Type *data = memberData(d->list.items[j]);
                if (!data) continue;
                sbPrintf(&types, "%s = type %s\n", data->ir, llType(&ir, data));
            }
        }
    }
    /* the file's declarations, then the instances of generics */
    NodeList all = {0};
    for (int i = 0; i < file->list.len; i++) {
        Node *d = file->list.items[i];
        if ((d->kind == N_FUNC && (d->list2.len || d->state == 4)) || (d->kind == N_STRUCT && d->list2.len)) {
            continue;                   /* generic: only its instances are compiled */
        }
        listPush(a, &all, d);
    }
    for (int i = 0; i < file->list2.len; i++) {
        listPush(a, &all, file->list2.items[i]);
    }
    for (int i = 0; i < all.len; i++) {
        Node *d = all.items[i];
        if (d->kind == N_STRUCT && d->type) {
            sbPrintf(&types, "%s = type { ", d->type->ir);
            for (int j = 0; j < d->list.len; j++) {
                sbPrintf(&types, "%s%s", j ? ", " : "", llType(&ir, d->list.items[j]->type));
            }
            sbPuts(&types, d->list.len ? " }\n" : "}\n");
        }
    }
    NodeList externs = {0};         /* C functions declared, by name */
    for (int i = 0; i < all.len; i++) {
        Node *d = all.items[i];
        const char *mod = d->module ? d->module : "main";
        if (d->kind == N_FUNC) {
            if (d->flags & F_EXTERN) {
                d->ir = fmt(&ir, "@%s", d->text);
                bool dup = false;
                for (int k = 0; k < externs.len; k++) {
                    dup = dup || strcmp(externs.items[k]->name, d->text) == 0;
                }
                if (dup) {
                    continue;           /* another module declared it already */
                }
                Node *rec = newNode(a, N_IDENT, 0, 0);
                rec->name = d->text;
                listPush(a, &externs, rec);
                StrBuf decl = {0};
                sbPrintf(&decl, "declare %s %s(", llType(&ir, d->type), d->ir);
                for (int j = 0; j < d->list.len; j++) {
                    sbPrintf(&decl, "%s%s", j ? ", " : "", llType(&ir, d->list.items[j]->type));
                }
                if (d->flags & F_VARIADIC) {
                    sbPuts(&decl, d->list.len ? ", ..." : "...");
                }
                sbPuts(&decl, ")\n");
                sbPuts(&ir.head, decl.buf);
                sbFree(&decl);
            } else if (d->flags & F_EXPORT) {
                d->ir = fmt(&ir, "@%s", d->text);
                if (strcmp(d->name, "main") == 0 && strcmp(mod, "main") == 0) {
                    mainFn = d;
                }
            } else if (d->list.len && (d->list.items[0]->flags & F_SELF)) {
                d->ir = fmt(&ir, "@\"m.%s.%s.%s\"", mod, d->list.items[0]->type->elem->name, d->name);
            } else {
                d->ir = fmt(&ir, "@\"f.%s.%s\"", mod, d->name);
                if (strcmp(d->name, "main") == 0 && strcmp(mod, "main") == 0) {
                    mainFn = d;
                }
            }
        } else if (d->kind == N_VAR) {
            d->ir = fmt(&ir, "@\"g.%s.%s\"", mod, d->name);
            sbPrintf(&ir.head, "%s = internal %sglobal %s %s\n", d->ir,
                     d->flags & F_THREADLOCAL ? "thread_local " : "", llType(&ir, d->type),
                     constValue(&ir, d->b, d->type));
        } else if (d->kind == N_DEFINE) {
            Node *id = d->list.items[0];
            id->ir = fmt(&ir, "@\"g.%s.%s\"", mod, id->name);
            sbPrintf(&ir.head, "%s = internal global %s %s\n", id->ir, llType(&ir, id->type),
                     constValue(&ir, d->b, id->type));
        }
    }
    for (int i = 0; i < all.len; i++) {
        Node *d = all.items[i];
        if (d->kind == N_FUNC && !(d->flags & (F_EXTERN | F_INTRINSIC))) {
            emitFunction(&ir, d, &funcs);
        }
    }

    /* the names of every enum's values, for printing them and errors */
    int nenums = 0;
    for (int i = 0; i < file->list.len; i++) {
        if (file->list.items[i]->kind == N_ENUM && file->list.items[i]->type) {
            nenums++;
        }
    }
    StrBuf table = {0};
    for (int k = 1; k <= nenums; k++) {
        for (int i = 0; i < file->list.len; i++) {
            Node *d = file->list.items[i];
            if (d->kind != N_ENUM || !d->type || d->type->count != k) {
                continue;
            }
            int n = d->list.len;
            sbPrintf(&ir.head, "@e%d.values = private constant [%d x i64] [", k, n);
            for (int j = 0; j < n; j++) {
                sbPrintf(&ir.head, "%si64 %lld", j ? ", " : "", (long long)d->list.items[j]->cval);
            }
            sbPuts(&ir.head, "]\n");
            const char **names = arenaAlloc(a, sizeof(char *) * (size_t)(n ? n : 1));
            for (int j = 0; j < n; j++) {
                names[j] = cString(&ir, d->list.items[j]->name);
            }
            sbPrintf(&ir.head, "@e%d.names = private constant [%d x ptr] [", k, n);
            for (int j = 0; j < n; j++) {
                sbPrintf(&ir.head, "%sptr %s", j ? ", " : "", names[j]);
            }
            sbPuts(&ir.head, "]\n");
            sbPrintf(&table, "%s{ ptr, i64, ptr, ptr } { ptr %s, i64 %d, ptr @e%d.values, ptr @e%d.names }",
                     k > 1 ? ", " : "", cString(&ir, d->name), n, k, k);
        }
    }
    sbPrintf(&ir.head, "@iris_enum_table = constant [%d x { ptr, i64, ptr, ptr }] [%s]\n", nenums,
             table.buf ? table.buf : "");
    sbPrintf(&ir.head, "@iris_enum_count = constant i64 %d\n", nenums);
    sbFree(&table);

    StrBuf out = {0};
    sbPrintf(&out, "; generated by iris %s from %s\n\n", IRIS_VERSION, ir.src->path);
    if (ir.extra.buf) {
        sbPuts(&funcs, ir.extra.buf);
        sbFree(&ir.extra);
    }
    const char **testNames = arenaAlloc(a, sizeof(char *) * (size_t)(prog->tests.len + 1));
    for (int i = 0; gTesting && i < prog->tests.len; i++) {
        Node *t = prog->tests.items[i];
        testNames[i] = stringConst(&ir, t->text, strlen(t->text));
    }
    sbPuts(&out, types.buf);
    if (ir.head.buf) {
        sbPuts(&out, ir.head.buf);
    }
    sbPuts(&out, "\n");
    if (funcs.buf) {
        sbPuts(&out, funcs.buf);
    }
    sbPrintf(&out, "define i32 @main(i32 %%argc, ptr %%argv)%s {\n", mode == MODE_DEBUG ? " #0" : "");
    sbPuts(&out, "  call void @iris_init(i32 %argc, ptr %argv)\n");
    if (gTesting) {
        /* iris test: each test that the filter lets through, then the summary */
        for (int i = 0; i < prog->tests.len; i++) {
            Node *t = prog->tests.items[i];
            const char *name = testNames[i];
            sbPrintf(&out, "  %%go%d = call i1 @iris_test_start(ptr %s)\n  br i1 %%go%d, label %%run%d, label %%next%d\n"
                           "run%d:\n  %%e%d = call i64 %s()\n  call void @iris_test_done(ptr %s, i64 %%e%d)\n"
                           "  br label %%next%d\nnext%d:\n",
                     i, name, i, i, i, i, i, t->ir, name, i, i, i);
        }
        sbPuts(&out, "  %rc = call i32 @iris_test_summary()\n  ret i32 %rc\n}\n\n");
    } else if (mainFn && mainFn->type->kind == TY_INT) {
        sbPrintf(&out, "  %%r = call i32 %s()\n  ret i32 %%r\n}\n\n", mainFn->ir);
    } else if (mainFn && mainFn->type->kind == TY_RESULT) {
        /* an error that gets out of main is printed with its trace */
        sbPrintf(&out, "  %%e = call i64 %s()\n  %%bad = icmp ne i64 %%e, 0\n"
                       "  br i1 %%bad, label %%fail, label %%ok\nfail:\n"
                       "  call void @iris_error_report(i64 %%e)\n  unreachable\nok:\n  ret i32 0\n}\n\n",
                 mainFn->ir);
    } else {
        sbPrintf(&out, "  call void %s()\n  ret i32 0\n}\n\n", mainFn ? mainFn->ir : "@\"f.main.main\"");
    }
    /* libc functions the generated code uses, unless import c declared them */
    static const struct { const char *name, *decl; } libc[] = {
        {"free", "declare void @free(ptr)\n"},
        {"strlen", "declare i64 @strlen(ptr)\n"},
    };
    for (size_t k = 0; k < sizeof libc / sizeof libc[0]; k++) {
        bool dup = false;
        for (int j = 0; j < externs.len; j++) {
            dup = dup || strcmp(externs.items[j]->name, libc[k].name) == 0;
        }
        if (!dup) {
            sbPuts(&out, libc[k].decl);
        }
    }
    sbPuts(&out,
           "declare void @iris_init(i32, ptr)\n"
           "declare void @iris_write_str(i32, ptr, ptr, i64)\n"
           "declare void @iris_write_i64(i32, ptr, i64)\n"
           "declare void @iris_write_u64(i32, ptr, i64)\n"
           "declare void @iris_write_duration(i32, ptr, i64)\n"
           "declare void @iris_write_bool(i32, ptr, i32)\n"
           "declare void @iris_print_end()\n"
           "declare i32 @iris_str_eq(ptr, i64, ptr, i64)\n"
           "declare ptr @iris_alloc(i64, i64, ptr, i32, i32, ptr)\n"
           "declare void @iris_string_append(ptr, ptr, i64)\n"
           "declare void @iris_string_free(ptr)\n"
           "declare void @iris_panic(ptr, i32, i32, ptr) noreturn\n"
           "declare void @iris_check_failed(ptr, ptr, i32, i32, ptr) noreturn\n"
           "declare void @iris_index_failed(i64, i64, ptr, i32, i32, ptr) noreturn\n"
           "declare void @iris_slice_failed(i64, i64, i64, ptr, i32, i32, ptr) noreturn\n"
           "declare void @iris_write_enum(i32, ptr, i64, i32)\n"
           "declare void @iris_write_errcode(i32, ptr, i64)\n"
           "declare void @iris_error_begin()\n"
           "declare i64 @iris_error_finish(i64, ptr, i32, i32, ptr)\n"
           "declare void @iris_trace_add(ptr, i32, i32, ptr)\n"
           "declare ptr @iris_error_message_ptr()\n"
           "declare i64 @iris_error_message_len()\n"
           "declare void @iris_error_report(i64) noreturn\n"
           "declare ptr @iris_enum_name(i64, i32)\n"
           "declare void @iris_write_flags(i32, ptr, i64, i32)\n"
           "declare void @iris_write_float(i32, ptr, double, i32)\n"
           "declare void @iris_spawn(ptr, ptr)\n"
           "declare void @llvm.dbg.declare(metadata, metadata, metadata)\n"
           "declare ptr @iris_thread_new(ptr, ptr)\n"
           "declare void @iris_thread_join(ptr)\n"
           "declare void @iris_thread_detach(ptr)\n"
           "declare void @iris_json_encode(ptr, ptr, ptr)\n"
           "declare void @iris_json_free(ptr, ptr)\n"
           "declare void @iris_expect_failed(ptr, i32, i32, ptr)\n"
           "declare i64 @iris_hash_u64(i64)\n"
           "declare i64 @iris_hash_bytes(ptr, i64)\n"
           "declare ptr @iris_bytes_dup(ptr, i64)\n"
           "declare i1 @iris_test_start(ptr)\n"
           "declare void @iris_test_done(ptr, i64)\n"
           "declare i32 @iris_test_summary()\n"
           "declare void @iris_mutex_lock(ptr)\n"
           "declare zeroext i1 @iris_mutex_try_lock(ptr)\n"
           "declare void @iris_mutex_unlock(ptr, ptr, i32, i32, ptr)\n"
           "declare void @iris_sleep(i64)\n"
           "declare ptr @iris_chan_new(i64, i64, ptr, i32, i32, ptr)\n"
           "declare void @iris_chan_send(ptr, ptr)\n"
           "declare void @iris_chan_recv(ptr, ptr)\n"
           "declare i64 @iris_select(i64, ptr, ptr, i64)\n"
           ""
           "\n!0 = !{!\"branch_weights\", i32 1, i32 1048575}\n");
    sbPrintf(&out, "!llvm.dbg.cu = !{!10}\n!llvm.module.flags = !{!1, !2}\n"
                   "!1 = !{i32 2, !\"Debug Info Version\", i32 3}\n!2 = !{i32 7, !\"Dwarf Version\", i32 4}\n"
                   "!10 = distinct !DICompileUnit(language: DW_LANG_C, file: !%d, producer: \"iris %s\", "
                   "isOptimized: %s, runtimeVersion: 0, emissionKind: FullDebug)\n!11 = !DISubroutineType(types: !{})\n",
             mainFile, IRIS_VERSION, mode == MODE_DEBUG ? "false" : "true");
    if (ir.meta.buf) {
        sbPuts(&out, ir.meta.buf);
    }
    sbFree(&ir.meta);
    if (mode == MODE_DEBUG) {
        sbPuts(&out, "attributes #0 = { sanitize_address }\n");
    }
    sbFree(&types);
    sbFree(&funcs);
    sbFree(&ir.head);
    return sbFinish(&out, a);
}
