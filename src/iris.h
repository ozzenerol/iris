/* Shared declarations for the Iris compiler. */
#ifndef IRIS_H
#define IRIS_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IRIS_VERSION "0.2.0"

/* ---------------------------------------------------------------- arena.c */

/* Memory for one compile. Everything is freed at once with arenaFree. */
typedef struct ArenaBlock ArenaBlock;
typedef struct {
    ArenaBlock *head;
} Arena;

void *arenaAlloc(Arena *a, size_t size);           /* zeroed */
char *arenaStrndup(Arena *a, const char *s, size_t n);
void arenaFree(Arena *a);

/* A string that grows; its buffer lives in malloc'd memory until sbFinish. */
typedef struct {
    char *buf;
    size_t len, cap;
} StrBuf;

void sbPutc(StrBuf *b, char c);
void sbPuts(StrBuf *b, const char *s);
void sbPutn(StrBuf *b, const char *s, size_t n);
void sbPrintf(StrBuf *b, const char *fmt, ...);
char *sbFinish(StrBuf *b, Arena *a);               /* copies into the arena, frees buf */
void sbFree(StrBuf *b);

/* ---------------------------------------------------------------- diag.c */

typedef struct {
    const char *path;
    const char *text;
    size_t len;
} Source;

extern int gErrors;

void errorAt(Source *src, int line, int col, const char *fmt, ...);
void verrorAt(Source *src, int line, int col, const char *fmt, va_list ap);

/* ---------------------------------------------------------------- lexer.c */

#define TOKENS(X)                                                          \
    X(T_EOF, "end of file") X(T_NEWLINE, "end of line")                    \
    X(T_IDENT, "name") X(T_INT, "number") X(T_FLOAT, "number")             \
    X(T_STRING, "string") X(T_STRING3, "string") X(T_RSTRING, "string")    \
    X(T_CHAR, "character") X(T_ASM_BODY, "assembly")                       \
    X(K_IF, "if") X(K_ELSE, "else") X(K_FOR, "for") X(K_IN, "in")          \
    X(K_MATCH, "match") X(K_BREAK, "break") X(K_CONTINUE, "continue")      \
    X(K_RETURN, "return") X(K_STRUCT, "struct") X(K_UNION, "union")        \
    X(K_ENUM, "enum") X(K_INTERFACE, "interface") X(K_TYPE, "type")        \
    X(K_DYN, "dyn") X(K_PUB, "pub") X(K_CONST, "const")                    \
    X(K_COMPTIME, "comptime") X(K_WHEN, "when") X(K_IMPORT, "import")      \
    X(K_TRY, "try") X(K_CATCH, "catch") X(K_DEFER, "defer")                \
    X(K_ERRDEFER, "errdefer") X(K_SCOPED, "scoped") X(K_SPAWN, "spawn")    \
    X(K_SELECT, "select") X(K_CHAN, "chan") X(K_ATOMIC, "atomic")          \
    X(K_THREADLOCAL, "threadlocal") X(K_ASM, "asm") X(K_TEST, "test")      \
    X(K_TRUE, "true") X(K_FALSE, "false") X(K_NULL, "null")                \
    X(P_LPAREN, "(") X(P_RPAREN, ")") X(P_LBRACK, "[") X(P_RBRACK, "]")    \
    X(P_LBRACE, "{") X(P_RBRACE, "}") X(P_COMMA, ",") X(P_DOT, ".")        \
    X(P_DOTDOT, "..") X(P_COLON, ":") X(P_DEFINE, ":=") X(P_ASSIGN, "=")   \
    X(P_EQ, "==") X(P_NE, "!=") X(P_LT, "<") X(P_LE, "<=") X(P_GT, ">")    \
    X(P_GE, ">=") X(P_SHL, "<<") X(P_SHR, ">>") X(P_SHL_ASSIGN, "<<=")     \
    X(P_SHR_ASSIGN, ">>=") X(P_PLUS, "+") X(P_PLUS_ASSIGN, "+=")           \
    X(P_MINUS, "-") X(P_MINUS_ASSIGN, "-=") X(P_STAR, "*")                 \
    X(P_STAR_ASSIGN, "*=") X(P_SLASH, "/") X(P_SLASH_ASSIGN, "/=")         \
    X(P_PERCENT, "%") X(P_PERCENT_ASSIGN, "%=") X(P_AMP, "&")              \
    X(P_AMP_ASSIGN, "&=") X(P_ANDAND, "&&") X(P_PIPE, "|")                 \
    X(P_PIPE_ASSIGN, "|=") X(P_OROR, "||") X(P_CARET, "^")                 \
    X(P_CARET_ASSIGN, "^=") X(P_TILDE, "~") X(P_BANG, "!")                 \
    X(P_ARROW, "=>") X(P_LARROW, "<-") X(P_AT, "@")

typedef enum {
#define X(k, s) k,
    TOKENS(X)
#undef X
    TOKEN_COUNT
} TokKind;

typedef struct {
    TokKind kind;
    const char *s;      /* points into the source */
    int len;
    int line, col;
} Token;

const char *tokName(TokKind k);

/* Splits text[start..end) into tokens. line and col are those of start.
 * The array ends with T_EOF. */
Token *lexRange(Arena *a, Source *src, const char *start, const char *end,
                int line, int col, int *count);

/* ---------------------------------------------------------------- ast */

typedef enum {
    /* declarations */
    N_FILE, N_IMPORT, N_IMPORT_ITEM, N_ATTR, N_FUNC, N_PARAM, N_TYPE_PARAM,
    N_STRUCT, N_FIELD, N_EMBED, N_ENUM, N_ENUM_MEMBER, N_INTERFACE, N_CONST,
    N_TYPEDEF, N_VAR, N_TEST, N_WHEN,
    /* types */
    N_T_NAME, N_T_PTR, N_T_SLICE, N_T_ARRAY, N_T_GENERIC, N_T_FUNC,
    N_T_TUPLE, N_T_ERR, N_T_DYN, N_T_CHAN,
    /* statements */
    N_BLOCK, N_EXPR_STMT, N_DEFINE, N_ASSIGN, N_RETURN, N_BREAK, N_CONTINUE,
    N_IF, N_FOR, N_MATCH, N_ARM, N_DEFER, N_ERRDEFER, N_SPAWN, N_SELECT,
    N_SELECT_ARM, N_ASM, N_LABELED,
    /* expressions */
    N_IDENT, N_INT, N_FLOAT, N_STRING, N_INTERP, N_CHAR, N_BOOL, N_NULL,
    N_UNARY, N_BINARY, N_CALL, N_NAMED_ARG, N_INDEX, N_SLICE, N_FIELD_ACCESS,
    N_ENUM_DOT, N_STRUCT_LIT, N_FIELD_INIT, N_ARRAY_LIT, N_TUPLE, N_TRY,
    N_CATCH, N_COMPTIME, N_CHAN_MAKE,
    /* added by the checker */
    N_CONVERT               /* a: the value; type: the target; flags F_EXPLICIT */
} NodeKind;

enum {
    F_PUB = 1 << 0,
    F_SCOPED = 1 << 1,
    F_THREADLOCAL = 1 << 2,
    F_ATOMIC = 1 << 3,
    F_PACKED = 1 << 4,
    F_FLAGS = 1 << 5,
    F_C_IMPORT = 1 << 6,
    F_FOR_IN = 1 << 7,
    F_EXPLICIT = 1 << 8,
};

typedef struct Node Node;
typedef struct Type Type;

typedef struct {
    Node **items;
    int len, cap;
} NodeList;

/* One node type for the whole tree. What each field means depends on the
 * kind; ast.c (the printer) is the reference. */
struct Node {
    NodeKind kind;
    int line, col;
    unsigned flags;
    TokKind op;             /* unary, binary and assignment operators */
    const char *name;       /* names, labels, units */
    const char *text;       /* decoded string contents, raw assembly */
    size_t textLen;
    uint64_t ival;
    double fval;
    Node *a, *b, *c, *d;
    NodeList list, list2, attrs;

    /* filled in by the checker */
    Type *type;             /* the type of an expression or declaration */
    Node *ref;              /* what a name refers to: a declaration node */
    bool isConst;           /* the expression's value is known: cval */
    int64_t cval;
    int state;              /* checker bookkeeping (constants: 0 new, 1 busy, 2 done) */
    const char *ir;         /* the IR writer's name for a variable or function */
};

Node *newNode(Arena *a, NodeKind kind, int line, int col);
void listPush(Arena *a, NodeList *l, Node *n);

void printTree(Node *n);
char *typeString(Arena *a, Node *t);

/* ---------------------------------------------------------------- parser.c */

/* snippet: allow statements at the top level, for checking doc examples. */
Node *parseSource(Arena *a, Source *src, bool snippet);

/* ---------------------------------------------------------------- check.c */

typedef enum {
    TY_ERROR,               /* after an error: fits anywhere, so errors don't cascade */
    TY_VOID,
    TY_NORETURN,            /* a block that always leaves: return, break, panic */
    TY_BOOL,
    TY_INT,
    TY_STR,
} TypeKind;

struct Type {
    TypeKind kind;
    int bits;               /* integers */
    bool isSigned;
    const char *name;
};

extern Type tyError, tyVoid, tyNoreturn, tyBool, tyStr;
extern Type tyI8, tyI16, tyI32, tyI64, tyU8, tyU16, tyU32, tyU64;

/* Checks the program and fills in types. Returns false after errors. */
bool checkProgram(Arena *a, Source *src, Node *file);

/* ---------------------------------------------------------------- ir.c */

typedef enum { MODE_DEBUG, MODE_RELEASE_SAFE, MODE_RELEASE_FAST } BuildMode;

/* Writes the checked program as LLVM IR. */
char *writeIR(Arena *a, Source *src, Node *file, BuildMode mode);

#endif
