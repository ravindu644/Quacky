# 00 · Overview

Quacky (placeholder name) is a tiny shell-flavoured scripting language whose **only** side
effect is appending text to one output stream, one character every 20 ms. Scripts compute
with ints, bools and strings, loop, branch, call functions, sleep, and `type` text. Nothing
else. The interpreter is a single static C11 binary for x86_64, i686, aarch64, armhf and
riscv64.

This folder is the blueprint for the real implementation. No interpreter code exists here.

## Goals
- Correct and boring. Every undefined-behaviour trap in C integer arithmetic handled in one function.
- Portable to 32-bit: `int64_t` values, `size_t` lengths, 64-bit `time_t` via musl 1.2.5.
- Precise cadence: absolute-deadline sleeps, no cumulative drift.
- Small: 60–130 KB static binaries, zero dependencies beyond libc.
- Readable: ~3 000 lines of C across six files.

## Non-goals
Speed beyond "invisible next to a 20 ms sleep". Floats, containers, closures, modules, stdin,
a REPL, a bytecode VM.

## Pipeline

```
 script.qk ──► read file, CRLF→LF
            ──► lexer      tokens as slices; `type` switches to raw-line mode        (02)
            ──► parser     recursive descent + Pratt; `for`/`elif` desugared;          (02)
                           `${expr}` sub-parsed; syntax errors exit 2 before any output
            ──► AST        arena-allocated, freed once at exit                         (02)
            ──► hoist      register all `func` declarations                            (03)
            ──► evaluator  tree-walk; status codes OK/ERR/RETURN/BREAK/CONTINUE;      (03)
                           hash-map frames, two-level scoping; refcounted strings      (04)
            ──► output     open(O_APPEND) once; per code point: clock_nanosleep        (05)
                           (CLOCK_MONOTONIC, TIMER_ABSTIME) then write(2)
            ──► exit       0 ok · 1 runtime · 2 syntax · 64+ usage                     (06)
```

## Document map

| Doc | Covers |
|---|---|
| [01-language-spec](01-language-spec.md) | Grammar, operators, semantics, `type` rules, sample program |
| [02-frontend](02-frontend.md) | Lexer incl. raw mode, Pratt parser, AST, arena |
| [03-evaluator](03-evaluator.md) | Status codes, frames, calls, hoisting |
| [04-values-and-strings](04-values-and-strings.md) | Value union, refcounting, `int_op` edge cases |
| [05-type-and-timing](05-type-and-timing.md) | Output fd, UTF-8 ticks, absolute deadlines, signals |
| [06-errors](06-errors.md) | Classes, message format, status-codes-vs-longjmp decision |
| [07-build-and-cross](07-build-and-cross.md) | Toolchains, flags, Makefile, 32-bit checklist |
| [08-testing](08-testing.md) | Golden files, timing tests, qemu, sanitizers |
| [09-references](09-references.md) | Every borrowed idea with `research/<repo>/<file>:<line>` |

## Future project layout

```
quacky/
  src/
    main.c        CLI (-o FILE, -d MS), read file, run, exit codes
    lex.c/.h      tokens, raw mode
    parse.c/.h    statements, Pratt, `type` segments, arena
    eval.c/.h     Interp, frames, status codes, calls
    value.c/.h    Value, QStr refcount, to_string, int_op
    output.c/.h   fd, ticks, sleep, write_all
  tests/          cases/*.qk + *.out, run.sh
  Makefile        five cross targets + native sanitizer build
  docs/developer/ this blueprint
```

## Key decisions in one screen

| Decision | Choice | Alternative rejected |
|---|---|---|
| Execution | tree-walker | bytecode VM: 3–10× faster evaluation that is already 1000× faster than the sleep |
| Error/control propagation | status codes (picol) | `setjmp/longjmp` (Lua): leaks refcounted strings unless a GC tracks them |
| Variable storage | hash map per frame, two scope levels | resolver + slots (clox): speed we cannot observe |
| Strings | refcount, arena literals immortal | leak-until-exit: unbounded in loops; GC: no cycles possible without containers |
| Timing | `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)` on a running deadline | relative `nanosleep`: accumulates wake-up latency per character |
| Output | raw `write(2)` per code point, `O_APPEND` | stdio: buffers, destroys the effect |
| Precedence | C table verbatim | "fixed" bitwise precedence: breaks bash users' intuition |
| Truthiness | none, conditions must be bool | int truthiness: hides type errors |

## When a bytecode VM would pay off
Only if the language gains a `-d 0` batch mode that people use for heavy computation (say,
millions of loop iterations with no `type`). Then: compile the AST to a flat `uint8_t` array
with a constant pool, run a `switch`-dispatch loop with a value stack, keep the same `Value`,
`int_op`, and output module. clox `c/compiler.c` and `c/vm.c` are the template. Everything in
02 (lexer, parser, AST) and 04–06 survives unchanged; only 03 is replaced.
