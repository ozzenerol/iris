/* The iris command. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE        /* mkdtemp on macOS */
#include "iris.h"

#include <stdint.h>
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
          "  bench [options] [file] [name] run the bench blocks, built with release-fast\n"
          "                               (--time=<duration> per bench, default 1s)\n"
          "  ir [options] <file>          print the LLVM IR\n"
          "  fmt [--check] <file|folder>  put code in the standard layout (- for stdin)\n"
          "  mod init [path]              make iris.mod: this folder becomes a project\n"
          "  get <package>@<version>      download a package and add it to iris.mod\n"
          "  lsp                          the language server, for editors\n"
          "  doc <module>[.name]          the documentation of a module, a file or a name\n"
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
          "  --cpu=<name>    release builds: use what this CPU has (native: the one\n"
          "                  building), such as AVX2; the program may not run on older ones\n"
          "  --pgo-gen       release builds: a program that records where it spends its\n"
          "                  time, in iris-pgo/ when it runs\n"
          "  --pgo=<path>    release builds: optimize with what --pgo-gen recorded\n"
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

static bool compileIR(const char *ir, const char *out, BuildMode mode, const char *dir, const char **links,
                      int nlinks);
static bool makeTempDir(char *buf, size_t n);
static void removeTree(const char *dir);

/* comptime: builds the helper for the machine this runs on, runs it, and
 * keeps the values it printed. False after an error. */
static bool runComptime(Arena *a, const char *path, const char **includes, int nincludes, char *helperSrc,
                        Program *prog) {
    gComptime = 1;
    gExtraPath = "<comptime>";
    gExtraText = helperSrc;
    Arena ha = {0};
    Program *helper = loadProgram(&ha, path, includes, nincludes);
    bool ok = helper && !gErrors && checkProgram(&ha, helper);
    Target saved = gTarget;
    gTarget.triple = NULL;          /* the helper runs here */
    char *ir = ok ? writeIR(&ha, helper, MODE_DEBUG) : NULL;
    char dir[4096];
    ok = ok && makeTempDir(dir, sizeof dir);
    char exe[4200], outPath[4200], errPath[4200];
    if (ok) {
        snprintf(exe, sizeof exe, "%s/program", dir);
        snprintf(outPath, sizeof outPath, "%s/comptime.out", dir);
        snprintf(errPath, sizeof errPath, "%s/comptime.err", dir);
        const char *links[256];
        int n = 0;
        for (int k = 0; k < helper->nlinks; k++) links[n++] = helper->links[k];
        for (int k = 0; k < helper->ncsources; k++) links[n++] = helper->csources[k];
        ok = compileIR(ir, exe, MODE_DEBUG, dir, links, n);
    }
    gTarget = saved;
    if (ok) {
        char *argv[] = {exe, NULL};
        fflush(stdout);
        int rc = runCommand(argv, outPath, errPath);
        size_t outLen = 0, errLen = 0;
        char *out = readAllFile(a, outPath, &outLen);
        char *err = readAllFile(a, errPath, &errLen);
        if (rc != 0) {
            fprintf(stderr, "iris: comptime: the code stopped with status %d\n%s", rc, err ? err : "");
            gErrors++;
            ok = false;
        } else {
            /* lines \x01 N \x02 JSON; the rest is what the code printed itself */
            gComptimeResults = arenaAlloc(a, sizeof(ComptimeResult) * (size_t)(gComptimeKeyCount + 1));
            gComptimeCount = 0;
            for (char *p = out; p && *p;) {
                char *e = strchr(p, '\n');
                if (!e) e = p + strlen(p);
                if (*p == '\x01') {
                    char *sep = memchr(p, '\x02', (size_t)(e - p));
                    int idx = atoi(p + 1);
                    if (sep && idx >= 0 && idx < gComptimeKeyCount) {
                        gComptimeResults[gComptimeCount].key = gComptimeKeys[idx];
                        gComptimeResults[gComptimeCount].json = arenaStrndup(a, sep + 1, (size_t)(e - sep - 1));
                        gComptimeCount++;
                    }
                } else if (e > p) {
                    fprintf(stderr, "comptime: %.*s\n", (int)(e - p), p);
                }
                p = *e ? e + 1 : e;
            }
        }
        unlink(outPath);
        unlink(errPath);
    }
    if (dir[0] && ok) removeTree(dir);
    /* the keys and results live in a; the helper's own tree goes */
    for (int i = 0; i < gComptimeKeyCount; i++) {
        gComptimeKeys[i] = arenaStrndup(a, gComptimeKeys[i], strlen(gComptimeKeys[i]));
    }
    arenaFree(&ha);
    gExtraPath = gExtraText = NULL;
    gComptime = ok ? 2 : 0;
    (void)prog;
    return ok;
}

/* Reads, parses and checks a program and its modules. Returns NULL after
 * errors. */
static Program *frontEnd(Arena *a, const char *path, const char **includes, int nincludes) {
    Program *prog = loadProgram(a, path, includes, nincludes);
    if (!prog || gErrors) {
        return NULL;
    }
    char *helper = comptimeHelperSource(a, prog);
    if (helper) {
        if (!runComptime(a, path, includes, nincludes, helper, prog)) {
            return NULL;
        }
        /* again, with the values in place of the comptime expressions */
        prog = loadProgram(a, path, includes, nincludes);
        if (!prog || gErrors) {
            return NULL;
        }
    }
    if (!checkProgram(a, prog)) {
        return NULL;
    }
    return prog;
}

static int runProgramTo(char **argv, const char *errPath) { return runCommand(argv, NULL, errPath); }

/* --cpu, --pgo-gen and --pgo: for release builds */
static const char *gCpu;
static bool gPgoGen;
static const char *gPgoUse;

/* The major version of the clang that builds programs, or 0. */
static int clangMajor(const char *cc, const char *dir) {
    char outPath[4200];
    snprintf(outPath, sizeof outPath, "%s/cc.version", dir);
    char *argv[] = {(char *)cc, "--version", NULL};
    int major = 0;
    if (runCommand(argv, outPath, "/dev/null") == 0) {
        FILE *f = fopen(outPath, "r");
        char line[512];
        if (f && fgets(line, sizeof line, f)) {
            char *v = strstr(line, "version ");
            if (v) major = atoi(v + 8);
        }
        if (f) fclose(f);
    }
    unlink(outPath);
    return major;
}

/* Merges the profiles that --pgo names (a .profdata file is used as it
 * is; a folder or .profraw files are merged into dir) with the
 * llvm-profdata of the same version as clang. NULL when that fails. */
static const char *pgoProfile(const char *cc, const char *dir, char *buf, size_t n) {
    size_t len = strlen(gPgoUse);
    if (len > 9 && strcmp(gPgoUse + len - 9, ".profdata") == 0) {
        return gPgoUse;
    }
    snprintf(buf, n, "%s/merged.profdata", dir);
    const char *tool = getenv("IRIS_PROFDATA");
    char named[64];
    int major = clangMajor(cc, dir);
    const char *tries[4];
    int ntries = 0;
    if (tool && *tool) tries[ntries++] = tool;
    if (major > 0) {
        snprintf(named, sizeof named, "llvm-profdata-%d", major);
        tries[ntries++] = named;
    }
    tries[ntries++] = "llvm-profdata";
    for (int i = 0; i < ntries; i++) {
        char *argv[] = {(char *)tries[i], "merge", "-o", buf, (char *)gPgoUse, NULL};
        int rc = runCommand(argv, NULL, NULL);
        if (rc == 0) return buf;
        if (rc != 127) return NULL;
    }
#ifdef __APPLE__
    char *argv[] = {"xcrun", "llvm-profdata", "merge", "-o", buf, (char *)gPgoUse, NULL};
    if (runCommand(argv, NULL, NULL) == 0) return buf;
#endif
    fprintf(stderr, "iris: --pgo needs llvm-profdata%s%s; install it or set IRIS_PROFDATA\n",
            major > 0 ? " " : "", major > 0 ? named + 13 : "");
    return NULL;
}

static int runProgram(char **argv) { return runCommand(argv, NULL, NULL); }

/* The folder that keeps compiled runtimes: $IRIS_PATH/rt or
 * ~/.cache/iris/rt, made when missing. False when it can't be. */
static bool runtimeCacheDir(char *buf, size_t n) {
    const char *base = getenv("IRIS_PATH");
    char tmp[3000];
    if (base && strlen(base) > 2900) return false;
    if (!base || !*base) {
        const char *home = getenv("HOME");
        if (!home || !*home) return false;
        if (strlen(home) > 2900) return false;
        snprintf(tmp, sizeof tmp, "%s/.cache", home);
        mkdir(tmp, 0755);
        snprintf(tmp, sizeof tmp, "%s/.cache/iris", home);
        base = tmp;
    }
    mkdir(base, 0755);
    snprintf(buf, n, "%s/rt", base);
    mkdir(buf, 0755);
    struct stat st;
    return stat(buf, &st) == 0 && S_ISDIR(st.st_mode);
}

/* The runtime compiled with flags, as an object file in the cache, named
 * by a hash of the runtime and the flags: compiled the first time, then
 * reused, so a program only compiles its own code. False when it can't be
 * made; rtPath is the runtime's source, written already. */
static bool cachedRuntime(char **flags, int nflags, const char *rtPath, char *obj, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (const char *p = runtimeSource; *p; p++) h = (h ^ (unsigned char)*p) * 1099511628211ULL;
    for (int i = 0; i < nflags; i++) {
        for (const char *p = flags[i]; *p; p++) h = (h ^ (unsigned char)*p) * 1099511628211ULL;
        h = (h ^ 0xff) * 1099511628211ULL;
    }
    char dir[4096];
    if (!runtimeCacheDir(dir, sizeof dir)) return false;
    snprintf(obj, n, "%s/%s-%016llx.o", dir, IRIS_VERSION, (unsigned long long)h);
    if (access(obj, R_OK) == 0) return true;
    /* compiled to a name of its own, then renamed, so programs built at
     * once don't see half a file */
    char part[4500];
    snprintf(part, sizeof part, "%s.%d.part", obj, (int)getpid());
    char *args[64];
    int k = 0;
    for (int i = 0; i < nflags && k < 58; i++) args[k++] = flags[i];
    args[k++] = "-c";
    args[k++] = (char *)rtPath;
    args[k++] = "-o";
    args[k++] = part;
    args[k] = NULL;
    if (runProgram(args) != 0 || rename(part, obj) != 0) {
        unlink(part);
        return false;
    }
    return true;
}

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
    /* the flags that compile both the program and the runtime */
    char *flags[48];
    int nf = 0;
    flags[nf++] = (char *)cc;
    flags[nf++] = mode == MODE_DEBUG ? "-O0" : "-O3";
    /* release builds optimize the program and the runtime as one, so the
     * runtime's small functions are inlined into the program */
    int lto = mode == MODE_DEBUG ? -1 : nf;
    int ltoArgs = 0;
    if (lto >= 0) {
        flags[nf++] = "-flto";
        /* loops start on a 32-byte boundary, so a small hot loop doesn't
         * straddle two of them by chance; with LTO the linker's code
         * generator is told */
        flags[nf++] = strcmp(gTarget.os, "macos") == 0 ? "-Wl,-mllvm,-align-loops=32" : "-Wl,-plugin-opt=-align-loops=32";
        ltoArgs = 2;
        flags[nf++] = "-falign-loops=32";
    }
    char cpuArg[256], pgoArg[4400], pgoBuf[4300];
    if (mode != MODE_DEBUG && gCpu) {
        /* x86 names a whole machine with -march; arm64 uses -mcpu */
        const char *arch = gTarget.triple ? gTarget.arch :
#if defined(__aarch64__)
            "aarch64";
#else
            "x86_64";
#endif
        snprintf(cpuArg, sizeof cpuArg, "%s=%s", strcmp(arch, "aarch64") == 0 ? "-mcpu" : "-march", gCpu);
        flags[nf++] = cpuArg;
    }
    /* a profile changes how the runtime compiles: such builds compile it
     * each time */
    bool cache = !(mode != MODE_DEBUG && (gPgoGen || gPgoUse)) && !getenv("IRIS_NO_RT_CACHE");
    if (mode != MODE_DEBUG && gPgoGen) {
        flags[nf++] = "-fprofile-generate=iris-pgo";
    } else if (mode != MODE_DEBUG && gPgoUse) {
        const char *profile = pgoProfile(cc, dir, pgoBuf, sizeof pgoBuf);
        if (!profile) {
            return false;
        }
        snprintf(pgoArg, sizeof pgoArg, "-fprofile-use=%s", profile);
        flags[nf++] = pgoArg;
        flags[nf++] = "-Wno-backend-plugin";  /* functions the profile didn't see */
    }
    flags[nf++] = "-g";
    flags[nf++] = "-Wno-override-module";
    flags[nf++] = "-w";
    char sysroot[4200];
    if (gTarget.triple) {
        flags[nf++] = "-target";
        flags[nf++] = (char *)gTarget.triple;
        const char *sr = getenv("IRIS_SYSROOT");
        if (sr && *sr) {
            snprintf(sysroot, sizeof sysroot, "--sysroot=%s", sr);
            flags[nf++] = sysroot;
        }
    }
    int asan = -1;
    if (mode == MODE_DEBUG) {
        asan = nf;
        flags[nf++] = "-fsanitize=address";   /* use after free, out-of-bounds memory */
    }
    char linkArgs[256][300];
    char errPath[4200];
    snprintf(errPath, sizeof errPath, "%s/clang.err", dir);
    int rc = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        char rtObj[4400];
        bool cached = cache && cachedRuntime(flags, nf, rtPath, rtObj, sizeof rtObj);
        char *args[320];
        int n = 0;
        for (int i = 0; i < nf; i++) args[n++] = flags[i];
        args[n++] = llPath;
        args[n++] = cached ? rtObj : rtPath;
        args[n++] = "-o";
        args[n++] = (char *)out;
        for (int i = 0; i < nlinks && i < 256; i++) {
            size_t ln = strlen(links[i]);
            if (ln > 2 && strcmp(links[i] + ln - 2, ".c") == 0) {
                args[n++] = (char *)links[i];       /* a C file of the program */
                continue;
            }
            snprintf(linkArgs[i], sizeof linkArgs[i], "-l%s", links[i]);
            args[n++] = linkArgs[i];
        }
        args[n++] = "-lm";              /* the math module's functions */
        args[n++] = "-pthread";         /* http.serve's threads */
        if (strcmp(gTarget.os, "linux") == 0) {
            args[n++] = "-ldl";         /* TLS loads OpenSSL when it is first used */
        }
        args[n] = NULL;
        rc = attempt == 0 ? runProgramTo(args, errPath) : runProgram(args);
        if (rc == 0 || rc == 127 || attempt == 1) break;
        if (mode == MODE_DEBUG) {
            /* not every clang has AddressSanitizer's libraries: build
             * without it when linking with it fails */
            for (int i = asan; i < nf - 1; i++) flags[i] = flags[i + 1];
            nf--;
        } else {
            /* a linker that can't do LTO: build without it */
            for (int i = lto; i + ltoArgs < nf; i++) flags[i] = flags[i + ltoArgs];
            nf -= ltoArgs;
        }
    }
    if (rc == 0 && mode == MODE_DEBUG && asan >= 0 && (asan >= nf || strcmp(flags[asan], "-fsanitize=address") != 0)) {
        fprintf(stderr, "iris: note: this clang can't use AddressSanitizer, so this debug build doesn't "
                        "check for use after free\n");
    }
    unlink(errPath);
    if (rc == 127) {
        fprintf(stderr, "iris: clang is needed to build programs; install it or set IRIS_CC\n");
    } else if (rc != 0 && gTarget.triple) {
        fprintf(stderr, "iris: building for %s-%s needs that platform's C library and linker; set IRIS_SYSROOT "
                        "to its files (see Tools > Other platforms in the docs)\n",
                gTarget.arch, gTarget.os);
    }
    return rc == 0;
}

/* A build folder and the files in it. */
static void removeTree(const char *dir) {
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        char path[4400];
        while ((e = readdir(d))) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
            unlink(path);
        }
        closedir(d);
    }
    rmdir(dir);
}

static void removeTreeArg(void *dir) { removeTree(dir); }

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
/* "500ms", "2s": nanoseconds, or -1 */
static long long parseBenchTime(const char *s) {
    char *end;
    double v = strtod(s, &end);
    static const struct { const char *unit; double ns; } units[] = {
        {"ns", 1}, {"us", 1e3}, {"ms", 1e6}, {"s", 1e9}, {"m", 60e9},
    };
    for (size_t k = 0; k < sizeof units / sizeof units[0]; k++) {
        if (end != s && v > 0 && strcmp(end, units[k].unit) == 0) {
            return (long long)(v * units[k].ns);
        }
    }
    return -1;
}

static int cmdCompile(const char *cmd, int argc, char **argv) {
    bool bench = strcmp(cmd, "bench") == 0;
    BuildMode mode = bench ? MODE_RELEASE_FAST : MODE_DEBUG;
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
        } else if (strncmp(argv[i], "--cpu=", 6) == 0 && argv[i][6]) {
            gCpu = argv[i] + 6;
        } else if (strcmp(argv[i], "--pgo-gen") == 0) {
            gPgoGen = true;
        } else if (strncmp(argv[i], "--pgo=", 6) == 0 && argv[i][6]) {
            gPgoUse = argv[i] + 6;
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc && strcmp(cmd, "build") == 0) {
            out = argv[++i];
        } else if (bench && strncmp(argv[i], "--time=", 7) == 0) {
            long long ns = parseBenchTime(argv[i] + 7);
            if (ns <= 0) {
                fprintf(stderr, "iris: --time takes a duration, such as 500ms or 2s, not %s\n", argv[i] + 7);
                return 2;
            }
            char v[32];
            snprintf(v, sizeof v, "%lld", ns);
            setenv("IRIS_BENCH_TIME", v, 1);
        } else if (argv[i][0] == '-' && argv[i][1]) {
            fprintf(stderr, "iris: unknown option %s\n", argv[i]);
            return 2;
        } else if (strcmp(cmd, "run") == 0 || strcmp(cmd, "test") == 0 || bench) {
            /* what follows is the program's arguments, or the test's name */
            path = argv[i++];
            break;
        } else if (!path) {
            path = argv[i];     /* build, check and ir take options after the file too */
        } else {
            usage(stderr);
            return 2;
        }
    }
    bool test = strcmp(cmd, "test") == 0 || bench;
    gWithTests = test || strcmp(cmd, "check") == 0;
    gBenching = bench;
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
    const char *allLinks[256];
    int nAll = 0;
    for (int k = 0; k < prog->nlinks; k++) allLinks[nAll++] = prog->links[k];
    for (int k = 0; k < nlinks; k++) allLinks[nAll++] = links[k];
    for (int k = 0; k < prog->ncsources; k++) allLinks[nAll++] = prog->csources[k];
    char dir[4096];
    if (!makeTempDir(dir, sizeof dir)) {
        goto done;
    }
    /* hello.iris -> hello; a folder -> its name */
    char def[4096];
    {
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
    }
    if (strcmp(cmd, "build") == 0) {
        if (!out) {
            out = def;
        }
        rc = compileIR(ir, out, mode, dir, allLinks, nAll) ? 0 : 1;
        removeTree(dir);
        goto done;
    }
    /* named after the program, which then sees its own name in argv[0] */
    char exe[8400];
    snprintf(exe, sizeof exe, "%s/%s", dir, def);
    if (!compileIR(ir, exe, mode, dir, allLinks, nAll)) {
        unlink(exe);
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
        rc = runCommandStarted(args, NULL, NULL, removeTreeArg, dir);
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

/* Whether a module path can be one: letters, digits, . _ ~ - and / between
 * parts, as in github.com/you/app. */
static bool validModulePath(const char *p) {
    if (!*p || *p == '/' || p[strlen(p) - 1] == '/') return false;
    for (const char *c = p; *c; c++) {
        bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ||
                  *c == '.' || *c == '_' || *c == '~' || *c == '-' || *c == '/';
        if (!ok || (*c == '/' && c[1] == '/')) return false;
    }
    return !strstr(p, "..") && strcmp(p, ".") != 0;
}

/* iris mod init [path]: an iris.mod in this folder, which makes it a project.
 * The path names it, and is where iris get will find it; it is the folder's
 * name when left out. */
static int cmdModInit(int argc, char **argv) {
    if (argc > 4) {
        fprintf(stderr, "usage: iris mod init [path]\n");
        return 2;
    }
    struct stat st;
    if (stat("iris.mod", &st) == 0) {
        fprintf(stderr, "iris: iris.mod is here already\n");
        return 1;
    }
    char cwd[4096];
    const char *path = argc == 4 ? argv[3] : NULL;
    if (!path) {
        if (!getcwd(cwd, sizeof cwd)) {
            fprintf(stderr, "iris: can't read the current folder\n");
            return 1;
        }
        const char *base = strrchr(cwd, '/');
        path = base && base[1] ? base + 1 : cwd;
    }
    if (!validModulePath(path)) {
        fprintf(stderr, "iris: '%s' can't be a module path: use letters, digits, . _ ~ - and / (as in github.com/you/app)\n", path);
        return 1;
    }
    FILE *f = fopen("iris.mod", "w");
    if (!f) {
        fprintf(stderr, "iris: can't write iris.mod\n");
        return 1;
    }
    fprintf(f, "module %s\n", path);
    fclose(f);
    fprintf(stderr, "iris: made iris.mod for %s\n", path);
    return 0;
}

static int cmdMod(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[2], "init") == 0) {
        return cmdModInit(argc, argv);
    }
    fprintf(stderr, "usage: iris mod <command>\n"
                    "  init [path]   make iris.mod for a new project in this folder\n");
    return 2;
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
        strcmp(cmd, "ir") == 0 || strcmp(cmd, "test") == 0 || strcmp(cmd, "bench") == 0) {
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
    if (strcmp(cmd, "doc") == 0) {
        return cmdDoc(argc, argv);
    }
    if (strcmp(cmd, "fmt") == 0) {
        return cmdFmt(argc, argv);
    }
    if (strcmp(cmd, "mod") == 0) {
        return cmdMod(argc, argv);
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
