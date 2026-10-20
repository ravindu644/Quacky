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
- [Typing text](#typing-text-the-type-statement)
- [Pressing keys](#pressing-keys-chords)
- [Holding keys down](#holding-keys-down)
- [Timing: delay, sleep, repeat](#timing-delay-sleep-repeat)
- [Conditions](#conditions-if-elif-else)
- [Loops](#loops-while-and-for)
- [Functions](#functions)
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

## Typing text: the `type` statement

`type` sends literal text to the host, exactly as written, and then presses Enter (it appends a
newline). Everything after the first space is taken verbatim, so `#`, `"` and `;` are ordinary
characters here.

```
type Hello from Quacky!
type   two leading spaces are kept (the first space after 'type' is the separator)
```

End a line with `\c` to type it **without** the trailing newline, so the next `type` continues on
the same line:

```
type user\c
type name          # the host sees: username
```

### Interpolation

Inside a `type` line, `$name` is replaced by a variable's value, and `${ ... }` evaluates any
expression. Use `\$` for a literal dollar sign.

```
name  = "world"
count = 3
type Hello $name, you have ${count * 2} items.
type that costs \$5
```

### Escapes in `type`

The same escapes as strings work here (`\n`, `\t`, `\xHH`, and so on), plus `\$` for a dollar sign
and `\c` to suppress the trailing newline.

```
type col1\tcol2\tcol3
type no newline after this\c
```

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

- **`delay MS`** sets a default pause inserted after every following statement. It stays in effect
  until you change it; `delay 0` turns it off. The default is 0.
- **`sleep MS`** pauses once, right now.
- **`repeat N`** re-runs the previous keystroke, `type`, chord, `hold` or `release` N more times.

```
delay 50             # from here on, pause 50 ms after each step
type opening...
sleep 400            # wait once, e.g. for a window to appear
down
repeat 5             # press Down six times in total
```

`repeat` only follows an action. Put it right after the thing you want repeated; any other statement
in between clears it.

## Conditions: if / elif / else

A condition must be a bool. There is no "truthiness", so compare explicitly (`if n != 0`, not
`if n`). Close every `if` with `fi`. The body may sit on the same line after `then`.

```
n = 2
if n == 0 then
  type zero
elif n == 1 then
  type one
else
  type many
fi
```

## Loops: while and for

`while` runs its body while a bool condition holds, between `do` and `done`:

```
i = 0
while i < 3 do
  type tick
  i = i + 1
done
```

`for` counts over an inclusive range `lo .. hi`. If `lo` is greater than `hi` the body never runs.
The loop variable is an ordinary variable and keeps its last value after the loop.

```
for i in 1 .. 5 do
  type line $i
done
```

`break` leaves a loop early and `continue` jumps to the next iteration:

```
i = 0
while true do
  i = i + 1
  if i == 3 then continue fi
  if i > 5 then break fi
  type $i
done
```

## Functions

Define a function with `func NAME params...` and close it with `end`. Functions live at the top
level and are visible everywhere, so you can call one defined further down. Arguments are passed by
value; `return` hands a value back.

```
func greet name
  type Hello $name!
end

func add a b
  return a + b
end

greet("world")
sum = add(2, 3)
type sum is $sum
```

A function has its own local variables, but it can still read and write top-level variables by name,
much like a shell function. Recursion works, up to a depth of 200.

```
func fib n
  if n < 2 then return n fi
  return fib(n - 1) + fib(n - 2)
end
type fib(10) = ${fib(10)}
```

## Things that will trip you up

A short list of rules that surprise people:

- **A syntax error types nothing.** The whole script is parsed before the first key is pressed, so a
  mistake anywhere means no output at all. Use `quacky --check script.qk` to check without running.
- **No truthiness.** `if n` is an error; write `if n != 0`.
- **No implicit conversions** except that `+` with a string turns the other side into text.
- **`a & b == 0` means `a & (b == 0)`** because of C precedence. Parenthesise: `(a & b) == 0`.
- **The End key is `endkey`**, because `end` is a keyword.
- **`type` keeps spaces** after the first one, and always presses Enter at the end unless you use
  `\c`.
- **`\$` only works in a `type` line**, not inside a `"string"`.
