/* Running other programs (clang, git) and writing files. */
#define _POSIX_C_SOURCE 200809L
#include "iris.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

int runCommand(char **argv, const char *outPath, const char *errPath) {
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        perror("iris: fork");
        return 1;
    }
    if (pid == 0) {
        if (outPath) {
            FILE *f = freopen(outPath, "w", stdout);
            (void)f;
        }
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

bool writeText(const char *path, const char *text, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "iris: can't write %s\n", path);
        return false;
    }
    bool ok = fwrite(text, 1, len, f) == len;
    ok = fclose(f) == 0 && ok;
    return ok;
}

/* A whole file into the arena, or NULL. */
char *readAllFile(Arena *a, const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    StrBuf b = {0};
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) sbPutn(&b, buf, n);
    fclose(f);
    *len = b.len;
    return sbFinish(&b, a);
}
