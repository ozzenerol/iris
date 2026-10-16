/* The iris command. */
#include "iris.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *out) {
    fputs("usage: iris <command> [arguments]\n"
          "\n"
          "commands:\n"
          "  parse [--snippet] <file>   print the syntax tree\n"
          "  lex <file>                 print the tokens\n"
          "  version                    print the version\n"
          "  help                       print this message\n",
          out);
}

static bool readFile(const char *path, Source *src) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "iris: can't open %s\n", path);
        return false;
    }
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    for (;;) {
        if (len == cap) {
            cap *= 2;
            char *p = realloc(buf, cap);
            if (!p) {
                fputs("iris: out of memory\n", stderr);
                exit(2);
            }
            buf = p;
        }
        size_t n = fread(buf + len, 1, cap - len, f);
        if (n == 0) {
            break;
        }
        len += n;
    }
    fclose(f);
    src->path = path;
    src->text = buf;
    src->len = len;
    return true;
}

static int cmdLex(const char *path) {
    Source src;
    if (!readFile(path, &src)) {
        return 1;
    }
    Arena a = {0};
    int n;
    Token *t = lexRange(&a, &src, src.text, src.text + src.len, 1, 1, &n);
    for (int i = 0; i < n; i++) {
        if (t[i].kind == T_NEWLINE || t[i].kind == T_EOF) {
            printf("%d:%d %s\n", t[i].line, t[i].col, tokName(t[i].kind));
        } else {
            printf("%d:%d %s %.*s\n", t[i].line, t[i].col, tokName(t[i].kind), t[i].len, t[i].s);
        }
    }
    arenaFree(&a);
    free((char *)src.text);
    return gErrors ? 1 : 0;
}

static int cmdParse(const char *path, bool snippet) {
    Source src;
    if (!readFile(path, &src)) {
        return 1;
    }
    Arena a = {0};
    Node *file = parseSource(&a, &src, snippet);
    if (!gErrors) {
        printTree(file);
    }
    arenaFree(&a);
    free((char *)src.text);
    return gErrors ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    const char *cmd = argv[1];
    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0) {
        printf("iris %s\n", IRIS_VERSION);
        return 0;
    }
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        usage(stdout);
        return 0;
    }
    if (strcmp(cmd, "lex") == 0 && argc == 3) {
        return cmdLex(argv[2]);
    }
    if (strcmp(cmd, "parse") == 0) {
        bool snippet = false;
        const char *path = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--snippet") == 0) {
                snippet = true;
            } else if (!path) {
                path = argv[i];
            } else {
                path = NULL;
                break;
            }
        }
        if (path) {
            return cmdParse(path, snippet);
        }
    }
    usage(stderr);
    return 2;
}
