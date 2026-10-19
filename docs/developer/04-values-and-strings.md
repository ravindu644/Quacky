# 04 · Values and strings

## 1. Value

```c
typedef enum { VAL_NONE, VAL_INT, VAL_BOOL, VAL_STR } ValType;   // VAL_NONE is internal only

typedef struct { const char *p; size_t len; } Str;              // borrowed slice, used by the AST

typedef struct QStr { int rc; size_t len; char data[]; } QStr;  // rc < 0  → immortal (arena/literal)

typedef struct {
    ValType type;
    union { int64_t i; bool b; QStr *s; };
} Value;
```

16 bytes on 64-bit targets, 12 on 32-bit. Passed and returned by value everywhere; only the
string payload is shared.

`VAL_NONE` exists so "function returned nothing" is representable; it is never storable in a
variable (assignment of a `VAL_NONE` is the runtime error "function 'f' returned no value",
raised by the caller, see 03 §5).

## 2. String ownership

Two kinds of `QStr`, one struct:

| Kind | Where allocated | `rc` | Freed |
|---|---|---|---|
| Literal / decoded `type` text | arena, at parse time | `-1` | never (arena dies at exit) |
| Runtime (`+`, `to_string`) | `malloc` | starts at 1 | when `rc` hits 0 |

```c
static inline void str_retain(QStr *s)  { if (s->rc > 0) s->rc++; }
static inline void str_release(QStr *s) { if (s->rc > 0 && --s->rc == 0) free(s); }
void val_retain(Value v)  { if (v.type == VAL_STR) str_retain(v.s); }
void val_release(Value v) { if (v.type == VAL_STR) str_release(v.s); }
```

Rules, so leaks and double frees are impossible to get wrong:
1. `eval_expr` returns a value **owned by the caller** (rc already counts the caller's reference).
2. Storing into a variable: `val_retain(new); val_release(old)`.
3. Temporaries in `eval_binary`: release both operands after producing the result.
4. `type` segments: release the interpolated value after `to_string`+emit.
5. Frame teardown releases every variable.

This is clox's `takeString`/`copyString` ownership split (`object.c:146-169`) without interning
or GC. Picol sidesteps the issue by `strdup`ing everywhere and freeing per command
(`picol.c:461-462`); that is simpler but allocates on every variable read, which we avoid.

### Why not "leak until exit"
A `while true do s = s + "x" done` style loop would grow without bound. The refcount is one
`int` and four one-line helpers. GC (mark-sweep over frames) is the upgrade if strings ever
become shared in cycles, which with no containers they cannot.

## 3. `to_string`

Single function used by `+` with a string operand and by `type` interpolation (spec §4.3, §5):

```c
QStr *to_string(Value v);        // VAL_STR → retain and return same pointer; INT → "%" PRId64; BOOL → "true"/"false"
```

Always `PRId64` from `<inttypes.h>`, never `%ld` (musl: `__PRI64` is `"l"` on 64-bit, `"ll"` on
32-bit, `include/inttypes.h:25-31`). The 32-bit toolchains would print garbage with `%ld`.

Concatenation allocates once: `malloc(sizeof(QStr) + a->len + b->len)`; no intermediate buffer.

## 4. Integer operations: one function, every edge case

```c
Status int_op(TokKind op, int64_t a, int64_t b, int64_t *out, Interp *I);
```

| op | Implementation | Source of idiom |
|---|---|---|
| `+ - *` | `(int64_t)((uint64_t)a + (uint64_t)b)` etc. Wraparound by definition, no UB. | Lua `intop` macro, `lvm.c:819` area; `l_castS2U/l_castU2S`, `llimits.h:145-155` |
| `/` | `b == 0` → ERR "division by zero"; `b == -1` → `0 - a` via unsigned (avoids `INT64_MIN / -1` trap); else `a / b` | Lua `luaV_idiv`, `lvm.c:773-785` (we keep C truncation, Lua floors) |
| `%` | `b == 0` → ERR; `b == -1` → `0`; else `a % b` | Lua `luaV_mod`, `lvm.c:793-805` |
| `<<` | `b < 0 || b >= 64` → `0`; else `(int64_t)((uint64_t)a << b)` | Lua `luaV_shiftl`, `lvm.c:825-834` |
| `>>` | `b < 0 || b >= 64` → `a < 0 ? -1 : 0`; else arithmetic: `a >= 0 ? a >> b : ~(~a >> b)` | C leaves `>>` of negatives implementation-defined; this is portable |
| `& \| ^` | plain | — |
| unary `-` | `(int64_t)(0u - (uint64_t)a)` | — |

`INT64_MIN / -1` and `INT64_MIN % -1` are the two cases that SIGFPE on x86 if left to the C
operators; both are caught by the `b == -1` branch. The spec chose wraparound over "error on
overflow" because shell arithmetic wraps; if that changes, `__builtin_add_overflow` et al. are
available on GCC 9 (verified) and replace the unsigned casts one for one.

## 5. Equality and ordering

```
equal(a, b):    types differ → false; INT/BOOL by value; STR by len then memcmp
compare(a, b):  both INT → numeric; both STR → memcmp over min(len), then shorter first; else ST_ERR
```

## 6. 32-bit checklist for this file
- `len` is `size_t`, values are `int64_t`; never mix them in arithmetic without a cast.
- `QStr` uses a flexible array member (C99/C11, fine on GCC 9.4).
- `_Static_assert(sizeof(int64_t) == 8, "")` in `value.h`, cheap insurance.
