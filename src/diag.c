/* Compile errors: file:line:col, the source line, and a mark under the
 * problem. */
#include "iris.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ERRORS 20

int gErrors;

void (*gDiagHook)(Source *src, int line, int col, const char *msg);

void verrorAt(Source *src, int line, int col, const char *fmt, va_list ap) {
    if (gErrors > 0 && src->path && strcmp(src->path, "<comptime>") == 0) {
        /* comptime's helper repeats a problem already reported in the program */
        gErrors++;
        return;
    }
    if (gDiagHook) {
        /* the language server collects errors instead of printing them */
        char msg[1024];
        vsnprintf(msg, sizeof msg, fmt, ap);
        gDiagHook(src, line, col, msg);
        gErrors++;
        return;
    }
    fprintf(stderr, "%s:%d:%d: error: ", src->path, line, col);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);

    const char *p = src->text, *end = src->text + src->len;
    for (int l = 1; l < line && p < end; p++) {
        if (*p == '\n') {
            l++;
        }
    }
    const char *e = p;
    while (e < end && *e != '\n' && *e != '\r') {
        e++;
    }
    if (p < end || line == 1) {
        fprintf(stderr, "    %.*s\n    ", (int)(e - p), p);
        for (int i = 1; i < col && p + i - 1 < e; i++) {
            fputc(p[i - 1] == '\t' ? '\t' : ' ', stderr);
        }
        fputs("^\n", stderr);
    }

    if (++gErrors >= MAX_ERRORS) {
        fprintf(stderr, "too many errors, stopping\n");
        exit(1);
    }
}

void errorAt(Source *src, int line, int col, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    verrorAt(src, line, col, fmt, ap);
    va_end(ap);
}
