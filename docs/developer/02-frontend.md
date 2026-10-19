# 02 · Frontend: lexer, parser, AST, arena

One pass from source text to an AST that lives in a single arena until exit. Lexer, parser and
AST are documented together because the `type` statement cuts across all three.

## 1. Source handling

- Read the whole file into one `char*` buffer, NUL-terminated. Keep it alive for the whole run: tokens and error messages point into it (clox `scanner.c:8-12`, chibicc `tokenize.c:100-112`).
- Normalise CRLF to LF in place before lexing (chibicc does this in `canonicalize_newline`, `tokenize.c:691`). Saves every later stage from thinking about `\r`.
- Keep a `const char *filename` for error messages.

## 2. Tokens

```c
typedef enum {
    T_EOF, T_NEWLINE, T_SEMI,
    T_INT, T_STRING, T_IDENT, T_RAW_LINE,          // RAW_LINE only after `type`
    // keywords
    T_IF, T_THEN, T_ELIF, T_ELSE, T_FI, T_WHILE, T_DO, T_DONE, T_FOR, T_IN,
    T_FUNC, T_END, T_RETURN, T_BREAK, T_CONTINUE, T_CONST, T_TRUE, T_FALSE,
    T_SLEEP, T_TYPE, T_HOLD, T_RELEASE, T_DELAY, T_REPEAT,
    // punctuation / operators
    T_LPAREN, T_RPAREN, T_COMMA, T_ASSIGN, T_DOTDOT,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT,
    T_SHL, T_SHR, T_AMP, T_PIPE, T_CARET, T_TILDE,
    T_LT, T_LE, T_GT, T_GE, T_EQ, T_NE, T_BANG, T_ANDAND, T_OROR,
    T_ERROR
} TokKind;

typedef struct {
    TokKind     kind;
    const char *start;   // slice into the source buffer, NOT a copy
    int         len;
    int         line, col;
} Token;
```

Tokens are slices (clox `scanner.c:65-72`); nothing is copied until the parser decides it
needs a value. `line`/`col` are computed as the lexer advances, so error reporting is O(1).
chibicc instead recomputes `line_no` by scanning from the file start on error (`tokenize.c:52-57`); either works, the precomputed version is simpler for a language with `col` in messages.

## 3. Lexer

A hand-written scanner with one character of lookahead, structured exactly like clox
`scanToken` (`scanner.c:205-255`): skip whitespace and comments, mark `start`, dispatch on the
first character. Deviations:

| Topic | Rule | Why |
|---|---|---|
| Newline | emit `T_NEWLINE`; collapse runs of newlines/`;`/blank lines into one token | the parser treats it as a statement separator; collapsing keeps the parser's `skip_seps()` trivial. picol does the same in `picolParseEol` (`picol.c:131-141`). |
| `#` comment | skip to `\n` **without consuming the `\n`** | the newline must still terminate the statement |
| `#[ ... ]#` | scan to `]#`; error "unclosed block comment" at the `#[` position | chibicc `tokenize.c:511-515` uses `strstr`; same idea |
| Keywords | after reading an identifier, switch on length then `memcmp` (clox `checkKeyword`, `scanner.c:117-128`) or a tiny sorted table; both are O(1)-ish for 20 keywords | no hash map needed |
| Numbers | `0x`, `0b`, decimal; parse with `strtoull` into `uint64_t` and range-check against `INT64_MAX` | overflow becomes a syntax error, never UB |
| Strings | `"` to `"` on one line; `\n` inside → "unterminated string" (clox `scanner.c:191-202`, chibicc `string_literal_end`, `tokenize.c:234-240`) | escape *decoding* happens in the parser, not the lexer, so the token stays a slice |
| `..` | emitted as `T_DOTDOT` when the lexer sees two dots | only used by `for` |

### 3.1 The `type` rule: raw mode

This is the one genuinely non-standard part of the lexer.

```
after emitting T_TYPE:
    if next char is ' '  → consume exactly one space
    start = current
    advance until '\n' or EOF            # do NOT treat '#', '"', ';' specially
    emit T_RAW_LINE(start, len)
    (the '\n' is then lexed normally into T_NEWLINE)
```

- The lexer needs one bit of state: `after_type`. Set it when `T_TYPE` is emitted, clear it after `T_RAW_LINE`. This is the same trick chibicc uses with `at_bol` to let the preprocessor consume a directive to end of line while the rest of the lexer stays free-form (`chibicc.h:88`, `preprocess.c:233`, `preprocess.c:82-89`).
- `type` is a keyword *everywhere*, not just at statement start. Simpler, and it is what makes `if x then type one` work.
- Interpolation is **not** lexed here. The raw slice goes to the parser whole.

## 4. Parser

Recursive descent for statements, Pratt for expressions. Single token of lookahead
(`cur`, `prev`), `advance()`, `check()`, `match()`, `expect()` helpers as in clox
`compiler.c:150-200`.

### 4.1 Statements

```
parse_program:   skip_seps; while !EOF: parse_statement; expect_sep_or_closer
parse_statement: switch cur.kind:
    T_IDENT     → if next is '=' → assignment; elif next is '(' → call_stmt;
                  elif cur names a key (keys.c) → parse_chord (see 4.4); else error
    T_CONST     → const_decl
    T_IF        → parse_if
    T_WHILE     → parse_while
    T_FOR       → parse_for  (desugar to: v = a; __end = b; while v <= __end do body; v = v + 1 done)
    T_FUNC      → parse_func (error if depth > 0: "functions must be top level")
    T_RETURN    → error if not inside func; optional expr if next token is not a sep/closer
    T_BREAK/CONTINUE → error if loop_depth == 0
    T_SLEEP     → expr
    T_DELAY     → expr                                         (* persistent default pause *)
    T_REPEAT    → expr                                         (* repeat previous action *)
    T_HOLD      → parse_chord body → S_HOLD
    T_RELEASE   → "all" → S_RELEASE(all); else parse_chord body → S_RELEASE
    T_TYPE      → expect T_RAW_LINE; parse_raw_line (see 4.3)
parse_block(closers...): skip_seps; while cur ∉ closers: parse_statement; expect sep unless cur ∈ closers
```

- `parse_if` loops on `T_ELIF`, building a chain of `IF` nodes where the `else` branch is the next `IF`. `elif` has no node type of its own.
- Statement terminator rule: after a statement the parser requires `T_NEWLINE`, `T_SEMI`, or a block-closing keyword (`fi`, `done`, `end`, `elif`, `else`). That is what allows `if x then type a fi` on one line.
- The parser tracks `loop_depth` and `in_func` counters so `break`/`continue`/`return` placement errors are **syntax** errors, caught before anything runs.

### 4.2 Expressions: Pratt

Direct transplant of clox `parsePrecedence` (`compiler.c:980-1002`) and its `rules[]` table
(`compiler.c:862-977`): one row per token kind with `prefix`, `infix`, `precedence`.
Precedence enum follows the spec's §3 table:

```c
typedef enum { P_NONE, P_OR, P_AND, P_BOR, P_BXOR, P_BAND, P_EQ, P_CMP, P_SHIFT, P_TERM, P_FACTOR, P_UNARY, P_CALL } Prec;
```

Rows that matter:

| Token | prefix | infix | prec |
|---|---|---|---|
| `(` | grouping | call (only when previous token was IDENT; the IDENT's prefix rule handles it) | P_CALL |
| `-` | unary | binary | P_TERM |
| `!` `~` | unary | — | — |
| `&&` | — | and_ (short-circuit node) | P_AND |
| `\|\|` | — | or_ | P_OR |
| INT STRING TRUE FALSE IDENT | literal / variable | — | — |
| every keyword, `)` `,` `..` NEWLINE `;` | — | — | P_NONE |

Because block keywords have `P_NONE`, `parse_expr()` stops in front of `then`/`do` with no
special casing. The parser does not need to know it is inside a condition.

`&&` / `||` are their own AST nodes (not generic binary ops) so the evaluator can
short-circuit. Call arguments are parsed with `parse_expr` separated by `,` up to `)`.

### 4.3 Parsing the raw `type` line

The parser receives `T_RAW_LINE` as a slice and splits it into **segments**:

```c
typedef struct { enum { SEG_TEXT, SEG_EXPR } kind; Str text; Expr *expr; } Seg;
```

Algorithm over the slice:
1. Walk bytes. On `\` decode an escape (chibicc `read_escaped_char`, `tokenize.c:180-231`, plus echo's `\c` and `\e`); `\$` yields literal `$`. `\c` sets `stop_output` on the node and ends scanning.
2. On `$` followed by identifier start → `SEG_EXPR` holding a `VAR` node (picol `picolParseVar`, `picol.c:173-191`, including the "lone `$` is literal" rule at `:183-185`).
3. On `${` → find the matching `}` counting nested braces, then **run a sub-lexer + `parse_expr` on that substring** with normal (non-raw) lexing. Unbalanced → syntax error at the `${`. Wren's `%(...)` interpolation does the same with a paren counter inside its main lexer (`wren_compiler.c`, `readString`, `MAX_INTERPOLATION_NESTING 8`); running a child lexer on the substring is simpler for us because `type` never nests.
4. Anything else appends to the current `SEG_TEXT`.
5. Unless `\c` was seen, append a final `SEG_TEXT` of `"\n"`.

Decoded text segments are copied into the arena (they are no longer slices once escapes are
decoded). Column numbers for errors inside `${...}` are `raw.col + offset`.

### 4.4 Chords and keystroke statements

Key names are **contextual**, not keyword tokens: the lexer emits a plain `T_IDENT` for
`ctrl`, `enter`, `f5`, `down`, and the parser decides whether it is a key by looking it up in
`keys.c`. This keeps the global keyword set small and lets `shift`/`enter` stay usable as
variable names (01 §2). A chord parses into a resolved form, so the evaluator never re-parses
names:

```
parse_chord:
    c = {mod: 0, usages: [], n: 0}
    loop:
        resolve the current token as a key:
            T_IDENT that is a modifier name → OR its bit into c.mod
            T_IDENT that is a named key      → look up its usage, append to c.usages
            T_IDENT of length 1 / T_INT of one digit → US char map (keys.c) → merge mod, append usage
            otherwise → syntax error "unknown key 'X'"
        if next token is '+' : consume it and continue
        else break
    if c.n > 6 : syntax error "too many keys in a chord (max 6)"
```

- A lone named key (`enter`) is a one-key chord: the loop runs once and stops at the newline.
- `hold`/`release` reuse `parse_chord` for their argument; `release all` is a special case
  recognised before `parse_chord`.
- Resolving at parse time means an unknown key name is a **syntax** error (caught before
  anything types), consistent with 06. The only keystroke error deferred to runtime is an
  unmappable `type` character, which depends on the chosen output layout.
- `keys.c` exposes two pure lookups the parser calls: `key_lookup(name) → {kind, bit|usage}`
  and `char_key(codepoint) → {mod, usage} | none`. The same tables serve the output module (05
  §4), so there is one source of truth for names and usages.

## 5. AST

Tagged structs, allocated from the arena, children by pointer. Keep it to two node families:

```c
typedef enum { E_INT, E_BOOL, E_STR, E_VAR, E_UNARY, E_BINARY, E_AND, E_OR, E_CALL } ExprKind;
typedef struct Expr {
    ExprKind kind; int line, col;
    union {
        int64_t  ival;  bool bval;  Str sval;  Str name;
        struct { TokKind op; struct Expr *a, *b; } bin;          // unary uses only a
        struct { Str name; struct Expr **args; int nargs; } call;
    };
} Expr;

typedef enum { S_ASSIGN, S_CONST, S_IF, S_WHILE, S_RETURN, S_BREAK, S_CONTINUE,
               S_SLEEP, S_TYPE, S_CALL,
               S_CHORD, S_HOLD, S_RELEASE, S_DELAY, S_REPEAT } StmtKind;

typedef struct { uint8_t mod; uint8_t usages[6]; int n; bool all; } Chord;   // resolved at parse time; `all` only for S_RELEASE

typedef struct Stmt {
    StmtKind kind; int line, col;
    union {
        struct { Str name; Expr *value; } assign;
        struct { Expr *cond; struct Block *then_b; struct Stmt *else_s; } iff;  // else_s: next IF, or a BLOCK, or NULL
        struct { Expr *cond; struct Block *body; } loop;
        Expr *expr;                                   // RETURN (nullable), SLEEP, DELAY, REPEAT, CALL
        struct { Seg *segs; int nsegs; bool stop_output; } type;
        Chord chord;                                  // CHORD, HOLD, RELEASE
    };
} Stmt;

typedef struct Block { Stmt **stmts; int n; } Block;
typedef struct Func  { Str name; Str *params; int nparams; Block *body; } Func;
typedef struct Program { Block *top; Func **funcs; int nfuncs; } Program;
```

`for` never appears in the AST (desugared), nor does `elif`.

## 6. Arena

Everything the frontend produces (nodes, decoded strings, arrays) comes from one bump
allocator and is freed once at exit. Lifetime is trivially correct and there is no `free()`
in the frontend at all (chibicc allocates with `calloc` and never frees, `tokenize.c:101`; we
go one step further and avoid `malloc` churn).

```c
typedef struct Chunk { struct Chunk *next; size_t used, cap; char data[]; } Chunk;
typedef struct { Chunk *head; } Arena;

void *arena_alloc(Arena *a, size_t n) {
    size_t align = _Alignof(max_align_t);              // <stddef.h>, C11
    n = (n + align - 1) & ~(align - 1);
    if (!a->head || a->head->cap - a->head->used < n) new_chunk(a, n > 64*1024 ? n : 64*1024);
    void *p = a->head->data + a->head->used; a->head->used += n; return p;
}
```

- Growable arrays during parsing (statement lists, args): build in a small `malloc`ed scratch vector, then copy the final array into the arena and free the scratch. Keeps the arena free of dead space.
- Out of memory: print and `exit(1)` (picol `xrealloc`, `picol.c:39-46`). No recovery path.
- `_Alignof(max_align_t)` requires `<stddef.h>`; verified to compile on all five toolchains including GCC 9.4.

## 7. Future warning hook
The spec keeps C's bitwise precedence. If it ever bites, the right place for a warning is the
`binary()` infix handler: when the operator is `&`/`|`/`^` and either child is an
unparenthesised comparison node, emit "suggest parentheses". Needs a `parenthesised` bit on
`Expr`; not built now.
