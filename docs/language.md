# The Quacky language

A guided tour of everything the language can do, with a small runnable example for each feature.
Quacky is shell-and-Python-flavoured: no indentation rules, one statement per line (or separated by
`;`), and a handful of keywords. If you have written a shell script, you already read most of this.

Every snippet here is a complete, valid script. Try any of them with a dry run, which prints a
readable transcript of what the keyboard would do without pressing a single key:

```sh
quacky script.qk
```

See [getting-started.md](getting-started.md) for building and running, and [keys.md](keys.md) for
the full list of key names you can press.

## Contents

- [Comments](#comments)
- [Values and variables](#values-and-variables)
- [Operators](#operators)
- [Typing text](#typing-text-type-and-typeln)
- [Pressing keys](#pressing-keys-chords)
- [Holding keys down](#holding-keys-down)
- [Timing: delay, sleep, repeat](#timing-delay-sleep-repeat)
- [Conditions](#conditions-if-elif-else)
- [Loops](#loops-while-and-for)
- [Functions](#functions)
- [Modules: import](#modules-import)
- [Things that will trip you up](#things-that-will-trip-you-up)

## Comments

A `#` runs to the end of the line. A block comment is `#[ ... ]#` and may span lines, but it does
not nest.

```
# this whole line is a comment
type hello          # and this is a trailing comment

#[ a block comment
   over several lines ]#
type world
```

## Values and variables

There are exactly three value types. No floats, no lists, no null.

| Type | Examples |
|---|---|
| **int** (signed 64-bit) | `42`, `-7`, `0xFF` (hex), `0b1010` (binary) |
| **bool** | `true`, `false` |
| **str** | `"hello"`, `"with \"quotes\" and a tab\t"` |

Assign to a variable with `=`. There is no declaration keyword; the first assignment creates it.

```
count = 10
count = count + 1
name  = "world"
ready = true
```

Use `const` for a value that must never change. Assigning to it later is an error.

```
const SPEED = 300
```

### Strings

Strings are written with double quotes and live on one line. These escape sequences work inside a
string: `\\`  `\"`  `\n`  `\t`  `\r`  `\e`  `\a`  `\b`  `\v`  `\f`, plus `\xHH` (a byte in hex) and
`\0NNN` (a byte in octal).

```
msg = "line one\nline two"
```

Strings do not interpolate variables. To build one from a value, join with `+`, which turns the
other side into text automatically:

```
count = 3
label = "you have " + count + " items"
```

## Operators

Integers support the usual arithmetic and bitwise operators. Comparisons produce a bool. `&&` and
`||` short-circuit and require bool operands.

```
a = 6
b = 4
sum  = a + b          # 10
rem  = a % b          # 2
bits = a & b | 1      # bitwise
big  = a > b          # true  (a bool)
ok   = big && b != 0  # true
```

`+` is also string concatenation when either side is a string. Everything else (`- * / % << >> & ^
|`) is integers only.

Precedence follows C, tightest first: unary `- ! ~`, then `* / %`, then `+ -`, then `<< >>`, then
`< <= > >=`, then `== !=`, then `&`, then `^`, then `|`, then `&&`, then `||`. When in doubt, use
parentheses.

Integer math is predictable: `+ - *` wrap on overflow (two's complement, never undefined), `/` and
`%` truncate toward zero and raise an error on divide-by-zero.

## Typing text: `type` and `typeln`

Two statements put text on the host. Both take everything after the first space verbatim, so `#`,
`"` and `;` are ordinary characters here, not comments or quotes. One consequence: a `type` or
`typeln` must be the **last** statement on its line. You can chain with `;` up to it, but anything
after it is typed, not run.

```
super ; sleep 250 ; type hello    # right: the type is last on the line
type hello ; enter                # wrong: this types "hello ; enter", Enter is never pressed
```

- **`type`** types exactly the text, with no Enter at the end.
- **`typeln`** types the text and then presses Enter (a trailing newline).

```
type Hello             # types: Hello
typeln Hello           # types: Hello, then presses Enter
```

It is the print / println split. Consecutive `type`s stay on one line; `typeln` ends the line:

```
type user
typeln name            # the host sees "username", then Enter
```

Spaces after the first one are kept:

```
type   three leading spaces are kept (the first space is just the separator)
```

### Interpolation

Inside a `type` or `typeln` line, `$name` is replaced by a variable's value, and `${ ... }`
evaluates any expression. Use `\$` for a literal dollar sign.

**`$` only works in typed text.** It means "paste this value into the text I am typing", so it
belongs only in a `type` or `typeln` line. Everywhere else, a `delay`, a `sleep`, an `if`, a
function-call argument, any expression, you are not typing text, so you just name the variable with
no `$`:

```
d = 300
delay d                  # right: delay takes an expression
delay $d                 # error: unexpected character '$'
open_app(browser)        # right: a call argument is an expression
type opening $browser    # right: $ belongs here, inside typed text
```

```
name  = "world"
count = 3
typeln Hello $name, you have ${count * 2} items.
typeln that costs \$5
```

### Escapes

The same escapes as strings work in both (`\n`, `\t`, `\xHH`, and so on), plus `\$` for a dollar
sign. A `\n` in the middle of the text presses Enter right there. `\c` stops output at that point
and drops a `typeln`'s trailing Enter, so `typeln x\c` types the same as `type x`:

```
typeln col1\tcol2\tcol3        # a tab-separated line, then Enter
typeln no Enter after this\c   # \c cancels typeln's trailing Enter
```

### Typing a file

To type a whole document, point `type_file` at it. The file's contents are typed **verbatim**: no
escape decoding and no `$` interpolation, so a passage full of `$`, `\`, `#`, quotes, or even
language keywords types exactly as written. `typeln_file` is the same but presses Enter at the end.

```
type_file "notes.txt"          # type the file's contents, no trailing Enter
typeln_file "notes.txt"        # type them, then press Enter
```

- **The path is relative to the script**, not to wherever you run Quacky from (an absolute path is
  used as-is). So a script and its data file travel together.
- **The file must be real text** (valid UTF-8, no NUL bytes); a binary file is rejected, not typed.
  (A valid character your US layout cannot produce is still a run-time error while typing over
  `--hid`, the same as with `type`.)
- **A literal path is checked when you check the script.** `quacky --check script.qk` resolves and
  reads the file, so a missing or non-text file is reported then and nothing is ever typed.
- **A computed path** (a variable or an expression, like `type_file base + "/notes.txt"`) can only
  be checked when the script runs, since the path is not known until then. `--check` prints a
  warning for it rather than reading the file.

## Pressing keys: chords

A line that names keys joined with `+` presses them together and releases them, a "chord". A single
key name on its own is a one-key chord.

```
enter
tab
ctrl + c
ctrl + alt + delete
super + r
shift + tab
```

Modifier keys are `ctrl`, `shift`, `alt`, `super`, and `altgr` (with aliases like `control`, `win`,
`meta`, `command`). Everything else, from `enter` and `f5` to the arrow keys, is a named key. Plain
letters, digits and symbols work too, so `ctrl + c` and `alt + 4` do what you expect. A chord holds
at most six non-modifier keys. The full table is in [keys.md](keys.md).

## Holding keys down

`hold` presses keys and keeps them down across the following lines until you `release` them. This is
how you build a selection or a held modifier.

```
hold shift
down
down
down
release shift        # selects three lines
```

`release all` lets go of everything currently held:

```
hold ctrl
hold shift
# ... some keys ...
release all
```

Releasing a key that is not held is an error, so a script cannot accidentally leave a key stuck.

## Timing: delay, sleep, repeat

- **`delay MS`** sets a default pause inserted *between* the keystrokes a script sends (a chord,
  `type`/`typeln`, `hold`/`release`), never before the first or after the last, and never on plain
  assignments or control flow. A `sleep` adds to it rather than replacing it, so `delay 300` then a
  `sleep 100` between two actions is a 400 ms gap. It stays in effect until you change it; `delay 0`
  turns it off. The default is 0.
- **`sleep MS`** pauses once, right now.
- **`keydelay MS`** sets the gap between *individual key events* (each character a `type` sends, each
  chord), the in-script twin of the `-d` command-line flag. It is a smaller-grained knob than
  `delay`: `delay` pauses between whole actions, `keydelay` slows the typing within them. It stays in
  effect until you change it; `keydelay 0` is full speed.
- **`fastmode N`** and **`ultramode N`** (`N` is `1` for on, `0` for off) switch the HID rollover
  strategy from inside the script, the in-script twins of the `--fast` and `--ultra` flags. They
  change only real `--hid` output (a dry run ignores them), and `ultramode` wins over `fastmode` when
  both are on. Handy to run most of a script at full `ultramode` speed but drop a key-sensitive
  passage back to normal rollover.
- **`repeat N`** re-runs the previous keystroke, `type`, `typeln`, chord, `hold` or `release` N more
  times.

```
delay 50             # from here on, a 50 ms gap between each step (action)
keydelay 10          # and 10 ms between the individual keys within a step
type opening...
sleep 400            # wait once, e.g. for a window to appear
down
repeat 5             # press Down six times in total
```

`repeat` only follows an action. Put it right after the thing you want repeated; any other statement
in between clears it.

In a dry run (no `--hid`), every pause is shown so you can see the timing: an explicit `sleep` prints
`[sleep 400ms]` and a `delay` gap prints `[delay 50ms]`, right where they happen in the output.

## Conditions: if / elif / else

A condition must be a bool. There is no "truthiness", so compare explicitly (`if n != 0`, not
`if n`). Close every `if` with `fi`. The body may sit on the same line after `then`.

```
n = 2
if n == 0 then
  typeln zero
elif n == 1 then
  typeln one
else
  typeln many
fi
```

## Loops: while and for

`while` runs its body while a bool condition holds, between `do` and `done`:

```
i = 0
while i < 3 do
  typeln tick
  i = i + 1
done
```

`for` counts over an inclusive range `lo .. hi`. If `lo` is greater than `hi` the body never runs.
The loop variable is an ordinary variable and keeps its last value after the loop.

```
for i in 1 .. 5 do
  typeln line $i
done
```

`break` leaves a loop early and `continue` jumps to the next iteration:

```
i = 0
while true do
  i = i + 1
  if i == 3 then continue fi
  if i > 5 then break fi
  typeln $i
done
```

## Functions

Define a function with `func NAME params...` and close it with `end`. Functions live at the top
level and are visible everywhere, so you can call one defined further down. Arguments are passed by
value; `return` hands a value back.

```
func greet name
  typeln Hello $name!
end

func add a b
  return a + b
end

greet("world")
sum = add(2, 3)
typeln sum is $sum
```

A function has its own local variables, but it can still read and write top-level variables by name,
much like a shell function. Recursion works, up to a depth of 200.

```
func fib n
  if n < 2 then return n fi
  return fib(n - 1) + fib(n - 2)
end
typeln fib(10) = ${fib(10)}
```

## Modules: import

`import` pulls another script into this one, so you can keep shared helpers and constants in their
own file. The path is written plainly, with no quotes, as the rest of the line:

```
import modules/util.qk
import ./tools/math.qk
```

It works like an include: the imported file's functions become available here (functions are visible
everywhere, so order does not matter), and its top-level code runs at the point of the `import`, so
any `const` or variable it sets is in effect from there on. Keep modules to definitions and
constants; a `type` in a module would be typed at the moment it is imported.

```
# consts.qk
const SPEED = 300
func banner
  typeln ========================
end
```

```
# main.qk
import ./consts.qk          # SPEED is now set, banner() is now callable
keydelay SPEED
banner()
```

A few rules:

- **Relative paths are relative to the file that writes the `import`**, not to wherever you run
  Quacky from, so a module can `import ./sibling.qk` and move as a unit. An absolute path is used
  as-is.
- **Each file is imported once.** Importing the same file again (directly, or through a diamond where
  two modules both import a third) pulls it in a single time, which also means an import cycle just
  stops rather than looping. Sameness is by the actual file on disk, not the text of the path, so a
  module reached once by a relative path and once by an absolute one is still imported only once.
- **`import` is top level only**, and like `type` it reads the rest of the line verbatim: a trailing
  `# comment`, a `;`, or anything else after the path becomes part of the path. One `import` per
  line, and keep comments on their own line.
- Everything is checked before anything is typed: a missing or non-text module, or a syntax error in
  one, is reported by `quacky --check` and types nothing.

## Things that will trip you up

A short list of rules that surprise people:

- **A syntax error types nothing.** The whole script is parsed before the first key is pressed, so a
  mistake anywhere means no output at all. Use `quacky --check script.qk` to check without running.
- **No truthiness.** `if n` is an error; write `if n != 0`.
- **No implicit conversions** except that `+` with a string turns the other side into text.
- **`a & b == 0` means `a & (b == 0)`** because of C precedence. Parenthesise: `(a & b) == 0`.
- **The End key is `endkey`**, because `end` is a keyword.
- **`type` does not press Enter; `typeln` does.** Reach for `typeln` when you want a line and Enter.
- **`type`/`typeln` keep spaces** after the first one.
- **A `type`/`typeln` must be last on its line.** It reads to the end of the line, so `type x ; enter`
  types `x ; enter` rather than pressing Enter. Chain with `;` up to the `type`, not after it.
- **`$name` only works in a `type`/`typeln` line.** Outside typed text, name the variable with no
  `$`: `delay d`, not `delay $d`. The `$` has no text to paste into.
- **`\$` only works in a `type`/`typeln` line**, not inside a `"string"`.
