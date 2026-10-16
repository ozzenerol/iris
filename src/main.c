/* The iris command. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE        /* mkdtemp on macOS */
#include "iris.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
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
          "  test [options] [file] [name] run the test blocks (those whose name has <name>)\n"
          "  ir [options] <file>          print the LLVM IR\n"
          "  fmt [--check] <file|folder>  put code in the standard layout (- for stdin)\n"
          "  get <package>@<version>      download a package and add it to iris.mod\n"
          "  lsp                          the language server, for editors\n"
          "  c <header.h>...              show what import c makes of C headers\n"
          "  parse [--snippet] <file>     print the syntax tree\n"
          "  lex <file>                   print the tokens\n"
          "  version                      print the version\n"
          "  help                         print this message\n"
          "\n"
          "options:\n"
          "  --mode=debug|release-safe|release-fast   (default: debug)\n"
          "  --target=<os>   build for another platform: x86_64-linux, aarch64-linux,\n"
          "                  x86_64-macos, aarch64-macos\n"
          "  -I <folder>      where C headers are, for import c\n"
          "  -l <library>     link a C library: -l sqlite3\n"
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

/* Reads, parses and checks a program and its modules. Returns NULL after
 * errors. */
static Program *frontEnd(Arena *a, const char *path, const char **includes, int nincludes) {
    Program *prog = loadProgram(a, path, includes, nincludes);
    if (!prog || gErrors) {
        return NULL;
    }
    if (!checkProgram(a, prog)) {
        return NULL;
    }
    return prog;
}

static int runProgramTo(char **argv, const char *errPath) { return runCommand(argv, NULL, errPath); }

static int runProgram(char **argv) { return runCommand(argv, NULL, NULL); }

/* Compiles the IR with clang into out, linking the C libraries named. */
static bool compileIR(const char *ir, const char *out, BuildMode mode, const char *dir, const char **links,
                      int nlinks) {
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
    char *args[200];
    char linkArgs[128][300];
    int n = 0;
    args[n++] = (char *)cc;
    args[n++] = mode == MODE_DEBUG ? "-O0" : "-O2";
    args[n++] = "-g";
    args[n++] = "-Wno-override-module";
    args[n++] = "-w";
    char sysroot[4200];
    if (gTarget.triple) {
        args[n++] = "-target";
        args[n++] = (char *)gTarget.triple;
        const char *sr = getenv("IRIS_SYSROOT");
        if (sr && *sr) {
            snprintf(sysroot, sizeof sysroot, "--sysroot=%s", sr);
            args[n++] = sysroot;
        }
    }
    int asan = n;
    if (mode == MODE_DEBUG) {
        args[n++] = "-fsanitize=address";   /* use after free, out-of-bounds memory */
    }
    args[n++] = llPath;
    args[n++] = rtPath;
    args[n++] = "-o";
    args[n++] = (char *)out;
    for (int i = 0; i < nlinks && i < 128; i++) {
        snprintf(linkArgs[i], sizeof linkArgs[i], "-l%s", links[i]);
        args[n++] = linkArgs[i];
    }
    args[n++] = "-lm";              /* the math module's functions */
    args[n++] = "-pthread";         /* http.serve's threads */
    if (strcmp(gTarget.os, "linux") == 0) {
        args[n++] = "-ldl";         /* TLS loads OpenSSL when it is first used */
    }
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
    } else if (rc != 0 && gTarget.triple) {
        fprintf(stderr, "iris: building for %s-%s needs that platform's C library and linker; set IRIS_SYSROOT "
                        "to its files (see Tools > Other platforms in the docs)\n",
                gTarget.arch, gTarget.os);
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
    const char *includes[64], *links[64];
    int nincludes = 0, nlinks = 0;
    int i = 2;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "-I") == 0 && i + 1 < argc && nincludes < 64) {
            includes[nincludes++] = argv[++i];
        } else if (strncmp(argv[i], "-I", 2) == 0 && argv[i][2] && nincludes < 64) {
            includes[nincludes++] = argv[i] + 2;
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc && nlinks < 64) {
            links[nlinks++] = argv[++i];
        } else if (strncmp(argv[i], "-l", 2) == 0 && argv[i][2] && nlinks < 64) {
            links[nlinks++] = argv[i] + 2;
        } else if (strncmp(argv[i], "--mode=", 7) == 0) {
            if (!parseMode(argv[i], &mode)) {
                fprintf(stderr, "iris: unknown mode %s\n", argv[i] + 7);
                return 2;
            }
        } else if (strncmp(argv[i], "--target=", 9) == 0) {
            if (!setTarget(argv[i] + 9)) {
                fprintf(stderr, "iris: unknown target %s; the targets are x86_64-linux, aarch64-linux, "
                                "x86_64-macos and aarch64-macos\n", argv[i] + 9);
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
    bool test = strcmp(cmd, "test") == 0;
    gWithTests = test || strcmp(cmd, "check") == 0;
    if (test) {
        gTesting = true;
        if (!path) {
            path = ".";
        }
    }
    if (!path || (i < argc && strcmp(cmd, "run") != 0 && !test)) {
        usage(stderr);
        return 2;
    }

    gTarget.mode = mode;
    Arena a = {0};
    Program *prog = frontEnd(&a, path, includes, nincludes);
    int rc = 1;
    if (!prog) {
        goto done;
    }
    if (strcmp(cmd, "check") == 0) {
        rc = 0;
        goto done;
    }
    char *ir = writeIR(&a, prog, mode);
    if (strcmp(cmd, "ir") == 0) {
        fputs(ir, stdout);
        rc = 0;
        goto done;
    }
    const char *allLinks[128];
    int nAll = 0;
    for (int k = 0; k < prog->nlinks; k++) allLinks[nAll++] = prog->links[k];
    for (int k = 0; k < nlinks; k++) allLinks[nAll++] = links[k];
    char dir[4096];
    if (!makeTempDir(dir, sizeof dir)) {
        goto done;
    }
    if (strcmp(cmd, "build") == 0) {
        char def[4096];
        if (!out) {
            /* hello.iris -> hello; a folder -> its name */
            char trimmed[4096];
            snprintf(trimmed, sizeof trimmed, "%s", path);
            size_t tl = strlen(trimmed);
            while (tl > 1 && trimmed[tl - 1] == '/') {
                trimmed[--tl] = 0;
            }
            const char *base = strrchr(trimmed, '/');
            base = base ? base + 1 : trimmed;
            snprintf(def, sizeof def, "%s", base);
            char *dot = strrchr(def, '.');
            if (dot && dot != def && strcmp(dot, ".iris") == 0) {
                *dot = 0;
            } else if (strcmp(def, ".") == 0 || strcmp(def, "..") == 0) {
                snprintf(def, sizeof def, "a.out");
            }
            out = def;
        }
        rc = compileIR(ir, out, mode, dir, allLinks, nAll) ? 0 : 1;
        removeTree(dir);
        goto done;
    }
    char exe[4200];
    snprintf(exe, sizeof exe, "%s/program", dir);
    if (!compileIR(ir, exe, mode, dir, allLinks, nAll)) {
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
    return rc;
}

/* The folder with iris.mod, from the current folder upward. */
static bool findProject(char *out, size_t n) {
    char cur[4096];
    if (!getcwd(cur, sizeof cur)) {
        return false;
    }
    for (;;) {
        char mod[4200];
        snprintf(mod, sizeof mod, "%s/iris.mod", cur);
        FILE *f = fopen(mod, "r");
        if (f) {
            fclose(f);
            snprintf(out, n, "%s", cur);
            return true;
        }
        char *slash = strrchr(cur, '/');
        if (!slash || slash == cur) {
            return false;
        }
        *slash = 0;
    }
}

/* iris get github.com/someone/pg@v1.2.0: clone the tag into the package
 * cache and put a require line in iris.mod. */
static int cmdGet(const char *spec) {
    char root[4096];
    if (!findProject(root, sizeof root)) {
        fprintf(stderr, "iris: get needs an iris.mod in this folder or one above it\n");
        return 1;
    }
    const char *at = strrchr(spec, '@');
    if (!at || at == spec || !at[1]) {
        fprintf(stderr, "iris: give the version to get: iris get %s@v1.0.0\n", spec);
        return 2;
    }
    char path[1024], version[256];
    snprintf(path, sizeof path, "%.*s", (int)(at - spec), spec);
    snprintf(version, sizeof version, "%s", at + 1);
    if (!strchr(path, '.') || strstr(path, "..") || path[0] == '/') {
        fprintf(stderr, "iris: %s isn't a package path, like github.com/someone/name\n", path);
        return 2;
    }

    const char *cache = getenv("IRIS_PATH");
    char cacheBuf[4096];
    if (!cache || !*cache) {
        const char *home = getenv("HOME");
        snprintf(cacheBuf, sizeof cacheBuf, "%s/.cache/iris", home ? home : ".");
        cache = cacheBuf;
    }
    char dir[6000];
    snprintf(dir, sizeof dir, "%s/pkg/%s@%s", cache, path, version);
    FILE *probe = fopen(dir, "r");
    if (probe) {
        fclose(probe);
    } else {
        char parent[6000];
        snprintf(parent, sizeof parent, "%s", dir);
        char *slash = strrchr(parent, '/');
        *slash = 0;
        char *mk[] = {"mkdir", "-p", parent, NULL};
        if (runProgram(mk) != 0) {
            return 1;
        }
        const char *base = getenv("IRIS_GET_BASE");    /* for tests: a folder of git repositories */
        char url[2048];
        if (base && *base) {
            snprintf(url, sizeof url, "%s/%s", base, path);
        } else {
            snprintf(url, sizeof url, "https://%s", path);
        }
        char *git[] = {"git", "-c", "advice.detachedHead=false", "clone", "--quiet", "--depth", "1",
                       "--branch", version, url, dir, NULL};
        int rc = runProgram(git);
        if (rc == 127) {
            fprintf(stderr, "iris: git is needed to get packages\n");
            return 1;
        }
        if (rc != 0) {
            fprintf(stderr, "iris: can't get %s at %s\n", path, version);
            return 1;
        }
    }

    /* iris.mod: replace an old require line for the package, or add one */
    char modPath[4200];
    snprintf(modPath, sizeof modPath, "%s/iris.mod", root);
    Source src;
    if (!readFile(modPath, &src)) {
        return 1;
    }
    StrBuf out = {0};
    bool done = false;
    const char *p = src.text, *end = src.text + src.len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        char word[16], pkg[1024];
        char line[2048];
        snprintf(line, sizeof line, "%.*s", (int)len, p);
        if (sscanf(line, "%15s %1023s", word, pkg) == 2 && strcmp(word, "require") == 0 && strcmp(pkg, path) == 0) {
            sbPrintf(&out, "require %s %s\n", path, version);
            done = true;
        } else {
            sbPutn(&out, p, len);
            sbPutc(&out, '\n');
        }
        p = nl ? nl + 1 : end;
    }
    if (!done) {
        sbPrintf(&out, "require %s %s\n", path, version);
    }
    free((char *)src.text);
    bool ok = writeText(modPath, out.buf, out.len);
    sbFree(&out);
    if (ok) {
        printf("got %s %s\n", path, version);
    }
    return ok ? 0 : 1;
}

/* iris fmt on one file: 0 when it is in the layout (or now is), 1 when
 * --check found it isn't, 2 on an error. */
static int fmtFile(const char *path, bool check) {
    Source src;
    if (strcmp(path, "-") == 0) {
        StrBuf in = {0};
        int c;
        while ((c = getchar()) != EOF) sbPutc(&in, (char)c);
        src.path = "<stdin>";
        src.text = in.buf ? in.buf : "";
        src.len = in.len;
    } else if (!readFile(path, &src)) {
        return 2;
    }
    Arena a = {0};
    char *out = formatSource(&a, &src);
    int rc = 0;
    if (!out) {
        rc = 2;
    } else if (strcmp(path, "-") == 0) {
        fputs(out, stdout);
    } else if (strlen(out) != src.len || memcmp(out, src.text, src.len) != 0) {
        if (check) {
            printf("%s\n", path);
            rc = 1;
        } else if (!writeText(path, out, strlen(out))) {
            fprintf(stderr, "iris fmt: can't write %s\n", path);
            rc = 2;
        }
    }
    arenaFree(&a);
    if (src.len || strcmp(path, "-") != 0) free((char *)src.text);
    return rc;
}

/* Every .iris file in a folder and the folders inside it. */
static int fmtTree(const char *path, bool check) {
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "iris fmt: can't find %s\n", path);
        return 2;
    }
    if (!S_ISDIR(st.st_mode)) {
        return fmtFile(path, check);
    }
    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, "iris fmt: can't read %s\n", path);
        return 2;
    }
    int worst = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char sub[4096];
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        size_t n = strlen(e->d_name);
        struct stat es;
        if (stat(sub, &es) != 0) continue;
        int rc = 0;
        if (S_ISDIR(es.st_mode)) {
            rc = fmtTree(sub, check);
        } else if (n > 5 && strcmp(e->d_name + n - 5, ".iris") == 0) {
            rc = fmtFile(sub, check);
        }
        if (rc > worst) worst = rc;
    }
    closedir(d);
    return worst;
}

static int cmdFmt(int argc, char **argv) {
    bool check = false;
    int worst = 0, files = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--check") == 0) {
            check = true;
            continue;
        }
        files++;
        int rc = strcmp(argv[i], "-") == 0 ? fmtFile("-", false) : fmtTree(argv[i], check);
        if (rc > worst) worst = rc;
    }
    if (!files) {
        worst = fmtTree(".", check);
    }
    return worst;
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
        strcmp(cmd, "ir") == 0 || strcmp(cmd, "test") == 0) {
        return cmdCompile(cmd, argc, argv);
    }
    if (strcmp(cmd, "c") == 0 && argc >= 3) {
        Arena a = {0};
        NodeList skipped = {0};
        const char *here[] = {"."};
        char *text = cImport(&a, (const char **)argv + 2, argc - 2, here, 1, &skipped);
        if (text) {
            fputs(text, stdout);
            for (int i = 0; i < skipped.len; i++) {
                printf("// left out: %s (%s)\n", skipped.items[i]->name, skipped.items[i]->text);
            }
        }
        arenaFree(&a);
        return text ? 0 : 1;
    }
    if (strcmp(cmd, "lsp") == 0) {
        return cmdLsp();
    }
    if (strcmp(cmd, "fmt") == 0) {
        return cmdFmt(argc, argv);
    }
    if (strcmp(cmd, "get") == 0 && argc == 3) {
        return cmdGet(argv[2]);
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
