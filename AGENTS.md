# AGENTS.md

Instructions for AI coding agents working in this repository. Read
[docs/developer/00-overview.md](docs/developer/00-overview.md) for how the pieces fit together and
the document map for the rest. Humans should start with the [README](README.md) and the user docs
under [docs/](docs/). This file holds the house rules and the facts you must not relearn the hard
way.

## What this is

Quacky is a tiny scripting language and a single static C11 interpreter for it. A script is
human-readable (shell-and-Python-flavoured, no indentation); the interpreter reads it and writes USB
HID keyboard reports to `/dev/hidgX`, so the host computer sees a keyboard typing text and key
combinations very fast. The gadget node comes from the companion **USB Arsenal** project, which owns
the USB side (configfs, the controller, the identity); Quacky only speaks keyboard, through two
output backends: real HID reports, or a readable transcript for safe dry-runs and the test suite.

This is an educational, free and open-source project: a learning-friendly, no-cost alternative to
expensive commercial typing-automation hardware, and a small interpreter to study.

## Build

Static musl builds for every target plus a host sanitizer build, all into `output/`. Bare `make`
prints help. Details are in
[docs/developer/07-build-and-cross.md](docs/developer/07-build-and-cross.md).

- Toolchains are `~/toolchains/<triple>-cross/bin/<triple>-gcc`, all musl 1.2.5, for `x86_64`,
  `i686`, `aarch64`, `arm-musleabihf` and `riscv64`. `find-cc` also honours `MUSL_CROSS=<dir>`.
- Per-arch targets `make native | x86_64 | aarch64 | armhf | x86 | riscv64` build one static binary
  into `output/`. `make all-build` builds all five; `make tarball` / `make all-tarball` package them;
  `make module` zips the phone archs into a KernelSU/Magisk module; `make qtype` cross-builds the
  `tools/qtype` meter for every arch into a tarball. `make clean` wipes `output/`.
- **`make debug-hardened`** is the one that builds with ASan+UBSan (host glibc, `output/quacky-hardened`),
  and the tests run it. `make native` is a plain static build, not instrumented.
- Logs are kernel-style (`CC`/`LD`/`STRIP`); `V=1` shows full commands.
- The version lives only in `src/include/quacky.h` (`QUACKY_VERSION`); `make bump NEW=x.y.z` rewrites
  it and `module/module.prop`.
- `SRC` is a `wildcard src/*.c`, so a new `.c` in `src/` is picked up with no Makefile edit. Headers
  live in `src/include/` and are found through `-Isrc/include`; there is no header-dependency
  tracking, so run `make clean` after editing a header.
- Flags are pinned (`-std=c11`, `-Werror`, `-Wconversion`, `-static`). Do not loosen them to silence
  a warning; fix the code. `-Wconversion` is what catches 32-bit narrowing before an armhf box does
  at 3 a.m.

## Layout

| Path | What lives there |
|---|---|
| `src/main.c` | CLI (`--hid DEV`, `-o FILE`, `--check`, `-d MS`, `--fast`), read the file, run, exit codes |
| `src/lex.c` | tokens, `type` raw mode, chords |
| `src/parse.c` | recursive-descent statements, Pratt expressions, the arena |
| `src/eval.c` | the tree-walker: frames, status codes, calls, keystroke statements |
| `src/value.c` | the `Value` union, refcounted strings, `to_string`, `int_op` |
| `src/output.c` | the one place that touches the OS for output: fd, backends, clock, HID reports, held-key set |
| `src/keys.c` | the single source of truth for key names and the US char to (modifier, usage) table |
| `src/include/*.h` | the module headers, one per `src/` unit, on the build's `-Isrc/include` path |
| `module/` | the KernelSU/Magisk module assets (`module.prop`, `customize.sh`, `uninstall.sh`) `make module` zips |
| `docs/developer/` | the design guide, nine numbered docs |
| `docs/` | the end-user documentation (language, keys, examples, getting started) |
| `tests/` | `cases/*.qk` with `.out` (transcript) and `.hid` (report bytes), plus `run.sh` |
| `output/` | all build artifacts (gitignored) |

One fact worth keeping: `output.c` and `keys.c` are the only modules that know the HID wire format.
Everything above them deals in characters, chords and key names, never bytes.

## Style

Four rules. They apply to code, comments, commit messages, and documentation.

**No em-dashes.** Use a comma, a full stop, or rewrite the sentence.

**No ASCII banner comments.** No rows of `-----`, no `=====`, no boxed section headers. An SPDX
licence block at the top of each file is fine.

**Comments sound like a person.** Say why the code does something, or what breaks if it does not.
Skip comments that restate the line below them.

```c
/* Bad: write the report to the fd */
write(fd, report, 8);

/* Good: the host sees a key as held until we send the release; without this it auto-repeats */
write(fd, release, 8);
```

**Ten lines that work beat a hundred that do the same thing.** Delete before you add. A smaller diff
in the right place is the goal, not a smaller diff anywhere.

## Framing

Describe the project as what it is: an educational, free and open-source interpreter that reads a
script and writes HID keyboard reports to `/dev/hidg0`, typing text and key combinations for the
host. The target computer is "the host" or "the target computer". Keep the wording factual and
neutral in code, comments, docs and commits; let the engineering speak for itself.

## Before you write it at all

Work down this list and stop at the first answer that holds.

1. Does this need to exist? A flag nobody asked for, a knob for a value that never changes, an
   abstraction with one implementation: skip it and say so in one line.
2. Does it already exist here? Grep first. The key tables in `keys.c`, the clock in `output.c`,
   `int_op` in `value.c` are each meant to be the one place their job is done.
3. Does libc or the OS already do it? `clock_nanosleep` paces the output; `write(2)` delivers it;
   `strtoull` parses numbers. Do not hand-roll what the standard library and the kernel provide.
4. Can it be one line? Then it is one line.
5. Only then write the smallest thing that works.

Two answers work? Take the higher one and move on. The list shortens the solution, never the
reading: trace the flow a change touches before you pick a rung. A small diff in the wrong place is
a second bug, not a lazy fix.

Never simplify away: input validation at a trust boundary, error handling that would leave a key
reported as held, the integer-overflow guards in `int_op`, or anything a request named explicitly.

## Reuse before you write

These are the choke points. Route through them; bypassing one is a bug, not a shortcut.

- **Key names and characters** go through the tables in `src/keys.c`. One source of truth for every
  key name and every US-layout character to (modifier, usage) mapping. Do not scatter key lookups.
- **Timing and output** go through `src/output.c`: the absolute-deadline clock, the held-key set,
  the two backends behind one interface. Nothing else writes to the device or sleeps.
- **Integer arithmetic** goes through `int_op` in `src/value.c`, where every overflow and
  divide-by-zero case is handled once. Add a new integer operation there, not at a call site.

## Hard constraints

- **A syntax error produces no output.** The lexer and parser run to completion (or `--check`)
  before a single key is pressed; a syntax error exits 2 having typed nothing. This is the safety
  guarantee the dry-run and `--check` rest on; never let a partially-parsed script reach the output
  module.
- **The clock is absolute-deadline.** `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)` on a running
  deadline (05 §5). Never regress it to a relative `nanosleep`; relative sleeps accumulate wake-up
  latency and the drift test will fail.
- **Values are `int64`, `bool`, `str` only, and conditions must be `bool`.** No float, no
  truthiness, no implicit conversions except string `+`. Every integer UB case lives in `int_op`;
  add new ones there, not at call sites.
- **Portable to 32-bit.** Values are `int64_t`, lengths `size_t`, never `long`/`int`; print with
  `PRId64`/`%zu`; keep `tv_nsec` arithmetic in range. The checklist is 07 §4.
- **USB Arsenal owns the gadget.** Quacky opens an existing `/dev/hidgX` and writes keyboard
  reports; it never composes, binds, or tears down a USB gadget, and never touches identity,
  storage, LEDs or buttons. In `--hid` mode fail loudly if the node is missing or a write fails; do
  not try to recover the gadget.

## Commits

- Prefix the subject: `feat:`, `fix:`, `docs:`, `refactor:`, or a subsystem (`lex:`, `parse:`,
  `eval:`, `value:`, `output:`, `keys:`, `build:`, `tests:`).
- Keep the subject under ~72 characters and in the imperative. Explain *why* in the body when it is
  not obvious.
- Sign off commits (`git commit -s`).

## Documentation

- The end-user docs live in [docs/](docs/); the design guide lives in
  [docs/developer/](docs/developer/) as nine numbered docs. Keep code and docs in sync, and record a
  decision in the design guide when you make it, not only in a commit.
- The README stays lean and claims only what is real.
