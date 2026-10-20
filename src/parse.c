/* The frontend's shared infrastructure: the arena all AST nodes come from, and
   the diagnostic renderer both error classes use. The recursive-descent and
   Pratt parser join this file in Phase 2. */

#include "parse.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define ARENA_CHUNK (64 * 1024)

static Chunk *new_chunk(size_t cap) {
    Chunk *c = malloc(sizeof *c + cap);
    if (!c) {
        /* No recovery path: out of memory is fatal (02 section 6). */
        fputs("quacky: out of memory\n", stderr);
        exit(1);
    }
    c->next = NULL;
    c->used = 0;
    c->cap  = cap;
    return c;
}

void *arena_alloc(Arena *a, size_t n) {
    size_t align = _Alignof(max_align_t);
    n = (n + align - 1) & ~(align - 1);
    if (!a->head || a->head->cap - a->head->used < n) {
        /* An allocation larger than a chunk gets its own exact chunk. */
        Chunk *c = new_chunk(n > ARENA_CHUNK ? n : ARENA_CHUNK);
        c->next = a->head;
        a->head = c;
    }
    void *p = (char *)a->head->data + a->head->used;
    a->head->used += n;
    return p;
}

void arena_free(Arena *a) {
    Chunk *c = a->head;
    while (c) {
        Chunk *next = c->next;
        free(c);
        c = next;
    }
    a->head = NULL;
}

void print_diagnostic(const char *file, const char *src,
                      int line, int col, const char *msg) {
    fprintf(stderr, "%s:%d:%d: error: %s\n", file, line, col, msg);

    /* Walk to the start of the offending line by counting newlines. */
    const char *ls = src;
    for (int n = 1; n < line && *ls; ls++) {
        if (*ls == '\n') n++;
    }
    /* Print the line, tabs collapsed to one space so the caret lines up. */
    fputs("  ", stderr);
    for (const char *p = ls; *p && *p != '\n'; p++)
        fputc(*p == '\t' ? ' ' : *p, stderr);
    fputc('\n', stderr);

    /* The caret sits under the column, matching the two-space indent above. */
    fputs("  ", stderr);
    for (int i = 1; i < col; i++) fputc(' ', stderr);
    fputs("^\n", stderr);
}

_Noreturn void syntax_error(const char *file, const char *src,
                           int line, int col, const char *fmt, ...) {
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    print_diagnostic(file, src, line, col, msg);
    exit(2);
}
