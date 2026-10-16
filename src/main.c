/* The iris command. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE        /* mkdtemp on macOS */
#include "iris.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* runtime/rt.c, turned into bytes by the Makefile */
static const char runtimeSource[] = {
#include "runtime.inc"
    0};

static void usage(FILE *out) {
    fputs("usage: iris <command> [arguments]\n"
          "\n"
          "commands:\n"
          "  run [options] <file> [args]  compile and run a program\n"
          "  build [options] <file>       compile a program (-o <name> for the output)\n"
          "  check <file>                 check a program without compiling it\n"
          "  ir [options] <file>          print the LLVM IR\n"
          "  parse [--snippet] <file>     print the syntax tree\n"
          "  lex <file>                   print the tokens\n"
          "  version                      print the version\n"
          "  help                         print this message\n"
          "\n"
          "options:\n"
          "  --mode=debug|release-safe|release-fast   (default: debug)\n"
          "\n"
          "clang compiles the IR; set IRIS_CC to use another clang.\n",
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

/* Parses and checks a file. Returns the tree, or NULL after errors. */
static Node *frontEnd(Arena *a, Source *src, const char *path) {
    if (!readFile(path, src)) {
        return NULL;
    }
    Node *file = parseSource(a, src, false);
    if (gErrors) {
        return NULL;
    }
    if (!checkProgram(a, src, file)) {
        return NULL;
    }
    return file;
}

/* Runs a program and waits for it. With errPath, its stderr goes to that
 * file instead of ours. */
static int runProgramTo(char **argv, const char *errPath) {
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        perror("iris: fork");
        return 1;
    }
    if (pid == 0) {
        if (errPath) {
            FILE *f = freopen(errPath, "w", stderr);
            (void)f;
        }
        execvp(argv[0], argv);
        fprintf(stderr, "iris: can't run %s\n", argv[0]);
        _exit(127);
    }
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        perror("iris: waitpid");
        return 1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 1;
}

static int runProgram(char **argv) { return runProgramTo(argv, NULL); }

static bool writeText(const char *path, const char *text, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "iris: can't write %s\n", path);
        return false;
    }
    bool ok = fwrite(text, 1, len, f) == len;
    ok = fclose(f) == 0 && ok;
    return ok;
}

/* Compiles the IR with clang into out. */
static bool compileIR(const char *ir, const char *out, BuildMode mode, const char *dir) {
    char llPath[4200], rtPath[4200];
    snprintf(llPath, sizeof llPath, "%s/program.ll", dir);
    snprintf(rtPath, sizeof rtPath, "%s/rt.c", dir);
    if (!writeText(llPath, ir, strlen(ir)) || !writeText(rtPath, runtimeSource, sizeof runtimeSource - 1)) {
        return false;
    }
    const char *cc = getenv("IRIS_CC");
    if (!cc || !*cc) {
        cc = "clang";
    }
    char *args[12];
    int n = 0;
    args[n++] = (char *)cc;
    args[n++] = mode == MODE_DEBUG ? "-O0" : "-O2";
    args[n++] = "-g";
    args[n++] = "-Wno-override-module";
    args[n++] = "-w";
    int asan = n;
    if (mode == MODE_DEBUG) {
        args[n++] = "-fsanitize=address";   /* use after free, out-of-bounds memory */
    }
    args[n++] = llPath;
    args[n++] = rtPath;
    args[n++] = "-o";
    args[n++] = (char *)out;
    args[n] = NULL;
    int rc;
    if (mode == MODE_DEBUG) {
        /* not every clang has AddressSanitizer's libraries: build without
         * it when linking with it fails */
        char errPath[4200];
        snprintf(errPath, sizeof errPath, "%s/clang.err", dir);
        rc = runProgramTo(args, errPath);
        if (rc != 0 && rc != 127) {
            for (int i = asan; args[i]; i++) {
                args[i] = args[i + 1];
            }
            rc = runProgram(args);
            if (rc == 0) {
                fprintf(stderr, "iris: note: this clang can't use AddressSanitizer, so this debug build doesn't "
                                "check for use after free\n");
            }
        }
        unlink(errPath);
    } else {
        rc = runProgram(args);
    }
    if (rc == 127) {
        fprintf(stderr, "iris: clang is needed to build programs; install it or set IRIS_CC\n");
    }
    return rc == 0;
}

static void removeTree(const char *dir) {
    const char *names[] = {"program.ll", "rt.c", "program"};
    char path[4200];
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        unlink(path);
    }
    rmdir(dir);
}

static bool makeTempDir(char *buf, size_t n) {
    const char *base = getenv("TMPDIR");
    if (!base || !*base) {
        base = "/tmp";
    }
    snprintf(buf, n, "%s/iris-XXXXXX", base);
    if (!mkdtemp(buf)) {
        perror("iris: mkdtemp");
        return false;
    }
    return true;
}

static bool parseMode(const char *arg, BuildMode *mode) {
    if (strcmp(arg, "--mode=debug") == 0) {
        *mode = MODE_DEBUG;
    } else if (strcmp(arg, "--mode=release-safe") == 0) {
        *mode = MODE_RELEASE_SAFE;
    } else if (strcmp(arg, "--mode=release-fast") == 0) {
        *mode = MODE_RELEASE_FAST;
    } else {
        return false;
    }
    return true;
}

/* iris run, build, check and ir */
static int cmdCompile(const char *cmd, int argc, char **argv) {
    BuildMode mode = MODE_DEBUG;
    const char *out = NULL, *path = NULL;
    int i = 2;
    for (; i < argc; i++) {
        if (strncmp(argv[i], "--mode=", 7) == 0) {
            if (!parseMode(argv[i], &mode)) {
                fprintf(stderr, "iris: unknown mode %s\n", argv[i] + 7);
                return 2;
            }
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc && strcmp(cmd, "build") == 0) {
            out = argv[++i];
        } else if (argv[i][0] == '-' && argv[i][1]) {
            fprintf(stderr, "iris: unknown option %s\n", argv[i]);
            return 2;
        } else {
            path = argv[i++];
            break;
        }
    }
    if (!path || (i < argc && strcmp(cmd, "run") != 0)) {
        usage(stderr);
        return 2;
    }

    Arena a = {0};
    Source src = {0};
    Node *file = frontEnd(&a, &src, path);
    int rc = 1;
    if (!file) {
        goto done;
    }
    if (strcmp(cmd, "check") == 0) {
        rc = 0;
        goto done;
    }
    char *ir = writeIR(&a, &src, file, mode);
    if (strcmp(cmd, "ir") == 0) {
        fputs(ir, stdout);
        rc = 0;
        goto done;
    }
    char dir[4096];
    if (!makeTempDir(dir, sizeof dir)) {
        goto done;
    }
    if (strcmp(cmd, "build") == 0) {
        char def[4096];
        if (!out) {
            /* hello.iris -> hello */
            const char *base = strrchr(path, '/');
            base = base ? base + 1 : path;
            snprintf(def, sizeof def, "%s", base);
            char *dot = strrchr(def, '.');
            if (dot && dot != def) {
                *dot = 0;
            } else {
                snprintf(def, sizeof def, "a.out");
            }
            out = def;
        }
        rc = compileIR(ir, out, mode, dir) ? 0 : 1;
        removeTree(dir);
        goto done;
    }
    char exe[4200];
    snprintf(exe, sizeof exe, "%s/program", dir);
    if (!compileIR(ir, exe, mode, dir)) {
        removeTree(dir);
        goto done;
    }
    {
        int n = argc - i;
        char **args = calloc((size_t)n + 2, sizeof(char *));
        args[0] = exe;
        for (int k = 0; k < n; k++) {
            args[k + 1] = argv[i + k];
        }
        fflush(stdout);
        rc = runProgram(args);
        free(args);
    }
    removeTree(dir);
done:
    arenaFree(&a);
    free((char *)src.text);
    return rc;
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
    if (strcmp(cmd, "run") == 0 || strcmp(cmd, "build") == 0 || strcmp(cmd, "check") == 0 ||
        strcmp(cmd, "ir") == 0) {
        return cmdCompile(cmd, argc, argv);
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
