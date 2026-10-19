/* The parser: tokens to a syntax tree. It checks the shape of the code
 * only; types are the checker's job.
 *
 * On an error the parser reports it, skips to the end of the statement and
 * goes on, so one run shows every syntax error. */
#include "iris.h"

#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Arena *arena;
    Source *src;
    Token *toks;
    int ntoks;
    int pos;
    bool noStructLit;       /* in the head of if, for, match, when */
    bool snippet;
    int depth;              /* nesting, to stop runaway recursion */
    jmp_buf *recover;
} Parser;

#define MAX_DEPTH 500

static Node *parseExpr(Parser *p);
static Node *parseBlock(Parser *p);
static Node *parseStmt(Parser *p);
static Node *parseSimpleStmt(Parser *p, bool allowList);
static Node *parseGuard(Parser *p, Node *s);
static Node *parseIf(Parser *p);
static Node *parseMatch(Parser *p);
static Node *tryType(Parser *p);
static Node *parseType(Parser *p);
static Node *parseTopLevel(Parser *p);

/* ---------------------------------------------------------------- tokens */

static Token *peek(Parser *p) { return &p->toks[p->pos]; }

static Token *peekAt(Parser *p, int k) {
    int i = p->pos + k;
    return &p->toks[i < p->ntoks ? i : p->ntoks - 1];
}

static bool at(Parser *p, TokKind k) { return peek(p)->kind == k; }

static Token *next(Parser *p) {
    Token *t = peek(p);
    if (t->kind != T_EOF) {
        p->pos++;
    }
    return t;
}

static bool accept(Parser *p, TokKind k) {
    if (at(p, k)) {
        next(p);
        return true;
    }
    return false;
}

static void skipNewlines(Parser *p) {
    while (at(p, T_NEWLINE)) {
        next(p);
    }
}

static bool isWord(Token *t, const char *w) {
    return t->kind == T_IDENT && (int)strlen(w) == t->len && memcmp(t->s, w, (size_t)t->len) == 0;
}

/* How a token is named in an error message. */
static const char *describe(Parser *p, Token *t) {
    static char buf[96];
    switch (t->kind) {
    case T_EOF: case T_NEWLINE:
        return tokName(t->kind);
    case T_IDENT: case T_INT: case T_FLOAT:
        snprintf(buf, sizeof buf, "'%.*s'", t->len > 40 ? 40 : t->len, t->s);
        return buf;
    case T_STRING: case T_STRING3: case T_RSTRING:
        return "a string";
    case T_CHAR:
        return "a character";
    case T_ASM_BODY:
        return "assembly";
    default:
        snprintf(buf, sizeof buf, "'%s'", tokName(t->kind));
        return buf;
    }
    (void)p;
}

static void fail(Parser *p, Token *t, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    verrorAt(p->src, t->line, t->col, fmt, ap);
    va_end(ap);
    longjmp(*p->recover, 1);
}

static Token *expect(Parser *p, TokKind k) {
    if (!at(p, k)) {
        fail(p, peek(p), "expected '%s', found %s", tokName(k), describe(p, peek(p)));
    }
    return next(p);
}

static Token *expectIdent(Parser *p, const char *what) {
    if (!at(p, T_IDENT)) {
        fail(p, peek(p), "expected %s, found %s", what, describe(p, peek(p)));
    }
    return next(p);
}

static char *tokText(Parser *p, Token *t) { return arenaStrndup(p->arena, t->s, (size_t)t->len); }

static Node *node(Parser *p, NodeKind k, Token *t) {
    Node *n = newNode(p->arena, k, t->line, t->col);
    n->src = p->src;
    return n;
}

static void push(Parser *p, NodeList *l, Node *n) { listPush(p->arena, l, n); }

/* After an error: skip to the end of the statement. */
static void sync(Parser *p) {
    int depth = 0;
    while (!at(p, T_EOF)) {
        TokKind k = peek(p)->kind;
        if (k == P_LBRACE) {
            depth++;
        } else if (k == P_RBRACE) {
            if (depth == 0) {
                return;
            }
            depth--;
        } else if (k == T_NEWLINE && depth == 0) {
            next(p);
            return;
        }
        next(p);
    }
}

/* The end of a statement or declaration: a newline, or a closing brace
 * that the caller handles. */
static void endStatement(Parser *p, bool braceOk) {
    if (at(p, T_NEWLINE)) {
        next(p);
        return;
    }
    if (at(p, T_EOF) || (braceOk && at(p, P_RBRACE))) {
        return;
    }
    /* a type that ends in * at the end of a line: type Handle = Node * */
    if (p->pos > 0 && p->toks[p->pos - 1].kind == P_STAR && peek(p)->line > p->toks[p->pos - 1].line) {
        return;
    }
    fail(p, peek(p), "expected the end of the line, found %s", describe(p, peek(p)));
}

/* ---------------------------------------------------------------- literals */

static const char *units[] = {"ns", "us", "ms", "s", "m", "h", "KiB", "MiB", "GiB"};

static Node *parseNumber(Parser *p, Token *t) {
    Node *n = node(p, t->kind == T_FLOAT ? N_FLOAT : N_INT, t);
    const char *s = t->s, *e = t->s + t->len;
    int base = 10;
    if (t->len > 2 && s[0] == '0' && strchr("xXbBoO", s[1])) {
        base = (s[1] == 'x' || s[1] == 'X') ? 16 : (s[1] == 'b' || s[1] == 'B') ? 2 : 8;
        s += 2;
    }
    char digits[128];
    size_t nd = 0;
    const char *q = s;
    for (; q < e; q++) {
        char c = *q;
        if (c == '_') {
            continue;
        }
        bool ok = (c >= '0' && c <= '9') || c == '.' ||
                  (base == 16 && ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) ||
                  (base == 10 && n->kind == N_FLOAT && (c == 'e' || c == 'E' ||
                                                        ((c == '+' || c == '-') && q > s && (q[-1] == 'e' || q[-1] == 'E'))));
        if (!ok) {
            break;
        }
        if (nd + 1 < sizeof digits) {
            digits[nd++] = c;
        }
    }
    digits[nd] = 0;
    if (q < e) {
        if (base != 10) {
            fail(p, t, "'%.*s' is not a valid number", t->len, t->s);
        }
        size_t ulen = (size_t)(e - q);
        bool known = false;
        for (size_t i = 0; i < sizeof units / sizeof units[0]; i++) {
            if (strlen(units[i]) == ulen && memcmp(units[i], q, ulen) == 0) {
                known = true;
            }
        }
        if (!known) {
            fail(p, t, "unknown unit '%.*s' after a number (units are ns, us, ms, s, m, h, KiB, MiB, GiB)",
                 (int)ulen, q);
        }
        n->name = arenaStrndup(p->arena, q, ulen);
    }
    if (nd == 0) {
        fail(p, t, "'%.*s' is not a valid number", t->len, t->s);
    }
    errno = 0;
    if (n->kind == N_FLOAT) {
        n->fval = strtod(digits, NULL);
    } else {
        char *end;
        n->ival = strtoull(digits, &end, base);
        if (*end) {
            fail(p, t, "'%.*s' is not a valid number", t->len, t->s);
        }
    }
    if (errno == ERANGE) {
        fail(p, t, "'%.*s' is too large", t->len, t->s);
    }
    n->text = tokText(p, t);
    return n;
}

static int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decodes the escape at s (just after the backslash). Returns how many
 * characters it used, or 0 if it is not valid. */
static int decodeEscape(const char *s, const char *e, char *out) {
    if (s >= e) {
        return 0;
    }
    switch (*s) {
    case 'n': *out = '\n'; return 1;
    case 't': *out = '\t'; return 1;
    case 'r': *out = '\r'; return 1;
    case '0': *out = '\0'; return 1;
    case '\\': *out = '\\'; return 1;
    case '"': *out = '"'; return 1;
    case '\'': *out = '\''; return 1;
    case 'x':
        if (e - s >= 3 && hexValue(s[1]) >= 0 && hexValue(s[2]) >= 0) {
            *out = (char)(hexValue(s[1]) * 16 + hexValue(s[2]));
            return 3;
        }
        return 0;
    default:
        return 0;
    }
}

static Node *parseChar(Parser *p, Token *t) {
    Node *n = node(p, N_CHAR, t);
    const char *s = t->s + 1, *e = t->s + t->len - 1;
    if (e <= s) {
        fail(p, t, "empty character literal");
    }
    char c;
    if (*s == '\\') {
        int used = decodeEscape(s + 1, e, &c);
        if (!used || s + 1 + used != e) {
            fail(p, t, "unknown escape in character literal");
        }
    } else {
        if (e - s != 1) {
            fail(p, t, "a character literal holds one byte; use a string for more");
        }
        c = *s;
    }
    n->ival = (unsigned char)c;
    return n;
}

static Node *parseExprIn(Parser *p, const char *start, const char *end, int line, int col);

/* A string literal: plain, triple-quoted or raw. Text in braces is an
 * expression; the result is N_STRING without any, N_INTERP with. */
static bool rawTriple(Token *t) {
    return t->kind == T_RSTRING && t->len >= 4 && !memcmp(t->s, "r\"\"\"", 4);
}

static bool closedString(Token *t) {
    int q = t->kind == T_STRING3 || rawTriple(t) ? 3 : 1;
    int open = t->kind == T_RSTRING ? q + 1 : q;
    if (t->len < open + q) {
        return false;
    }
    for (int i = 1; i <= q; i++) {
        if (t->s[t->len - i] != '"') {
            return false;
        }
    }
    return true;
}

static Node *parseString(Parser *p, Token *t) {
    if (!closedString(t)) {
        /* the lexer has reported it */
        Node *n = node(p, N_STRING, t);
        n->text = "";
        return n;
    }
    if (t->kind == T_RSTRING && !rawTriple(t)) {
        Node *n = node(p, N_STRING, t);
        n->text = arenaStrndup(p->arena, t->s + 2, (size_t)t->len - 3);
        n->textLen = (size_t)t->len - 3;
        return n;
    }
    bool raw = t->kind == T_RSTRING;     /* r"""...""" */
    bool triple = t->kind == T_STRING3 || raw;
    int q = triple ? 3 : 1;
    const char *s = t->s + q + (raw ? 1 : 0), *e = t->s + t->len - q;
    int line = t->line, col = t->col + q + (raw ? 1 : 0);

    /* triple-quoted: drop the first line break and a last line of only
     * spaces, and the indentation all lines share */
    int indent = 0;
    if (triple) {
        if (s < e && *s == '\n') {
            s++;
            line++;
            col = 1;
        } else if (s + 1 < e && s[0] == '\r' && s[1] == '\n') {
            s += 2;
            line++;
            col = 1;
        }
        const char *lastNl = NULL;
        for (const char *c = s; c < e; c++) {
            if (*c == '\n') {
                lastNl = c;
            }
        }
        if (lastNl) {
            bool blank = true;
            for (const char *c = lastNl + 1; c < e; c++) {
                if (*c != ' ' && *c != '\t') {
                    blank = false;
                }
            }
            if (blank) {
                e = lastNl;
                if (e > s && e[-1] == '\r') {
                    e--;
                }
            }
        }
        indent = 1 << 30;
        const char *ls = s;
        while (ls <= e) {
            const char *le = ls;
            while (le < e && *le != '\n') {
                le++;
            }
            int sp = 0;
            const char *c = ls;
            while (c < le && (*c == ' ' || *c == '\t')) {
                c++;
                sp++;
            }
            if (c < le && *c != '\r' && sp < indent) {
                indent = sp;
            }
            ls = le + 1;
        }
        if (indent == 1 << 30) {
            indent = 0;
        }
    }

    Node *interp = NULL;
    StrBuf lit = {0};
    bool lineStart = triple;
    int litLine = line, litCol = col;

    for (const char *c = s; c < e;) {
        if (lineStart) {
            for (int i = 0; i < indent && c < e && (*c == ' ' || *c == '\t'); i++) {
                c++;
                col++;
            }
            lineStart = false;
            continue;
        }
        if (*c == '\n') {
            sbPutc(&lit, '\n');
            c++;
            line++;
            col = 1;
            lineStart = triple;
            continue;
        }
        if (*c == '\\' && !raw) {
            char out;
            int used = decodeEscape(c + 1, e, &out);
            if (!used) {
                sbFree(&lit);
                errorAt(p->src, line, col, "unknown escape '\\%c' in a string", c + 1 < e ? c[1] : ' ');
                longjmp(*p->recover, 1);
            }
            sbPutc(&lit, out);
            c += 1 + used;
            col += 1 + used;
            continue;
        }
        if ((*c == '{' || *c == '}') && raw) {
            sbPutc(&lit, *c);
            c++;
            col++;
            continue;
        }
        if ((*c == '{' || *c == '}') && c + 1 < e && c[1] == *c) {
            sbPutc(&lit, *c);
            c += 2;
            col += 2;
            continue;
        }
        if (*c == '}') {
            sbFree(&lit);
            errorAt(p->src, line, col, "a '}' in a string must be written '}}'");
            longjmp(*p->recover, 1);
        }
        if (*c == '{') {
            /* find the matching brace, skipping nested strings */
            const char *x = c + 1;
            int depth = 0, xl = line, xc = col + 1;
            int exprLine = xl, exprCol = xc;
            while (x < e && !(*x == '}' && depth == 0)) {
                if (*x == '{') {
                    depth++;
                } else if (*x == '}') {
                    depth--;
                } else if (*x == '"' || *x == '\'') {
                    char qc = *x;
                    x++;
                    xc++;
                    while (x < e && *x != qc) {
                        if (*x == '\\') {
                            x++;
                            xc++;
                        }
                        x++;
                        xc++;
                    }
                }
                if (*x == '\n') {
                    xl++;
                    xc = 0;
                }
                x++;
                xc++;
            }
            if (x >= e) {
                sbFree(&lit);
                errorAt(p->src, line, col, "this '{' in a string has no closing '}'");
                longjmp(*p->recover, 1);
            }
            const char *es = c + 1;
            bool empty = true;
            for (const char *k = es; k < x; k++) {
                if (*k != ' ' && *k != '\t') {
                    empty = false;
                }
            }
            if (empty) {
                sbFree(&lit);
                errorAt(p->src, line, col, "empty {} in a string; write {{}} for braces");
                longjmp(*p->recover, 1);
            }
            if (!interp) {
                interp = node(p, N_INTERP, t);
            }
            if (lit.len) {
                Node *part = newNode(p->arena, N_STRING, litLine, litCol);
                part->src = p->src;
                part->textLen = lit.len;
                part->text = sbFinish(&lit, p->arena);
                push(p, &interp->list, part);
            }
            push(p, &interp->list, parseExprIn(p, es, x, exprLine, exprCol));
            line = xl;
            col = xc + 1;
            c = x + 1;
            litLine = line;
            litCol = col;
            continue;
        }
        sbPutc(&lit, *c);
        c++;
        col++;
    }

    if (!interp) {
        Node *n = node(p, N_STRING, t);
        n->textLen = lit.len;
        n->text = sbFinish(&lit, p->arena);
        return n;
    }
    if (lit.len) {
        Node *part = newNode(p->arena, N_STRING, litLine, litCol);
        part->src = p->src;
        part->textLen = lit.len;
        part->text = sbFinish(&lit, p->arena);
        push(p, &interp->list, part);
    } else {
        sbFree(&lit);
    }
    return interp;
}

/* Parses an expression from part of the source (the inside of {} in a
 * string). Errors go to the same recovery point as the enclosing statement. */
static Node *parseExprIn(Parser *p, const char *start, const char *end, int line, int col) {
    Parser sub = *p;
    sub.toks = lexRange(p->arena, p->src, start, end, line, col, &sub.ntoks);
    sub.pos = 0;
    sub.noStructLit = false;
    Node *e = parseExpr(&sub);
    skipNewlines(&sub);
    if (!at(&sub, T_EOF)) {
        fail(&sub, peek(&sub), "unexpected %s inside {} in a string", describe(&sub, peek(&sub)));
    }
    return e;
}

/* ---------------------------------------------------------------- types */

/* Tries to read a type. Returns NULL and leaves the position unchanged if
 * the tokens are not a type. Never reports errors. */
static Node *tryType(Parser *p) {
    int save = p->pos;
    Token *t = peek(p);
    Node *ty = NULL;

    if (t->kind == P_BANG) {
        next(p);
        Node *inner = tryType(p);
        if (!inner) {
            goto fail;
        }
        ty = node(p, N_T_ERR, t);
        ty->a = inner;
        return ty;
    }
    if (t->kind == K_DYN) {
        next(p);
        Node *inner = tryType(p);
        if (!inner) {
            goto fail;
        }
        ty = node(p, N_T_DYN, t);
        ty->a = inner;
        return ty;
    }
    if (t->kind == K_CHAN) {
        next(p);
        if (!accept(p, P_LBRACK)) {
            goto fail;
        }
        Node *inner = tryType(p);
        if (!inner || !accept(p, P_RBRACK)) {
            goto fail;
        }
        ty = node(p, N_T_CHAN, t);
        ty->a = inner;
    } else if (t->kind == P_LPAREN) {
        next(p);
        ty = node(p, N_T_TUPLE, t);
        do {
            Node *e = tryType(p);
            if (!e) {
                goto fail;
            }
            push(p, &ty->list, e);
        } while (accept(p, P_COMMA));
        if (!accept(p, P_RPAREN) || ty->list.len < 2) {
            goto fail;
        }
    } else if (t->kind == T_IDENT) {
        next(p);
        ty = node(p, N_T_NAME, t);
        if (at(p, P_DOT) && peekAt(p, 1)->kind == T_IDENT) {
            next(p);
            Token *member = next(p);
            ty->name = arenaStrndup(p->arena, t->s, (size_t)(member->s + member->len - t->s));
        } else {
            ty->name = tokText(p, t);
        }
    } else {
        goto fail;
    }

    for (;;) {
        Token *s = peek(p);
        if (s->kind == P_STAR) {
            next(p);
            Node *ptr = node(p, N_T_PTR, s);
            ptr->a = ty;
            ty = ptr;
            continue;
        }
        if (s->kind == P_LBRACK) {
            if (peekAt(p, 1)->kind == P_RBRACK) {
                next(p);
                next(p);
                Node *sl = node(p, N_T_SLICE, s);
                sl->a = ty;
                ty = sl;
                continue;
            }
            if (peekAt(p, 1)->kind == T_INT && peekAt(p, 2)->kind == P_RBRACK) {
                next(p);
                Node *arr = node(p, N_T_ARRAY, s);
                arr->a = ty;
                arr->b = parseNumber(p, next(p));
                next(p);
                ty = arr;
                continue;
            }
            if (ty->kind != N_T_NAME) {
                if (peekAt(p, 1)->kind == T_IDENT && peekAt(p, 2)->kind == P_RBRACK) {
                    /* u8[4][ROWS]: a size from a constant, which the checker finds */
                    next(p);
                    Node *g = node(p, N_T_GENERIC, s);
                    g->a = ty;
                    Token *k = next(p);
                    Node *name = node(p, N_T_NAME, k);
                    name->name = tokText(p, k);
                    push(p, &g->list, name);
                    next(p);
                    ty = g;
                    continue;
                }
                break;
            }
            int s2 = p->pos;
            next(p);
            Node *g = node(p, N_T_GENERIC, s);
            g->a = ty;
            bool ok = true;
            do {
                Node *arg = tryType(p);
                if (!arg) {
                    ok = false;
                    break;
                }
                push(p, &g->list, arg);
            } while (accept(p, P_COMMA));
            if (!ok || !accept(p, P_RBRACK)) {
                p->pos = s2;
                break;
            }
            ty = g;
            continue;
        }
        if (s->kind == P_LPAREN) {
            int s2 = p->pos;
            next(p);
            Node *fn = node(p, N_T_FUNC, s);
            fn->a = ty;
            bool ok = true;
            if (!at(p, P_RPAREN)) {
                do {
                    Node *arg = tryType(p);
                    if (!arg) {
                        ok = false;
                        break;
                    }
                    push(p, &fn->list, arg);
                } while (accept(p, P_COMMA));
            }
            if (!ok || !accept(p, P_RPAREN)) {
                p->pos = s2;
                break;
            }
            ty = fn;
            continue;
        }
        break;
    }
    return ty;

fail:
    p->pos = save;
    return NULL;
}

static Node *parseType(Parser *p) {
    Node *t = tryType(p);
    if (!t) {
        fail(p, peek(p), "expected a type, found %s", describe(p, peek(p)));
    }
    return t;
}

/* ---------------------------------------------------------------- expressions */

static bool typeLike(Node *e) {
    return e->kind == N_IDENT || e->kind == N_FIELD_ACCESS || e->kind == N_INDEX;
}

static Node *parseStructLit(Parser *p, Node *type, Token *at_) {
    Node *lit = node(p, N_STRUCT_LIT, at_);
    lit->a = type;
    bool saved = p->noStructLit;
    p->noStructLit = false;
    expect(p, P_LBRACE);
    skipNewlines(p);
    while (!at(p, P_RBRACE)) {
        Token *name = expectIdent(p, "a field name");
        Node *f = node(p, N_FIELD_INIT, name);
        f->name = tokText(p, name);
        expect(p, P_COLON);
        f->a = parseExpr(p);
        push(p, &lit->list, f);
        skipNewlines(p);
        if (!accept(p, P_COMMA)) {
            break;
        }
        skipNewlines(p);
    }
    skipNewlines(p);
    expect(p, P_RBRACE);
    p->noStructLit = saved;
    return lit;
}

static Node *parseCallArgs(Parser *p, Node *callee, Token *t) {
    Node *call = node(p, N_CALL, t);
    call->a = callee;
    bool saved = p->noStructLit;
    p->noStructLit = false;
    expect(p, P_LPAREN);
    while (!at(p, P_RPAREN)) {
        if (at(p, T_IDENT) && peekAt(p, 1)->kind == P_COLON) {
            Token *name = next(p);
            next(p);
            Node *arg = node(p, N_NAMED_ARG, name);
            arg->name = tokText(p, name);
            arg->a = parseExpr(p);
            push(p, &call->list, arg);
        } else if (at(p, T_IDENT) && peekAt(p, 1)->kind == P_LBRACK && peekAt(p, 2)->kind == P_RBRACK) {
            /* a slice type given to a call that takes a type: json.decode(User[], text) */
            push(p, &call->list, parseType(p));
        } else {
            push(p, &call->list, parseExpr(p));
        }
        if (!accept(p, P_COMMA)) {
            break;
        }
    }
    expect(p, P_RPAREN);
    p->noStructLit = saved;
    return call;
}

static Node *parseUnary(Parser *p);

static Node *parsePrimary(Parser *p) {
    Token *t = peek(p);
    switch (t->kind) {
    case T_IDENT: {
        next(p);
        Node *n = node(p, N_IDENT, t);
        n->name = tokText(p, t);
        return n;
    }
    case T_INT: case T_FLOAT:
        return parseNumber(p, next(p));
    case T_STRING: case T_STRING3: case T_RSTRING:
        return parseString(p, next(p));
    case T_CHAR:
        return parseChar(p, next(p));
    case K_TRUE: case K_FALSE: {
        next(p);
        Node *n = node(p, N_BOOL, t);
        n->ival = t->kind == K_TRUE;
        return n;
    }
    case K_NULL:
        next(p);
        return node(p, N_NULL, t);
    case P_LPAREN: {
        next(p);
        bool saved = p->noStructLit;
        p->noStructLit = false;
        Node *e = parseExpr(p);
        if (at(p, P_COMMA)) {
            Node *tup = node(p, N_TUPLE, t);
            push(p, &tup->list, e);
            while (accept(p, P_COMMA)) {
                push(p, &tup->list, parseExpr(p));
            }
            e = tup;
        }
        expect(p, P_RPAREN);
        p->noStructLit = saved;
        return e;
    }
    case P_LBRACK: {
        next(p);
        Node *arr = node(p, N_ARRAY_LIT, t);
        bool saved = p->noStructLit;
        p->noStructLit = false;
        while (!at(p, P_RBRACK)) {
            push(p, &arr->list, parseExpr(p));
            if (!accept(p, P_COMMA)) {
                break;
            }
        }
        expect(p, P_RBRACK);
        p->noStructLit = saved;
        return arr;
    }
    case P_DOT: {
        next(p);
        if (at(p, P_LBRACE)) {
            return parseStructLit(p, NULL, t);
        }
        Token *name = expectIdent(p, "a name after '.'");
        Node *n = node(p, N_ENUM_DOT, t);
        n->name = tokText(p, name);
        return n;
    }
    case K_IF:
        return parseIf(p);
    case K_MATCH:
        return parseMatch(p);
    case K_THREAD: {
        next(p);
        Node *n = node(p, N_THREAD, t);
        n->a = parseUnary(p);
        return n;
    }
    case K_CHAN: {
        next(p);
        Node *n = node(p, N_CHAN_MAKE, t);
        expect(p, P_LBRACK);
        n->a = parseType(p);
        expect(p, P_RBRACK);
        expect(p, P_LPAREN);
        n->b = parseExpr(p);
        expect(p, P_RPAREN);
        return n;
    }
    default:
        fail(p, t, "expected an expression, found %s", describe(p, t));
        return NULL;
    }
}

static Node *parsePostfix(Parser *p) {
    Node *e = parsePrimary(p);
    for (;;) {
        Token *t = peek(p);
        if (t->kind == P_LPAREN) {
            e = parseCallArgs(p, e, t);
        } else if (t->kind == P_LBRACK) {
            next(p);
            bool saved = p->noStructLit;
            p->noStructLit = false;
            if (at(p, P_COLON)) {
                next(p);
                Node *sl = node(p, N_SLICE, t);
                sl->a = e;
                if (!at(p, P_RBRACK)) {
                    sl->c = parseExpr(p);
                }
                e = sl;
            } else {
                Node *first = parseExpr(p);
                if (accept(p, P_COLON)) {
                    Node *sl = node(p, N_SLICE, t);
                    sl->a = e;
                    sl->b = first;
                    if (!at(p, P_RBRACK)) {
                        sl->c = parseExpr(p);
                    }
                    e = sl;
                } else {
                    Node *ix = node(p, N_INDEX, t);
                    ix->a = e;
                    push(p, &ix->list, first);
                    while (accept(p, P_COMMA)) {
                        push(p, &ix->list, parseExpr(p));
                    }
                    e = ix;
                }
            }
            expect(p, P_RBRACK);
            p->noStructLit = saved;
        } else if (t->kind == P_INC || t->kind == P_DEC) {
            next(p);
            Node *n = node(p, N_INCDEC, t);
            n->op = t->kind;
            n->flags |= F_POSTFIX;
            n->a = e;
            e = n;
        } else if (t->kind == P_DOT) {
            next(p);
            Token *name = expectIdent(p, "a field or method name after '.'");
            Node *f = node(p, N_FIELD_ACCESS, name);
            f->a = e;
            f->name = tokText(p, name);
            e = f;
        } else if (t->kind == P_LBRACE && !p->noStructLit && typeLike(e)) {
            e = parseStructLit(p, e, t);
        } else {
            return e;
        }
    }
}

static Node *parseUnaryInner(Parser *p);

static Node *parseUnary(Parser *p) {
    if (++p->depth > MAX_DEPTH) {
        fail(p, peek(p), "this expression is nested too deeply");
    }
    Node *n = parseUnaryInner(p);
    p->depth--;
    return n;
}

static Node *parseUnaryInner(Parser *p) {
    Token *t = peek(p);
    switch (t->kind) {
    case P_MINUS: case P_BANG: case P_TILDE: case P_AMP: case P_STAR: case P_LARROW: {
        next(p);
        Node *n = node(p, N_UNARY, t);
        n->op = t->kind;
        n->a = parseUnary(p);
        return n;
    }
    case P_INC: case P_DEC: {
        next(p);
        Node *n = node(p, N_INCDEC, t);
        n->op = t->kind;
        n->a = parseUnary(p);
        return n;
    }
    case K_NOT:
        /* `not` takes a whole comparison, so it can't go where only one
         * operand fits: a == not b */
        fail(p, t, "'not' can't be used here; put it in parentheses, or use '!'");
        return NULL;
    case K_TRY: {
        next(p);
        Node *n = node(p, N_TRY, t);
        n->a = parseUnary(p);
        return n;
    }
    case K_COMPTIME: {
        /* comptime takes all of the expression after it */
        next(p);
        Node *n = node(p, N_COMPTIME, t);
        n->a = parseExpr(p);
        return n;
    }
    default:
        return parsePostfix(p);
    }
}

static int precedence(TokKind k) {
    switch (k) {
    case P_LARROW: return 1;
    case P_DOTDOT: return 2;
    case P_OROR: case K_OR: return 3;
    case P_ANDAND: case K_AND: return 4;
    case P_EQ: case P_NE: case P_LT: case P_LE: case P_GT: case P_GE: case K_IS: return 5;
    case P_PLUS: case P_MINUS: case P_PIPE: case P_CARET: return 6;
    case P_STAR: case P_SLASH: case P_PERCENT: case P_SHL: case P_SHR: case P_AMP: return 7;
    default: return 0;
    }
}

/* An operand: a unary expression, with a catch after it if there is one.
 * catch takes one value or a block, so in `f() catch 0 != 5` the catch is
 * done before the comparison. */
static Node *parseOperand(Parser *p) {
    Node *e = parseUnary(p);
    while (at(p, K_CATCH)) {
        Token *t = next(p);
        Node *c = node(p, N_CATCH, t);
        c->a = e;
        if (at(p, T_IDENT) && peekAt(p, 1)->kind == P_LBRACE) {
            c->name = tokText(p, next(p));
            c->c = parseBlock(p);
        } else if (at(p, T_IDENT) && peekAt(p, 1)->kind == K_MATCH) {
            c->name = tokText(p, next(p));
            c->c = parseMatch(p);
        } else {
            c->b = parseUnary(p);
        }
        e = c;
    }
    return e;
}

/* `not` comes between `and` and the comparisons, as in Python and Ruby:
 * `not a == b` is `!(a == b)`, and `not a and b` is `!a && b`. */
#define NOT_PREC 5

static Node *parseBinary(Parser *p, int minPrec) {
    Node *left;
    if (at(p, K_NOT) && minPrec <= NOT_PREC) {
        Token *t = next(p);
        left = node(p, N_UNARY, t);
        left->op = P_BANG;
        left->ival = 1;                 /* written as `not`, for messages */
        left->a = parseBinary(p, NOT_PREC);
    } else {
        left = parseOperand(p);
    }
    for (;;) {
        Token *t = peek(p);
        int prec = precedence(t->kind);
        if (prec == 0 || prec < minPrec) {
            return left;
        }
        next(p);
        Node *n = node(p, N_BINARY, t);
        n->op = t->kind == K_AND ? P_ANDAND : t->kind == K_OR ? P_OROR : t->kind;
        if (t->kind == K_IS) {
            /* a is b is a == b, and a is not b is a != b */
            bool negated = accept(p, K_NOT);
            n->op = negated ? P_NE : P_EQ;
            n->ival = negated ? 2 : 1;      /* written as words, for messages */
        }
        n->a = left;
        n->b = parseBinary(p, prec + 1);
        left = n;
    }
}

/* Whether the `if` at the parser's position has an `else` after its
 * condition, on the same line: `a if c else b` is a value, where `a if c`
 * alone is a guard on the statement. */
static bool elseFollows(Parser *p) {
    int depth = 0;
    for (int i = p->pos + 1; i < p->ntoks; i++) {
        switch (p->toks[i].kind) {
        case P_LPAREN: case P_LBRACK: case P_LBRACE:
            depth++;
            break;
        case P_RPAREN: case P_RBRACK: case P_RBRACE:
            if (--depth < 0) return false;
            break;
        case K_ELSE:
            if (depth == 0) return true;
            break;
        case P_COMMA:
            if (depth == 0) return false;
            break;
        case T_NEWLINE: case T_EOF:
            if (depth == 0) return false;
            break;
        default:
            break;
        }
    }
    return false;
}

/* A block holding one expression, the value of an if. */
static Node *valueBlock(Parser *p, Node *e) {
    Node *b = newNode(p->arena, N_BLOCK, e->line, e->col);
    b->src = p->src;
    Node *s = newNode(p->arena, N_EXPR_STMT, e->line, e->col);
    s->src = p->src;
    s->a = e;
    push(p, &b->list, s);
    return b;
}

static Node *parseExpr(Parser *p) {
    Node *e = parseBinary(p, 1);
    if (at(p, K_IF) && elseFollows(p)) {
        /* a if c else b: if c { a } else { b } */
        Token *t = next(p);
        Node *n = node(p, N_IF, t);
        n->a = parseBinary(p, 1);
        expect(p, K_ELSE);
        n->c = valueBlock(p, e);
        n->d = valueBlock(p, parseExpr(p));
        return n;
    }
    return e;
}

/* An expression where a struct literal can't start, like an if condition:
 * in `if x {` the brace opens the block. */
static Node *parseHeadExpr(Parser *p) {
    bool saved = p->noStructLit;
    p->noStructLit = true;
    Node *e = parseExpr(p);
    p->noStructLit = saved;
    return e;
}

/* ---------------------------------------------------------------- statements */

static Node *parseBlock(Parser *p) {
    if (++p->depth > MAX_DEPTH) {
        fail(p, peek(p), "blocks are nested too deeply");
    }
    Token *open = expect(p, P_LBRACE);
    Node *b = node(p, N_BLOCK, open);
    bool saved = p->noStructLit;
    p->noStructLit = false;
    skipNewlines(p);
    while (!at(p, P_RBRACE) && !at(p, T_EOF)) {
        jmp_buf jb;
        jmp_buf *prev = p->recover;
        int start = p->pos, depth = p->depth;
        p->recover = &jb;
        if (!setjmp(jb)) {
            Node *s = parseStmt(p);
            push(p, &b->list, s);
            endStatement(p, true);
        } else {
            p->pos = start;
            p->depth = depth;
            sync(p);
        }
        p->recover = prev;
        skipNewlines(p);
    }
    p->noStructLit = saved;
    if (!at(p, P_RBRACE)) {
        errorAt(p->src, open->line, open->col, "this '{' has no closing '}'");
        longjmp(*p->recover, 1);
    }
    next(p);
    p->depth--;
    return b;
}

static Node *parseIf(Parser *p) {
    Token *t = expect(p, K_IF);
    Node *n = node(p, N_IF, t);
    n->a = parseHeadExpr(p);
    n->c = parseBlock(p);
    if (at(p, T_NEWLINE) && peekAt(p, 1)->kind == K_ELSE) {
        next(p);
    }
    if (accept(p, K_ELSE)) {
        n->d = at(p, K_IF) ? parseIf(p) : parseBlock(p);
    }
    return n;
}

/* The body of a match or select arm: a block, or one simple statement. */
static Node *parseArmBody(Parser *p) {
    if (at(p, P_LBRACE)) {
        return parseBlock(p);
    }
    Token *t = peek(p);
    switch (t->kind) {
    case K_RETURN: case K_BREAK: case K_CONTINUE:
        return parseStmt(p);
    default:
        return parseGuard(p, parseSimpleStmt(p, false));
    }
}

static void endArm(Parser *p) {
    if (accept(p, P_COMMA) || at(p, T_NEWLINE)) {
        skipNewlines(p);
        return;
    }
    if (!at(p, P_RBRACE)) {
        fail(p, peek(p), "expected the end of the line, found %s", describe(p, peek(p)));
    }
}

static Node *parseMatch(Parser *p) {
    Token *t = expect(p, K_MATCH);
    Node *m = node(p, N_MATCH, t);
    m->a = parseHeadExpr(p);
    bool saved = p->noStructLit;
    p->noStructLit = false;
    Token *open = expect(p, P_LBRACE);
    skipNewlines(p);
    while (!at(p, P_RBRACE)) {
        if (at(p, T_EOF)) {
            errorAt(p->src, open->line, open->col, "this '{' has no closing '}'");
            longjmp(*p->recover, 1);
        }
        Node *arm = node(p, N_ARM, peek(p));
        do {
            push(p, &arm->list, parseExpr(p));
        } while (accept(p, P_COMMA));
        expect(p, P_ARROW);
        arm->c = parseArmBody(p);
        push(p, &m->list, arm);
        endArm(p);
    }
    next(p);
    p->noStructLit = saved;
    return m;
}

static Node *parseSelect(Parser *p) {
    Token *t = expect(p, K_SELECT);
    Node *s = node(p, N_SELECT, t);
    Token *open = expect(p, P_LBRACE);
    skipNewlines(p);
    while (!at(p, P_RBRACE)) {
        if (at(p, T_EOF)) {
            errorAt(p->src, open->line, open->col, "this '{' has no closing '}'");
            longjmp(*p->recover, 1);
        }
        Node *arm = node(p, N_SELECT_ARM, peek(p));
        if (at(p, T_IDENT) && peekAt(p, 1)->kind == P_DEFINE) {
            arm->name = tokText(p, next(p));
            next(p);
        }
        arm->a = parseExpr(p);
        expect(p, P_ARROW);
        arm->c = parseArmBody(p);
        push(p, &s->list, arm);
        endArm(p);
    }
    next(p);
    return s;
}

static Node *parseFor(Parser *p) {
    Token *t = expect(p, K_FOR);
    Node *n = node(p, N_FOR, t);
    if (at(p, T_IDENT) && (peekAt(p, 1)->kind == K_IN ||
                           (peekAt(p, 1)->kind == P_COMMA && peekAt(p, 2)->kind == T_IDENT &&
                            peekAt(p, 3)->kind == K_IN))) {
        n->flags |= F_FOR_IN;
        do {
            Token *v = next(p);
            Node *id = node(p, N_IDENT, v);
            id->name = tokText(p, v);
            push(p, &n->list, id);
        } while (accept(p, P_COMMA));
        expect(p, K_IN);
        n->a = parseHeadExpr(p);
    } else if (!at(p, P_LBRACE)) {
        n->a = parseHeadExpr(p);
    }
    n->c = parseBlock(p);
    return n;
}

static bool isAssignOp(TokKind k) {
    switch (k) {
    case P_ASSIGN: case P_PLUS_ASSIGN: case P_MINUS_ASSIGN: case P_STAR_ASSIGN:
    case P_SLASH_ASSIGN: case P_PERCENT_ASSIGN: case P_AMP_ASSIGN:
    case P_PIPE_ASSIGN: case P_CARET_ASSIGN: case P_SHL_ASSIGN: case P_SHR_ASSIGN:
        return true;
    default:
        return false;
    }
}

static bool startsType(Parser *p) {
    TokKind k = peek(p)->kind;
    return k == T_IDENT || k == P_LPAREN || k == P_BANG || k == K_DYN || k == K_CHAN;
}

/* `Type name` followed by one of these is a declaration. */
static bool declFollows(Parser *p) {
    if (!at(p, T_IDENT)) {
        return false;
    }
    TokKind k = peekAt(p, 1)->kind;
    return k == P_ASSIGN || k == T_NEWLINE || k == P_RBRACE || k == T_EOF;
}

static Node *parseVarRest(Parser *p, Node *type, unsigned flags) {
    Token *name = expectIdent(p, "a variable name");
    Node *v = node(p, N_VAR, name);
    v->name = tokText(p, name);
    v->a = type;
    v->flags = flags;
    if (accept(p, P_ASSIGN)) {
        v->b = parseExpr(p);
    }
    return v;
}

/* The right side of := or =: one expression, or several separated by
 * commas, which become a tuple: x, y := 1, 2 */
static Node *parseValues(Parser *p, bool allowList) {
    Token *t = peek(p);
    Node *e = parseExpr(p);
    if (!allowList || !at(p, P_COMMA)) {
        return e;
    }
    Node *tup = node(p, N_TUPLE, t);
    push(p, &tup->list, e);
    while (accept(p, P_COMMA)) {
        push(p, &tup->list, parseExpr(p));
    }
    return tup;
}

/* A declaration, assignment, := or expression statement. allowList is
 * false in match arms, where a comma ends the arm. */
static Node *parseSimpleStmt(Parser *p, bool allowList) {
    Token *t = peek(p);
    /* in a match arm (allowList false) a declaration makes no sense, so
     * `w * h` is a product, not a pointer declaration */
    if (allowList && startsType(p)) {
        int save = p->pos;
        Node *type = tryType(p);
        if (type && declFollows(p)) {
            return parseVarRest(p, type, 0);
        }
        p->pos = save;
    }

    Node *e = parseExpr(p);
    NodeList targets = {0};
    push(p, &targets, e);
    while (allowList && accept(p, P_COMMA)) {
        push(p, &targets, parseExpr(p));
    }
    Token *op = peek(p);
    if (op->kind == P_DEFINE) {
        next(p);
        Node *d = node(p, N_DEFINE, t);
        for (int i = 0; i < targets.len; i++) {
            if (targets.items[i]->kind != N_IDENT) {
                fail(p, op, "only names can be declared with ':='");
            }
        }
        d->list = targets;
        d->b = parseValues(p, allowList);
        return d;
    }
    if (isAssignOp(op->kind)) {
        next(p);
        Node *a = node(p, N_ASSIGN, op);
        a->op = op->kind;
        a->list = targets;
        a->b = parseValues(p, allowList && op->kind == P_ASSIGN);
        if (targets.len > 1 && op->kind != P_ASSIGN) {
            fail(p, op, "'%s' takes one target", tokName(op->kind));
        }
        return a;
    }
    if (targets.len > 1) {
        fail(p, op, "expected ':=' or '=' after a list of names, found %s", describe(p, op));
    }
    Node *s = node(p, N_EXPR_STMT, t);
    s->a = e;
    return s;
}

static Node *parseWhen(Parser *p, bool top);

/* Whether the `if` at the parser's position is a guard (`return if done`)
 * rather than an if that gives a value (`return if ok { 1 } else { 2 }`):
 * a guard has no block on its line. */
static bool guardFollows(Parser *p) {
    int depth = 0;
    for (int i = p->pos + 1; i < p->ntoks; i++) {
        switch (p->toks[i].kind) {
        case P_LPAREN: case P_LBRACK:
            depth++;
            break;
        case P_RPAREN: case P_RBRACK:
            depth--;
            break;
        case P_LBRACE:
            if (depth == 0) {
                return false;
            }
            break;
        case P_RBRACE:
            if (depth == 0) {
                return true;
            }
            break;
        case T_NEWLINE: case T_EOF:
            return true;
        default:
            break;
        }
    }
    return true;
}

/* A guard after a statement, `return if n < 0` or `print(x) if verbose`,
 * becomes an if around it. */
static Node *parseGuard(Parser *p, Node *s) {
    if (!at(p, K_IF)) {
        return s;
    }
    switch (s->kind) {
    case N_RETURN: case N_BREAK: case N_CONTINUE: case N_EXPR_STMT: case N_ASSIGN: case N_SPAWN:
        break;
    case N_VAR: case N_DEFINE:
        fail(p, peek(p), "a declaration can't have an 'if' after it: the name would only exist when the condition holds");
        return s;
    case N_DEFER: case N_ERRDEFER:
        fail(p, peek(p), "a defer can't have an 'if' after it; put the defer in an if block");
        return s;
    default:
        return s;
    }
    Token *t = next(p);
    Node *n = node(p, N_IF, t);
    n->a = parseHeadExpr(p);
    if (at(p, P_LBRACE) || at(p, K_ELSE)) {
        fail(p, peek(p), "an 'if' after a statement takes only a condition; write a full if to have a block or an else");
    }
    Node *b = node(p, N_BLOCK, t);
    push(p, &b->list, s);
    n->c = b;
    return n;
}

static Node *parseStmtCore(Parser *p);

static Node *parseStmt(Parser *p) {
    return parseGuard(p, parseStmtCore(p));
}

static Node *parseStmtCore(Parser *p) {
    Token *t = peek(p);
    switch (t->kind) {
    case K_IF:
        return parseIf(p);
    case K_FOR:
        return parseFor(p);
    case K_MATCH:
        return parseMatch(p);
    case K_SELECT:
        return parseSelect(p);
    case K_WHEN:
        return parseWhen(p, false);
    case P_LBRACE:
        return parseBlock(p);
    case K_RETURN: {
        next(p);
        Node *r = node(p, N_RETURN, t);
        if (!at(p, T_NEWLINE) && !at(p, P_RBRACE) && !at(p, T_EOF) && !at(p, P_COMMA) &&
            !(at(p, K_IF) && guardFollows(p))) {
            r->a = parseExpr(p);
        }
        return r;
    }
    case K_BREAK: case K_CONTINUE: {
        next(p);
        Node *b = node(p, t->kind == K_BREAK ? N_BREAK : N_CONTINUE, t);
        if (at(p, T_IDENT)) {
            b->name = tokText(p, next(p));
        }
        return b;
    }
    case K_DEFER: case K_ERRDEFER: case K_SPAWN: {
        next(p);
        Node *d = node(p, t->kind == K_DEFER ? N_DEFER : t->kind == K_ERRDEFER ? N_ERRDEFER : N_SPAWN, t);
        d->a = t->kind == K_SPAWN ? parseExpr(p) : parseStmtCore(p);
        return d;
    }
    case K_ASM: {
        next(p);
        Token *body = expect(p, T_ASM_BODY);
        Node *a = node(p, N_ASM, t);
        a->text = tokText(p, body);
        a->textLen = (size_t)body->len;
        return a;
    }
    case K_SCOPED: case K_THREADLOCAL: {
        next(p);
        unsigned flag = t->kind == K_SCOPED ? F_SCOPED : F_THREADLOCAL;
        if (flag == F_SCOPED && at(p, T_IDENT) && peekAt(p, 1)->kind == P_DEFINE) {
            /* scoped name := value: the type is the value's */
            Token *name = next(p);
            next(p);
            Node *v = node(p, N_VAR, name);
            v->name = tokText(p, name);
            v->flags = flag;
            v->b = parseExpr(p);
            return v;
        }
        Node *type = parseType(p);
        return parseVarRest(p, type, flag);
    }
    case T_IDENT:
        if (peekAt(p, 1)->kind == P_COLON && peekAt(p, 2)->kind == K_FOR) {
            next(p);
            next(p);
            Node *l = node(p, N_LABELED, t);
            l->name = tokText(p, t);
            l->a = parseFor(p);
            return l;
        }
        return parseSimpleStmt(p, true);
    default:
        return parseSimpleStmt(p, true);
    }
}

/* ---------------------------------------------------------------- declarations */

static void parseAttrs(Parser *p, NodeList *attrs) {
    while (at(p, P_AT)) {
        Token *t = next(p);
        Token *name = expectIdent(p, "an attribute name after '@'");
        Node *a = node(p, N_ATTR, t);
        a->name = tokText(p, name);
        if (at(p, P_LPAREN)) {
            next(p);
            while (!at(p, P_RPAREN)) {
                push(p, &a->list, parseExpr(p));
                if (!accept(p, P_COMMA)) {
                    break;
                }
            }
            expect(p, P_RPAREN);
        }
        push(p, attrs, a);
        skipNewlines(p);
    }
}

static void parseTypeParams(Parser *p, NodeList *out) {
    expect(p, P_LBRACK);
    do {
        Token *name = expectIdent(p, "a type parameter name");
        Node *tp = node(p, N_TYPE_PARAM, name);
        tp->name = tokText(p, name);
        if (accept(p, P_COLON)) {
            tp->a = parseType(p);
        }
        push(p, out, tp);
    } while (accept(p, P_COMMA));
    expect(p, P_RBRACK);
}

static void parseParams(Parser *p, NodeList *out) {
    expect(p, P_LPAREN);
    Node *prevType = NULL;
    while (!at(p, P_RPAREN)) {
        Token *t = peek(p);
        Node *param = NULL;
        TokKind after = peekAt(p, 1)->kind;
        if (prevType && t->kind == T_IDENT &&
            (after == P_COMMA || after == P_RPAREN || after == P_ASSIGN)) {
            next(p);
            param = node(p, N_PARAM, t);
            param->a = prevType;
        } else {
            Node *type = tryType(p);
            if (!type) {
                fail(p, peek(p), "expected a parameter type, found %s", describe(p, peek(p)));
            }
            Token *dots = peek(p);
            bool rest = accept(p, P_ELLIPSIS);
            if (rest) {
                /* T... name: the arguments left over, as a T[] */
                Node *sl = node(p, N_T_SLICE, dots);
                sl->a = type;
                type = sl;
            }
            t = expectIdent(p, "a parameter name");
            param = node(p, N_PARAM, t);
            param->a = type;
            if (rest) {
                param->flags |= F_REST;
                if (!at(p, P_RPAREN)) {
                    fail(p, peek(p), "a '...' parameter takes the arguments left over, so it comes last and has no default");
                }
            }
            prevType = rest ? NULL : type;
        }
        param->name = tokText(p, t);
        if (accept(p, P_ASSIGN)) {
            param->b = parseExpr(p);
        }
        push(p, out, param);
        if (!accept(p, P_COMMA)) {
            break;
        }
    }
    expect(p, P_RPAREN);
}

/* The rest of a function after its return type: the name (Type.name for a
 * method), type parameters, parameters, and the body. */
static Node *parseFuncRest(Parser *p, Node *ret, NodeList attrs, unsigned flags, bool needBody) {
    Token *name = expectIdent(p, "a function name");
    Node *f = node(p, N_FUNC, name);
    f->a = ret;
    f->attrs = attrs;
    f->flags = flags;
    NodeList typeParams = {0};
    if (at(p, P_LBRACK)) {
        parseTypeParams(p, &typeParams);
    }
    if (accept(p, P_DOT)) {
        /* a method: what came before the dot is the receiver type */
        Node *recv = node(p, N_T_NAME, name);
        recv->name = tokText(p, name);
        if (typeParams.len) {
            Node *g = node(p, N_T_GENERIC, name);
            g->a = recv;
            for (int i = 0; i < typeParams.len; i++) {
                Node *tn = node(p, N_T_NAME, name);
                tn->name = typeParams.items[i]->name;
                push(p, &g->list, tn);
            }
            recv = g;
        }
        f->b = recv;
        typeParams = (NodeList){0};
        name = expectIdent(p, "a method name after '.'");
        if (at(p, P_LBRACK)) {
            parseTypeParams(p, &typeParams);
        }
    }
    f->name = tokText(p, name);
    f->list2 = typeParams;
    parseParams(p, &f->list);
    if (at(p, P_LBRACE)) {
        f->c = parseBlock(p);
    } else if (accept(p, P_ARROW)) {
        f->d = parseExpr(p);
    } else if (needBody && !attrs.len) {
        /* with attributes (@extern) a function can be declared without a body */
        fail(p, peek(p), "expected '{' or '=>' to start the function body, found %s", describe(p, peek(p)));
    }
    return f;
}

static Node *parseStruct(Parser *p, NodeList attrs, unsigned flags) {
    Token *t = expect(p, K_STRUCT);
    Token *name = expectIdent(p, "a struct name");
    Node *s = node(p, N_STRUCT, name);
    s->name = tokText(p, name);
    s->attrs = attrs;
    s->flags = flags;
    (void)t;
    if (at(p, P_LBRACK)) {
        parseTypeParams(p, &s->list2);
    }
    for (;;) {
        if (isWord(peek(p), "packed")) {
            next(p);
            s->flags |= F_PACKED;
        } else if (isWord(peek(p), "align")) {
            next(p);
            expect(p, P_LPAREN);
            s->a = parseExpr(p);
            expect(p, P_RPAREN);
        } else {
            break;
        }
    }
    Token *open = expect(p, P_LBRACE);
    skipNewlines(p);
    while (!at(p, P_RBRACE)) {
        if (at(p, T_EOF)) {
            errorAt(p->src, open->line, open->col, "this '{' has no closing '}'");
            longjmp(*p->recover, 1);
        }
        NodeList fattrs = {0};
        parseAttrs(p, &fattrs);
        unsigned fflags = accept(p, K_PUB) ? F_PUB : 0;
        Token *ft = peek(p);
        Node *type = parseType(p);
        Node *f;
        if (at(p, T_NEWLINE) || at(p, P_RBRACE)) {
            f = node(p, N_EMBED, ft);
            f->a = type;
        } else {
            Token *fname = expectIdent(p, "a field name");
            f = node(p, N_FIELD, fname);
            f->name = tokText(p, fname);
            f->a = type;
            if (accept(p, P_ASSIGN)) {
                f->b = parseExpr(p);
            }
            if (accept(p, P_COLON)) {
                f->c = parseExpr(p);
            }
        }
        f->attrs = fattrs;
        f->flags |= fflags;
        push(p, &s->list, f);
        endStatement(p, true);
        skipNewlines(p);
    }
    next(p);
    return s;
}

static Node *parseEnum(Parser *p, NodeList attrs, unsigned flags) {
    expect(p, K_ENUM);
    Token *name = expectIdent(p, "an enum name");
    Node *e = node(p, N_ENUM, name);
    e->name = tokText(p, name);
    e->attrs = attrs;
    e->flags = flags;
    if (isWord(peek(p), "flags")) {
        next(p);
        e->flags |= F_FLAGS;
    }
    Token *open = expect(p, P_LBRACE);
    skipNewlines(p);
    while (!at(p, P_RBRACE)) {
        if (at(p, T_EOF)) {
            errorAt(p->src, open->line, open->col, "this '{' has no closing '}'");
            longjmp(*p->recover, 1);
        }
        Token *mname = expectIdent(p, "an enum value name");
        Node *m = node(p, N_ENUM_MEMBER, mname);
        m->name = tokText(p, mname);
        if (at(p, P_LPAREN)) {
            parseParams(p, &m->list);
        }
        if (accept(p, P_ASSIGN)) {
            m->b = parseExpr(p);
        }
        push(p, &e->list, m);
        if (accept(p, P_COMMA) || at(p, T_NEWLINE)) {
            skipNewlines(p);
            continue;
        }
        if (!at(p, P_RBRACE)) {
            fail(p, peek(p), "expected ',' or the end of the line after an enum value, found %s",
                 describe(p, peek(p)));
        }
    }
    next(p);
    return e;
}

static Node *parseInterface(Parser *p, NodeList attrs, unsigned flags) {
    expect(p, K_INTERFACE);
    Token *name = expectIdent(p, "an interface name");
    Node *in = node(p, N_INTERFACE, name);
    in->name = tokText(p, name);
    in->attrs = attrs;
    in->flags = flags;
    Token *open = expect(p, P_LBRACE);
    skipNewlines(p);
    while (!at(p, P_RBRACE)) {
        if (at(p, T_EOF)) {
            errorAt(p->src, open->line, open->col, "this '{' has no closing '}'");
            longjmp(*p->recover, 1);
        }
        Node *ret = parseType(p);
        push(p, &in->list, parseFuncRest(p, ret, (NodeList){0}, 0, false));
        endStatement(p, true);
        skipNewlines(p);
    }
    next(p);
    return in;
}

static Node *parseImport(Parser *p) {
    Token *t = expect(p, K_IMPORT);
    Node *imp = node(p, N_IMPORT, t);
    do {
        Token *it = peek(p);
        Node *item = node(p, N_IMPORT_ITEM, it);
        if (isWord(it, "c") && (peekAt(p, 1)->kind == T_STRING)) {
            next(p);
            Node *s = parseString(p, next(p));
            if (s->kind != N_STRING) {
                fail(p, it, "an import path can't contain {}");
            }
            item->text = s->text;
            item->flags |= F_C_IMPORT;
        } else if (it->kind == T_STRING) {
            Node *s = parseString(p, next(p));
            if (s->kind != N_STRING) {
                fail(p, it, "an import path can't contain {}");
            }
            item->text = s->text;
        } else {
            Token *name = expectIdent(p, "a module name or a quoted path");
            item->name = tokText(p, name);
        }
        push(p, &imp->list, item);
    } while (accept(p, P_COMMA));
    return imp;
}

static Node *parseWhen(Parser *p, bool top) {
    Token *t = expect(p, K_WHEN);
    Node *w = node(p, N_WHEN, t);
    w->a = parseHeadExpr(p);
    if (top) {
        Token *open = expect(p, P_LBRACE);
        Node *b = node(p, N_BLOCK, open);
        skipNewlines(p);
        while (!at(p, P_RBRACE)) {
            if (at(p, T_EOF)) {
                errorAt(p->src, open->line, open->col, "this '{' has no closing '}'");
                longjmp(*p->recover, 1);
            }
            push(p, &b->list, parseTopLevel(p));
            endStatement(p, true);
            skipNewlines(p);
        }
        next(p);
        w->c = b;
    } else {
        w->c = parseBlock(p);
    }
    if (at(p, T_NEWLINE) && peekAt(p, 1)->kind == K_ELSE) {
        next(p);
    }
    if (accept(p, K_ELSE)) {
        if (at(p, K_WHEN)) {
            w->d = parseWhen(p, top);
        } else if (top) {
            /* reuse the when parser for a plain block of declarations */
            Token *open = expect(p, P_LBRACE);
            Node *b = node(p, N_BLOCK, open);
            skipNewlines(p);
            while (!at(p, P_RBRACE)) {
                if (at(p, T_EOF)) {
                    errorAt(p->src, open->line, open->col, "this '{' has no closing '}'");
                    longjmp(*p->recover, 1);
                }
                push(p, &b->list, parseTopLevel(p));
                endStatement(p, true);
                skipNewlines(p);
            }
            next(p);
            w->d = b;
        } else {
            w->d = parseBlock(p);
        }
    }
    return w;
}

static Node *parseTopLevel(Parser *p) {
    NodeList attrs = {0};
    parseAttrs(p, &attrs);
    Token *start = peek(p);
    unsigned flags = 0;
    if (accept(p, K_PUB)) {
        flags |= F_PUB;
    }
    Token *t = peek(p);
    switch (t->kind) {
    case K_IMPORT:
        if (attrs.len || flags) {
            fail(p, start, "an import can't have attributes or pub");
        }
        return parseImport(p);
    case K_CONST: {
        next(p);
        Node *c;
        if (at(p, T_IDENT) && peekAt(p, 1)->kind == P_ASSIGN) {
            Token *name = next(p);
            c = node(p, N_CONST, name);
            c->name = tokText(p, name);
        } else {
            Node *type = parseType(p);
            Token *name = expectIdent(p, "a constant name");
            c = node(p, N_CONST, name);
            c->name = tokText(p, name);
            c->a = type;
        }
        expect(p, P_ASSIGN);
        c->b = parseExpr(p);
        c->flags = flags;
        c->attrs = attrs;
        return c;
    }
    case K_TYPE: {
        next(p);
        Token *name = expectIdent(p, "a type name");
        Node *td = node(p, N_TYPEDEF, name);
        td->name = tokText(p, name);
        expect(p, P_ASSIGN);
        td->a = parseType(p);
        td->flags = flags;
        return td;
    }
    case K_STRUCT:
        return parseStruct(p, attrs, flags);
    case K_ENUM:
        return parseEnum(p, attrs, flags);
    case K_INTERFACE:
        return parseInterface(p, attrs, flags);
    case K_TEST: {
        next(p);
        Token *name = peek(p);
        if (name->kind != T_STRING) {
            fail(p, name, "expected the test's name in quotes, found %s", describe(p, name));
        }
        Node *ts = node(p, N_TEST, t);
        Node *s = parseString(p, next(p));
        ts->text = s->kind == N_STRING ? s->text : "";
        ts->c = parseBlock(p);
        return ts;
    }
    case K_WHEN:
        return parseWhen(p, true);
    case K_THREADLOCAL: {
        next(p);
        Node *type = parseType(p);
        Node *v = parseVarRest(p, type, flags | F_THREADLOCAL);
        v->attrs = attrs;
        return v;
    }
    default:
        break;
    }

    if (t->kind == T_IDENT && peekAt(p, 1)->kind == P_DEFINE) {
        Node *d = parseSimpleStmt(p, true);
        d->flags |= flags;
        return d;
    }
    if (startsType(p)) {
        int save = p->pos;
        Node *type = tryType(p);
        if (type && at(p, T_IDENT)) {
            TokKind k = peekAt(p, 1)->kind;
            if (k == P_LPAREN || k == P_LBRACK || k == P_DOT) {
                return parseFuncRest(p, type, attrs, flags, true);
            }
            if (k == P_ASSIGN || k == T_NEWLINE || k == T_EOF || k == P_RBRACE) {
                Node *v = parseVarRest(p, type, flags);
                v->attrs = attrs;
                return v;
            }
        }
        p->pos = save;
    }
    if (p->snippet && !attrs.len && !flags) {
        return parseStmt(p);
    }
    fail(p, t, "expected a declaration, found %s", describe(p, t));
    return NULL;
}

Node *parseSource(Arena *a, Source *src, bool snippet) {
    Parser p = {0};
    p.arena = a;
    p.src = src;
    p.snippet = snippet;
    p.toks = lexRange(a, src, src->text, src->text + src->len, 1, 1, &p.ntoks);

    Node *file = newNode(a, N_FILE, 1, 1);
    file->src = src;
    skipNewlines(&p);
    while (!at(&p, T_EOF)) {
        jmp_buf jb;
        int start = p.pos;
        p.recover = &jb;
        if (!setjmp(jb)) {
            Node *d = parseTopLevel(&p);
            push(&p, &file->list, d);
            endStatement(&p, false);
        } else {
            p.pos = start;
            p.depth = 0;
            sync(&p);
            if (at(&p, P_RBRACE)) {
                next(&p);
            }
        }
        skipNewlines(&p);
    }
    return file;
}
