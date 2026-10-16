/* Copies of syntax trees, for generics: each use of a generic struct or
 * function with new types gets its own copy of the declaration, which the
 * checker then checks like any other code. Copies are made from the
 * generic declaration, which itself is never checked, so the fields the
 * checker fills in are still empty. */
#include "iris.h"

#include <string.h>

static void cloneList(Arena *a, NodeList *dst, NodeList *src) {
    *dst = (NodeList){0};
    for (int i = 0; i < src->len; i++) {
        listPush(a, dst, cloneNode(a, src->items[i]));
    }
}

Node *cloneNode(Arena *a, Node *n) {
    if (!n) {
        return NULL;
    }
    Node *c = arenaAlloc(a, sizeof(Node));
    *c = *n;
    c->a = cloneNode(a, n->a);
    c->b = cloneNode(a, n->b);
    c->c = cloneNode(a, n->c);
    c->d = cloneNode(a, n->d);
    cloneList(a, &c->list, &n->list);
    cloneList(a, &c->list2, &n->list2);
    cloneList(a, &c->attrs, &n->attrs);
    c->type = NULL;
    c->ref = NULL;
    c->isConst = false;
    c->cval = 0;
    c->state = 0;
    c->builtin = 0;
    c->ir = NULL;
    return c;
}
