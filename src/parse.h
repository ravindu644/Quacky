#ifndef QUACKY_PARSE_H
#define QUACKY_PARSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lex.h"     /* TokKind, stored on binary/unary Expr nodes */
#include "value.h"   /* Str */

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

/* The AST (02 section 5). Everything is arena-allocated and freed once at exit.
   `for` and `elif` never appear as node kinds: the parser desugars `for` into a
   `while` and chains `elif`/`else` into nested S_IF nodes. */

typedef enum {
    E_INT, E_BOOL, E_STR, E_VAR, E_UNARY, E_BINARY, E_AND, E_OR, E_CALL
} ExprKind;

typedef struct Expr {
    ExprKind kind;
    int      line, col;
    union {
        int64_t ival;                                      /* E_INT */
        bool    bval;                                      /* E_BOOL */
        QStr   *sval;                                      /* E_STR: immortal arena string, escapes decoded */
        Str     name;                                      /* E_VAR */
        struct { TokKind op; struct Expr *a, *b; } bin;    /* E_UNARY (a only), E_BINARY, E_AND, E_OR */
        struct { Str name; struct Expr **args; int nargs; } call;  /* E_CALL */
    };
} Expr;

typedef enum {
    S_ASSIGN, S_CONST, S_IF, S_WHILE, S_RETURN, S_BREAK, S_CONTINUE,
    S_SLEEP, S_TYPE, S_TYPE_FILE, S_CALL, S_CHORD, S_HOLD, S_RELEASE, S_DELAY, S_REPEAT
} StmtKind;

/* A chord resolved at parse time: modifier bits plus up to six key usages.
   `all` is set only for `release all`. `text` is the source span of the keys
   (e.g. "ctrl + c"), kept so the transcript backend can tag the action without
   reverse-mapping usages back to names. */
typedef struct { uint8_t mod; uint8_t usages[6]; int n; bool all; Str text; } Chord;

/* One piece of a `type` line: literal text (escapes already decoded, arena
   copied) or an interpolated expression. */
typedef struct { enum { SEG_TEXT, SEG_EXPR } kind; Str text; Expr *expr; } Seg;

typedef struct Block { struct Stmt **stmts; int n; } Block;

typedef struct Stmt {
    StmtKind kind;
    int      line, col;
    union {
        struct { Str name; Expr *value; } assign;                        /* S_ASSIGN, S_CONST */
        struct { Expr *cond; struct Block *then_b; struct Stmt *else_s; } iff;  /* S_IF; else_s: next S_IF or NULL */
        struct { Expr *cond; struct Block *body; } loop;                 /* S_WHILE */
        Expr *expr;                                                      /* S_RETURN (nullable), S_SLEEP, S_DELAY, S_REPEAT, S_CALL */
        struct { Seg *segs; int nsegs; bool stop_output; } type;         /* S_TYPE */
        /* S_TYPE_FILE: a literal path is read and validated at parse time (bytes/len
           set, path NULL); a computed path is read at run time (path set, bytes NULL). */
        struct { const char *bytes; size_t len; Expr *path; bool add_newline; } typefile;
        Chord chord;                                                     /* S_CHORD, S_HOLD, S_RELEASE */
    };
} Stmt;

typedef struct Func { Str name; Str *params; int nparams; Block *body; } Func;
typedef struct Program { Block *top; Func **funcs; int nfuncs; } Program;

/* Lex and parse the whole source into one arena-allocated Program. A syntax
   error prints a diagnostic and exits 2 from inside; on success the returned
   tree is complete. `src` is normalised in place (CRLF to LF). */
Program *parse(Arena *a, const char *filename, char *src, bool check);

#endif
