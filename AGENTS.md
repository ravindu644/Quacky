# AGENTS.md

Instructions for AI coding agents working in this repository. Read
[docs/developer/00-overview.md](docs/developer/00-overview.md) for how the pieces fit together
and the document map for the rest. This file holds the house rules and the facts you must not
relearn the hard way.

## What this is

Quacky is a tiny scripting language and a single static C11 interpreter for it. A script is
human-readable (shell-and-Python-flavoured, no indentation); the interpreter reads it and
writes USB HID keyboard reports to `/dev/hidgX`, so the host computer sees a keyboard typing
text and key combinations at supersonic speed. The gadget node comes from the companion
**USB Arsenal** project, which owns the USB side (configfs, the controller, the identity);
Quacky only speaks keyboard, through two output backends: real HID reports, or a readable
transcript for safe dry-runs and the test suite.

This is an educational, free and open-source project: a learning-friendly, no-cost alternative
to expensive commercial typing-automation hardware, and a small interpreter to study.

**The repository is currently a blueprint.** The design lives in `docs/developer/`; no
interpreter code exists yet. When you add code, follow the blueprint and keep it in sync.

## Build

The design targets five static musl builds plus a native sanitizer build. Details and the
Makefile skeleton are in
[docs/developer/07-build-and-cross.md](docs/developer/07-build-and-cross.md).

- Toolchains are `~/toolchains/<triple>-cross/bin/<triple>-gcc`, all musl 1.2.5, for
  `x86_64`, `i686`, `aarch64`, `arm-musleabihf` and `riscv64`.
- `make` cross-compiles all five into `bin/`; `make native` builds with ASan+UBSan for the
  tests; `make clean` wipes `bin/`.
- `SRC` is a `wildcard src/*.c`, so a new `.c` in `src/` is picked up with no Makefile edit;
  there is no header-dependency tracking, so `make clean` after editing a header.
- Flags are pinned (`-std=c11`, `-Werror`, `-Wconversion`, `-static`). Do not loosen them to
  silence a warning; fix the code. `-Wconversion` is what catches 32-bit narrowing before an
  armhf box does at 3 a.m.

## Layout

| Path | What lives there |
|---|---|
| `src/main.c` | CLI (`--hid DEV`, `-o FILE`, `--check`, `-d MS`), read the file, run, exit codes |
| `src/lex.c` | tokens, `type` raw mode, chords |
| `src/parse.c` | recursive-descent statements, Pratt expressions, the arena |
| `src/eval.c` | the tree-walker: frames, status codes, calls, keystroke statements |
| `src/value.c` | the `Value` union, refcounted strings, `to_string`, `int_op` |
| `src/output.c` | the one place that touches the OS for output: fd, backends, clock, HID reports, held-key set |
| `src/keys.c` | the single source of truth for key names and the US char→(modifier,usage) table |
| `docs/developer/` | this blueprint, nine numbered docs |
| `tests/` | `cases/*.qk` with `.out` (transcript) and `.hid` (report bytes), plus `run.sh` |

One fact worth keeping: `output.c` and `keys.c` are the only modules that know the HID wire
format. Everything above them deals in characters, chords and key names, never bytes.

## Style

Four rules. They apply to code, comments, commit messages, and documentation.

**No em-dashes.** Use a comma, a full stop, or rewrite the sentence.

**No ASCII banner comments.** No rows of `-----`, no `=====`, no boxed section headers. An
SPDX licence block at the top of each file is fine.

**Comments sound like a person.** Say why the code does something, or what breaks if it does
not. Skip comments that restate the line below them.

```c
/* Bad: write the report to the fd */
write(fd, report, 8);

/* Good: the host sees a key as held until we send the release; without this it auto-repeats */
write(fd, release, 8);
```

**Ten lines that work beat a hundred that do the same thing.** Delete before you add. A smaller
diff in the right place is the goal, not a smaller diff anywhere.

### Framing and wording (a hard rule)

Describe the project as what it is: an educational, free and open-source interpreter that reads
a script and writes HID keyboard reports to `/dev/hidg0`, typing text and key combinations at
supersonic speed for the host. The target computer is "the host" or "the target computer".

Never write the words **"ducky", "rubber ducky", "duckyscript"** (the project name "Quacky" is
fine), nor **"attack", "keystroke injection", "payload", "exfiltrate", "victim", "malicious"**,
anywhere: code, comments, docs, commit messages. Convey the same meaning with neutral, positive
phrasing instead. This is not optional and it is checked (this file is the one place the banned
words appear, because it has to state the rule, so the check skips it):

```sh
grep -riE 'ducky|rubber duck|duckyscript|attack|keystroke injection|payload|exfiltrat|victim|malicious' \
  docs README.md src
```

must print nothing.

## Before you write it at all

Work down this list and stop at the first answer that holds.

1. Does this need to exist? A flag nobody asked for, a knob for a value that never changes, an
   abstraction with one implementation: skip it and say so in one line.
2. Does it already exist here? Grep first. The key tables in `keys.c`, the clock in
   `output.c`, `int_op` in `value.c` are each meant to be the one place their job is done.
3. **Does libc or the OS already do it?** `clock_nanosleep` paces the output; `write(2)`
   delivers it; `strtoull` parses numbers. Do not hand-roll what the standard library and the
   kernel already provide.
4. Can it be one line? Then it is one line.
5. Only then write the smallest thing that works.

Two answers work? Take the higher one and move on. The list shortens the solution, never the
reading: trace the flow a change touches before you pick a rung. A small diff in the wrong
place is a second bug, not a lazy fix.

Never simplify away: input validation at a trust boundary, error handling that would leave a
key reported as held, the integer-overflow guards in `int_op`, or anything a request named
explicitly.

## Hard constraints

- **A syntax error produces no output.** The lexer/parser run to completion (or
  `--check`) before a single key is pressed; a syntax error exits 2 having typed nothing. This
  is the safety guarantee the dry-run and `--check` rest on; never let a partially-parsed
  script reach the output module.
- **The clock is absolute-deadline.** `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)` on a
  running deadline (05 §5). Never regress it to a relative `nanosleep`; relative sleeps
  accumulate wake-up latency and the drift test will fail.
- **Values are `int64`, `bool`, `str` only, and conditions must be `bool`.** No float, no
  truthiness, no implicit conversions except string `+`. Every integer UB case lives in
  `int_op`; add new ones there, not at call sites.
- **Portable to 32-bit.** Values are `int64_t`, lengths `size_t`, never `long`/`int`; print
  with `PRId64`/`%zu`; keep `tv_nsec` arithmetic in range. The checklist is 07 §4.
- **USB Arsenal owns the gadget.** Quacky opens an existing `/dev/hidgX` and writes keyboard
  reports; it never composes, binds, or tears down a USB gadget, and never touches identity,
  storage, LEDs or buttons. In `--hid` mode fail loudly if the node is missing or a write
  fails; do not try to recover the gadget.

## Commits

- Prefix the subject: `feat:`, `fix:`, `docs:`, `refactor:`, or a subsystem (`lex:`, `parse:`,
  `eval:`, `value:`, `output:`, `keys:`, `build:`, `tests:`).
- Keep the subject under ~72 characters and in the imperative. Explain *why* in the body when
  it is not obvious.
- Sign off commits (`git commit -s`).

## Documentation

- The design lives in `docs/developer/` as nine numbered docs; keep code and blueprint in sync,
  and record a decision there when you make it, not only in a commit.
- The README stays lean and claims only what is real: at the blueprint stage it describes the
  design, not a shipped binary.
