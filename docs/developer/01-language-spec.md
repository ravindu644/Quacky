# 01 · Language specification

Quacky is a shell-flavoured scripting language whose only observable side effect is
appending characters to one output stream, one character per tick (default 20 ms).

## 1. Lexical structure

| Element | Rule |
|---|---|
| Encoding | UTF-8 source. The lexer is byte-oriented; non-ASCII bytes are only legal inside strings, comments and `type` lines. |
| Newlines | `\n` ends a statement. A `\r` immediately before `\n` is dropped (CRLF scripts work). |
| Statement separator | newline or `;`. Blank lines and repeated separators are ignored. |
| Line comment | `#` to end of line. Not recognised inside a string or inside a `type` line. |
| Block comment | `#[` … `]#`, may span lines, does **not** nest. Unterminated → syntax error at the opening `#[`. |
| Identifiers | `[A-Za-z_][A-Za-z0-9_]*`, case-sensitive. |
| Integers | decimal `[0-9]+`, hex `0x[0-9a-fA-F]+`, binary `0b[01]+`. Out of int64 range → syntax error. |
| Strings | `"..."` on one line. Escapes: `\\ \" \n \t \r \e \$ \xHH \0NNN`. Unknown escape → syntax error. |
| Booleans | `true`, `false`. |

### Keywords (reserved everywhere, cannot be variable names)

```
if then elif else fi while do done for in func end return break continue
const true false sleep type
```

## 2. Grammar (EBNF)

```ebnf
program     = { sep } { statement { sep } } EOF ;
sep         = NEWLINE | ";" ;

statement   = assignment
            | const_decl
            | if_stmt
            | while_stmt
            | for_stmt
            | func_decl
            | return_stmt
            | "break"
            | "continue"
            | sleep_stmt
            | type_stmt
            | call_stmt ;

assignment  = IDENT "=" expr ;
const_decl  = "const" IDENT "=" expr ;

if_stmt     = "if" expr "then" block
              { "elif" expr "then" block }
              [ "else" block ]
              "fi" ;
while_stmt  = "while" expr "do" block "done" ;
for_stmt    = "for" IDENT "in" expr ".." expr "do" block "done" ;   (* inclusive range, optional feature *)
func_decl   = "func" IDENT { IDENT } sep block "end" ;                (* top level only *)
return_stmt = "return" [ expr ] ;
sleep_stmt  = "sleep" expr ;                                          (* milliseconds *)
type_stmt   = "type" RAW_LINE ;                                       (* see §5 *)
call_stmt   = IDENT "(" [ expr { "," expr } ] ")" ;

block       = { sep } { statement { sep } } ;                         (* ends at the keyword that closes it *)

expr        = Pratt expression over the operator table in §3 ;
primary     = INT | STRING | "true" | "false" | IDENT
            | IDENT "(" [ expr { "," expr } ] ")"
            | "(" expr ")" ;
```

Block-closing keywords (`then do done fi elif else end`) have no prefix or infix rule in the
expression parser, so an expression naturally stops in front of them. That is what makes
`while i < 3 do` and `if i == 1 then type one` work without parentheses.

A statement may follow `then`, `do`, or `else` on the same line:

```
if i == 1 then type one
elif i == 2 then type two
else type zero
fi
```

## 3. Operators

Precedence copied from C verbatim (bash `$(( ))` uses the same table, so shell users get
no surprises). Row 1 binds tightest. All binary operators are left-associative.

| # | Operators | Operand types → result |
|---|---|---|
| 1 | unary `-` `!` `~` | int→int, bool→bool, int→int |
| 2 | `*` `/` `%` | int,int→int |
| 3 | `+` `-` | int,int→int; `+` also str,any→str and any,str→str (see §4.3) |
| 4 | `<<` `>>` | int,int→int |
| 5 | `<` `<=` `>` `>=` | int,int→bool; str,str→bool (bytewise) |
| 6 | `==` `!=` | same type→bool; different types → `false` / `true` |
| 7 | `&` | int,int→int |
| 8 | `^` | int,int→int |
| 9 | `\|` | int,int→int |
| 10 | `&&` | bool,bool→bool, short-circuit |
| 11 | `\|\|` | bool,bool→bool, short-circuit |

`a & b == 0` therefore parses as `a & (b == 0)` and is a type error at runtime, exactly as in C.
The spec accepts this cost for shell compatibility; the frontend doc notes where a warning could
be added later. Grouping with `( )` overrides everything.

Any operand of the wrong type is a **runtime error** (no implicit conversion except string `+`).

## 4. Values and semantics

### 4.1 Types
`int` (signed 64-bit), `bool`, `str` (immutable byte string). No floats, arrays, nil, or closures.

### 4.2 Integer rules
| Case | Behaviour |
|---|---|
| `+ - *` overflow | wraps (two's complement), computed through unsigned casts, never UB |
| `/` `%` | truncate toward zero (C semantics) |
| `x / 0`, `x % 0` | runtime error |
| `INT64_MIN / -1` | wraps to `INT64_MIN`; `INT64_MIN % -1` is `0` |
| `<<` `>>` by `n` | `n < 0` or `n >= 64` → `0`; `>>` is arithmetic (sign-propagating) |
| `-INT64_MIN` | wraps to `INT64_MIN` |

### 4.3 Strings
Immutable. `+` concatenates; when exactly one side is a string, the other side is converted with
the same `to_string` used by `type` interpolation: ints in decimal, `true`/`false` for bools.
Comparison `<` etc. is `memcmp` over bytes, shorter prefix sorts first.

### 4.4 Truthiness
There is none. `if`, `elif`, `while`, `!`, `&&`, `||` require `bool`; anything else is a runtime
error. Shell users write `if n != 0 then`, which is the intended style.

### 4.5 Variables and scope
- Assignment creates a variable if it does not exist; no declaration keyword.
- Two scope levels only: **global** and **current function frame**.
- Lookup: current frame, then global.
- Assignment: if the name exists in the current frame → update it; else if it exists globally → update the global; else create it in the current frame. Top-level code's frame *is* the global frame. This is bash-like (globals writable from functions) and needs no resolver.
- `const NAME = expr` creates a variable that any later assignment to rejects with a runtime error. Re-declaring a `const` is also an error.
- Reading an undefined variable is a runtime error.

### 4.6 Functions
- Declared only at top level; declaration is hoisted (all `func` declarations are registered before the first statement runs, so forward calls work).
- Parameters are passed by value; arity mismatch is a runtime error.
- `return expr` returns a value. `return` or falling off the end returns *no value*; using such a call inside an expression is a runtime error ("function returned nothing").
- Recursion allowed; depth over **200 frames** is a runtime error, not a crash.

### 4.7 Control flow
- `break` / `continue` are legal only inside `while`/`for`; elsewhere a **syntax** error.
- `return` outside a function is a syntax error.
- `for v in a..b` iterates `v = a, a+1, …, b` inclusive; if `a > b` the body never runs. The loop variable is a normal variable (persists after the loop). It desugars to `while` in the parser.

### 4.8 `sleep expr`
`expr` must be an int ≥ 0, milliseconds. Negative → runtime error. Timing is absolute-deadline
based (see 05).

## 5. The `type` statement

```
type Hello $name, you have ${count * 2} items.\n
```

- Everything after `type` and a single space up to end of line is the raw text. The lexer is in **raw mode** here: `#` is not a comment, `"` is not a string, `;` does not end the statement.
- Leading: exactly one space after `type` is consumed; further spaces are text. Trailing: `\r` is stripped, other trailing spaces are kept.
- Interpolation: `$name` (identifier chars) and `${ expr }` (any expression, sub-lexed in normal mode, braces must balance). A `$` not followed by an identifier or `{` is literal. `\$` is always literal `$`.
- Escapes, as `echo -e`: `\\ \n \t \r \e \a \b \v \f \xHH \0NNN`. `\c` stops output and suppresses the trailing newline (echo's convention).
- Output = text + `\n`. A bare `type` with nothing after it emits just `\n`.
- Each UTF-8 **code point** is one tick: write it with one `write(2)`, then sleep until the next deadline. Multi-byte characters are never split across ticks.
- Interpolated values use `to_string` from §4.3.

## 6. Sample programs

```
# greet.qk
const DELAY = 300
func greet name
  type Hello $name!
  sleep DELAY
end

names = "world"
greet(names)

i = 0
while i < 3 do
  if i == 1 then type one
  elif i == 2 then type two # this '#' is printed, it's inside the type line
  else type zero
  fi
  i = i + 1
done

#[ block comment
   spanning lines ]#
func fib n
  if n < 2 then return n fi
  return fib(n - 1) + fib(n - 2)
end
type fib(20) = ${fib(20)}
type done\c
```

Expected output stream (20 ms per character):

```
Hello world!
zero
one
two # this '#' is printed, it's inside the type line
fib(20) = 6765
done
```
(no newline after `done`).

## 7. Explicit non-goals
No floats, arrays, maps, nil, closures, nested functions, modules, string indexing, stdin, or
any output channel other than the `type` stream. Adding any of these is a new design decision,
not an extension of this spec.
