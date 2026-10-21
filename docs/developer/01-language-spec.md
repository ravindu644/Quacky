# 01 · Language specification

Quacky is a shell-flavoured scripting language that drives a USB HID keyboard. A script
computes with ints, bools and strings, loops, branches, calls functions, and emits two kinds
of key activity: `type`/`typeln` write literal text, and a **chord** (`ctrl + c`) presses keys
together. The interpreter turns each into USB HID keyboard reports written to `/dev/hidgX`
(the node provided by the companion USB Arsenal project), so the host computer sees a real
keyboard typing at supersonic speed. For safe dry-runs and the test suite it can instead write
a human-readable transcript (see 05). Pacing is one key event per tick (default 5 ms).

This is an educational, free and open-source tool for learning how HID keyboards, USB gadgets
and small interpreters work.

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
const true false sleep type typeln type_file typeln_file hold release delay keydelay repeat
```

### Key names (contextual, not reserved)

Key words such as `ctrl`, `shift`, `enter`, `f5`, `down` are recognised **only where a key is
expected**: at the start of a chord statement, after a `+`, and as the argument of
`hold`/`release`. They are *not* global keywords, so `shift` and `enter` remain legal variable
names, and the End key (`endkey`) does not collide with the block-closing `end`. The full list
is in §6.1.

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
            | type_file_stmt
            | chord_stmt
            | hold_stmt
            | release_stmt
            | delay_stmt
            | keydelay_stmt
            | repeat_stmt
            | call_stmt ;

assignment  = IDENT "=" expr ;
const_decl  = "const" IDENT "=" expr ;

chord_stmt  = key { "+" key } ;                                      (* see §6 *)
hold_stmt   = "hold" key { "+" key } ;
release_stmt= "release" ( "all" | key { "+" key } ) ;
delay_stmt  = "delay" expr ;                                         (* persistent default pause, ms *)
keydelay_stmt = "keydelay" expr ;                                    (* persistent per-key tick, ms (in-script -d) *)
repeat_stmt = "repeat" expr ;                                        (* re-run the previous action expr times *)
key         = KEY_NAME | LETTER | DIGIT ;                            (* KEY_NAME from §6.1 *)

if_stmt     = "if" expr "then" block
              { "elif" expr "then" block }
              [ "else" block ]
              "fi" ;
while_stmt  = "while" expr "do" block "done" ;
for_stmt    = "for" IDENT "in" expr ".." expr "do" block "done" ;   (* inclusive range, optional feature *)
func_decl   = "func" IDENT { IDENT } sep block "end" ;                (* top level only *)
return_stmt = "return" [ expr ] ;
sleep_stmt  = "sleep" expr ;                                          (* milliseconds *)
type_stmt   = ( "type" | "typeln" ) RAW_LINE ;                        (* see §5 *)
type_file_stmt = ( "type_file" | "typeln_file" ) expr ;              (* expr is a string path, §5 *)
call_stmt   = IDENT "(" [ expr { "," expr } ] ")" ;

block       = { sep } { statement { sep } } ;                         (* ends at the keyword that closes it *)

expr        = Pratt expression over the operator table in §3 ;
primary     = INT | STRING | "true" | "false" | IDENT
            | IDENT "(" [ expr { "," expr } ] ")"
            | "(" expr ")" ;
```

Block-closing keywords (`then do done fi elif else end`) have no prefix or infix rule in the
expression parser, so an expression naturally stops in front of them. That is what makes
`while i < 3 do` and `if i == 1 then typeln one` work without parentheses.

A statement that begins with an identifier is dispatched by its first and second tokens: `=`
→ assignment, `(` → call, a key name (§6.1) → chord statement, otherwise the
"expected assignment or call" error. So `enter` is a keystroke, `down` is a keystroke, but
`enter = 5` is still an assignment to a variable named `enter`.

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

## 5. The `type` and `typeln` statements

```
type   Hello $name                        (* no trailing newline *)
typeln Hello $name, you have ${count * 2} items.
```

Both statements share one raw-line body (`parse_raw_line`). They differ in a single bit: `typeln`
appends a trailing `\n` (Enter), `type` does not. This is the print / println split; it makes "no
Enter" the default and keeps a hidden Enter out of `type`.

- Everything after the keyword and a single space up to end of line is the raw text. The lexer is in **raw mode** here: `#` is not a comment, `"` is not a string, `;` does not end the statement.
- Leading: exactly one space after the keyword is consumed; further spaces are text. Trailing: `\r` is stripped, other trailing spaces are kept.
- Interpolation: `$name` (identifier chars) and `${ expr }` (any expression, sub-lexed in normal mode, braces must balance). A `$` not followed by an identifier or `{` is literal. `\$` is always literal `$`.
- Escapes, as `echo -e`: `\\ \n \t \r \e \a \b \v \f \xHH \0NNN`. `\c` stops output at that point and, on a `typeln`, suppresses the trailing newline (echo's convention), so `typeln X\c` equals `type X`.
- Output: `type` is exactly the text; `typeln` is the text + `\n`. A bare `type` emits nothing, a bare `typeln` emits just `\n`. The trailing newline is baked into the last text segment at parse time, so eval and output add nothing.
- Each UTF-8 **code point** is one tick: write it with one `write(2)`, then sleep until the next deadline. Multi-byte characters are never split across ticks.
- Interpolated values use `to_string` from §4.3.

### 5.1 `type_file` and `typeln_file`

`type_file expr` types the contents of a file verbatim (no escape decoding, no interpolation);
`typeln_file` appends a trailing newline. The path `expr` is a string, resolved **relative to the
running script's directory** (an absolute path is used as-is). The file must be properly encoded
UTF-8 text with no NUL byte, so a binary file is rejected rather than typed. That confirms the file
is text; whether each character has a US-layout key is the separate run-time check `type` already
makes in the HID backend (§6), not a parse-time one.

Resolution and validation happen in two places, keyed on whether the path is a compile-time
constant:

- A **literal** path (`type_file "notes.txt"`) is resolved, read and validated **at parse time**, by
  the shared loader in `textfile.c`. A missing or non-text file is a parse-phase error: it exits 2
  having typed nothing, and `--check` reports it, exactly like any other syntax error. The validated
  bytes are baked into the arena, so evaluation only streams them.
- A **computed** path (a variable or expression) is not known until the script runs, so it is
  resolved and validated **at run time** (a failure is a run-time error, exit 1). Under `--check`
  the parser emits a warning that the path could not be checked statically, then continues.

```
type_file "notes.txt"            (* checked by --check *)
base = "/tmp"
typeln_file base + "/notes.txt"  (* checked when it runs; --check warns *)
```

## 6. Keystrokes and HID output

`type` and `typeln` cover literal text. Everything else a keyboard can do (named keys, modifier
combinations, keeping a key down) is expressed with chords and the `hold`/`release` verbs.

### 6.1 Key names

Key names are lowercase words, recognised only in key position (§2). They fall into two groups.

**Modifiers** (set a bit in the report's modifier byte; never occupy a key slot):

| Name | Aliases | Modifier bit |
|---|---|---|
| `ctrl` | `control` | `0x01` (left control) |
| `shift` | | `0x02` (left shift) |
| `alt` | `option` | `0x04` (left alt) |
| `super` | `win`, `gui`, `meta`, `command` | `0x08` (left GUI / Super / Windows / Command) |
| `altgr` | `ralt` | `0x40` (right alt, for layouts that need it) |

**Named keys** (one HID usage, occupy a key slot):

```
enter  esc  backspace  tab  space  delete  insert  endkey  home  pageup  pagedown
up  down  left  right   capslock  numlock  scrolllock  printscreen  pause  menu
f1 f2 f3 f4 f5 f6 f7 f8 f9 f10 f11 f12 … f24
```

The End key is spelled `endkey` because `end` is reserved for closing a `func` block. A single
letter or digit in key position (`c`, `4`) is mapped through the same US table as `type`
(§6.4), so `ctrl + c` and `alt + 4` work. Usage numbers are listed in Appendix A of 05.

### 6.2 Chords

A chord is one or more keys joined with `+`, pressed **together** and released together:

```
enter                 # one named key
ctrl + c
ctrl + alt + delete
super + r
shift + tab
```

- Modifiers in the chord are OR'd into the report's modifier byte; the non-modifier keys fill
  the key slots. At most **6** non-modifier keys (boot-protocol rollover); more is a runtime
  error.
- If a letter/digit key needs Shift on the US layout (`$`, `A`), that Shift is merged with any
  modifiers you wrote.
- A lone named key (`enter`) is just a one-key chord.

### 6.3 Holding keys: `hold` / `release`

Chords press and release in one step. To keep keys down across later statements, use `hold`;
undo it with `release`:

```
hold shift
down
down
down
release shift            # types three Shift+Down, i.e. a downward selection
```

- `hold` adds its keys to a persistent held set that is merged into **every** report until
  released; `release` removes them. `release all` clears the whole held set.
- Releasing a key that is not held is a runtime error.

### 6.4 Typing text as keys

In HID output each code point produced by `type` is looked up in a US-QWERTY
character → (modifier, usage) table and emitted as a press then a release. `\n` presses
`enter`, `\t` presses `tab`. Uppercase and shifted symbols carry the Shift bit; there is no
runtime shift logic, the table holds it. A code point with no US-layout key is a runtime error
in HID mode (`no US-layout key for U+XXXX`); in transcript mode it is written verbatim. One
documented hook (05) allows alternate layout tables later. v1 ships US QWERTY only.

### 6.5 Pacing: `delay`, `keydelay` and `repeat`

```
delay 100                # from here on, a 100 ms gap between actions (delay 0 turns it off)
keydelay 10              # and 10 ms between individual key events (the in-script -d)
sleep 500                # one-shot pause of 500 ms, right here
down
repeat 5                 # press down five more times (six presses total)
```

- `delay N` sets a **persistent** default pause inserted *between* the output actions a script
  sends (keystrokes, `type`/`typeln`, chords, `hold`/`release`), never before the first or after
  the last, and never on assignments or control flow. It is distinct from `sleep N`, which pauses
  once where written and adds to the gap rather than replacing it. Both take an int ≥ 0 (ms);
  negative is a runtime error. Default is `delay 0`.
- `keydelay N` sets a **persistent** per-key-event tick, the gap between each character a `type`
  emits and between chords, the in-script equivalent of the `-d` flag (it writes the same
  `tick_ns`). This is a finer grain than `delay`: `delay` spaces whole actions, `keydelay` spaces
  the keys within them. Int ≥ 0 (ms), negative is a runtime error; default `keydelay 0` is full
  speed. A later `keydelay` or `-d` is just the value in effect from that point.
- `repeat N` re-runs the immediately preceding keystroke, `type`, or chord statement N more
  times. `repeat` with no preceding action, or after a non-keystroke statement, is a runtime
  error.

### 6.6 What drives the hardware

Each chord/keystroke becomes USB HID boot-keyboard reports written to `/dev/hidgX`: an 8-byte
press report `[modifiers, 0, k1..k6]` followed by a release report (the held set, or all
zeroes). Timing, the report layout, the device node and the dry-run transcript are specified in
05; device composition (which `/dev/hidgX` exists, the USB identity) is owned by USB Arsenal,
not by Quacky.

## 7. Sample programs

```
# greet.qk
const DELAY = 300
func greet name
  typeln Hello $name!
  sleep DELAY
end

names = "world"
greet(names)

i = 0
while i < 3 do
  if i == 1 then typeln one
  elif i == 2 then typeln two # this '#' is printed, it's inside the type line
  else typeln zero
  fi
  i = i + 1
done

#[ block comment
   spanning lines ]#
func fib n
  if n < 2 then return n fi
  return fib(n - 1) + fib(n - 2)
end
typeln fib(20) = ${fib(20)}
type done
```

Expected output stream (5 ms per character):

```
Hello world!
zero
one
two # this '#' is printed, it's inside the type line
fib(20) = 6765
done
```
(no newline after `done`).

A keystroke-oriented script, `open-notes.qk`, that opens a run dialog on the host and types:

```
# open-notes.qk
delay 50               # settle time after every step
super + r              # open the Run dialog
sleep 400
typeln notepad         # type the name and press Enter to launch it
sleep 600
type Hello from Quacky!   # no Enter: the cursor stays on this line
hold shift
home                   # select the line we just typed
release shift
```

## 8. Explicit non-goals
No floats, arrays, maps, nil, closures, nested functions, modules, string indexing, stdin, or
any output other than the HID key stream (and its dry-run transcript). Mouse reports, consumer
/ media keys, LED and button handling, USB identity and storage are **not** part of the
language: USB Arsenal owns the gadget. Adding any of these is a new design decision, not an
extension of this spec.
