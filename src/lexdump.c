/* TEMPORARY Phase 1 driver. It dumps one line per token so the lexer can be
   exercised before the parser (Phase 2) and the real CLI (Phase 7) exist, and
   it carries main() so `make native` links. Delete this file when src/main.c
   arrives. */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "lex.h"
#include "parse.h"

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(66); }             /* EX_NOINPUT */
    if (fseek(f, 0, SEEK_END) != 0) { perror(path); exit(66); }
    long n = ftell(f);
    if (n < 0) { perror(path); exit(66); }
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fputs("out of memory\n", stderr); exit(1); }
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

/* Prove the arena works: varied-size allocations stay aligned, distinct, and
   writable to the edge. ASan and UBSan on `make native` turn any overflow or
   misalignment here into a hard failure. */
static void arena_selfcheck(void) {
    Arena a = {0};
    size_t align = _Alignof(max_align_t);
    char *prev = NULL;
    for (size_t i = 1; i <= 5000; i += 137) {
        char *p = arena_alloc(&a, i);
        assert(((uintptr_t)p % align) == 0);
        assert(p != prev);
        for (size_t j = 0; j < i; j++) p[j] = (char)j;
        prev = p;
    }
    arena_free(&a);
}

static void print_escaped(const char *s, int len) {
    for (int i = 0; i < len; i++) {
        char c = s[i];
        if      (c == '\n') fputs("\\n",  stdout);
        else if (c == '\t') fputs("\\t",  stdout);
        else if (c == '"')  fputs("\\\"", stdout);
        else if (c == '\\') fputs("\\\\", stdout);
        else                fputc(c, stdout);
    }
}

int main(int argc, char **argv) {
    arena_selfcheck();

    if (argc != 2) {
        fprintf(stderr, "usage: %s FILE.qk\n", argv[0]);
        return 64;                                   /* EX_USAGE */
    }

    char *src = read_file(argv[1]);
    Lexer lx;
    lex_init(&lx, argv[1], src);

    for (;;) {
        Token t = lex_next(&lx);
        printf("%-10s %d:%d \"", tok_kind_name(t.kind),
               (int)(t.start - lx.src), t.len);
        print_escaped(t.start, t.len);
        printf("\" @%d:%d\n", t.line, t.col);
        if (t.kind == T_EOF) break;
    }

    free(src);
    return 0;
}
