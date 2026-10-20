# 00 · Overview

Quacky is a tiny shell-flavoured scripting language that drives a USB HID keyboard. Scripts
compute with ints, bools and strings, loop, branch, call functions, pause, `type` text, and
press key **chords** (`ctrl + c`). The interpreter turns that into USB HID keyboard reports
written to `/dev/hidgX` (the node the companion **USB Arsenal** project exposes through
configfs), so the host computer sees a keyboard typing at supersonic speed, one key event
every 5 ms by default. For a safe dry-run and for the test suite it instead writes a
human-readable transcript. The interpreter is a single static C11 binary for x86_64, i686,
aarch64, armhf and riscv64.

It is an educational, free and open-source project: a learning-friendly, no-cost alternative
to expensive commercial typing-automation hardware, and a small, readable interpreter to study.

This folder is the design guide: how the interpreter in [`src/`](../../src/) is built, module by
module. Read it alongside the code.

## Goals
- Correct and boring. Every undefined-behaviour trap in C integer arithmetic handled in one function.
- Portable to 32-bit: `int64_t` values, `size_t` lengths, 64-bit `time_t` via musl 1.2.5.
- Precise cadence: absolute-deadline sleeps, no cumulative drift.
- Small: 60–130 KB static binaries, zero dependencies beyond libc.
- Readable: ~3 200 lines of C across seven files.
- Safe to rehearse: a dry-run transcript and a `--check` parse step before anything types.

## Non-goals
Speed beyond "invisible next to a 5 ms tick". Floats, containers, closures, modules, stdin,
a REPL, a bytecode VM. Mouse reports, consumer/media keys, LEDs, buttons, USB identity and
storage: USB Arsenal owns the gadget, Quacky only writes keyboard reports to the node it
provides.

## Pipeline

```
 script.qk ──► read file, CRLF→LF
            ──► lexer      tokens as slices; `type`/`typeln` switch to raw-line mode (02)
            ──► parser     recursive descent + Pratt; `for`/`elif` desugared;          (02)
                           `${expr}` sub-parsed; syntax errors exit 2 before any output
            ──► AST        arena-allocated, freed once at exit                         (02)
            ──► hoist      register all `func` declarations                            (03)
            ──► evaluator  tree-walk; status codes OK/ERR/RETURN/BREAK/CONTINUE;      (03)
                           hash-map frames, two-level scoping; refcounted strings;      (04)
                           chords/hold/release/delay/repeat drive the output module
            ──► output     open node/file once; per key event: clock_nanosleep         (05)
                           (CLOCK_MONOTONIC, TIMER_ABSTIME) then write(2).
                           HID backend → 8-byte reports to /dev/hidgX;
                           transcript backend → readable text (dry-run + tests)
            ──► exit       0 ok · 1 runtime · 2 syntax · 64+ usage                     (06)
```

CLI: `quacky script.qk` writes a transcript to stdout (dry-run); `quacky --hid /dev/hidg0
script.qk` types for real; `quacky -o out.txt script.qk` writes a transcript to a file (tests);
`quacky --check script.qk` parses only and reports syntax errors; `-d MS` sets the tick
(default 5 ms); `--fast` rolls each typed key into the next for roughly twice the HID throughput.

## Document map

| Doc | Covers |
|---|---|
| [01-language-spec](01-language-spec.md) | Grammar, operators, semantics, `type` and keystroke rules, sample programs |
| [02-frontend](02-frontend.md) | Lexer incl. raw mode and chords, Pratt parser, AST, arena |
| [03-evaluator](03-evaluator.md) | Status codes, frames, calls, hoisting, keystroke statements |
| [04-values-and-strings](04-values-and-strings.md) | Value union, refcounting, `int_op` edge cases |
| [05-type-and-timing](05-type-and-timing.md) | Output backends (HID + transcript), keymap, reports, absolute deadlines, signals |
| [06-errors](06-errors.md) | Classes, message format, status-codes-vs-longjmp decision |
| [07-build-and-cross](07-build-and-cross.md) | Toolchains, flags, Makefile, 32-bit checklist |
| [08-testing](08-testing.md) | Golden files, timing tests, qemu, sanitizers |
| [09-references](09-references.md) | Prior art: the projects studied and the idea borrowed from each |

## Project layout

```
quacky/
  src/            the interpreter, one .c per module
    main.c        CLI (--hid DEV, -o FILE, --check, -d MS, --fast), read file, run, exit codes
    lex.c         tokens, raw mode, chords
    parse.c       statements, Pratt, `type` segments, chords, arena
    eval.c        Interp, frames, status codes, calls, keystroke statements
    value.c       Value, QStr refcount, to_string, int_op
    output.c      fd, backends, ticks, sleep/delay, HID reports, held set, write_all
    keys.c        key-name table and US char→(modifier,usage) table
    textfile.c    type_file loader: resolve, read, strict-UTF-8 validate
    include/      the module headers, one per src/ unit (built with -Isrc/include)
  tests/          cases/*.qk + *.out (transcript) + *.hid (report bytes), run.sh
  tools/          qtype, a typing-speed meter (not part of the shipped binary)
  Makefile        five cross targets + native sanitizer build
  docs/           end-user documentation (language, keys, examples, getting started)
  docs/developer/ this design guide
```

## Key decisions in one screen

| Decision | Choice | Alternative rejected |
|---|---|---|
| Execution | tree-walker | bytecode VM: 3–10× faster evaluation that is already 1000× faster than the sleep |
| Error/control propagation | status codes (picol) | `setjmp/longjmp` (Lua): leaks refcounted strings unless a GC tracks them |
| Variable storage | hash map per frame, two scope levels | resolver + slots (clox): speed we cannot observe |
| Strings | refcount, arena literals immortal | leak-until-exit: unbounded in loops; GC: no cycles possible without containers |
| Timing | `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)` on a running deadline | relative `nanosleep`: accumulates wake-up latency per character |
| Output | raw `write(2)` per key event; two backends (HID reports, transcript) behind one interface | stdio: buffers, destroys the cadence; a single hard-wired backend: no safe dry-run or golden tests |
| Precedence | C table verbatim | "fixed" bitwise precedence: breaks bash users' intuition |
| Truthiness | none, conditions must be bool | int truthiness: hides type errors |

## When a bytecode VM would pay off
Only if the language gains a `-d 0` batch mode that people use for heavy computation (say,
millions of loop iterations with no `type`). Then: compile the AST to a flat `uint8_t` array
with a constant pool, run a `switch`-dispatch loop with a value stack, keep the same `Value`,
`int_op`, and output module. clox `c/compiler.c` and `c/vm.c` are the template. Everything in
02 (lexer, parser, AST) and 04–06 survives unchanged; only 03 is replaced.
