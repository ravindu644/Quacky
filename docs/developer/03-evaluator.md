# 03 · Evaluator

A recursive tree-walker. Every statement evaluator returns a status code; every expression
evaluator returns a status code and writes its result through an out-parameter. No
`setjmp`, no global error state besides the message buffer.

## 1. Status codes

```c
typedef enum { ST_OK, ST_ERR, ST_RETURN, ST_BREAK, ST_CONTINUE } Status;
```

Lifted from picol (`picol.c:63`). The non-OK codes travel up the C stack until something
consumes them:

| Code | Produced by | Consumed by |
|---|---|---|
| `ST_ERR` | any runtime failure | `main`, which prints `interp->err` and exits 1 |
| `ST_RETURN` | `return` stmt (value parked in `interp->ret`) | `call_function`, which turns it into `ST_OK` + value |
| `ST_BREAK` / `ST_CONTINUE` | the statements | the innermost loop (`picol.c:659-667` is the exact shape) |

Because the parser already rejected `break`/`continue`/`return` in illegal positions, the
evaluator never has to handle an unconsumed one; it can `assert` on it.

Every call site is one line: `Status s = eval_x(...); if (s != ST_OK) return s;`. Expression
evaluation only ever yields `ST_OK` or `ST_ERR`.

## 2. Interpreter state

```c
typedef struct Var { Str name; Value val; bool is_const; struct Var *next; } Var;
typedef struct Frame { Var *buckets[64]; struct Frame *parent; } Frame;   // parent is NULL for globals

typedef struct {
    Program *prog;
    Frame   *globals, *frame;        // frame == globals at top level
    Func   **funcs; int nfuncs;      // hoisted at startup, looked up by name (linear or small hash)
    int      depth;                  // call depth, limit 200
    Value    ret;                    // parked return value
    Stmt    *last_action;            // last keystroke/type/chord stmt, for `repeat`; NULL if none
    char     err[512];               // runtime error message
    int      err_line, err_col;
    Output  *out;                    // see 05 (owns the held-key set and default_delay)
} Interp;
```

A frame is a fixed 64-bucket chained hash map of `Var` (picol uses a plain linked list,
`picol.c:84-87`, `:322-330`; a bucket array costs 20 more lines and avoids O(n) lookups in long
scripts). Hash: FNV-1a over the name bytes. Frames are `malloc`ed on call and freed on return
along with their `Var` nodes; the `Value`s inside are released per 04.

### 2.1 Variable lookup and assignment (spec §4.5)

```
lookup(name):  v = find(frame, name); if (!v && frame != globals) v = find(globals, name); return v
assign(name, val):
    v = find(frame, name)             ?: find(globals, name)
    if (v && v->is_const) → ST_ERR "cannot assign to constant 'X'"
    if (v) replace value (release old, retain new)
    else insert into *current* frame
const(name, val): if find(frame,name) → ST_ERR "constant 'X' already declared"; insert with is_const
```

## 3. Expression evaluation

```c
Status eval_expr(Interp *I, Expr *e, Value *out);
```

| Node | Behaviour |
|---|---|
| `E_INT/E_BOOL/E_STR` | produce the constant; string literals are arena strings, so no copy and no refcount (04 §2) |
| `E_VAR` | `lookup`; missing → `ST_ERR "undefined variable 'x'"` |
| `E_UNARY` | eval child, type-check, apply. `-` on int wraps via unsigned negate; `!` on bool; `~` on int |
| `E_BINARY` | eval both, dispatch on `op` with the type matrix from spec §3. Int arithmetic goes through `int_op()` (04 §4) so every UB case is handled in one place |
| `E_AND/E_OR` | eval left; must be bool; short-circuit; eval right; must be bool |
| `E_CALL` | see §5 |
| `E_SETTING` | read a live output setting (`delay`/`keydelay`/`fastmode`/`ultramode`, tagged in `bin.op`) back as an int through the `out_*` getters; the mirror of the setter statements (05 §6) |

Type errors carry both the expected and the actual type name:
`"operator '+' expects int and int, got str and bool"`.

## 4. Statement evaluation

```c
Status exec_stmt(Interp *I, Stmt *s);
Status exec_block(Interp *I, Block *b);   // loops over stmts, returns first non-OK
```

Runtime errors are raised with `rt_error(Interp *I, int line, int col, const char *fmt, ...)`,
taking the failing node's `line`/`col` rather than a `const Node *`: `Expr` and `Stmt` are
distinct types with no shared node, and passing the two ints is simpler and UB-free (06 §4).

### 4.1 Default delay and `repeat`

Two cross-statement behaviours live here rather than in the output module:

- **Default delay.** The `default_delay` is a gap inserted *between* output actions only. At the
  top of `exec_stmt`, for the action kinds (`S_TYPE`, `S_TYPE_FILE`, `S_CHORD`, `S_HOLD`,
  `S_RELEASE`), the evaluator calls `out_action_gap(I->out)` once, before dispatching the statement;
  that pauses `default_delay` ms (0 = no pause) before every action but the first. The call lives at
  the statement boundary on purpose: a `type` emits several segments through repeated `out_type`
  calls, but is still one action, so one gap. This means `delay 100` paces the keystrokes a script
  sends without pausing on assignments, `if`/`while`/`for`, or a bare call, and without a leading or
  trailing pause. An explicit `sleep` is separate and adds on top of the gap; it does not replace
  it. The `delay` statement itself only stores the value (05 §6). This is deliberate: a prior design
  paused after every statement, which wasted real time on pure arithmetic and control flow.
- **`repeat N`.** `exec_stmt` records a pointer to the last **action** statement it ran
  (`S_TYPE`, `S_TYPE_FILE`, `S_CHORD`, `S_HOLD`, `S_RELEASE`) in `I->last_action`, and clears it on
  any other statement. `repeat N` re-runs `I->last_action` N times; a null `last_action` is the
  runtime error "repeat with no preceding action". `repeat` does not become the new `last_action`,
  so two `repeat`s in a row both refer to the original action. Each re-run is an action, so the
  default gap falls between the re-runs through `out_action_gap`.

| Stmt | Behaviour |
|---|---|
| `S_ASSIGN` / `S_CONST` | eval, then `assign` / `const` |
| `S_IF` | eval cond → must be bool → `exec_block(then)` or recurse into `else_s` |
| `S_WHILE` | loop: eval cond (bool); `s = exec_block(body)`; `ST_BREAK` → return OK; `ST_CONTINUE` → continue; other non-OK → propagate |
| `S_RETURN` | eval optional expr into `I->ret` (or `VAL_NONE`); return `ST_RETURN` |
| `S_BREAK/CONTINUE` | return the code |
| `S_SLEEP` | eval → int ≥ 0 else error → `out_sleep_ms(I->out, ms)` (05) |
| `S_TYPE` | for each segment: text → emit bytes; expr → eval, `to_string`, emit; then `"\n"` unless `stop_output`. Emission goes through `out_type(I->out, bytes, len)` which does the per-code-point pacing and the US keymap (05) |
| `S_CALL` | evaluate the call, discard the value (a no-value return is fine here) |
| `S_CHORD` | `out_chord(...)`: press the resolved chord and release (05 §2) |
| `S_HOLD` | `out_hold(...)`: add the chord's keys to the persistent held set |
| `S_RELEASE` | `out_release(..., c.all)`: remove them (or clear all); releasing a key not held is an error |
| `S_DELAY` | eval → int ≥ 0 else error → `out_set_default_delay(I->out, ms)` |
| `S_KEYDELAY` | eval → int ≥ 0 else error → `out_set_tick(I->out, ms)` (the per-key tick, the in-script `-d`) |
| `S_FASTMODE` | eval → int ≥ 0 else error → `out_set_fast(I->out, n != 0)` (in-script `--fast`, HID only) |
| `S_ULTRAMODE` | eval → int ≥ 0 else error → `out_set_ultra(I->out, n != 0)` (in-script `--ultra`, HID only; wins over fast) |
| `S_REPEAT` | eval → int ≥ 0 else error → re-run `I->last_action` that many times (see §4.1); no preceding action → error |

## 5. Function calls

```
call_function(I, Func *f, Value *args, int nargs, Value *out):
    if (nargs != f->nparams)      → ST_ERR "f expects 2 arguments, got 3"
    if (++I->depth > 200)         → ST_ERR "recursion too deep"      (picol.c:61, :389-393)
    Frame *fr = new_frame(parent = I->globals)   // NOT the caller's frame: no closures, no dynamic scope
    bind params into fr; save I->frame; I->frame = fr
    I->ret = VAL_NONE
    Status s = exec_block(I, f->body)
    restore I->frame; free_frame(fr); I->depth--
    if (s == ST_RETURN) { *out = I->ret; return ST_OK; }
    if (s == ST_OK)     { *out = VAL_NONE; return ST_OK; }
    return s;                                     // ST_ERR
```

The caller of `eval_expr` for an `E_CALL` checks `out->type == VAL_NONE` and raises
`"function 'f' returned no value"` when the call is used as an expression. `S_CALL` skips that
check.

Frames link to `globals`, not to the caller, which is what gives the two-level scoping of
spec §4.5 for free (picol pushes a frame whose parent is the previous frame, `picol.c:713-714`,
giving dynamic scope; we deliberately do not).

## 6. Hoisting and startup

```
main:
    parse → Program
    register every Func by name (duplicate → syntax error reported at the second declaration)
    open output (05), install SIGPIPE ignore
    exec_block(top)
    flush/close, exit code per 06
```

`import` (01 §6.7) is resolved entirely inside `parse`: the imported file's functions and top-level
statements are spliced into this one `Program` at the import point, so the evaluator sees a single
merged program and `import` leaves no runtime node. Imported statements therefore run in place, and
imported functions are registered and hoisted exactly like local ones.

## 7. Recursion depth and the C stack

Each script call costs roughly 3–5 nested C frames (`exec_block → exec_stmt → eval_expr →
call_function`), each under 200 bytes with `-O2`. 200 script frames ≈ 200 KB, well inside
the 8 MB main-thread default on every target. Deeply nested *expressions* also recurse; a
parenthesis nest 10 000 deep would overflow. Cap expression nesting in the parser at 256 with a
syntax error, then the C stack is bounded without any runtime check.

## 8. Upgrades deliberately not built
- **Resolver/slot indices** (clox `resolveLocal`, `compiler.c:426-432`): replaces hash lookups with array indexing. Only worth it if a profile ever shows `lookup` above `clock_nanosleep`, which it cannot in a sleep-dominated program.
- **Bytecode VM**: see 00-overview.
