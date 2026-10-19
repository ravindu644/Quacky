# 06 · Errors

## 1. Two classes

| Class | Detected by | Examples | Exit |
|---|---|---|---|
| **Syntax** | lexer/parser, before anything runs | unterminated string, `break` outside loop, `func` inside `if`, duplicate `func`, unbalanced `${` | 2 |
| **Runtime** | evaluator / output | undefined variable, type mismatch, `/ 0`, const reassignment, arity, recursion depth, `EPIPE` | 1 |
| Usage | `main` | unknown flag, unreadable script, cannot open `-o` file | 64 (`EX_USAGE`), 66 (`EX_NOINPUT`), 73 (`EX_CANTCREAT`) from `<sysexits.h>` |

Exit 0 means the whole program ran. A syntax error therefore guarantees **no output was
produced**, which matters for a tool whose output is append-only.

## 2. Message format

```
greet.qk:12:9: error: operator '+' expects int and int, got str and bool
  total = name + flag
               ^
```

`file:line:col: error: message`, then the offending source line, then a caret under the
column. This is chibicc's `verror_at` (`tokenize.c:28-50`): find the line containing `loc` by
walking back to the previous `\n`, print it, print `col` spaces and `^`. Since tokens are
slices into the source buffer (02 §2), `loc` is just `tok.start`. chibicc computes the display
width for the caret so tabs and wide characters line up; replace tabs with a single space when
printing the line and the simple column count is correct.

Runtime errors use the `line`/`col` stored on the AST node that failed (every `Expr`/`Stmt`
carries them, 02 §5). clox prints only `[line N] in script` (`vm.c:46-64`); the caret line is
worth the extra 15 lines of code.

## 3. Propagation: status codes, not longjmp

Decision: **status codes** (03 §1).

| | Status codes (picol) | `setjmp`/`longjmp` (Lua) |
|---|---|---|
| `return`/`break`/`continue` | same mechanism, free | still need their own path |
| Cleanup between throw and catch | normal C control flow: every `str_release` runs | must be done by the catcher, or leaked; Lua keeps all live values on its own stack so it has nothing to clean on the C stack |
| Pitfalls | one `if (s != ST_OK) return s;` per call site; forgettable but greppable | locals modified after `setjmp` must be `volatile`; `jmp_buf` must live in a frame that is still active; Lua chains them: `lua_longjmp.previous`, `ldo.c:61-65`, `LUAI_TRY/THROW`, `:97-105`, `luaD_rawrunprotected`, `:162-166` |
| Cost | one compare per node | one `setjmp` per protected call |

For a refcounted-string tree-walker the cleanup argument decides it. If the interpreter ever
grows a GC that tracks every live value (as Lua's does), `longjmp` becomes viable and the
Lua files above are the template.

## 4. Raising

```c
Status rt_error(Interp *I, const Node *at, const char *fmt, ...);   // formats into I->err, records at->line/col, returns ST_ERR
```

Call sites read `return rt_error(I, e, "undefined variable '%.*s'", (int)name.len, name.p);`.
`main` prints via the same `print_diagnostic(file, src, line, col, msg)` the parser uses, so
both classes look identical to the user.

The parser raises with `syntax_error(P, tok, fmt, ...)` which prints immediately and
`exit(2)`: there is no error recovery or multi-error reporting. First error wins. (clox's
panic-mode synchronisation is an upgrade, not a need.)

## 5. Message catalogue (so wording is consistent)

| Condition | Text |
|---|---|
| unknown identifier | `undefined variable 'x'` |
| const write | `cannot assign to constant 'X'` |
| redeclared const | `constant 'X' already declared` |
| type mismatch | `operator '<op>' expects <t> and <t>, got <t> and <t>` |
| condition not bool | `condition must be bool, got int` |
| div/mod zero | `division by zero` |
| sleep negative | `sleep expects a non-negative int` |
| arity | `function 'f' expects 2 arguments, got 3` |
| no value | `function 'f' returned no value` |
| unknown function | `undefined function 'f'` |
| depth | `recursion too deep (limit 200)` |
| output | `output error: <strerror>` |

Type names in messages: `int`, `bool`, `str`.
