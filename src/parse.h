#ifndef QUACKY_PARSE_H
#define QUACKY_PARSE_H

#include <stddef.h>
#include <stdint.h>

/* The 32-bit portability assumptions, stated once so a future reader does not
   have to rediscover them on an armhf box (07 section 4). */
_Static_assert(sizeof(int64_t) == 8, "Quacky values are exactly 64 bits");
_Static_assert(sizeof(void *) >= 4, "pointers must be at least 32 bits");

/* One bump allocator feeds the whole frontend. Everything it hands out lives
   until arena_free runs once at exit, so no node is ever freed on its own
   (02 section 6). */
typedef struct Chunk {
    struct Chunk *next;
    size_t        used, cap;   /* byte counts into data */
    /* A flexible array of max_align_t makes the region start suitably aligned
       for any value; arena_alloc then only has to round the size it hands out.
       A plain char[] would sit at an 8-byte offset and mis-align on targets
       where max_align_t is 16 bytes. */
    max_align_t   data[];
} Chunk;

typedef struct {
    Chunk *head;
} Arena;

void *arena_alloc(Arena *a, size_t n);
void  arena_free(Arena *a);

/* Both error classes print through one renderer so they look identical to the
   user (06 section 2): "file:line:col: error: message", the offending source
   line with tabs shown as a single space, then a caret under the column.
   syntax_error formats its message and exits 2, because a syntax error has to
   leave no output behind. col is 1-based. */
void print_diagnostic(const char *file, const char *src,
                      int line, int col, const char *msg);

_Noreturn void syntax_error(const char *file, const char *src,
                           int line, int col, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));

#endif
