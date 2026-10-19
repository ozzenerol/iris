/* iris fmt: puts a file's code in the standard layout.
 *
 * It works on tokens, not on the syntax tree, so comments and the text of
 * strings stay exactly as written. Between two tokens it decides the
 * spacing; at the start of a line, the indentation. Comments and blank
 * lines are read from the source between the tokens.
 *
 * Indentation: a line is one level deeper than the line where the
 * innermost bracket still open was opened. A line that starts with a
 * closing bracket gets the indentation of the line that opened it.
 *
 * Spacing: a space after a comma or a colon, around binary operators and
 * assignments, none inside () and [], none around . and .., none after a
 * unary operator. Where a token could mean two things (* in a pointer
 * type or a product, { opening a block or a struct literal), the spacing
 * the author wrote is kept. Everything else keeps "space or no space" as
 * written. At most one blank line in a row; none right after a { or right
 * before a }.
 *
 * As a check, the result is split into tokens again: if they differ from
 * the original ones in anything but layout, nothing is written. */

#include "iris.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    TokKind kind;
    int line;           /* the source line it was opened on */
    int indent;         /* the indentation of that line in the output */
} Open;

typedef struct {
    StrBuf out;
    Open stack[512];
    int depth;
    int indent;         /* the current output line's indentation */
    bool lineEmpty;     /* nothing written on the current output line yet */
} Fmt;

static bool endsOperand(TokKind k) {
    switch (k) {
    case T_IDENT: case T_INT: case T_FLOAT: case T_STRING: case T_STRING3: case T_RSTRING: case T_CHAR:
    case P_RPAREN: case P_RBRACK: case P_RBRACE: case K_TRUE: case K_FALSE: case K_NULL:
        return true;
    default:
        return false;
    }
}

/* Whether toks[i] ends an operand: a postfix ++ or -- does, x++. */
static bool endsOperandAt(const Token *toks, int i) {
    TokKind k = toks[i].kind;
    if (k == P_INC || k == P_DEC) {
        return i > 0 && toks[i - 1].kind != T_NEWLINE && endsOperandAt(toks, i - 1);
    }
    return endsOperand(k);
}

static bool isBinary(TokKind k) {
    switch (k) {
    case P_DEFINE: case P_ASSIGN: case P_EQ: case P_NE: case P_LT: case P_LE: case P_GT: case P_GE:
    case P_SHL: case P_SHR: case P_SHL_ASSIGN: case P_SHR_ASSIGN: case P_PLUS: case P_PLUS_ASSIGN:
    case P_MINUS: case P_MINUS_ASSIGN: case P_STAR: case P_STAR_ASSIGN: case P_SLASH: case P_SLASH_ASSIGN:
    case P_PERCENT: case P_PERCENT_ASSIGN: case P_AMP: case P_AMP_ASSIGN: case P_ANDAND: case P_PIPE:
    case P_PIPE_ASSIGN: case P_OROR: case P_CARET: case P_CARET_ASSIGN: case P_ARROW: case P_LARROW: case K_AND:
    case K_OR: case K_IS:
        return true;
    default:
        return false;
    }
}

/* An operator that can also be unary is unary after something that isn't
 * the end of an operand: -x, !ok, &v, *p, <-ch, ~bits. */
static bool canBeUnary(TokKind k) {
    return k == P_MINUS || k == P_BANG || k == P_AMP || k == P_STAR || k == P_LARROW || k == P_TILDE || k == P_INC ||
           k == P_DEC;
}

static bool isUnaryAt(const Token *toks, int i, int prev) {
    TokKind k = toks[i].kind;
    if (k == P_BANG || k == P_TILDE) {
        return true;
    }
    if (i > 0 && toks[i - 1].kind == T_NEWLINE) {
        return canBeUnary(k);           /* at the start of a statement */
    }
    return canBeUnary(k) && (prev < 0 || !endsOperandAt(toks, prev));
}

/* Whether the source had blank space between two tokens. */
static bool hadSpace(const Token *a, const Token *b) {
    return b->s > a->s + a->len;
}

static bool isKeyword(TokKind k) {
    return k >= K_IF && k <= K_NULL;
}

/* A * in a pointer type, written the Iris way: u8 *p, Request *. */
static bool pointerStar(const Token *toks, int i, int prev) {
    if (prev < 0 || !endsOperandAt(toks, prev)) {
        return false;
    }
    const Token *s = &toks[i], *n = &toks[i + 1];
    return hadSpace(&toks[prev], s) && n->kind != T_EOF && n->kind != T_NEWLINE && !hadSpace(s, n);
}

static int prevToken(const Token *toks, int i) {
    i--;
    while (i >= 0 && toks[i].kind == T_NEWLINE) i--;
    return i;
}

/* Space between toks[prev] and toks[i], on one line. inBrackets: the
 * innermost open bracket is a [, where : slices. */
static bool spaceBetween(const Token *toks, int prev, int i, bool inBrackets) {
    const Token *a = &toks[prev], *b = &toks[i];
    TokKind pk = a->kind, k = b->kind;
    bool written = hadSpace(a, b);

    if (k == P_COMMA || k == P_RPAREN || k == P_RBRACK || k == P_COLON || (k == P_DOT && endsOperand(pk)) ||
        k == P_ELLIPSIS) {
        return false;                           /* i64... xs */
    }
    if (pk == P_ELLIPSIS) {
        return true;
    }
    char last = a->s[a->len - 1];
    if ((last == '-' || last == '+') && b->s[0] == last && pk != T_STRING && pk != T_CHAR) {
        return true;                            /* - -x, which would read as --x */
    }
    if ((k == P_INC || k == P_DEC) && i == prev + 1 && endsOperandAt(toks, prev)) {
        return false;                           /* x++ */
    }
    if (k == P_DOT) {
        /* .read, .{x: 1}: a value of a type worked out from the context */
        return pk != P_LPAREN && pk != P_LBRACK && pk != P_DOT && pk != P_AT &&
               (written || isBinary(pk) || isKeyword(pk) || pk == P_COMMA || pk == P_COLON);
    }
    if (pk == P_LPAREN || pk == P_LBRACK || pk == P_DOT || pk == P_AT || pk == P_DOTDOT || k == P_DOTDOT) {
        return false;
    }
    if (pk == P_COLON) {
        if (!inBrackets) {
            return true;                        /* name: value */
        }
        /* in []: a type bound, [T: Ordered], or a slice, xs[a:b] */
        int pp = prevToken(toks, prev);
        return pp >= 0 && toks[pp].kind == T_IDENT && toks[pp].s[0] >= 'A' && toks[pp].s[0] <= 'Z' &&
               k == T_IDENT && b->s[0] >= 'A' && b->s[0] <= 'Z';
    }
    if (pk == P_COMMA) {
        return true;
    }
    if (k == P_LPAREN && (pk == T_IDENT || pk == P_RPAREN || pk == P_RBRACK || pk == K_CHAN)) {
        return false;                           /* a call */
    }
    if (k == P_LBRACK && (pk == T_IDENT || pk == P_RPAREN || pk == P_RBRACK || pk == K_CHAN ||
                          pk == T_STRING || pk == T_RSTRING)) {
        return false;                           /* an index, a slice or a generic type */
    }
    if (canBeUnary(pk) && isUnaryAt(toks, prev, prevToken(toks, prev))) {
        return false;                           /* -x, !ok, &v, *p, <-ch */
    }
    if (pk == P_STAR && pointerStar(toks, prev, prevToken(toks, prev))) {
        return false;
    }
    if (k == P_STAR && pointerStar(toks, i, prev)) {
        return true;
    }
    if (canBeUnary(k) && isUnaryAt(toks, i, prev)) {
        return written || isBinary(pk) || isKeyword(pk);
    }
    if (k == P_LBRACE) {
        /* after a name, { may start a struct literal: Point{x: 1}. Anywhere
         * else, and after the name in struct Node, it opens a block. */
        int pp = prevToken(toks, prev);
        bool declName = pk == T_IDENT && pp >= 0 &&
                        (toks[pp].kind == K_STRUCT || toks[pp].kind == K_ENUM || toks[pp].kind == K_UNION ||
                         toks[pp].kind == K_INTERFACE);
        if ((pk != T_IDENT && pk != P_RBRACK) || declName) {
            return true;
        }
        return written;
    }
    if (isBinary(k) || isBinary(pk)) {
        return true;
    }
    return written;
}

static void newline(Fmt *f) {
    sbPutc(&f->out, '\n');
    f->lineEmpty = true;
}

static void put(Fmt *f, const char *s, size_t n) {
    if (f->lineEmpty) {
        for (int i = 0; i < f->indent * 4; i++) sbPutc(&f->out, ' ');
        f->lineEmpty = false;
    }
    for (size_t k = 0; k < n; k++) sbPutc(&f->out, s[k]);
}

/* The indentation for a new line that starts with token k. */
static int lineIndent(Fmt *f, TokKind k) {
    if ((k == P_RBRACE || k == P_RPAREN || k == P_RBRACK) && f->depth > 0) {
        return f->stack[f->depth - 1].indent;
    }
    return f->depth > 0 ? f->stack[f->depth - 1].indent + 1 : 0;
}

/* Writes a comment, without the spaces at its end. */
static void putComment(Fmt *f, const char *s, size_t n) {
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) n--;
    put(f, s, n);
}

/* The source between two tokens: comments and line breaks. Returns the
 * number of line breaks before the next token. */
static int gap(Fmt *f, const char *p, const char *end, int prevKind, TokKind next, bool first) {
    int breaks = 0, pending = 0;
    while (p < end) {
        if (*p == '\n') {
            breaks++;
            pending++;
            p++;
            continue;
        }
        if (*p == ' ' || *p == '\t' || *p == '\r') {
            p++;
            continue;
        }
        const char *c = p;
        if (p + 1 < end && p[1] == '/') {
            while (p < end && *p != '\n') p++;
        } else if (p + 1 < end && p[1] == '*') {
            p += 2;
            while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) p++;
            p = p + 2 <= end ? p + 2 : end;
        } else {
            p++;                /* not reached: the lexer accepted it */
            continue;
        }
        if (pending == 0 && !first) {
            put(f, "\x01", 1); /* a comment at the end of a line: aligned later */
        } else {
            if (!f->lineEmpty) newline(f);
            if (pending >= 2 && !first && prevKind != P_LBRACE) newline(f);
            f->indent = lineIndent(f, T_IDENT);
        }
        putComment(f, c, (size_t)(p - c));
        pending = 0;
        first = false;
    }
    if (pending > 0 || breaks > 0) {
        if (!f->lineEmpty) newline(f);
        if (pending >= 2 && next != P_RBRACE && prevKind != P_LBRACE) newline(f);
    }
    return breaks;
}

/* Lines next to each other that end with a comment get their comments in
 * one column, a space after the longest line's code. \x01 marks where a
 * comment at the end of a line starts. */
static void alignComments(StrBuf *b) {
    StrBuf out = {0};
    size_t i = 0;
    while (i < b->len) {
        /* a run of lines with a mark */
        size_t j = i, widest = 0;
        int lines = 0;
        for (;;) {
            size_t end = j;
            while (end < b->len && b->buf[end] != '\n') end++;
            const char *mark = memchr(b->buf + j, '\x01', end - j);
            if (!mark) break;
            size_t w = (size_t)(mark - (b->buf + j));
            if (w > widest) widest = w;
            lines++;
            j = end < b->len ? end + 1 : end;
            if (j >= b->len) break;
        }
        if (!lines) {
            size_t end = i;
            while (end < b->len && b->buf[end] != '\n') end++;
            sbPutn(&out, b->buf + i, end - i);
            if (end < b->len) sbPutc(&out, '\n');
            i = end < b->len ? end + 1 : end;
            continue;
        }
        for (int k = 0; k < lines; k++) {
            size_t end = i;
            while (end < b->len && b->buf[end] != '\n') end++;
            const char *mark = memchr(b->buf + i, '\x01', end - i);
            size_t w = (size_t)(mark - (b->buf + i));
            sbPutn(&out, b->buf + i, w);
            for (size_t p = w; p < widest + 1; p++) sbPutc(&out, ' ');
            sbPutn(&out, mark + 1, (size_t)(b->buf + end - mark - 1));
            if (end < b->len) sbPutc(&out, '\n');
            i = end < b->len ? end + 1 : end;
        }
    }
    free(b->buf);
    *b = out;
}

/* Tokens as kind and text, for comparing before and after. */
static bool sameTokens(Arena *a, Source *src, const char *text, size_t len) {
    Source s2 = *src;
    s2.text = text;
    s2.len = len;
    int n1, n2;
    int before = gErrors;
    Token *t1 = lexRange(a, src, src->text, src->text + src->len, 1, 1, &n1);
    Token *t2 = lexRange(a, &s2, text, text + len, 1, 1, &n2);
    if (gErrors != before || n1 != n2) return false;
    for (int i = 0; i < n1; i++) {
        if (t1[i].kind != t2[i].kind || t1[i].len != t2[i].len || memcmp(t1[i].s, t2[i].s, (size_t)t1[i].len)) {
            return false;
        }
    }
    return true;
}

/* The source in the standard layout, or NULL when it has errors. */
char *formatSource(Arena *a, Source *src) {
    int before = gErrors;
    int n;
    Token *toks = lexRange(a, src, src->text, src->text + src->len, 1, 1, &n);
    if (gErrors != before) {
        return NULL;
    }
    if (memchr(src->text, '\x01', src->len)) {
        fprintf(stderr, "iris fmt: %s: has a control character \\x01, which fmt doesn't handle\n", src->path);
        return NULL;
    }
    Fmt f = {.lineEmpty = true};
    int prev = -1;
    const char *pos = src->text;
    for (int i = 0; i < n; i++) {
        Token *t = &toks[i];
        if (t->kind == T_NEWLINE) continue;
        const char *gapEnd = t->kind == T_EOF ? src->text + src->len : t->s;
        int breaks = gap(&f, pos, gapEnd, prev >= 0 ? (int)toks[prev].kind : -1, t->kind, prev < 0);
        if (t->kind == T_EOF) break;
        if (prev < 0 || breaks > 0 || f.lineEmpty) {
            f.indent = lineIndent(&f, t->kind);
            if (prev >= 0 && (isBinary(toks[prev].kind) || toks[prev].kind == P_DOT)) {
                f.indent++;     /* the statement goes on from the line before */
            }
        } else if (spaceBetween(toks, prev, i, f.depth > 0 && f.stack[f.depth - 1].kind == P_LBRACK)) {
            put(&f, " ", 1);
        }
        if (t->kind == T_ASM_BODY) {
            /* the braces around an asm block are not tokens: the block as written */
            put(&f, t->s - 1, (size_t)t->len + 2);
            pos = t->s + t->len + 1;
            prev = i;
            continue;
        }
        put(&f, t->s, (size_t)t->len);
        if (t->kind == P_LPAREN || t->kind == P_LBRACK || t->kind == P_LBRACE) {
            if (f.depth < (int)(sizeof f.stack / sizeof f.stack[0])) {
                f.stack[f.depth++] = (Open){t->kind, t->line, f.indent};
            }
        } else if ((t->kind == P_RPAREN || t->kind == P_RBRACK || t->kind == P_RBRACE) && f.depth > 0) {
            f.depth--;
        }
        pos = t->s + t->len;
        prev = i;
    }
    if (!f.lineEmpty) newline(&f);
    /* no blank lines at the end */
    while (f.out.len >= 2 && f.out.buf[f.out.len - 1] == '\n' && f.out.buf[f.out.len - 2] == '\n') {
        f.out.len--;
    }
    if (f.out.len == 0) {
        sbPutc(&f.out, '\n');
        f.out.len = 0;
    }
    alignComments(&f.out);
    char *result = sbFinish(&f.out, a);
    if (!result) result = arenaStrndup(a, "", 0);
    if (!sameTokens(a, src, result, strlen(result))) {
        fprintf(stderr, "iris fmt: %s: the result would change the code, so it was left as it is; please report it\n",
                src->path);
        return NULL;
    }
    return result;
}
