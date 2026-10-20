#ifndef QUACKY_EVAL_H
#define QUACKY_EVAL_H

#include "output.h" /* Output */
#include "parse.h"  /* Program, Block, Stmt, Expr, Func */
#include "value.h"  /* Value, Status */

/* The tree-walker (03). Status codes, not longjmp, so every str_release on the
   way up the stack runs as normal C control flow (06 §3). */

typedef struct Var {
  Str name;
  Value val;
  bool is_const;
  struct Var *next;
} Var;
typedef struct Frame {
  Var *buckets[64];
  struct Frame *parent;
} Frame; /* parent NULL = globals */

typedef struct {
  Program *prog;
  Frame *globals, *frame; /* frame == globals at top level */
  Func **funcs;
  int nfuncs;        /* registered at startup, looked up by name */
  int depth;         /* call depth, limit 200 */
  Value ret;         /* parked return value */
  Stmt *last_action; /* last keystroke/type/chord stmt, for `repeat`; NULL if
                        none */
  const char *script_path; /* the running script's path, for script-relative
                              type_file */
  char err[512];           /* runtime error message */
  int err_line, err_col;
  Output *out; /* owns the fd, the clock, and the held-key set (05) */
} Interp;

/* Formats into I->err, records the failing node's line/col, returns ST_ERR, so
   a call site reads `return rt_error(I, e->line, e->col, "...", ...);` (06 §4).
   It takes line/col rather than a node pointer, since Expr and Stmt are
   distinct types and there is no shared node. */
Status rt_error(Interp *I, int line, int col, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/* Register the program's functions, then run the top block. Returns ST_OK or
   ST_ERR (with I->err set); the caller maps that to an exit code. */
Status run(Interp *I, Program *prog);

#endif
