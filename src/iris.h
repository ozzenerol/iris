/* Shared declarations for the Iris compiler. */
#ifndef IRIS_H
#define IRIS_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IRIS_VERSION "0.67.1"

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
/* when set, errors go here instead of stderr (the language server) */
extern void (*gDiagHook)(Source *src, int line, int col, const char *msg);
/* when set, source files are read through it first (the language server's open documents) */
extern const char *(*gReadHook)(const char *path, size_t *len);
/* when set, the names of the .iris files open in a folder, also those not saved yet */
extern int (*gFolderHook)(const char *dir, const char **names, int max);
const char *stdSource(const char *path);
int stdModuleNames(const char **out, int max);
bool stdModuleExists(const char *name);

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
    X(K_SELECT, "select") X(K_CHAN, "chan") X(K_THREAD, "thread")          \
    X(K_THREADLOCAL, "threadlocal") X(K_ASM, "asm") X(K_TEST, "test")      \
    X(K_NOT, "not") X(K_AND, "and") X(K_OR, "or") X(K_IS, "is")            \
    X(K_TRUE, "true") X(K_FALSE, "false") X(K_NULL, "null")                \
    X(P_LPAREN, "(") X(P_RPAREN, ")") X(P_LBRACK, "[") X(P_RBRACK, "]")    \
    X(P_LBRACE, "{") X(P_RBRACE, "}") X(P_COMMA, ",") X(P_DOT, ".")        \
    X(P_DOTDOT, "..") X(P_ELLIPSIS, "...") X(P_COLON, ":") X(P_DEFINE, ":=") X(P_ASSIGN, "=")   \
    X(P_EQ, "==") X(P_NE, "!=") X(P_LT, "<") X(P_LE, "<=") X(P_GT, ">")    \
    X(P_GE, ">=") X(P_SHL, "<<") X(P_SHR, ">>") X(P_SHL_ASSIGN, "<<=")     \
    X(P_SHR_ASSIGN, ">>=") X(P_PLUS, "+") X(P_PLUS_ASSIGN, "+=")           \
    X(P_MINUS, "-") X(P_MINUS_ASSIGN, "-=") X(P_STAR, "*")                 \
    X(P_STAR_ASSIGN, "*=") X(P_SLASH, "/") X(P_SLASH_ASSIGN, "/=")         \
    X(P_PERCENT, "%") X(P_PERCENT_ASSIGN, "%=") X(P_AMP, "&")              \
    X(P_AMP_ASSIGN, "&=") X(P_ANDAND, "&&") X(P_PIPE, "|")                 \
    X(P_PIPE_ASSIGN, "|=") X(P_OROR, "||") X(P_CARET, "^")                 \
    X(P_CARET_ASSIGN, "^=") X(P_TILDE, "~") X(P_BANG, "!")                 \
    X(P_ARROW, "=>") X(P_LARROW, "<-") X(P_AT, "@")                 \
    X(P_INC, "++") X(P_DEC, "--")

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
    N_CATCH, N_COMPTIME, N_CHAN_MAKE, N_THREAD,
    N_INCDEC,               /* ++x, x++, --x, x--: a: the place; op: P_INC or P_DEC; flags F_POSTFIX */
    N_FMTSPEC,              /* {value:spec} in text: a: the value; text: the spec, as fmtSpecParse reads it */
    /* added by the checker */
    N_CONVERT,              /* a: the value; type: the target; flags F_EXPLICIT */
    N_SELF_PARAM            /* the implicit self parameter of a method */
} NodeKind;

enum {
    F_PUB = 1 << 0,
    F_SCOPED = 1 << 1,
    F_THREADLOCAL = 1 << 2,
    F_BENCH = 1 << 3,       /* a bench block, and the function it became */
    F_PACKED = 1 << 4,
    F_FLAGS = 1 << 5,
    F_C_IMPORT = 1 << 6,
    F_FOR_IN = 1 << 7,
    F_EXPLICIT = 1 << 8,
    F_SELF = 1 << 9,
    F_EXTERN = 1 << 10,     /* @extern: a C function, declared without a body */
    F_REPORTED = 1 << 11,   /* an error about this name was reported already */
    F_VARIADIC = 1 << 12,   /* a C function taking more arguments: printf */
    F_EXPORT = 1 << 13,     /* @export: a function C can call by name */
    F_TEMPTEXT = 1 << 14,   /* text with {} values passed as a str: a String until the statement ends */
    F_JSON = 1 << 15,       /* a struct with @json */
    F_REQUIRED = 1 << 16,   /* a field with @json(required) */
    F_TEMPSTR = 1 << 17,    /* a String from a call passed as a str: freed when the statement ends */
    F_INTRINSIC = 1 << 18,  /* @builtin in the standard library: the compiler makes the call */
    F_TESTFN = 1 << 19,     /* the function a test block became; text is the test's name */
    F_INSTANCE = 1 << 20,   /* a copy of a generic function or method for one set of types */
    F_READONLY = 1 << 21,   /* a global made by comptime: it can't change */
    F_COMPTIMED = 1 << 22,  /* a const whose value comptime worked out; type is its type */
    F_INLINE = 1 << 23,     /* @inline: always put the function's code where it is called */
    F_UNCHECKED = 1 << 24,  /* @unchecked: no checks in the function's code */
    F_ENV = 1 << 25,        /* a struct with @env */
    F_LAZY = 1 << 26,       /* a generic struct's method for one instance, not checked until it is used */
    F_CLI = 1 << 27,        /* a struct with @flags */
    F_REST = 1 << 28,       /* a T... parameter: the arguments left over, as a T[] */
    F_NORETURN = 1 << 29,   /* @noreturn: a call to the function never comes back, like panic */
    F_POSTFIX = 1 << 30,    /* x++ and x--: the value is the one before the change */
};
#define F_STACK (1u << 31)      /* the IR writer's: an alloc kept on the stack, and its free left out */

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
    Source *src;            /* the file it comes from */
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
    int builtin;            /* calls: BI_... */
    const char *ir;         /* the IR writer's name for a variable or function */
    void *scope;            /* top-level declarations: the checker's scope for their names */
    const char *module;     /* top-level declarations: the module's name, "main" for the program */
    int inst;               /* generic instances: a number of their own, so that List[Record] of
                             * two modules' Records get different IR names */
    /* struct fields, filled in by the checker's layout */
    int slot;               /* the LLVM element that holds the field */
    int bitOff, bitWidth;   /* bitfields: the lowest bit and how many bits; bitWidth 0 otherwise */
    int64_t offset;         /* where the field (or its bitfield's unit) starts, in bytes */
};

Node *newNode(Arena *a, NodeKind kind, int line, int col);

/* How a value in text is written, after a ':' in its braces:
 * [[fill]align][+][0][width][.precision][type]. */
typedef struct {
    int fill;               /* a code point; ' ' unless given */
    char align;             /* '<', '>', '^', or 0: numbers right, the rest left */
    bool plus, zero;        /* '+': a sign on positive numbers; '0': zeros after the sign */
    int64_t width;          /* at least this many characters; 0 for none */
    int64_t prec;           /* digits after the point, or the most characters of text; -1 for none */
    char type;              /* 'x', 'X', 'o', 'b' for integers, 'e', 'f' for floats, or 0 */
} FmtSpec;

bool fmtSpecParse(const char *s, size_t n, FmtSpec *out);
void listPush(Arena *a, NodeList *l, Node *n);

void printTree(Node *n);
char *typeString(Arena *a, Node *t);

/* generic.c: a deep copy of a tree, with the checker's fields cleared */
Node *cloneNode(Arena *a, Node *n);

/* ---------------------------------------------------------------- parser.c */

/* snippet: allow statements at the top level, for checking doc examples. */
Node *parseSource(Arena *a, Source *src, bool snippet);

/* ---------------------------------------------------------------- proc.c */

/* Runs a program and waits for it; stdout and stderr go to the files given
 * (NULL: ours). Returns its exit status, 127 if it can't run. */
int runCommand(char **argv, const char *outPath, const char *errPath);
int runCommandStarted(char **argv, const char *outPath, const char *errPath, void (*started)(void *), void *arg);
char *readAllFile(Arena *a, const char *path, size_t *len);
bool writeText(const char *path, const char *text, size_t len);

/* fmt.c: the source in the standard layout, or NULL when it has errors */
char *formatSource(Arena *a, Source *src);

/* lsp.c: the language server on standard input and output */
int cmdLsp(void);

/* doc.c: iris doc */
int cmdDoc(int argc, char **argv);

/* ---------------------------------------------------------------- cimport.c */

/* Reads C headers with clang and writes them as an Iris module: @extern
 * functions, structs, type aliases and constants. Names Iris can't use go
 * to skipped (N_IDENT nodes: name, and text says why). Returns NULL after
 * errors, which it reports. */
char *cImport(Arena *a, const char **headers, int n, const char **includeDirs, int nInclude,
              NodeList *skipped);

/* ---------------------------------------------------------------- module.c */

/* A module: a folder of files, or the one file of a small program. */
typedef struct Module Module;
struct Module {
    Module *next;
    const char *path;       /* as imported: "fs", "net/http", "github.com/x/pg"; "main" for the program */
    const char *name;       /* what code calls it: the last part of the path */
    bool isStd;             /* from the standard library inside iris */
    bool isC;               /* made by import c from C headers */
    NodeList files;         /* N_FILE nodes */
    Node *anchor;           /* stands for the module in import lists (N_IMPORT); ref of each import item */
    int state;              /* loading: 0 new, 1 loading, 2 done */
};

typedef struct {
    Module *modules;        /* every module, the program's first */
    Module *main;
    NodeList instances;     /* filled by the checker: copies of generics */
    const char *links[64];  /* C libraries to link: link lines in iris.mod */
    int nlinks;
    const char *csources[64];   /* .c files in the program's folders, compiled with it */
    int ncsources;
    NodeList tests;         /* iris test: the functions the test blocks became */
    NodeList benches;       /* iris bench: the functions the bench blocks became */
    Module *core;           /* std/core: its names are seen by every module */
    Module *text;           /* std/str: its functions are also methods of str and String */
    bool textImplicit;      /* ... loaded only for that: only what is called is compiled */
} Program;

/* iris test: test blocks are compiled, and main runs them */
extern bool gTesting;
/* iris bench: main runs the bench blocks instead */
extern bool gBenching;

/* comptime.c: 0 no comptime, 1 checking the helper, 2 the program with the values */
typedef struct {
    const char *key, *json;
} ComptimeResult;
extern int gComptime;
extern ComptimeResult *gComptimeResults;
extern int gComptimeCount;
extern char **gComptimeKeys;
extern int gComptimeKeyCount;
extern const char *gExtraPath, *gExtraText;     /* a file added to the program's module */
char *comptimeKey(Arena *a, Node *n);
char *comptimeHelperSource(Arena *a, Program *prog);
Node *comptimeLiteral(Arena *a, const char *json, Type *t, Node *at);
const char *comptimeResult(const char *key);
/* test blocks are checked (iris check and iris test) */
extern bool gWithTests;

/* Reads the program at path (a file or a folder) and every module it
 * imports. includes: folders for C headers (-I). Returns NULL after errors. */
Program *loadProgram(Arena *a, const char *path, const char **includes, int nincludes);
/* The language server's loading: a file with syntax errors still loads, from
 * what the parser could read, so names around the error stay known. */
extern bool gLenient;

/* ---------------------------------------------------------------- check.c */

typedef enum {
    TY_ERROR,               /* after an error: fits anywhere, so errors don't cascade */
    TY_VOID,
    TY_NORETURN,            /* a block that always leaves: return, break, panic */
    TY_BOOL,
    TY_INT,
    TY_FLOAT,               /* f32, f64: bits */
    TY_NULL,                /* the type of null: becomes any pointer */
    TY_PTR,
    TY_SLICE,               /* str is the slice of u8 */
    TY_ARRAY,
    TY_STRUCT,
    TY_STRING,              /* the built-in String */
    TY_ENUM,                /* count: the enum's error kind number, from 1 */
    TY_RESULT,              /* !T: elem is T (tyVoid for !void) */
    TY_ERRVAL,              /* Error: a code and, in the runtime, a message and a trace */
    TY_ERRCODE,             /* e.code: any enum value, with its enum */
    TY_TUPLE,               /* several values: elems */
    TY_DYN,                 /* dyn Interface: decl is the N_INTERFACE */
    TY_FUNC,                /* a function: elem is what it returns, elems its parameters */
    TY_CHAN,                /* chan[T]: elem */
    TY_ATOMIC,              /* Atomic[T]: elem */
    TY_MUTEX,               /* Mutex */
    TY_THREAD,              /* Thread: what thread f() gives */
} TypeKind;

struct Type {
    TypeKind kind;
    int bits;               /* integers */
    bool isSigned;
    const char *name;       /* as written in messages: u8[], Point *, str */
    Type *elem;             /* pointers, slices, arrays */
    int64_t count;          /* arrays */
    Node *decl;             /* structs: the N_STRUCT node */
    NodeList methods;       /* structs: N_FUNC nodes with a receiver */
    int state;              /* structs: 0 new, 1 resolving fields, 2 done */
    const char *ir;         /* structs and data enums: the LLVM type name */
    Type **elems;           /* tuples */
    int nelems;
    bool hasData;           /* enums whose values carry data */
    bool isFlags;           /* flag enums */
    int64_t payload;        /* data enums: bytes of room for the largest value's data */
    Node *generic;          /* instances of generic structs: the generic declaration */
    Type **args;            /* ... and the types it was made with */
    int nargs;
    void *scope;            /* instances: the checker's scope with the type parameters */
    bool bigEndian;         /* u16be, u32be, u64be, i16be...: an integer kept in network byte order */
    Type *plain;            /* ... and the integer it reads as */
    /* structs, once laid out: size, alignment and the LLVM elements, padding included */
    bool laidOut;
    int64_t size, align;
    Type **slots;           /* NULL: padding of padBytes[i] bytes */
    int64_t *padBytes;
    int nslots;
};

extern Type tyError, tyVoid, tyNoreturn, tyBool, tyNull, tyStr, tyString, tyErrorValue, tyErrCode;
extern Type tyI8, tyI16, tyI32, tyI64, tyU8, tyU16, tyU32, tyU64, tyF32, tyF64, tyDuration, tyMutex, tyThread;
int64_t typeSizeOf(Type *t);
int64_t typeAlignOf(Type *t);
int64_t fieldOffset(Type *t, int index);
bool isJsonStruct(Type *t);
bool isJsonList(Type *t);
Node *textMethod(Program *prog, const char *name);

/* What a call does when it isn't a call to a user function. */
enum {
    BI_NONE, BI_PRINT, BI_PANIC, BI_FORMAT, BI_ALLOC, BI_FREE,
    BI_STR_COPY, BI_STRING_APPEND, BI_STRING_FREE, BI_STRING_RESERVE, BI_STRING_INSERT, BI_STRING_TRUNCATE,
    BI_STRING_CLEAR, BI_ERROR,
    BI_ENUM_NEW,            /* a value of a data enum: ref is the member */
    BI_FLAGS_HAS,
    BI_DYN_CALL,            /* a call through dyn: ival is the method's place in the interface */
    BI_INDIRECT,            /* a call through a function value: a is the value */
    BI_SLEEP,
    BI_AFTER,               /* after(d), in a select */
    BI_ATOMIC_LOAD, BI_ATOMIC_STORE, BI_ATOMIC_ADD, BI_ATOMIC_SUB, BI_ATOMIC_SWAP, BI_ATOMIC_CAS,
    BI_MUTEX_LOCK, BI_MUTEX_UNLOCK, BI_MUTEX_TRY_LOCK,
    BI_THREAD_JOIN, BI_CHAN_FREE,
    BI_JSON_ENCODE, BI_JSON_DECODE, BI_JSON_FREE,
    BI_EXPECT,
    BI_HASH, BI_KEY_OWN, BI_KEY_FREE,
    BI_GROW,                /* List's: growSlice(s, n) */
    BI_TABLE_OF,
    BI_ENV_LOAD,
    BI_FLAGS_PARSE,
    BI_ARENA_ALLOC,         /* arena.alloc(T, n): b is a pointer to the Arena */
};

/* Checks the program and fills in types. Returns false after errors. */
bool checkProgram(Arena *a, Program *prog);

/* ---------------------------------------------------------------- ir.c */

typedef enum { MODE_DEBUG, MODE_RELEASE_SAFE, MODE_RELEASE_FAST } BuildMode;

/* What the program is built for: when tests os, arch and mode. triple is
 * clang's name for it, or NULL for the machine the compiler runs on. */
typedef struct {
    const char *os, *arch, *triple;
    BuildMode mode;
} Target;
extern Target gTarget;
bool setTarget(const char *name);

/* Writes the checked program as LLVM IR. */
char *writeIR(Arena *a, Program *prog, BuildMode mode);

#endif
