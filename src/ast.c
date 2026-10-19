/* Syntax tree nodes, and the printer behind `iris parse`.
 *
 * The printer is also the reference for which fields each node kind uses. */
#include "iris.h"

#include <stdio.h>
#include <string.h>

Node *newNode(Arena *a, NodeKind kind, int line, int col) {
    Node *n = arenaAlloc(a, sizeof(Node));
    n->kind = kind;
    n->line = line;
    n->col = col;
    return n;
}

void listPush(Arena *a, NodeList *l, Node *n) {
    if (l->len == l->cap) {
        int cap = l->cap ? l->cap * 2 : 4;
        Node **items = arenaAlloc(a, sizeof(Node *) * (size_t)cap);
        if (l->len) {
            memcpy(items, l->items, sizeof(Node *) * (size_t)l->len);
        }
        l->items = items;
        l->cap = cap;
    }
    l->items[l->len++] = n;
}

/* ---------------------------------------------------------------- types */

static void putType(StrBuf *b, Node *t) {
    if (!t) {
        sbPuts(b, "?");
        return;
    }
    switch (t->kind) {
    case N_T_NAME:
        sbPuts(b, t->name);
        break;
    case N_T_PTR:
        putType(b, t->a);
        sbPuts(b, t->a->kind == N_T_PTR ? "*" : " *");
        break;
    case N_T_SLICE:
        putType(b, t->a);
        sbPuts(b, "[]");
        break;
    case N_T_ARRAY:
        putType(b, t->a);
        sbPrintf(b, "[%llu]", (unsigned long long)t->b->ival);
        break;
    case N_T_GENERIC:
    case N_T_FUNC:
        putType(b, t->a);
        sbPutc(b, t->kind == N_T_GENERIC ? '[' : '(');
        for (int i = 0; i < t->list.len; i++) {
            if (i) {
                sbPuts(b, ", ");
            }
            putType(b, t->list.items[i]);
        }
        sbPutc(b, t->kind == N_T_GENERIC ? ']' : ')');
        break;
    case N_T_TUPLE:
        sbPutc(b, '(');
        for (int i = 0; i < t->list.len; i++) {
            if (i) {
                sbPuts(b, ", ");
            }
            putType(b, t->list.items[i]);
        }
        sbPutc(b, ')');
        break;
    case N_T_ERR:
        sbPutc(b, '!');
        putType(b, t->a);
        break;
    case N_T_DYN:
        sbPuts(b, "dyn ");
        putType(b, t->a);
        break;
    case N_T_CHAN:
        sbPuts(b, "chan[");
        putType(b, t->a);
        sbPutc(b, ']');
        break;
    default:
        sbPuts(b, "?");
        break;
    }
}

char *typeString(Arena *a, Node *t) {
    StrBuf b = {0};
    putType(&b, t);
    return sbFinish(&b, a);
}

/* ---------------------------------------------------------------- printer */

static void putQuoted(StrBuf *b, const char *s, size_t n) {
    sbPutc(b, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '\n': sbPuts(b, "\\n"); break;
        case '\t': sbPuts(b, "\\t"); break;
        case '\r': sbPuts(b, "\\r"); break;
        case '\\': sbPuts(b, "\\\\"); break;
        case '"': sbPuts(b, "\\\""); break;
        default:
            if (c < 32 || c >= 127) {
                sbPrintf(b, "\\x%02x", c);
            } else {
                sbPutc(b, (char)c);
            }
        }
    }
    sbPutc(b, '"');
}

static void flagWords(StrBuf *b, unsigned f) {
    if (f & F_PUB) sbPuts(b, " pub");
    if (f & F_SCOPED) sbPuts(b, " scoped");
    if (f & F_THREADLOCAL) sbPuts(b, " threadlocal");
    if (f & F_PACKED) sbPuts(b, " packed");
    if (f & F_FLAGS) sbPuts(b, " flags");
}

static void printNode(Node *n, int depth, const char *label);

static void line(int depth, const char *label, const char *text) {
    printf("%*s%s%s%s\n", depth * 2, "", label ? label : "", label ? ": " : "", text);
}

static void child(Node *n, int depth, const char *label) {
    if (n) {
        printNode(n, depth, label);
    }
}

static void children(NodeList *l, int depth, const char *label) {
    for (int i = 0; i < l->len; i++) {
        printNode(l->items[i], depth, label);
    }
}

static void printNode(Node *n, int depth, const char *label) {
    StrBuf b = {0};
    Arena tmp = {0};
    int d = depth + 1;

    switch (n->kind) {
    case N_FILE:
        line(depth, label, "file");
        children(&n->list, d, NULL);
        break;
    case N_IMPORT:
        line(depth, label, "import");
        children(&n->list, d, NULL);
        break;
    case N_IMPORT_ITEM:
        if (n->flags & F_C_IMPORT) {
            sbPuts(&b, "c ");
            putQuoted(&b, n->text, strlen(n->text));
        } else if (n->text) {
            sbPuts(&b, "path ");
            putQuoted(&b, n->text, strlen(n->text));
        } else {
            sbPrintf(&b, "module %s", n->name);
        }
        line(depth, label, b.buf);
        break;
    case N_ATTR:
        sbPrintf(&b, "attr @%s", n->name);
        line(depth, label, b.buf);
        children(&n->list, d, NULL);
        break;
    case N_FUNC:
        if (n->b) {
            sbPrintf(&b, "method %s.%s", typeString(&tmp, n->b), n->name);
        } else {
            sbPrintf(&b, "func %s", n->name);
        }
        flagWords(&b, n->flags);
        line(depth, label, b.buf);
        children(&n->attrs, d, NULL);
        children(&n->list2, d, NULL);
        children(&n->list, d, NULL);
        line(d, "ret", typeString(&tmp, n->a));
        child(n->c, d, "body");
        child(n->d, d, "body");
        break;
    case N_TYPE_PARAM:
        sbPrintf(&b, "typeparam %s", n->name);
        if (n->a) {
            sbPrintf(&b, ": %s", typeString(&tmp, n->a));
        }
        line(depth, label, b.buf);
        break;
    case N_PARAM:
        sbPrintf(&b, "param %s %s%s", n->name, typeString(&tmp, n->a), n->flags & F_REST ? " rest" : "");
        line(depth, label, b.buf);
        child(n->b, d, "default");
        break;
    case N_STRUCT:
        sbPrintf(&b, "struct %s", n->name);
        flagWords(&b, n->flags);
        line(depth, label, b.buf);
        children(&n->attrs, d, NULL);
        children(&n->list2, d, NULL);
        child(n->a, d, "align");
        children(&n->list, d, NULL);
        break;
    case N_FIELD:
        sbPrintf(&b, "field %s %s", n->name, typeString(&tmp, n->a));
        flagWords(&b, n->flags);
        line(depth, label, b.buf);
        children(&n->attrs, d, NULL);
        child(n->b, d, "default");
        child(n->c, d, "bits");
        break;
    case N_EMBED:
        sbPrintf(&b, "embed %s", typeString(&tmp, n->a));
        line(depth, label, b.buf);
        children(&n->attrs, d, NULL);
        break;
    case N_ENUM:
        sbPrintf(&b, "enum %s", n->name);
        flagWords(&b, n->flags);
        line(depth, label, b.buf);
        children(&n->attrs, d, NULL);
        children(&n->list, d, NULL);
        break;
    case N_ENUM_MEMBER:
        sbPrintf(&b, "member %s", n->name);
        line(depth, label, b.buf);
        children(&n->list, d, NULL);
        child(n->b, d, "value");
        break;
    case N_INTERFACE:
        sbPrintf(&b, "interface %s", n->name);
        flagWords(&b, n->flags);
        line(depth, label, b.buf);
        children(&n->list, d, NULL);
        break;
    case N_CONST:
        sbPrintf(&b, "const %s", n->name);
        if (n->a) {
            sbPrintf(&b, " %s", typeString(&tmp, n->a));
        }
        flagWords(&b, n->flags);
        line(depth, label, b.buf);
        children(&n->attrs, d, NULL);
        child(n->b, d, "value");
        break;
    case N_TYPEDEF:
        sbPrintf(&b, "type %s = %s", n->name, typeString(&tmp, n->a));
        flagWords(&b, n->flags);
        line(depth, label, b.buf);
        break;
    case N_VAR:
        sbPrintf(&b, "var %s %s", n->name, n->a ? typeString(&tmp, n->a) : ":=");
        flagWords(&b, n->flags);
        line(depth, label, b.buf);
        children(&n->attrs, d, NULL);
        child(n->b, d, "value");
        break;
    case N_TEST:
        sbPuts(&b, "test ");
        putQuoted(&b, n->text, strlen(n->text));
        line(depth, label, b.buf);
        child(n->c, d, "body");
        break;
    case N_WHEN:
        line(depth, label, "when");
        child(n->a, d, "cond");
        child(n->c, d, "then");
        child(n->d, d, "else");
        break;

    case N_BLOCK:
        line(depth, label, "block");
        children(&n->list, d, NULL);
        break;
    case N_EXPR_STMT:
        printNode(n->a, depth, label);
        break;
    case N_DEFINE:
        sbPuts(&b, "define");
        for (int i = 0; i < n->list.len; i++) {
            sbPrintf(&b, "%s %s", i ? "," : "", n->list.items[i]->name);
        }
        sbPrintf(&b, "%s", "");
        line(depth, label, b.buf);
        child(n->b, d, "value");
        break;
    case N_ASSIGN:
        sbPrintf(&b, "assign %s", tokName(n->op));
        line(depth, label, b.buf);
        children(&n->list, d, "target");
        child(n->b, d, "value");
        break;
    case N_RETURN:
        line(depth, label, "return");
        child(n->a, d, NULL);
        break;
    case N_BREAK:
    case N_CONTINUE:
        sbPuts(&b, n->kind == N_BREAK ? "break" : "continue");
        if (n->name) {
            sbPrintf(&b, " %s", n->name);
        }
        line(depth, label, b.buf);
        break;
    case N_IF:
        line(depth, label, "if");
        child(n->a, d, "cond");
        child(n->c, d, "then");
        child(n->d, d, "else");
        break;
    case N_FOR:
        sbPuts(&b, "for");
        if (n->flags & F_FOR_IN) {
            for (int i = 0; i < n->list.len; i++) {
                sbPrintf(&b, "%s %s", i ? "," : "", n->list.items[i]->name);
            }
            sbPuts(&b, " in");
        }
        line(depth, label, b.buf);
        child(n->a, d, (n->flags & F_FOR_IN) ? "iter" : "cond");
        child(n->c, d, "body");
        break;
    case N_MATCH:
        line(depth, label, "match");
        child(n->a, d, "subject");
        children(&n->list, d, NULL);
        break;
    case N_ARM:
        line(depth, label, "arm");
        children(&n->list, d, "pattern");
        child(n->c, d, "body");
        break;
    case N_DEFER:
    case N_ERRDEFER:
    case N_SPAWN:
    case N_THREAD:
        line(depth, label, n->kind == N_DEFER ? "defer" : n->kind == N_ERRDEFER ? "errdefer" :
                           n->kind == N_THREAD ? "thread" : "spawn");
        child(n->a, d, NULL);
        break;
    case N_SELECT:
        line(depth, label, "select");
        children(&n->list, d, NULL);
        break;
    case N_SELECT_ARM:
        sbPuts(&b, "arm");
        if (n->name) {
            sbPrintf(&b, " %s :=", n->name);
        }
        line(depth, label, b.buf);
        child(n->a, d, "recv");
        child(n->c, d, "body");
        break;
    case N_ASM:
        sbPuts(&b, "asm ");
        putQuoted(&b, n->text, n->textLen);
        line(depth, label, b.buf);
        break;
    case N_LABELED:
        sbPrintf(&b, "label %s", n->name);
        line(depth, label, b.buf);
        child(n->a, d, NULL);
        break;

    case N_IDENT:
        sbPrintf(&b, "name %s", n->name);
        line(depth, label, b.buf);
        break;
    case N_INT:
        sbPrintf(&b, "int %llu", (unsigned long long)n->ival);
        if (n->name) {
            sbPrintf(&b, " %s", n->name);
        }
        line(depth, label, b.buf);
        break;
    case N_FLOAT:
        sbPrintf(&b, "float %g", n->fval);
        if (n->name) {
            sbPrintf(&b, " %s", n->name);
        }
        line(depth, label, b.buf);
        break;
    case N_STRING:
        sbPuts(&b, "str ");
        putQuoted(&b, n->text, n->textLen);
        line(depth, label, b.buf);
        break;
    case N_INTERP:
        line(depth, label, "interp");
        children(&n->list, d, NULL);
        break;
    case N_CHAR:
        sbPrintf(&b, "char %llu", (unsigned long long)n->ival);
        line(depth, label, b.buf);
        break;
    case N_BOOL:
        line(depth, label, n->ival ? "true" : "false");
        break;
    case N_NULL:
        line(depth, label, "null");
        break;
    case N_UNARY:
        sbPrintf(&b, "unary %s", tokName(n->op));
        line(depth, label, b.buf);
        child(n->a, d, NULL);
        break;
    case N_INCDEC:
        sbPrintf(&b, "%s %s", n->flags & F_POSTFIX ? "postfix" : "prefix", tokName(n->op));
        line(depth, label, b.buf);
        child(n->a, d, NULL);
        break;
    case N_BINARY:
        sbPrintf(&b, "binary %s", tokName(n->op));
        line(depth, label, b.buf);
        child(n->a, d, NULL);
        child(n->b, d, NULL);
        break;
    case N_CALL:
        line(depth, label, "call");
        child(n->a, d, "callee");
        children(&n->list, d, NULL);
        break;
    case N_NAMED_ARG:
        sbPrintf(&b, "arg %s", n->name);
        line(depth, label, b.buf);
        child(n->a, d, NULL);
        break;
    case N_INDEX:
        line(depth, label, "index");
        child(n->a, d, "of");
        children(&n->list, d, NULL);
        break;
    case N_SLICE:
        line(depth, label, "slice");
        child(n->a, d, "of");
        child(n->b, d, "from");
        child(n->c, d, "to");
        break;
    case N_FIELD_ACCESS:
        sbPrintf(&b, "field .%s", n->name);
        line(depth, label, b.buf);
        child(n->a, d, NULL);
        break;
    case N_ENUM_DOT:
        sbPrintf(&b, "dot .%s", n->name);
        line(depth, label, b.buf);
        break;
    case N_STRUCT_LIT:
        line(depth, label, n->a ? "struct-lit" : "struct-lit .");
        child(n->a, d, "type");
        children(&n->list, d, NULL);
        break;
    case N_FIELD_INIT:
        sbPrintf(&b, "init %s", n->name);
        line(depth, label, b.buf);
        child(n->a, d, NULL);
        break;
    case N_ARRAY_LIT:
        line(depth, label, "array");
        children(&n->list, d, NULL);
        break;
    case N_TUPLE:
        line(depth, label, "tuple");
        children(&n->list, d, NULL);
        break;
    case N_TRY:
        line(depth, label, "try");
        child(n->a, d, NULL);
        break;
    case N_CATCH:
        sbPuts(&b, "catch");
        if (n->name) {
            sbPrintf(&b, " %s", n->name);
        }
        line(depth, label, b.buf);
        child(n->a, d, "call");
        child(n->b, d, "value");
        child(n->c, d, "handler");
        break;
    case N_COMPTIME:
        line(depth, label, "comptime");
        child(n->a, d, NULL);
        break;
    case N_CHAN_MAKE:
        sbPrintf(&b, "chan %s", typeString(&tmp, n->a));
        line(depth, label, b.buf);
        child(n->b, d, "size");
        break;
    default:
        if (n->kind >= N_T_NAME && n->kind <= N_T_CHAN) {
            sbPrintf(&b, "type %s", typeString(&tmp, n));
            line(depth, label, b.buf);
        } else {
            line(depth, label, "?");
        }
        break;
    }
    sbFree(&b);
    arenaFree(&tmp);
}

void printTree(Node *n) { printNode(n, 0, NULL); }
