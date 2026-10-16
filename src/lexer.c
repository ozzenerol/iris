/* The lexer: source text to tokens.
 *
 * A newline ends a statement when the last token on the line can end one
 * (a name, a literal, a closing bracket, return, break, continue, true,
 * false, null). Inside ( ) and [ ] newlines are ignored. */
#include "iris.h"

#include <stdlib.h>
#include <string.h>

static const char *tokNames[] = {
#define X(k, s) s,
    TOKENS(X)
#undef X
};

const char *tokName(TokKind k) { return tokNames[k]; }

typedef struct {
    Arena *arena;
    Source *src;
    const char *p, *end;
    int line, col;
    Token *toks;
    int n, cap;
    char stack[512];        /* open brackets */
    int depth;
    TokKind last;
} Lexer;

static void push(Lexer *L, TokKind k, const char *s, int len, int line, int col) {
    if (L->n == L->cap) {
        int cap = L->cap ? L->cap * 2 : 256;
        Token *t = arenaAlloc(L->arena, sizeof(Token) * (size_t)cap);
        if (L->n) {
            memcpy(t, L->toks, sizeof(Token) * (size_t)L->n);
        }
        L->toks = t;
        L->cap = cap;
    }
    L->toks[L->n++] = (Token){k, s, len, line, col};
    L->last = k;
}

static bool endsStatement(TokKind k) {
    switch (k) {
    case T_IDENT: case T_INT: case T_FLOAT: case T_STRING: case T_STRING3:
    case T_RSTRING: case T_CHAR: case T_ASM_BODY: case P_RPAREN:
    case P_RBRACK: case P_RBRACE: case K_RETURN: case K_BREAK:
    case K_CONTINUE: case K_TRUE: case K_FALSE: case K_NULL:
        return true;
    default:
        return false;
    }
}

/* Called at each line break: adds a T_NEWLINE if the line ended a statement. */
static void lineBreak(Lexer *L) {
    if (L->depth > 0 && L->depth <= (int)sizeof L->stack) {
        char top = L->stack[L->depth - 1];
        if (top == '(' || top == '[') {
            return;
        }
    }
    if (L->n > 0 && endsStatement(L->last)) {
        push(L, T_NEWLINE, L->p, 0, L->line, L->col);
    }
}

static void adv(Lexer *L) {
    if (*L->p == '\n') {
        L->line++;
        L->col = 1;
    } else {
        L->col++;
    }
    L->p++;
}

static void lexError(Lexer *L, int line, int col, const char *msg) {
    errorAt(L->src, line, col, "%s", msg);
}

static bool isIdentStart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool isDigit(char c) { return c >= '0' && c <= '9'; }

static bool isIdentChar(char c) { return isIdentStart(c) || isDigit(c); }

static const struct {
    const char *word;
    TokKind kind;
} keywords[] = {
    {"if", K_IF}, {"else", K_ELSE}, {"for", K_FOR}, {"in", K_IN},
    {"match", K_MATCH}, {"break", K_BREAK}, {"continue", K_CONTINUE},
    {"return", K_RETURN}, {"struct", K_STRUCT}, {"union", K_UNION},
    {"enum", K_ENUM}, {"interface", K_INTERFACE}, {"type", K_TYPE},
    {"dyn", K_DYN}, {"pub", K_PUB}, {"const", K_CONST},
    {"comptime", K_COMPTIME}, {"when", K_WHEN}, {"import", K_IMPORT},
    {"try", K_TRY}, {"catch", K_CATCH}, {"defer", K_DEFER},
    {"errdefer", K_ERRDEFER}, {"scoped", K_SCOPED}, {"spawn", K_SPAWN},
    {"select", K_SELECT}, {"chan", K_CHAN}, {"thread", K_THREAD},
    {"threadlocal", K_THREADLOCAL}, {"asm", K_ASM}, {"test", K_TEST},
    {"true", K_TRUE}, {"false", K_FALSE}, {"null", K_NULL},
};

static TokKind keyword(const char *s, int len) {
    for (size_t i = 0; i < sizeof keywords / sizeof keywords[0]; i++) {
        if ((int)strlen(keywords[i].word) == len && memcmp(keywords[i].word, s, (size_t)len) == 0) {
            return keywords[i].kind;
        }
    }
    return T_IDENT;
}

/* Operators, longest first. */
static const struct {
    const char *s;
    TokKind kind;
} puncts[] = {
    {"<<=", P_SHL_ASSIGN}, {">>=", P_SHR_ASSIGN},
    {"..", P_DOTDOT}, {":=", P_DEFINE}, {"==", P_EQ}, {"!=", P_NE},
    {"<=", P_LE}, {">=", P_GE}, {"<<", P_SHL}, {">>", P_SHR},
    {"+=", P_PLUS_ASSIGN}, {"-=", P_MINUS_ASSIGN}, {"*=", P_STAR_ASSIGN},
    {"/=", P_SLASH_ASSIGN}, {"%=", P_PERCENT_ASSIGN}, {"&=", P_AMP_ASSIGN},
    {"&&", P_ANDAND}, {"|=", P_PIPE_ASSIGN}, {"||", P_OROR},
    {"^=", P_CARET_ASSIGN}, {"=>", P_ARROW}, {"<-", P_LARROW},
    {"(", P_LPAREN}, {")", P_RPAREN}, {"[", P_LBRACK}, {"]", P_RBRACK},
    {"{", P_LBRACE}, {"}", P_RBRACE}, {",", P_COMMA}, {".", P_DOT},
    {":", P_COLON}, {"=", P_ASSIGN}, {"<", P_LT}, {">", P_GT},
    {"+", P_PLUS}, {"-", P_MINUS}, {"*", P_STAR}, {"/", P_SLASH},
    {"%", P_PERCENT}, {"&", P_AMP}, {"|", P_PIPE}, {"^", P_CARET},
    {"~", P_TILDE}, {"!", P_BANG}, {"@", P_AT},
};

/* Skips a "..." string inside the braces of an interpolated string. */
static bool skipNestedString(Lexer *L) {
    adv(L);
    while (L->p < L->end && *L->p != '"' && *L->p != '\n') {
        if (*L->p == '\\' && L->p + 1 < L->end) {
            adv(L);
        }
        adv(L);
    }
    if (L->p >= L->end || *L->p != '"') {
        return false;
    }
    adv(L);
    return true;
}

/* A "..." or """...""" string. Braces hold expressions, so a quote inside
 * them belongs to a nested string, not to the end of this one. */
static void lexString(Lexer *L, bool triple) {
    const char *start = L->p;
    int line = L->line, col = L->col;
    for (int i = 0; i < (triple ? 3 : 1); i++) {
        adv(L);
    }
    int braces = 0;
    for (;;) {
        if (L->p >= L->end || (!triple && *L->p == '\n')) {
            lexError(L, line, col, "this string has no closing quote");
            break;
        }
        char c = *L->p;
        if (c == '\\' && L->p + 1 < L->end && L->p[1] != '\n') {
            adv(L);
            adv(L);
            continue;
        }
        if (c == '{') {
            if (braces == 0 && L->p + 1 < L->end && L->p[1] == '{') {
                adv(L);
            } else {
                braces++;
            }
            adv(L);
            continue;
        }
        if (c == '}') {
            if (braces == 0 && L->p + 1 < L->end && L->p[1] == '}') {
                adv(L);
            } else if (braces > 0) {
                braces--;
            }
            adv(L);
            continue;
        }
        if (c == '"') {
            if (braces > 0) {
                if (!skipNestedString(L)) {
                    lexError(L, line, col, "this string has no closing quote");
                    break;
                }
                continue;
            }
            if (!triple) {
                adv(L);
                break;
            }
            if (L->p + 2 < L->end && L->p[1] == '"' && L->p[2] == '"') {
                adv(L);
                adv(L);
                adv(L);
                break;
            }
        }
        adv(L);
    }
    push(L, triple ? T_STRING3 : T_STRING, start, (int)(L->p - start), line, col);
}

static void lexNumber(Lexer *L) {
    const char *start = L->p;
    int line = L->line, col = L->col;
    TokKind kind = T_INT;
    if (*L->p == '0' && L->p + 1 < L->end && strchr("xXbBoO", L->p[1])) {
        adv(L);
        adv(L);
        while (L->p < L->end && (isIdentChar(*L->p))) {
            adv(L);
        }
    } else {
        while (L->p < L->end && (isDigit(*L->p) || *L->p == '_')) {
            adv(L);
        }
        if (L->p + 1 < L->end && *L->p == '.' && isDigit(L->p[1])) {
            kind = T_FLOAT;
            adv(L);
            while (L->p < L->end && (isDigit(*L->p) || *L->p == '_')) {
                adv(L);
            }
        }
        if (L->p < L->end && (*L->p == 'e' || *L->p == 'E')) {
            const char *q = L->p + 1;
            if (q < L->end && (*q == '+' || *q == '-')) {
                q++;
            }
            if (q < L->end && isDigit(*q)) {
                kind = T_FLOAT;
                while (L->p < q) {
                    adv(L);
                }
                while (L->p < L->end && isDigit(*L->p)) {
                    adv(L);
                }
            }
        }
        /* a unit: 5s, 200ms, 64KiB */
        while (L->p < L->end && isIdentChar(*L->p)) {
            adv(L);
        }
    }
    push(L, kind, start, (int)(L->p - start), line, col);
}

Token *lexRange(Arena *a, Source *src, const char *start, const char *end,
                int line, int col, int *count) {
    Lexer L = {0};
    L.arena = a;
    L.src = src;
    L.p = start;
    L.end = end;
    L.line = line;
    L.col = col;
    L.last = T_NEWLINE;

    while (L.p < L.end) {
        char c = *L.p;
        if (c == '\n') {
            lineBreak(&L);
            adv(&L);
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            adv(&L);
            continue;
        }
        if (c == '/' && L.p + 1 < L.end && L.p[1] == '/') {
            while (L.p < L.end && *L.p != '\n') {
                adv(&L);
            }
            continue;
        }
        if (c == '/' && L.p + 1 < L.end && L.p[1] == '*') {
            int l = L.line, cl = L.col;
            bool broke = false;
            adv(&L);
            adv(&L);
            while (L.p < L.end && !(*L.p == '*' && L.p + 1 < L.end && L.p[1] == '/')) {
                if (*L.p == '\n' && !broke) {
                    lineBreak(&L);
                    broke = true;
                }
                adv(&L);
            }
            if (L.p >= L.end) {
                lexError(&L, l, cl, "this comment has no closing */");
            } else {
                adv(&L);
                adv(&L);
            }
            continue;
        }
        if (c == 'r' && L.end - L.p >= 4 && !memcmp(L.p + 1, "\"\"\"", 3)) {
            /* r"""...""": raw, over several lines */
            const char *s = L.p;
            int l = L.line, cl = L.col;
            for (int i = 0; i < 4; i++) {
                adv(&L);
            }
            while (L.p < L.end && !(L.end - L.p >= 3 && !memcmp(L.p, "\"\"\"", 3))) {
                adv(&L);
            }
            if (L.p >= L.end) {
                lexError(&L, l, cl, "this string has no closing \"\"\"");
            } else {
                adv(&L);
                adv(&L);
                adv(&L);
            }
            push(&L, T_RSTRING, s, (int)(L.p - s), l, cl);
            continue;
        }
        if (c == 'r' && L.p + 1 < L.end && L.p[1] == '"') {
            const char *s = L.p;
            int l = L.line, cl = L.col;
            adv(&L);
            adv(&L);
            while (L.p < L.end && *L.p != '"' && *L.p != '\n') {
                adv(&L);
            }
            if (L.p >= L.end || *L.p != '"') {
                lexError(&L, l, cl, "this string has no closing quote");
            } else {
                adv(&L);
            }
            push(&L, T_RSTRING, s, (int)(L.p - s), l, cl);
            continue;
        }
        if (isIdentStart(c)) {
            const char *s = L.p;
            int l = L.line, cl = L.col;
            while (L.p < L.end && isIdentChar(*L.p)) {
                adv(&L);
            }
            TokKind k = keyword(s, (int)(L.p - s));
            push(&L, k, s, (int)(L.p - s), l, cl);
            if (k == K_ASM) {
                while (L.p < L.end && (*L.p == ' ' || *L.p == '\t')) {
                    adv(&L);
                }
                if (L.p < L.end && *L.p == '{') {
                    int bl = L.line, bc = L.col;
                    adv(&L);
                    const char *body = L.p;
                    int nest = 0;
                    while (L.p < L.end && !(*L.p == '}' && nest == 0)) {
                        if (*L.p == '{') {
                            nest++;
                        } else if (*L.p == '}') {
                            nest--;
                        }
                        adv(&L);
                    }
                    if (L.p >= L.end) {
                        lexError(&L, bl, bc, "this asm block has no closing }");
                        push(&L, T_ASM_BODY, body, (int)(L.p - body), bl, bc);
                    } else {
                        push(&L, T_ASM_BODY, body, (int)(L.p - body), bl, bc);
                        adv(&L);
                    }
                }
            }
            continue;
        }
        if (isDigit(c)) {
            lexNumber(&L);
            continue;
        }
        if (c == '"') {
            bool triple = L.p + 2 < L.end && L.p[1] == '"' && L.p[2] == '"';
            lexString(&L, triple);
            continue;
        }
        if (c == '\'') {
            const char *s = L.p;
            int l = L.line, cl = L.col;
            adv(&L);
            while (L.p < L.end && *L.p != '\'' && *L.p != '\n') {
                if (*L.p == '\\' && L.p + 1 < L.end) {
                    adv(&L);
                }
                adv(&L);
            }
            if (L.p >= L.end || *L.p != '\'') {
                lexError(&L, l, cl, "this character has no closing quote");
            } else {
                adv(&L);
            }
            push(&L, T_CHAR, s, (int)(L.p - s), l, cl);
            continue;
        }
        bool found = false;
        for (size_t i = 0; i < sizeof puncts / sizeof puncts[0]; i++) {
            size_t n = strlen(puncts[i].s);
            if ((size_t)(L.end - L.p) >= n && memcmp(L.p, puncts[i].s, n) == 0) {
                TokKind k = puncts[i].kind;
                if (k == P_LPAREN || k == P_LBRACK || k == P_LBRACE) {
                    if (L.depth < (int)sizeof L.stack) {
                        L.stack[L.depth] = c;
                    }
                    L.depth++;
                } else if ((k == P_RPAREN || k == P_RBRACK || k == P_RBRACE) && L.depth > 0) {
                    L.depth--;
                }
                push(&L, k, L.p, (int)n, L.line, L.col);
                for (size_t j = 0; j < n; j++) {
                    adv(&L);
                }
                found = true;
                break;
            }
        }
        if (!found) {
            int l = L.line, cl = L.col;
            if ((unsigned char)c >= 0x80) {
                lexError(&L, l, cl, "characters outside ASCII can only appear in strings and comments");
                while (L.p < L.end && (unsigned char)*L.p >= 0x80) {
                    adv(&L);
                }
            } else {
                errorAt(src, l, cl, "unexpected character '%c'", c);
                adv(&L);
            }
        }
    }
    lineBreak(&L);
    push(&L, T_EOF, L.p, 0, L.line, L.col);
    *count = L.n;
    return L.toks;
}
