/* memrun OUT PROGRAM [ARGS]: runs the program and writes its peak memory
   (KiB) to OUT. vfork, so the peak isn't the parent's, as it is after a
   fork from Python. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: memrun OUT PROGRAM [ARGS]\n");
        return 2;
    }
    pid_t pid = vfork();
    if (pid == 0) {
        execvp(argv[2], argv + 2);
        _exit(127);
    }
    int status;
    struct rusage ru;
    if (wait4(pid, &status, 0, &ru) < 0) {
        return 1;
    }
    FILE *f = fopen(argv[1], "w");
    if (f) {
        fprintf(f, "%ld\n", ru.ru_maxrss);
        fclose(f);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
