#ifndef QUACKY_VALUE_H
#define QUACKY_VALUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h> /* free, for the inline str_release below */

#include "lex.h" /* TokKind, int_op's operator selector */

/* The value model (04). Three types only: int64, bool, str. No float, no
   truthiness; the evaluator keeps conditions strictly bool. */

typedef enum {
  VAL_NONE,
  VAL_INT,
  VAL_BOOL,
  VAL_STR
} ValType; /* VAL_NONE is internal only */

/* A borrowed slice into the source buffer. The AST uses it for names and string
   literals so nothing is copied until a value is actually needed (04 section
   1). */
typedef struct {
  const char *p;
  size_t len;
} Str;

/* One string struct, two lifetimes told apart by rc: rc < 0 is immortal (a
   literal or decoded `type` text living in the arena, never freed); rc > 0 is a
   runtime string from `+` or to_string, freed when the last reference drops
   (04 section 2). data is a flexible array, so one malloc holds header + bytes.
 */
typedef struct QStr {
  int rc;
  size_t len;
  char data[];
} QStr;

typedef struct {
  ValType type;
  union {
    int64_t i;
    bool b;
    QStr *s;
  };
} Value;

/* Status travels up the evaluator's C stack (03 section 1). Its real home is
   the evaluator, but int_op and compare below live in value.c and need it, and
   Phase 4's eval.h includes this header anyway, so it is declared once here. */
typedef enum { ST_OK, ST_ERR, ST_RETURN, ST_BREAK, ST_CONTINUE } Status;

/* Refcounting (04 section 2). An immortal string (rc < 0) ignores both, so the
   same helpers work whether a value points at an arena literal or a heap
   string. The two Value wrappers are in value.c; these two are hot and stay
   inline. */
static inline void str_retain(QStr *s) {
  if (s->rc > 0)
    s->rc++;
}
static inline void str_release(QStr *s) {
  if (s->rc > 0 && --s->rc == 0)
    free(s);
}

void val_retain(Value v);
void val_release(Value v);

/* to_string: VAL_STR retains and returns the same pointer; VAL_INT formats with
   PRId64; VAL_BOOL gives "true"/"false". The result is owned by the caller.
   str_concat implements string `+`: one allocation, no intermediate buffer. */
QStr *to_string(Value v);
QStr *str_concat(const QStr *a, const QStr *b);
/* Wrap a NUL-terminated C string as a fresh runtime QStr (rc = 1), owned by the
   caller. Used for a value computed as a C string, like the host_os result. */
QStr *qstr_dup(const char *s);

/* Every integer UB case lives here, not at call sites (04 section 4). Returns
   ST_OK and writes *out, except `/`/`%` by zero which set *err to a message and
   return ST_ERR. op must be an integer binary operator; anything else is a
   caller bug. The evaluator forwards *err through rt_error (the signature takes
   a message out-param rather than the Interp the blueprint first sketched, so
   int_op stays a pure leaf with no evaluator dependency). */
Status int_op(TokKind op, int64_t a, int64_t b, int64_t *out, const char **err);

/* Unary minus, the one unary case with signed-overflow UB (negating INT64_MIN).
   `~` is UB-free, so the evaluator applies it directly. */
static inline int64_t int_neg(int64_t a) { return (int64_t)(0u - (uint64_t)a); }

/* equal never fails: different types compare unequal. compare orders two ints
   or two strings and writes -1/0/1 into *out; mixed types set *err and return
   ST_ERR (04 section 5). */
bool equal(Value a, Value b);
Status compare(Value a, Value b, int *out, const char **err);

_Static_assert(sizeof(int64_t) == 8, "Quacky values are exactly 64 bits");

#endif
