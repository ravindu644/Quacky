# DuckyScript 3.0 — Complete Language Specification

An exhaustive reference for the DuckyScript 3.0 language as published by Hak5
(documentation.hak5.org / shop.hak5.org). It is written as an implementation
blueprint for an interpreter targeting the Linux kernel's **USB HID gadget**
framework (`/dev/hidgN` + a `configfs` report descriptor).

The language has two layers:

1. **Keystroke injection** — the original DuckyScript surface (`STRING`, keys,
   modifiers, `DELAY`). Every 1.0/2.0 command is still valid in 3.0.
2. **Structured programming** — variables, constants, operators, conditionals,
   loops, functions, extensions, and a large set of built-in `$_` state
   variables.

Hardware features such as a status LED, a physical button, dual-mode USB
(HID + mass storage), and SD-card exfiltration are part of the official
language. On a generic Linux HID gadget most of those have no hardware to drive;
the per-command notes flag what you emulate, stub, or ignore.

> **Convention in this document.** Each entry lists **Command/Keyword**,
> **Parameters**, **Description**, **Code Example**, and **Interpreter
> Implementation Notes**. Examples are deliberately benign (typing text, blinking
> an LED, counting) and exist only to show syntax.

---

## Table of Contents

1. [Language Basics](#1-language-basics)
2. [Timing & State Management](#2-timing--state-management)
3. [Basic HID Keystrokes](#3-basic-hid-keystrokes)
4. [Variables & Memory](#4-variables--memory)
5. [Operators](#5-operators)
6. [Control Flow](#6-control-flow)
7. [Functions](#7-functions)
8. [Advanced / Hardware-Specific Features](#8-advanced--hardware-specific-features)
9. [Built-in Internal Variables (`$_`)](#9-built-in-internal-variables-_)
10. [Appendix A — HID Usage Mapping](#appendix-a--hid-usage-mapping)
11. [Appendix B — Suggested Lexer / Parser Architecture](#appendix-b--suggested-lexer--parser-architecture)

---

## 1. Language Basics

### 1.1 Overall structure

A payload is a UTF-8 (practically ASCII) text file, one statement per line.
There is no statement terminator and no expression statement outside the few
constructs that allow it (assignment, a bare function call). The file is read
**top to bottom, once**, except where control flow (`WHILE`, function calls,
`RESTART_PAYLOAD`) redirects it.

**Interpreter Implementation Notes**

- Lex **line by line**. The newline is a significant token, not whitespace. A
  great deal of the grammar is "a keyword, then the rest of the line is its
  argument."
- The grammar is **not** free-form like C. Block openers (`IF … THEN`,
  `WHILE …`, `FUNCTION NAME()`, `BUTTON_DEF`, `EXTENSION Name`, the `*_BLOCK`
  string forms) each have a dedicated `END_*` / `END_STRING…` terminator on its
  own line. Build your AST around matched opener/closer line pairs, not braces.
- Keep a two-pass model: **pass 1** registers `DEFINE` constants, `FUNCTION`
  bodies, `EXTENSION` bodies, and `ATTACKMODE` (which must be first); **pass 2**
  executes. Functions and the button handler must be known before the lines
  that call them run.

### 1.2 Line endings

| Command / Keyword | *(line structure)* |
|---|---|
| **Parameters** | None |
| **Description** | Each command sits on its own line. Lines are separated by a line feed. The reference payloads ship with Unix (`LF`) endings; Windows (`CRLF`) files also occur in the wild. |
| **Code Example** | *(see any example below — one command per line)* |

**Interpreter Implementation Notes**

- Split on `\n` and **strip a trailing `\r`** so `CRLF` files parse identically.
  A stray `\r` left on the end of `ENTER` or `TAB` turns a known keyword into an
  unknown token.
- A blank line is a no-op. Do not emit a keystroke for it.

### 1.3 Whitespace

| Command / Keyword | *(indentation / spacing)* |
|---|---|
| **Parameters** | None |
| **Description** | Leading indentation on a line is **not significant** to the compiler; it is purely cosmetic and conventionally used to show block nesting. Within a command, the **first** run of spaces separates the keyword from its argument. |
| **Code Example** | <pre>WHILE ( $FOO > 0 )<br>    LED_G<br>    DELAY 500<br>END_WHILE</pre> |

**Interpreter Implementation Notes**

- Trim leading/trailing whitespace **only** when tokenising command keywords and
  numeric/expression arguments.
- **Do not trim the body of `STRING`.** After the single space that follows the
  `STRING` keyword, every remaining character including interior spaces is
  literal text. See [3.1](#31-string).
- Hak5's documented quirk: `STRING` **drops trailing spaces** on the line. If a
  payload needs a trailing space it must use a `STRING` block or append a
  `SPACE`. Replicate the trailing-space trim to stay byte-compatible.

### 1.4 Comments

#### `REM`

| | |
|---|---|
| **Command / Keyword** | `REM <text>` |
| **Parameters** | Free text to end of line. |
| **Description** | A single-line comment. Performs no keystroke injection and no action of any kind. |
| **Code Example** | `REM This payload opens a text editor and types a greeting` |

#### `REM_BLOCK` / `END_REM`

| | |
|---|---|
| **Command / Keyword** | `REM_BLOCK` … `END_REM` |
| **Parameters** | None; every line in between is ignored. |
| **Description** | A multi-line comment block. All lines between the two markers are discarded by the compiler. |
| **Code Example** | <pre>REM_BLOCK<br>    Author: example<br>    Target: cross-platform demo<br>END_REM</pre> |

**Interpreter Implementation Notes**

- `REM` consumes the **whole remaining line**, so a `REM` may legally contain
  text that looks like other commands. Strip it in the lexer before keyword
  dispatch.
- `REM_BLOCK` sets a "skip" state in the line reader until a line that is exactly
  `END_REM` (after trimming). Nesting is not documented; treat the first
  `END_REM` as the terminator.
- Comments must not participate in the `ATTACKMODE`-must-be-first rule: a leading
  `REM`/`REM_BLOCK` is allowed above `ATTACKMODE`.

### 1.5 Case sensitivity

- **Commands and keywords are UPPERCASE** (`STRING`, `DELAY`, `END_IF`). Treat the
  keyword table as case-sensitive uppercase to match the official compiler; do
  not silently accept `string`.
- **Boolean literals** `TRUE` / `FALSE` are uppercase.
- Variable names (`$NAME`) and constant names (`#NAME`) are user-defined; match
  them exactly as written.
- `STRING` argument text is of course free-form and preserves its own case.

---

## 2. Timing & State Management

### 2.1 `DELAY`

| | |
|---|---|
| **Command / Keyword** | `DELAY <ms>` |
| **Parameters** | An unsigned integer **or** a variable, in **milliseconds**. Documented minimum **20 ms**. |
| **Description** | Pauses payload execution for the given time. Used to let the target process previous keystrokes (e.g. wait for the Run dialog to open before typing). |
| **Code Example** | <pre>GUI r<br>DELAY 500<br>STRING notepad<br>ENTER</pre> |

**Interpreter Implementation Notes**

- **Blocking delay.** Execution halts; no keystrokes are emitted during the wait.
  Implement with `nanosleep`/`clock_nanosleep` (monotonic). Do **not** busy-wait.
- `DELAY $VAR` is legal — resolve the operand from the symbol table at run time,
  not parse time.
- Enforce or at least respect the 20 ms floor. Real hardware needs settle time
  between HID reports; on a gadget you still want a small inter-report gap so the
  host's USB polling (typically 1–8 ms) actually samples each report.
- Accuracy is "best effort." The spec itself notes timing varies with host and
  attack mode; do not promise sub-millisecond precision.

### 2.2 `DEFAULT_DELAY` / `DEFAULTDELAY`

| | |
|---|---|
| **Command / Keyword** | `DEFAULT_DELAY <ms>` (alias `DEFAULTDELAY`) |
| **Parameters** | Unsigned integer, milliseconds. |
| **Description** | Sets a delay inserted **automatically after every subsequent command**. Carried over from DuckyScript 1.0; useful for pacing a whole payload without a `DELAY` on each line. |
| **Code Example** | <pre>DEFAULT_DELAY 100<br>STRING line one<br>ENTER<br>STRING line two<br>ENTER</pre> |

**Interpreter Implementation Notes**

- Store as interpreter state; after executing each command, sleep this many ms.
  Default is 0 (disabled).
- Both spellings must map to the same handler.
- Order of operations: the per-command `DELAY` plus the `DEFAULT_DELAY` both
  apply. Keep them additive and predictable.

### 2.3 `DEFAULT_CHORD`

| | |
|---|---|
| **Command / Keyword** | `DEFAULT_CHORD <ms>` |
| **Parameters** | Unsigned integer, milliseconds. |
| **Description** | Sets how long a key "chord" (a key plus its modifiers) is held down before the release report is sent. Raising it helps targets that drop fast keypresses. |
| **Code Example** | `DEFAULT_CHORD 50` |

**Interpreter Implementation Notes**

- This is the dwell time between the **key-down report** and the **key-up
  (empty) report** for each injected keystroke/combo. Insert it inside your
  "press then release" primitive, not between separate commands.
- Default is small/zero; many flaky targets improve at 5–50 ms.

### 2.4 Jitter

| | |
|---|---|
| **Command / Keyword** | `$_JITTER_ENABLED = TRUE` / `FALSE`, and `$_JITTER_MAX = <ms>` |
| **Parameters** | `$_JITTER_ENABLED` boolean; `$_JITTER_MAX` unsigned integer 0–65535 ms (default 20). |
| **Description** | When enabled, inserts a **random** delay between keystrokes, up to `$_JITTER_MAX`. Intended to make injection timing look less machine-perfect. |
| **Code Example** | <pre>$_JITTER_ENABLED = TRUE<br>$_JITTER_MAX = 40<br>STRINGLN the quick brown fox</pre> |

**Interpreter Implementation Notes**

- These are **internal variables**, set through normal assignment, not bespoke
  commands. Your assignment handler must recognise the reserved `$_` names and
  route them to engine state (see [Section 9](#9-built-in-internal-variables-_)).
- Per keystroke, if jitter is on, sleep `rand() % ($_JITTER_MAX + 1)` ms **in
  addition** to `DEFAULT_DELAY`/`DELAY`. Seed the PRNG once (see
  [8.5 Randomization](#85-randomization) for the `seed.bin` model).

### 2.5 State save/restore commands (timing-adjacent)

| Command | Description | Interpreter note |
|---|---|---|
| `SAVE_HOST_KEYBOARD_LOCK_STATE` | Snapshot host Caps/Num/Scroll LED state. | Store current `$_CAPSLOCK_ON` etc. into `$_SAVED_*`. Requires the host→device LED output report (see [Appendix A](#appendix-a--hid-usage-mapping)). |
| `RESTORE_HOST_KEYBOARD_LOCK_STATE` | Toggle locks back to the saved state. | Compare live vs saved, press `CAPSLOCK`/`NUMLOCK`/`SCROLLLOCK` as needed. |
| `RESET` | Clears the keystroke buffer / releases all keys. | Emit one empty HID report (all zeroes). A good cleanup primitive. |

---

## 3. Basic HID Keystrokes

This is the core of what a HID-gadget interpreter emits on the wire. Every entry
here ultimately becomes one or more **8-byte boot-protocol keyboard reports**
(`[modifier, reserved, k1..k6]`) followed by a release report, unless noted.

### 3.1 `STRING`

| | |
|---|---|
| **Command / Keyword** | `STRING <text>` |
| **Parameters** | The remainder of the line, as literal text. |
| **Description** | Types the given text as a sequence of keystrokes. Letters, digits, punctuation, and spaces are all supported. Uppercase letters and shifted symbols are produced by automatically asserting `SHIFT`. Does **not** press Enter at the end. |
| **Code Example** | `STRING Hello, World! 100% done.` |

**Interpreter Implementation Notes**

- **The single most important command.** Implement a full **character → (HID
  usage, shift?)** table for the active keyboard layout. The reference firmware
  injects US layout by default; non-US layouts are handled by language files.
  For a Linux gadget, build at minimum a US-QWERTY map and leave a hook for
  alternate maps.
- For each character: set the modifier byte (`SHIFT` bit for uppercase/shifted),
  put the usage in `k1`, send the report, then send an **empty report to release**.
  Without the release report the host sees the key as held and auto-repeats.
- Characters with no single-key mapping (e.g. accented letters) need either a
  layout that has them or a Unicode-entry fallback; the base spec assumes they
  are reachable on the active layout.
- Apply the **trailing-space trim** quirk ([1.3](#13-whitespace)).
- `STRING` with no argument types nothing (but is valid).

### 3.2 `STRINGLN`

| | |
|---|---|
| **Command / Keyword** | `STRINGLN <text>` |
| **Parameters** | Remainder of line as literal text. |
| **Description** | Same as `STRING`, then presses **ENTER**. Convenient for line-oriented output (shells, editors). |
| **Code Example** | `STRINGLN echo payload reached step one` |

**Interpreter Implementation Notes**

- Implement as `STRING` + one `ENTER` keystroke. Reuse the same code path.

### 3.3 `STRING` / `END_STRING` block

| | |
|---|---|
| **Command / Keyword** | `STRING` … `END_STRING` |
| **Parameters** | The lines between the markers. |
| **Description** | A multi-line form that **joins** the enclosed lines into a single typed string: leading whitespace is stripped and the newlines between lines are **ignored** (not typed). Lets you lay out a long single-line string across several source lines. |
| **Code Example** | <pre>STRING<br>    one two<br>    three four<br>END_STRING<br>REM types: "one twothree four"</pre> |

**Interpreter Implementation Notes**

- Collect lines until `END_STRING`, left-trim each, concatenate with **no**
  separator, then type the result as one `STRING`. No trailing Enter.

### 3.4 `STRINGLN` / `END_STRINGLN` block

| | |
|---|---|
| **Command / Keyword** | `STRINGLN` … `END_STRINGLN` |
| **Parameters** | The lines between the markers. |
| **Description** | Types each enclosed line **as written**, each followed by **ENTER**. Only the first leading tab of each line is stripped; other whitespace is preserved. Good for emitting a multi-line here-doc or file. |
| **Code Example** | <pre>STRINGLN<br>line one<br>line two<br>END_STRINGLN</pre> |

**Interpreter Implementation Notes**

- For each enclosed line: strip **one** leading tab only, type the line, press
  `ENTER`. Preserve interior/trailing spaces unlike single-line `STRING`.
- Distinguish the two block forms in the parser by the opener keyword; both close
  with their matching `END_*`.

### 3.5 Modifier keys

| Keyword(s) | Meaning |
|---|---|
| `GUI`, `WINDOWS` | Left GUI / Windows / Super key (Command on macOS targets) |
| `COMMAND` | macOS Command (same HID usage as GUI) |
| `CONTROL`, `CTRL` | Control |
| `ALT` | Alt / Option |
| `SHIFT` | Shift |
| `OPTION` | macOS Option (alias of ALT) |

| | |
|---|---|
| **Parameters** | Optionally **one** following key or character on the same line. |
| **Description** | A modifier used **alone** taps that modifier. Followed by a key, it forms a combination (chord): the modifier(s) are held while the final key is pressed, then all release together. Multiple modifiers can be stacked. |
| **Code Example** | <pre>GUI r<br>CTRL ALT DELETE<br>CTRL SHIFT ESCAPE<br>ALT F4<br>GUI SPACE</pre> |

**Interpreter Implementation Notes**

- Parse a keystroke line as `MOD* [KEY]`: zero or more modifier tokens, then at
  most one terminal key or literal character.
- Map each modifier to its **modifier-byte bit**: `CTRL`=0x01, `SHIFT`=0x02,
  `ALT`=0x04, `GUI`=0x08 (left-side bits). `GUI`/`WINDOWS`/`COMMAND` collapse to
  the same bit; `ALT`/`OPTION` likewise.
- A bare modifier (e.g. `GUI`) presses and releases that modifier with an empty
  key slot. A combo sets the modifier bits **and** the key usage in one report,
  holds `DEFAULT_CHORD` ms, then releases with an empty report.
- A single literal character after a modifier (`GUI r`) goes through the same
  char→usage lookup as `STRING`, but any `SHIFT` the character itself needs must
  be merged with the explicit modifiers.

### 3.6 `INJECT_MOD`

| | |
|---|---|
| **Command / Keyword** | `INJECT_MOD <modifier>` |
| **Parameters** | A single modifier keyword. |
| **Description** | Presses a modifier **by itself** as an explicit injection, e.g. tapping the Windows key alone. Primarily used so a modifier can be injected where bare-modifier parsing would otherwise be ambiguous, and before `HOLD` of a lone modifier. |
| **Code Example** | `INJECT_MOD WINDOWS` |

**Interpreter Implementation Notes**

- Equivalent to setting just the modifier bit, sending the report, then
  releasing. Keep it as a distinct opcode so `HOLD`/`RELEASE` of a lone modifier
  compose correctly ([8.4](#84-holding-keys)).

### 3.7 Named keys

All of the following are valid standalone key commands and valid as the terminal
key of a modifier combo.

**Navigation / editing**

```
UP        DOWN        LEFT        RIGHT
UPARROW   DOWNARROW   LEFTARROW   RIGHTARROW
PAGEUP    PAGEDOWN    HOME        END
INSERT    DELETE (DEL)            BACKSPACE
TAB       SPACE
```

**System / control**

```
ENTER        ESCAPE (ESC)     PAUSE (BREAK)    PRINTSCREEN
MENU (APP)   CAPSLOCK          NUMLOCK          SCROLLLOCK
```

**Function keys**

```
F1 F2 F3 F4 F5 F6 F7 F8 F9 F10 F11 F12
```

| | |
|---|---|
| **Parameters** | None (they *are* the key). May be preceded by modifiers. |
| **Description** | Each injects the corresponding non-printable or control key as a single HID usage. `UP`/`UPARROW` and friends are aliases; `DEL`/`DELETE`, `ESC`/`ESCAPE`, `BREAK`/`PAUSE`, `APP`/`MENU` are aliases. |
| **Code Example** | <pre>STRING cd ~<br>ENTER<br>DOWNARROW<br>DOWNARROW<br>TAB<br>F5</pre> |

**Interpreter Implementation Notes**

- Keep a **keyword → HID usage** table (see [Appendix A](#appendix-a--hid-usage-mapping))
  with all aliases pointing at one usage. Resolve aliases at lex time so the
  executor only sees canonical usages.
- These are press+release like any key, subject to `DEFAULT_CHORD`.
- Watch the documented quirk where one source listed `F0` for `F10`; always map
  `F10` to usage `0x43`. Reject `F0` or map it defensively.
- `NUMLOCK`/`CAPSLOCK`/`SCROLLLOCK` as *keys* toggle the host lock; the resulting
  LED state comes back via the host's output report (see [Appendix A](#appendix-a--hid-usage-mapping)
  and the `WAIT_FOR_*` / `$_*LOCK_ON` features).

### 3.8 Lock keys as commands

| Command / Keyword | Description |
|---|---|
| `CAPSLOCK` | Toggle Caps Lock on the host. |
| `NUMLOCK` | Toggle Num Lock. |
| `SCROLLLOCK` (also seen as `SCROLLOCK`) | Toggle Scroll Lock. |

**Interpreter Implementation Notes**

- Accept the misspelling `SCROLLOCK` as an alias of `SCROLLLOCK`; it appears in
  the reference material.
- To *know* the resulting state (for `$_CAPSLOCK_ON` etc.) you must read the HID
  **output report** the host sends to set keyboard LEDs. On a Linux gadget this
  arrives as a readable event on the HID function; parse bit 0 = Num, bit 1 =
  Caps, bit 2 = Scroll.

---

## 4. Variables & Memory

### 4.1 `VAR` — variable declaration

| | |
|---|---|
| **Command / Keyword** | `VAR $NAME = <value>` |
| **Parameters** | A name beginning with `$`; an initial value that is an integer, a boolean (`TRUE`/`FALSE`), or an expression. |
| **Description** | Declares a variable and assigns its initial value. Values are **unsigned 16-bit integers (0–65535)**. Booleans are stored as integers: `FALSE`=0, `TRUE`=1, and **any non-zero value is truthy**. |
| **Code Example** | <pre>VAR $COUNTER = 0<br>VAR $LIMIT = 10<br>VAR $DONE = FALSE</pre> |

**Interpreter Implementation Notes**

- Maintain a **single global symbol table** mapping `$NAME → uint16`. There is
  **no block or function scope** (see [4.3](#43-scope)).
- Store everything as `uint16_t`. Arithmetic wraps mod 65536 — match that so
  overflow behaviour is identical to the reference.
- `TRUE`/`FALSE` are literals that lex to 1/0. Truthiness in conditions is
  "non-zero," not "== 1."
- `VAR` vs plain assignment: `VAR` *declares* (first introduction); later lines
  reassign with `$NAME = …` **without** `VAR`. You may treat a re-`VAR` as a
  reassignment, but flag double declaration if you want stricter diagnostics.

### 4.2 Assignment (reassignment)

| | |
|---|---|
| **Command / Keyword** | `$NAME = <expression>` |
| **Parameters** | Left side: an existing variable (or a reserved `$_` internal variable). Right side: an expression. |
| **Description** | Reassigns a variable. There is **no `+=` / `-=`**; augmented assignment is written by referencing the variable on both sides. |
| **Code Example** | <pre>$COUNTER = ( $COUNTER + 1 )<br>$DONE = ( $COUNTER >= $LIMIT )</pre> |

**Interpreter Implementation Notes**

- The assignment statement is one of the few lines that is **not** a keyword
  command. Detect it by a leading `$` token followed by `=`.
- Evaluate the right-hand expression ([Section 5](#5-operators)), then store.
- Assigning to a reserved `$_` name must update **engine state**, not just a
  table slot (jitter, random bounds, LED toggles, timeouts, exfil mode, etc.).

### 4.3 Scope

| | |
|---|---|
| **Description** | **All variables are global to the payload.** Any function can read or write any variable; functions do **not** take arguments inside their `()`. A variable set before a function call is visible inside it, and changes made inside persist after it returns. |
| **Code Example** | <pre>VAR $TIMER = 3<br>FUNCTION COUNTDOWN()<br>    WHILE ( $TIMER > 0 )<br>        STRING .<br>        $TIMER = ( $TIMER - 1 )<br>    END_WHILE<br>END_FUNCTION<br>COUNTDOWN()<br>REM $TIMER is now 0</pre> |

**Interpreter Implementation Notes**

- Do **not** implement call frames or local scopes. One flat environment.
  Function calls are effectively `GOSUB`: push return address, jump, run body,
  pop. No parameter binding.
- `RETURN <value>` yields a value for expression context but creates no local
  bindings ([Section 7](#7-functions)).

### 4.4 `DEFINE` — constants

| | |
|---|---|
| **Command / Keyword** | `DEFINE #NAME <value>` |
| **Parameters** | A name beginning with `#`; a replacement value (number or text token). |
| **Description** | A **compile-time textual replacement**, like a C `#define`. Every later occurrence of `#NAME` is substituted with the value before execution. Constants are immutable and carry no storage at run time. |
| **Code Example** | <pre>DEFINE #DELAY_SHORT 200<br>DEFINE #APP notepad<br>DELAY #DELAY_SHORT<br>STRING #APP</pre> |

**Interpreter Implementation Notes**

- Process `DEFINE` in **pass 1** and perform **lexical substitution** on all
  subsequent lines before parsing them. It is text replacement, not a typed
  value: `#APP` above expands to the token `notepad`.
- Constants (`#`) and variables (`$`) occupy different namespaces and behave
  differently: `#` is resolved at compile time and cannot change; `$` lives in
  the run-time table and can.
- Guard against recursive/forward `DEFINE` references; the reference compiler
  expects a constant to be defined before use.

### 4.5 Value model summary

| Aspect | Rule | Interpreter note |
|---|---|---|
| Numeric type | Unsigned 16-bit integer, 0–65535 | Use `uint16_t`; wrap on overflow. |
| Booleans | `FALSE`=0, `TRUE`=1, non-zero = true | Store in the same `uint16_t`. |
| Strings | Only as `STRING` arguments; **not** a first-class variable type | You cannot assign text to `$VAR`. Text lives only on `STRING`/`STRINGLN` lines. |
| Negative numbers | Not a native type; subtraction wraps | `( 0 - 1 )` = 65535. |

---

## 5. Operators

Operators appear inside expressions, which are used in assignments, `IF`/`ELSE
IF` conditions, `WHILE` conditions, and function `RETURN` values. Parentheses set
precedence and are **conventionally required** around whole conditions and
around arithmetic right-hand sides.

### 5.1 Arithmetic

| Operator | Meaning | Example |
|---|---|---|
| `=` | Assignment | `$X = 5` |
| `+` | Addition | `$X = ( $X + 1 )` |
| `-` | Subtraction | `$X = ( $X - 1 )` |
| `*` | Multiplication | `$AREA = ( $W * $H )` |
| `/` | Integer division | `$HALF = ( $X / 2 )` |
| `%` | Modulo (remainder) | `$ODD = ( $X % 2 )` |
| `^` | Exponent (power) | `$SQ = ( $X ^ 2 )` |

### 5.2 Comparison

| Operator | Meaning |
|---|---|
| `==` | Equal |
| `!=` | Not equal |
| `>` | Greater than |
| `<` | Less than |
| `>=` | Greater than or equal |
| `<=` | Less than or equal |

### 5.3 Logical

| Operator | Meaning |
|---|---|
| `&&` | Logical AND |
| `\|\|` | Logical OR |

### 5.4 Bitwise

| Operator | Meaning |
|---|---|
| `&` | Bitwise AND |
| `\|` | Bitwise OR |
| `>>` | Right shift |
| `<<` | Left shift |

### 5.5 Grouping

| | |
|---|---|
| **Command / Keyword** | `( … )` |
| **Description** | Parentheses define precedence and group sub-expressions. Hak5's examples wrap conditions in `IF ( … ) THEN` and wrap arithmetic right-hand sides in `$X = ( $X + 1 )`. |
| **Code Example** | `$R = ( ( $A + $B ) * ( $C - 1 ) )` |

**Interpreter Implementation Notes**

- Expressions are the one place you need a **real sub-parser**, not line
  keyword dispatch. A shunting-yard or Pratt parser over the operator set above
  is ideal; emit an expression AST (or RPN) and evaluate in `uint16_t`.
- **Suggested precedence**, high → low: `^` ; `*` `/` `%` ; `+` `-` ;
  `<<` `>>` ; `<` `<=` `>` `>=` ; `==` `!=` ; `&` ; `|` ; `&&` ; `||`.
  Because the language style wraps almost everything in explicit `()`, exact
  precedence rarely bites, but define it so unparenthesised input is
  deterministic. Validate against the reference where you can.
- **Augmented assignment does not exist.** Do not add `+=`; the idiom is
  `$X = ( $X + 1 )`. If you choose to accept `+=` as an extension, document it as
  non-standard.
- Division by zero and `0 - n` wrap: decide and document (reference uses wrapping
  unsigned math). Guard `/` and `%` against a zero divisor so you don't crash.
- Logical vs bitwise: `&&`/`||` operate on truthiness (non-zero), `&`/`|` on the
  16-bit value. Keep them distinct tokens; a lexer that greedily reads `&` then
  `&` must prefer the two-char `&&`.
- Operands may be: a literal, `TRUE`/`FALSE`, a `$VAR`, a `$_` internal variable,
  or a **function call** (`NAME()`) used for its `RETURN` value ([Section 7](#7-functions)).

---

## 6. Control Flow

### 6.1 `IF` / `THEN` / `END_IF`

| | |
|---|---|
| **Command / Keyword** | `IF ( <condition> ) THEN` … `END_IF` |
| **Parameters** | A boolean condition expression (conventionally parenthesised). |
| **Description** | Runs the enclosed block **only if** the condition is true. The statement's parts are: the `IF` keyword, the condition, the `THEN` keyword, one or more lines of block code, and the `END_IF` keyword. After the block (whether run or skipped) execution continues past `END_IF`. |
| **Code Example** | <pre>IF ( $_CAPSLOCK_ON == TRUE ) THEN<br>    STRING Caps Lock is on<br>END_IF</pre> |

### 6.2 `ELSE`

| | |
|---|---|
| **Command / Keyword** | `ELSE` |
| **Parameters** | None. |
| **Description** | Optional branch inside an `IF` block; its code runs when the `IF` condition was false. The whole construct still closes with a single `END_IF`. |
| **Code Example** | <pre>IF ( $_CAPSLOCK_ON == TRUE ) THEN<br>    STRING Caps Lock is on<br>ELSE<br>    STRING Caps Lock is off<br>END_IF</pre> |

### 6.3 `ELSE IF`

| | |
|---|---|
| **Command / Keyword** | `ELSE IF ( <condition> ) THEN` |
| **Parameters** | A boolean condition; `THEN` follows it. |
| **Description** | Optional chained branch: a new condition tested when the preceding `IF`/`ELSE IF` were false. Any number can chain. The entire chain closes with **one** `END_IF`. |
| **Code Example** | <pre>IF ( $_OS == WINDOWS ) THEN<br>    STRING Hello Windows!<br>ELSE IF ( $_OS == MACOS ) THEN<br>    STRING Hello Mac!<br>ELSE IF ( $_OS == LINUX ) THEN<br>    STRING Hello Linux!<br>ELSE<br>    STRING Hello World!<br>END_IF</pre> |

**Interpreter Implementation Notes (6.1–6.3)**

- Parse an `IF` as a node holding an ordered list of `(condition, block)` arms
  plus an optional trailing `ELSE` block, terminated by `END_IF`. Note the whole
  chain uses **one** `END_IF`, unlike C's nested braces.
- `THEN` is a required keyword, not sugar. Enforce `IF <cond> THEN` and
  `ELSE IF <cond> THEN`; a bare `ELSE` takes **no** `THEN`.
- Evaluate arms top-down, run the first true arm's block, skip the rest, resume
  after `END_IF`.
- Blocks nest. Track an opener stack so nested `IF`/`WHILE`/`FUNCTION` match their
  own `END_*`.
- The condition is a full expression — reuse the [Section 5](#5-operators) evaluator;
  truthiness is non-zero.

### 6.4 `WHILE` / `END_WHILE`

| | |
|---|---|
| **Command / Keyword** | `WHILE ( <condition> )` … `END_WHILE` |
| **Parameters** | A boolean condition (parenthesised), **or** the bare literal `TRUE`. |
| **Description** | Repeats the enclosed block as long as the condition is true. On reaching `END_WHILE`, execution jumps **back** to re-test the condition (unlike `IF`, which falls through). When the condition becomes false, execution continues past `END_WHILE`. |
| **Code Example** | <pre>REM blink the LED 42 times<br>VAR $FOO = 42<br>WHILE ( $FOO > 0 )<br>    LED_G<br>    DELAY 500<br>    LED_OFF<br>    DELAY 500<br>    $FOO = ( $FOO - 1 )<br>END_WHILE<br>LED_R</pre> |

### 6.5 Infinite loop — `WHILE TRUE`

| | |
|---|---|
| **Command / Keyword** | `WHILE TRUE` … `END_WHILE` |
| **Parameters** | The bare literal `TRUE` (parentheses may be **omitted**). |
| **Description** | Loops forever. Omitting parentheses is slightly more efficient because no expression is evaluated each pass. Ends only when interrupted (button press handing control to `BUTTON_DEF`, device unplugged, or `STOP_PAYLOAD`). |
| **Code Example** | <pre>WHILE TRUE<br>    LED_G<br>    DELAY 500<br>    LED_OFF<br>    DELAY 500<br>END_WHILE</pre> |

**Interpreter Implementation Notes (6.4–6.5)**

- Represent a `WHILE` as `(condition, block)`; after the block, loop to the
  condition check. Model this as a jump target, not host-language recursion, so
  deep/long loops don't blow a stack.
- Special-case `WHILE TRUE` (and `WHILE ( TRUE )`) to skip expression evaluation.
- There is **no documented `BREAK` or `CONTINUE`.** Loop exit is by the condition
  going false, by the button handler taking over, by `STOP_PAYLOAD`, or by
  `RESTART_PAYLOAD`. Do not invent `BREAK` unless you mark it non-standard.
- An infinite loop must remain **interruptible**: service the button/`$_BUTTON_*`
  and host-LED input between iterations, and make `STOP_PAYLOAD` able to unwind.
  On a Linux gadget, poll your control inputs (button GPIO or emulated trigger)
  inside the loop body dispatch, not only at `DELAY` boundaries.

---

## 7. Functions

### 7.1 `FUNCTION` / `END_FUNCTION`

| | |
|---|---|
| **Command / Keyword** | `FUNCTION NAME()` … `END_FUNCTION` |
| **Parameters** | A name immediately followed by empty `()`. **No parameters** go inside the parentheses. |
| **Description** | Defines a reusable block of DuckyScript. Defining a function does **not** run it. The body may contain any commands, including `RETURN`. |
| **Code Example** | <pre>FUNCTION COUNTDOWN()<br>    WHILE ( $TIMER > 0 )<br>        STRING .<br>        $TIMER = ( $TIMER - 1 )<br>        DELAY 500<br>    END_WHILE<br>END_FUNCTION</pre> |

### 7.2 Calling a function

| | |
|---|---|
| **Command / Keyword** | `NAME()` |
| **Parameters** | None inside the parentheses (globals carry state instead). |
| **Description** | Executes the function body. Appears either as a standalone statement line or inside an expression (when the function returns a value). |
| **Code Example** | <pre>VAR $TIMER = 3<br>COUNTDOWN()<br>SPACE<br>$TIMER = 5<br>COUNTDOWN()</pre> |

### 7.3 `RETURN`

| | |
|---|---|
| **Command / Keyword** | `RETURN <value>` |
| **Parameters** | An integer or boolean (or an expression yielding one). Optional. |
| **Description** | Returns a value from a function so the **call can be used as an expression** (e.g. inside an `IF`). Also ends the function early. A function without `RETURN` is called only for its side effects. |
| **Code Example** | <pre>FUNCTION TEST_BUTTON()<br>    STRING Press the button within 5 seconds.<br>    VAR $TIMER = 5<br>    WHILE ( $TIMER > 0 )<br>        STRING .<br>        DELAY 1000<br>        $TIMER = ( $TIMER - 1 )<br>    END_WHILE<br>    ENTER<br>    IF ( $_BUTTON_PUSH_RECEIVED == TRUE ) THEN<br>        RETURN TRUE<br>    ELSE IF ( $_BUTTON_PUSH_RECEIVED == FALSE ) THEN<br>        RETURN FALSE<br>    END_IF<br>END_FUNCTION<br><br>IF ( TEST_BUTTON() == TRUE ) THEN<br>    STRINGLN The button was pressed!<br>ELSE<br>    STRINGLN The button was not pressed!<br>END_IF</pre> |

**Interpreter Implementation Notes (7.1–7.3)**

- Register every `FUNCTION NAME()` body in **pass 1** into a map
  `name → block`. Calls may appear before or after the definition lexically; the
  reference examples define before calling, but resolving in a pre-pass is safer.
- A call is implemented as **GOSUB**: push the current execution position onto a
  return stack, jump to the body, run to `END_FUNCTION` or a `RETURN`, then pop.
  Because scope is global ([4.3](#43-scope)), there is nothing else to save/restore
  except the instruction pointer and the pending return value.
- **No parameters, no locals.** Communication is entirely through global
  variables. Do not bind anything from the `()`.
- `RETURN value` sets a "last return value" and unwinds the current function. In
  **expression context** (`IF ( NAME() == TRUE )`), evaluate the call first, take
  its return value as the operand. In **statement context** (`NAME()` on its own
  line), discard the return value.
- Guard against unbounded recursion — the language allows a function to call
  others, and nothing stops indirect recursion. Cap your return-stack depth and
  error cleanly rather than overflowing the host stack.
- `RETURN` with no value is legal as an early exit; treat the return value as
  undefined/0 in that case and only meaningful where the script actually reads it.

---

## 8. Advanced / Hardware-Specific Features

These depend on Rubber Ducky hardware (status LED, physical button, dual USB
personality, SD card). On a generic Linux HID gadget you will **emulate, stub, or
ignore** most of them. Each note says which.

### 8.1 `ATTACKMODE`

| | |
|---|---|
| **Command / Keyword** | `ATTACKMODE <mode> [identifiers…]` |
| **Parameters** | One or more of `HID`, `STORAGE`, `OFF` (combine `HID STORAGE`). Optional USB identifiers: `VID_xxxx`, `PID_xxxx` (16-bit hex, used as a pair); `MAN_…`, `PROD_…` (up to 32 alphanumeric chars); `SERIAL_…` (12 digits). `MAN`/`PROD`/`SERIAL` are used as a set. Random variants exist: `VID_RANDOM`, `PID_RANDOM`, `MAN_RANDOM`, `PROD_RANDOM`, `SERIAL_RANDOM`. |
| **Description** | Selects the USB personality the device presents: keyboard (`HID`), mass storage (`STORAGE`), both, or no device (`OFF`). Optional identifiers spoof the device descriptor. **Must be the first command** (after any `REM`) to take effect; otherwise the device defaults to `HID`. |
| **Code Example** | `ATTACKMODE HID VID_046D PID_C31C MAN_HAK5 PROD_DUCKY SERIAL_1337` |

**Interpreter Implementation Notes**

- On a Linux gadget this maps directly to **configfs/USB gadget composition**:
  `HID` = add an HID keyboard function (`usb_f_hid`), `STORAGE` = add a
  mass-storage function (`usb_f_mass_storage`), `HID STORAGE` = a composite with
  both, `OFF` = unbind the UDC (no device). The VID/PID/strings map to
  `idVendor`, `idProduct`, `iManufacturer`, `iProduct`, `iSerialNumber`.
- Honour "**must be first**": parse it in pass 1 and compose the gadget **before**
  any keystroke, because the host must enumerate the keyboard before reports mean
  anything. If absent, default to `HID`.
- Reconfiguring USB composition mid-run forces a **re-enumeration** (you must
  unbind and rebind the UDC). That is disruptive; batch identifier changes with
  the initial mode where possible.
- If you don't implement mass storage, treat `STORAGE` as a no-op/stub but keep
  parsing it so payloads load; log that storage is unsupported.

### 8.2 `SAVE_ATTACKMODE` / `RESTORE_ATTACKMODE`

| Command | Description | Interpreter note |
|---|---|---|
| `SAVE_ATTACKMODE` | Save the current attack mode and identifiers. | Snapshot your gadget composition descriptor to memory. |
| `RESTORE_ATTACKMODE` | Restore the saved attack mode. | Re-compose the gadget from the snapshot (re-enumerates). |

### 8.3 Button

| Command / Keyword | Parameters | Description |
|---|---|---|
| `WAIT_FOR_BUTTON_PRESS` | none | Blocks until the physical button is pressed, then continues. |
| `BUTTON_DEF` … `END_BUTTON` | block | Defines code that runs **whenever** the button is pressed (an interrupt handler). If defined, a press runs this instead of the default behaviour. |
| `DISABLE_BUTTON` | none | Stop the button from triggering `BUTTON_DEF`. |
| `ENABLE_BUTTON` | none | Re-enable `BUTTON_DEF` triggering. |

| | |
|---|---|
| **Code Example** | <pre>BUTTON_DEF<br>    STRING button pressed<br>END_BUTTON<br>STRINGLN Press the button...<br>WAIT_FOR_BUTTON_PRESS<br>STRINGLN done</pre> |

**Interpreter Implementation Notes**

- Register the `BUTTON_DEF` body in pass 1 as an **event handler**, not inline
  code (it is skipped during normal top-down flow and invoked only on press).
- On a gadget with no button, bind this to any trigger you have (a GPIO, a
  signal, a Unix socket command, a keyboard combo on the host side). If there is
  truly no trigger, `WAIT_FOR_BUTTON_PRESS` would block forever — either stub it
  to return immediately or make the trigger configurable, and document the
  choice.
- Related internal state: `$_BUTTON_ENABLED`, `$_BUTTON_USER_DEFINED`,
  `$_BUTTON_PUSH_RECEIVED`, `$_BUTTON_TIMEOUT` (default 1000 ms). Set
  `$_BUTTON_PUSH_RECEIVED` when a press is observed; code frequently checks and
  then clears it.

### 8.4 Holding keys

| Command / Keyword | Parameters | Description |
|---|---|---|
| `HOLD <key>` | a key or modifier | Press and **hold** a key without releasing it. Multiple `HOLD`s stack (chord held open). |
| `RELEASE <key>` | a key or modifier | Release a specific previously-held key. |

| | |
|---|---|
| **Code Example** | <pre>HOLD SHIFT<br>DOWNARROW<br>DOWNARROW<br>RELEASE SHIFT</pre> |

**Interpreter Implementation Notes**

- This is where a **persistent key-state model** matters. Keep a live set of
  currently-held usages and the live modifier byte. `HOLD` adds to the set and
  re-sends the report; `RELEASE` removes from the set and re-sends; a normal
  keystroke momentarily adds then removes.
- Remember the **6-key rollover** limit of the boot keyboard report: at most 6
  non-modifier usages held at once. If a payload holds more, either report
  `ErrorRollOver` (0x01 in all slots, matching real keyboards) or cap and warn.
- To hold a lone modifier, combine with `INJECT_MOD` semantics: set the modifier
  bit in the live modifier byte and keep it set until `RELEASE`.
- `RESET` / `RELEASE` of everything = send the empty report and clear the set.

### 8.5 Randomization

| Command / variable | Description |
|---|---|
| `RANDOM_LOWERCASE_LETTER` | Types one random lowercase letter. |
| `RANDOM_UPPERCASE_LETTER` | Types one random uppercase letter. |
| `RANDOM_LETTER` | Random letter, either case. |
| `RANDOM_NUMBER` | Types one random digit. |
| `RANDOM_SPECIAL` | Types one random special character. |
| `RANDOM_CHAR` | Types one random character from any of the above classes. |
| `$_RANDOM_MIN`, `$_RANDOM_MAX` | Bounds (0–65535, default 0–9) for the random integer. |
| `$_RANDOM_INT` | **Read** this to get a fresh random integer in `[MIN, MAX]`. |

| | |
|---|---|
| **Code Example** | <pre>$_RANDOM_MIN = 1<br>$_RANDOM_MAX = 6<br>VAR $ROLL = $_RANDOM_INT<br>RANDOM_LETTER<br>RANDOM_NUMBER</pre> |

**Interpreter Implementation Notes**

- The `RANDOM_*` **commands** each inject a single random keystroke: pick a
  character from the class and type it like `STRING` would one char.
- `$_RANDOM_INT` is a **computed read**: every time an expression reads it,
  produce a new value in `[$_RANDOM_MIN, $_RANDOM_MAX]`. Implement as a special
  getter in your variable-resolution path, not a stored slot.
- Hardware seeds a `seed.bin` on the SD card on first use so sequences differ
  across runs. On Linux, seed your PRNG from `/dev/urandom` once at start (or
  from a persisted seed file if you want reproducibility), and expose
  `$_RANDOM_SEED` if you implement it.
- `VID_RANDOM` etc. feed [8.1](#81-attackmode); random VID/PID can make the host
  fail to enumerate, so the docs warn against them.

### 8.6 LED control

| Command / Keyword | Description |
|---|---|
| `LED_OFF` | All LED modes off. |
| `LED_R` | Red LED on. |
| `LED_G` | Green LED on. |
| *(internal LED toggles)* | `$_SYSTEM_LEDS_ENABLED`, `$_STORAGE_LEDS_ENABLED`, `$_INJECTING_LEDS_ENABLED`, `$_EXFIL_LEDS_ENABLED`, `$_LED_SHOW_CAPS/NUM/SCROLL`, `$_LED_CONTINUOUS_SHOW_STORAGE_ACTIVITY`. |

| | |
|---|---|
| **Code Example** | <pre>LED_G<br>DELAY 500<br>LED_OFF</pre> |

**Interpreter Implementation Notes**

- These drive the **device's own status LED**, which a generic HID gadget does
  not have. Map them to any status output you do have (a board LED via sysfs
  `/sys/class/leds/*`, a console log line, a GUI indicator) or make them no-ops.
  Keep parsing them so LED-heavy payloads run.
- The `$_*_LEDS_ENABLED` family are booleans gating automatic LED behaviours; if
  you have no LED, store them but act on nothing.

### 8.7 Payload control flow (whole-payload)

| Command / Keyword | Description | Interpreter note |
|---|---|---|
| `RESTART_PAYLOAD` | Restart execution from the beginning. | Reset the instruction pointer to line 0. Decide whether to also reset variables (reference restarts the script; globals persist unless re-`VAR`'d at the top). |
| `STOP_PAYLOAD` | Stop execution. | Unwind loops/functions and halt the executor cleanly. |
| `RESET` | Clear the keystroke buffer / release keys. | Send one empty HID report; clear your held-key set. |

### 8.8 Payload hiding

| Command / Keyword | Description |
|---|---|
| `HIDE_PAYLOAD` | Hide `inject.bin` and `seed.bin` on the device's storage. |
| `RESTORE_PAYLOAD` | Restore (unhide) them. |

**Interpreter Implementation Notes**

- These manipulate files on the Ducky's **mass-storage partition** and are only
  meaningful under `ATTACKMODE OFF` or `ATTACKMODE HID` (per the docs). On a
  Linux gadget without emulated storage these are no-ops/stubs. Parse and ignore.

### 8.9 Lock-key waits and state

| Command / Keyword | Description |
|---|---|
| `WAIT_FOR_CAPS_ON` / `WAIT_FOR_CAPS_OFF` / `WAIT_FOR_CAPS_CHANGE` | Block until Caps Lock reaches/leaves/changes state. |
| `WAIT_FOR_NUM_ON` / `_OFF` / `_CHANGE` | Same for Num Lock. |
| `WAIT_FOR_SCROLL_ON` / `_OFF` / `_CHANGE` | Same for Scroll Lock. |
| `SAVE_HOST_KEYBOARD_LOCK_STATE` | Snapshot current lock state into `$_SAVED_*`. |
| `RESTORE_HOST_KEYBOARD_LOCK_STATE` | Restore locks to the snapshot. |

**Interpreter Implementation Notes**

- All of these depend on reading the host's **keyboard LED output report**. On a
  Linux HID gadget, `read()` the HID function fd (or handle the SET_REPORT
  output) to learn the host's Num/Caps/Scroll LED bits, and update
  `$_CAPSLOCK_ON`, `$_NUMLOCK_ON`, `$_SCROLLLOCK_ON`.
- The `WAIT_FOR_*_CHANGE` family is the backbone of **Keystroke Reflection**
  exfiltration (the host is driven to toggle lock LEDs to signal bits back to the
  device). Implement these as blocking waits on the LED-report event.
- Set `$_RECEIVED_HOST_LOCK_LED_REPLY` once you have seen at least one LED report,
  so payloads can tell whether the channel works.

### 8.10 Exfiltration

| Command / variable | Description |
|---|---|
| `EXFIL $VAR` | Write a variable's value to `loot.bin` on the device storage. |
| `$_EXFIL_MODE_ENABLED = TRUE` | Enable Keystroke Reflection, capturing lock-LED-encoded data to `loot.bin`. |

**Interpreter Implementation Notes**

- `EXFIL` writes to the device's **SD card**; on a Linux gadget redirect it to a
  local file you control (e.g. a loot log). It is a local write, not a network
  action, in the base language. Keep it bounded and in a path you own.
- `$_EXFIL_MODE_ENABLED` turns on the lock-LED capture loop described in
  [8.9](#89-lock-key-waits-and-state). This is advanced and optional; stub if you
  are not implementing the reflection channel.

### 8.11 OS detection

| Variable | Values |
|---|---|
| `$_OS` | `WINDOWS`, `MACOS`, `LINUX`, `CHROMEOS`, `ANDROID`, `IOS` (populated by the OS_DETECT extension / host-config requests). |
| `$_HOST_CONFIGURATION_REQUEST_COUNT` | Count of USB configuration requests seen, used to infer the OS. |

**Interpreter Implementation Notes**

- OS detection is driven by observing **USB enumeration behaviour** (how many
  `GET_CONFIGURATION`/`SET_CONFIGURATION` and descriptor requests the host makes,
  and their pattern). On a Linux gadget you can count these via the UDC/gadget
  events. The official mechanism ships as the `OS_DETECT` **extension**
  ([8.12](#812-extensions)) that sets `$_OS`.
- If you don't implement detection, let `$_OS` be settable so payloads using the
  `IF ( $_OS == … )` pattern still parse and run deterministically.

### 8.12 Extensions

| | |
|---|---|
| **Command / Keyword** | `EXTENSION Name` … `END_EXTENSION`, with a `VERSION x.y` line inside. |
| **Parameters** | `Name` is the extension's title; the body holds functions (and a `VERSION`). |
| **Description** | A packaged, reusable block, typically wrapping one or more `FUNCTION`s, that you paste into a payload. Once included, its functions are called like any other. Payload Studio can auto-insert official extensions; others are copied from the Hak5 payload repo. There is **no `IMPORT` statement** — inclusion is textual. |
| **Code Example** | <pre>EXTENSION ASCIIDUCK<br>    VERSION 1.0<br>    FUNCTION DUCK()<br>        STRINGLN   __(.)&lt; QUACK!<br>    END_FUNCTION<br>END_EXTENSION<br><br>STRING Let's run our first extension:<br>DUCK()</pre> |

**Interpreter Implementation Notes**

- Treat `EXTENSION Name … END_EXTENSION` as a **namespace-ish wrapper that is
  registered, not executed inline**: in pass 1, walk its body and register the
  `FUNCTION`s (and note `VERSION`) exactly as top-level functions. During normal
  top-down execution, **skip** the extension block the way you skip
  `FUNCTION`/`BUTTON_DEF` bodies.
- Because inclusion is textual (copy-paste), you don't need a module loader; you
  just need the parser to accept the wrapper and hoist its functions into the
  global function table. Function names are global, so watch for collisions.
- Common official extensions and the globals they populate: `OS_DETECT` →
  `$_OS`; `TRANSLATE` → provides `TRANSLATE_INT()` / `TRANSLATE_HEX()` /
  `TRANSLATE_BOOL()` that type a `$INPUT` value as decimal/hex/boolean text. If
  you want drop-in compatibility, ship equivalents.

---

## 9. Built-in Internal Variables (`$_`)

Reserved variables beginning with `$_`. Some are **writable settings**, some are
**read-only sensors**, and a few (like `$_RANDOM_INT`) are **computed on read**.
Your variable-resolution layer must special-case the whole `$_` namespace and
route reads/writes to engine state rather than the plain symbol table.

### 9.1 Button

| Variable | R/W | Meaning |
|---|---|---|
| `$_BUTTON_ENABLED` | R/W | Whether the button triggers `BUTTON_DEF`. |
| `$_BUTTON_USER_DEFINED` | R | Whether a `BUTTON_DEF` exists. |
| `$_BUTTON_PUSH_RECEIVED` | R/W | Set true on a press; scripts read then clear it. |
| `$_BUTTON_TIMEOUT` | R/W | Press-wait timeout, default **1000** ms. |

### 9.2 LED

`$_SYSTEM_LEDS_ENABLED`, `$_STORAGE_LEDS_ENABLED`,
`$_LED_CONTINUOUS_SHOW_STORAGE_ACTIVITY`, `$_INJECTING_LEDS_ENABLED`,
`$_EXFIL_LEDS_ENABLED`, `$_LED_SHOW_CAPS`, `$_LED_SHOW_NUM`, `$_LED_SHOW_SCROLL`
— all R/W booleans gating automatic LED behaviour.

### 9.3 ATTACKMODE (read-only)

| Variable | Meaning |
|---|---|
| `$_CURRENT_VID` | Active Vendor ID. |
| `$_CURRENT_PID` | Active Product ID. |
| `$_CURRENT_ATTACKMODE` | 0=OFF, 1=HID, 2=STORAGE, 3=HID+STORAGE. |

### 9.4 Random

`$_RANDOM_INT` (computed read), `$_RANDOM_MIN`, `$_RANDOM_MAX`, `$_RANDOM_SEED`,
and `$_RANDOM_*_KEYCODE` variants backing the `RANDOM_*` commands.

### 9.5 Jitter

`$_JITTER_ENABLED` (R/W bool), `$_JITTER_MAX` (R/W, 0–65535 ms, default 20).

### 9.6 Lock keys

`$_CAPSLOCK_ON`, `$_NUMLOCK_ON`, `$_SCROLLLOCK_ON` (read sensors),
`$_SAVED_CAPSLOCK_ON`, `$_SAVED_NUMLOCK_ON`, `$_SAVED_SCROLLLOCK_ON` (snapshots),
`$_RECEIVED_HOST_LOCK_LED_REPLY` (have we seen an LED report yet).

### 9.7 Storage / exfil / OS

`$_STORAGE_ACTIVITY_TIMEOUT` (default 1000), `$_EXFIL_MODE_ENABLED` (R/W bool),
`$_HOST_CONFIGURATION_REQUEST_COUNT` (R), `$_OS` (R; one of WINDOWS, MACOS, LINUX,
CHROMEOS, ANDROID, IOS).

**Interpreter Implementation Notes**

- Keep a dispatch table keyed by reserved name → `{getter, setter}`. Unknown
  `$_` names should error (they are reserved), unknown plain `$` names should
  error as "undeclared variable."
- Read-only variables must reject assignment. Computed reads
  (`$_RANDOM_INT`) must run their getter every time.
- Defaults to seed at interpreter start: `$_BUTTON_TIMEOUT = 1000`,
  `$_STORAGE_ACTIVITY_TIMEOUT = 1000`, `$_JITTER_MAX = 20`,
  `$_RANDOM_MIN = 0`, `$_RANDOM_MAX = 9`, booleans `FALSE`.

---

## Appendix A — HID Usage Mapping

The executor's final job is turning commands into **USB HID boot-keyboard
reports**. The classic 8-byte report is:

```
byte 0 : modifier bitmap
byte 1 : reserved (0)
byte 2 : keycode 1
byte 3 : keycode 2
byte 4 : keycode 3
byte 5 : keycode 4
byte 6 : keycode 5
byte 7 : keycode 6
```

**Modifier bitmap (byte 0):**

| Bit | Mask | Modifier (DuckyScript keyword) |
|---|---|---|
| 0 | 0x01 | Left Ctrl (`CTRL`/`CONTROL`) |
| 1 | 0x02 | Left Shift (`SHIFT`) |
| 2 | 0x04 | Left Alt (`ALT`/`OPTION`) |
| 3 | 0x08 | Left GUI (`GUI`/`WINDOWS`/`COMMAND`) |
| 4–7 | — | right-side variants (rarely needed) |

**Representative keycodes (USB HID Usage Page 0x07):**

| DuckyScript | Usage (hex) | DuckyScript | Usage (hex) |
|---|---|---|---|
| `a`–`z` | 0x04–0x1D | `ENTER` | 0x28 |
| `1`–`9` | 0x1E–0x26 | `ESCAPE` | 0x29 |
| `0` | 0x27 | `BACKSPACE` | 0x2A |
| `TAB` | 0x2B | `SPACE` | 0x2C |
| `CAPSLOCK` | 0x39 | `F1`–`F12` | 0x3A–0x45 |
| `PRINTSCREEN` | 0x46 | `SCROLLLOCK` | 0x47 |
| `PAUSE` | 0x48 | `INSERT` | 0x49 |
| `HOME` | 0x4A | `PAGEUP` | 0x4B |
| `DELETE` | 0x4C | `END` | 0x4D |
| `PAGEDOWN` | 0x4E | `RIGHTARROW` | 0x4F |
| `LEFTARROW` | 0x50 | `DOWNARROW` | 0x51 |
| `UPARROW` | 0x52 | `NUMLOCK` | 0x53 |
| `MENU`/`APP` | 0x65 | | |

**Notes**

- Shifted characters (uppercase letters, `!@#$…`) reuse the base usage **plus**
  the `SHIFT` bit in byte 0. Build the char→(usage, shift) table from the active
  layout.
- Every keystroke is **press report → release report**. The release report is all
  zeroes (or the remaining held keys if you keep a held-key set for `HOLD`).
- The host must have **enumerated** the gadget before any report counts. Compose
  the gadget (`ATTACKMODE`) and wait for the UDC to bind first.
- Linux gadget write path: open `/dev/hidg0` and `write()` the 8-byte report;
  read the host's LED output report from the same fd (or via the function's
  SET_REPORT) for the lock-state features.

---

## Appendix B — Suggested Lexer / Parser Architecture

A concrete plan for the lexer, AST, and executor, matched to this language's
line-oriented grammar.

**1. Line reader / preprocessor**

- Read file, normalise `CRLF`→`LF`, keep line numbers for diagnostics.
- Strip `REM` lines and `REM_BLOCK … END_REM` spans.
- Apply `DEFINE #NAME value` textual substitution across remaining lines.

**2. Lexer (per line)**

- First token = the **command keyword** (uppercase) in most cases. Dispatch on
  it. Exceptions that are not keyword-led: a line starting with `$NAME =`
  (assignment) and a line that is a bare `NAME()` (function call).
- For `STRING`/`STRINGLN`: after the keyword + one space, the rest of the line is
  a single **raw text token** (no further tokenising).
- For expressions (conditions, RHS of `=`, `RETURN`): tokenise into numbers,
  `TRUE`/`FALSE`, `$VAR`, `$_INTERNAL`, operators, parentheses, and `NAME()`
  calls.

**3. Parser / AST**

- Node types: `KeystrokeCmd` (STRING, keys, modifiers, HOLD/RELEASE, RANDOM_*),
  `Delay`, `Assign`, `VarDecl`, `If` (arms + else), `While`, `FunctionDef`,
  `Call`, `Return`, `ButtonDef`, `ExtensionDef`, `AttackMode`, `Setting` (the
  `$_` writes), and `Misc` (LED, payload control, exfil, waits).
- Match block openers to their `END_*` with an explicit stack; report
  unterminated blocks with the opener's line number.
- Pass 1 hoists: `DEFINE`, `FunctionDef`, `ExtensionDef` (hoist its functions),
  `ButtonDef`, and the leading `AttackMode`.

**4. Expression evaluator**

- Pratt/shunting-yard over the [Section 5](#5-operators) operator set, evaluating in
  `uint16_t` with wraparound. Operand resolvers: literal, boolean, `$VAR`
  (symbol table), `$_` (engine dispatch, incl. computed reads), `NAME()` (run the
  function, take its `RETURN`).

**5. Executor**

- A program counter over the top-level statement list; a **return stack** for
  function calls (GOSUB model, no frames). `WHILE` jumps back to its condition;
  `IF` falls through. `RESTART_PAYLOAD` resets PC to 0; `STOP_PAYLOAD` ends the
  loop.
- A single global environment (symbol table) plus an engine-state struct for all
  `$_` settings/sensors and the live HID key-state (modifier byte + held-key
  set).
- The button handler and lock-LED reader run as **asynchronous inputs** checked
  between statements / inside loops so `WAIT_FOR_*` and `BUTTON_DEF` work during
  long or infinite loops.

**6. HID output layer**

- `press(usage, modifiers)` → write down-report, hold `DEFAULT_CHORD`, write
  release-report; apply `DELAY`/`DEFAULT_DELAY`/jitter around it.
- `hold(usage)` / `release(usage)` mutate the held-key set and re-emit.
- Respect 6-key rollover; emit `ErrorRollOver` or cap on overflow.

---

*Compiled from the official Hak5 DuckyScript 3.0 documentation
(documentation.hak5.org and shop.hak5.org/pages/duckyscript-3-0). Commands,
parameters, defaults, and quirks follow the published reference; the
interpreter-implementation notes are guidance for a Linux USB HID gadget target.*
