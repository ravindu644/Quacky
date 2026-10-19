# Roadmap: Quacky

**Goal.** One static C11 binary that reads a human-readable Quacky script and writes USB HID
keyboard reports to `/dev/hidgX`, so the host computer sees a keyboard typing text and key
combinations at supersonic speed. The same binary can instead write a readable transcript, for
a safe dry-run and for the test suite, and can parse a script without typing anything.

**Background.** The language and the interpreter are fully specified in `docs/developer/`:
[00-overview](00-overview.md) is the map, [01-language-spec](01-language-spec.md) the grammar
and semantics, [02-frontend](02-frontend.md) the lexer/parser/AST, [03-evaluator](03-evaluator.md)
the tree-walker, [04-values-and-strings](04-values-and-strings.md) the value model,
[05-type-and-timing](05-type-and-timing.md) the output backends and HID reports,
[06-errors](06-errors.md) the diagnostics, [07-build-and-cross](07-build-and-cross.md) the
build, [08-testing](08-testing.md) the tests, [09-references](09-references.md) the borrowed
ideas. Every box below points at the doc that specifies it.

**Decisions.**
- Tree-walker, not a bytecode VM: evaluation is already a thousand times faster than the tick.
- Control flow travels on status codes, not `setjmp`/`longjmp`, so refcounted strings always
  clean up.
- Values are `int64`, `bool`, `str` only; conditions must be `bool` (no truthiness); every
  integer UB case lives in one `int_op`.
- One output module, two backends behind one interface: real HID reports (`--hid`) and a
  readable transcript (default, `-o FILE`). `--check` parses without opening output.
- The pacing clock is an absolute deadline (`clock_nanosleep`, `CLOCK_MONOTONIC`,
  `TIMER_ABSTIME`), so cadence does not drift.
- Key names are contextual, not reserved, resolved through one table in `keys.c`; US-QWERTY
  only in v1, with a hook for other layouts.
- USB Arsenal owns the gadget (configfs, controller, identity). Quacky only opens an existing
  `/dev/hidgX` and writes keyboard reports.

Tick a box only when the item is implemented and verified as described in its phase.

## Phase 0. Blueprint and scaffolding
- [x] The nine developer docs: language, frontend, evaluator, values, output/timing, errors,
      build, testing, references
- [x] `AGENTS.md`, `README.md`, and the project's framing and wording rules
- [x] External reference material removed; no forbidden words in the kept files
- [ ] Repo scaffolding: `src/` with the seven `.c`/`.h` stubs from 00, `tests/`, a `.gitignore`
      that ignores `bin/` and `research/`
- [ ] Arena bump allocator, freed once at exit (02 §6)
- [ ] Diagnostics plumbing shared by both error classes: `print_diagnostic(file, src, line,
      col, msg)` with the caret line, `rt_error`, `syntax_error`, the exit codes (06)

## Phase 1. Lexer (02)
- [ ] Read the whole file, NUL-terminate, normalise CRLF to LF, keep the filename; tokens are
      slices into this buffer
- [ ] Token kinds and `Token {kind, start, len, line, col}`; line/col precomputed
- [ ] Identifiers and keywords (length + `memcmp`), including `hold`/`release`/`delay`/`repeat`
- [ ] Integers (decimal, `0x`, `0b`) range-checked to `INT64_MAX`; booleans
- [ ] Strings to the closing quote on one line (escape decoding deferred to the parser)
- [ ] Line comment `#` to end of line, block comment `#[ … ]#` with the unclosed error
- [ ] Operators and punctuation, two-character tokens greedy (`&&`, `||`, `==`, `!=`, `<=`,
      `>=`, `<<`, `>>`, `..`), and `+`
- [ ] `type` raw mode: the `after_type` bit emits the rest of the line as `T_RAW_LINE`

## Phase 2. Parser and AST (02)
- [ ] Recursive descent over statements, `skip_seps`, the terminator rule (newline / `;` /
      block closer)
- [ ] Pratt expressions: the precedence enum, the `rules[]` table, `&&`/`||` as their own
      short-circuit nodes; cap expression nesting at 256
- [ ] Statement dispatch: assignment vs call vs chord by the first two tokens and a `keys.c`
      lookup (01 grammar note)
- [ ] `if`/`elif`/`else`/`fi` chain, `while`, `for` desugared to `while`, `func` (top level
      only); misplaced `return`/`break`/`continue` are syntax errors via `loop_depth`/`in_func`
- [ ] `type` raw line into segments: escape decoding, `$name`, `${expr}` sub-lexer, `\c`
- [ ] Chords: `parse_chord` resolving names/letters/digits into a `Chord {mod, usages, n}`;
      `hold`/`release`/`release all`; `delay`/`repeat` as expr statements; `unknown key` and
      `too many keys in a chord` as syntax errors
- [ ] The whole AST arena-allocated; `for` and `elif` never appear as node kinds

## Phase 3. Values and strings (04)
- [ ] `Value` union; `QStr` refcount with arena literals immortal (`rc = -1`)
- [ ] `str_retain`/`str_release`/`val_retain`/`val_release` and the five ownership rules
- [ ] `to_string` (int decimal via `PRId64`, bool `true`/`false`), string `+` concat
- [ ] `int_op`: div/mod by zero, `INT64_MIN / -1`, `% -1`, out-of-range shifts, two's-complement
      wrap, all handled without UB (UBSan proves it)
- [ ] `equal`/`compare`; the 32-bit `_Static_assert`s

## Phase 4. Evaluator (03)
- [ ] `Interp` state; frames as 64-bucket FNV-1a hash maps; two-level scope with
      lookup/assign/const and their error wording (matches 06)
- [ ] Expression evaluation with the type matrix; type errors naming expected and actual types
- [ ] Statement evaluation: assign, const, if, while, sleep, type, call
- [ ] Function calls: GOSUB with a return stack, arity check, depth limit 200, `VAL_NONE` used
      as an expression raises "returned no value"
- [ ] Keystroke statements: chord, hold, release, delay; `repeat` via `last_action`; the
      default-delay `out_post_statement` after every statement
- [ ] Hoisting of `func` declarations, startup, `SIGPIPE` ignore

## Phase 5. Key tables (`keys.c`, 02 §4.4, 05 §4)
- [ ] Key-name table: modifiers with their bits and aliases, named keys with their usages,
      `endkey`; `key_lookup(name)`
- [ ] US char → `{modifier, usage}` table: letters, digits, shifted symbols with the Shift bit,
      and `\n` → enter, `\t` → tab, `\b` → backspace; `char_key(codepoint)`
- [ ] One source of truth: the parser and the output module both call these

## Phase 6. Output module (05)
- [ ] `Output` state; open per mode (`--hid` device `O_WRONLY`; `-o` file `O_APPEND|O_CREAT`;
      else stdout); `--check` opens nothing
- [ ] The clock: `wait_tick`, `ts_add_ns`, the catch-up rule; `-d MS` with the 32-bit ns cap
- [ ] `out_sleep_ms`, `out_set_default_delay`, `out_post_statement`
- [ ] Transcript backend: `type` text verbatim, every key action as a tag
- [ ] HID backend: the 8-byte report builder, press then release, the held set merged into
      every report, six-key rollover error
- [ ] Signals and failure: `SIGPIPE` ignored, `EPIPE`/`EIO`/`ESHUTDOWN`/`ENOSPC` to a runtime
      error and exit 1

## Phase 7. CLI and wiring (`main.c`, 00, 06)
- [ ] Option parsing: `--hid DEV`, `-o FILE`, `--check`, `-d MS`, `--help`; usage errors exit
      64/66/73
- [ ] Read file, run, map the result to exit 0 (ok) / 1 (runtime) / 2 (syntax)
- [ ] End to end: a script runs as a transcript, `--check`s clean, and `--hid` to a regular
      file writes the expected bytes

## Phase 8. Build and cross-compilation (07)
- [ ] Makefile: the five musl cross targets and the native ASan+UBSan build; bare `make` prints
      help
- [ ] `-Werror` clean on all five toolchains, including `-Wconversion` on the 32-bit ones
- [ ] Static binaries verified with `file`, sizes in the 60 to 130 KB range
- [ ] `qemu-user-static` installed; the golden suite runs on i686 and armhf first

## Phase 9. Testing (08)
- [ ] `tests/run.sh`: transcript `cmp`, `.hid` byte `cmp` (writing `--hid` to a regular file),
      error stderr+exit compare
- [ ] Golden cases: arith, precedence, strings, control, funcs, type_escapes, comments, keys
- [ ] `.hid` byte-capture for the keys case (chords, hold/release, shifted characters)
- [ ] Error cases: one per catalogue entry plus the syntax errors, including the keystroke ones
- [ ] `--check` run over every case (clean on good ones, exit 2 and no output on bad ones)
- [ ] Timing tests on the native build: 100 characters at `-d 20`, sleep-then-type lands on
      time, the 500-character drift test
- [ ] Golden suite under ASan+UBSan; the byte-mutation fuzzer asserting exit in {0,1,2}

## Phase 10. Real HID verification (needs a USB Arsenal gadget)
- [ ] Dry-run transcript of a real script reads correctly before anything types
- [ ] `--hid /dev/hidg0` on a board or phone running USB Arsenal types a line into an editor on
      a host computer
- [ ] A chord (`super + r`, `ctrl + alt + delete`) reaches the host as the right combination
- [ ] `hold shift` + arrows selects text on the host; `release all` leaves no key stuck
- [ ] A non-US character in `type` fails loudly under `--hid` with the `no US-layout key` error

## Stretch
- [ ] Alternate keyboard layouts behind the `keys.c` hook (a second char table, selected by a
      flag)
- [ ] CI that builds the five static binaries on every push
- [ ] A `-d 0` heavy-compute batch mode, and the bytecode VM from 00, only if a profile ever
      shows evaluation mattering next to the tick

## Verification on hardware
- [ ] A Linux host typed into through a `/dev/hidgX` provided by USB Arsenal, on at least one
      board or phone, covering `type`, chords, and `hold`/`release`

## Status log
- 2026-10-20: Blueprint complete. The language was reoriented from a text typewriter into one
  that drives a USB HID keyboard: verb-less chords joined with `+`, `hold`/`release`,
  `delay`/`repeat`, and a dual output backend (readable transcript and real HID reports) with a
  `--check` parse mode. Every decision is ported into the nine developer docs; `AGENTS.md` and a
  rewritten `README.md` were added, along with the framing and wording rules; the external
  reference material was removed. No interpreter code exists yet. This roadmap is the path from
  here to a verified binary.
