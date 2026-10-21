# Getting started

This walks you from a fresh clone to a script that types on a real computer. You will write a tiny
Quacky program, rehearse it safely, then run it for real over USB.

## What you need

- A C toolchain (`gcc` or `clang`) and `make`, to build the interpreter.
- To type on a real host: a device that can present itself as a USB keyboard, exposing a
  `/dev/hidgN` node. That node is set up by the companion
  [USB Arsenal](https://github.com/ravindu644/USB-Arsenal) project, which owns the USB side.
  You do not need any of this for a dry run.

## Build

```sh
make            # print the list of build targets
make native     # build a static binary for this machine, into output/quacky
make all-build  # cross-compile every supported architecture into output/
make clean      # remove output/
```

For trying things out, `make native` is the quickest. It produces `output/quacky`. Run `make clean`
after editing a header, since the build does not track header dependencies. `make` on its own just
lists the targets.

The examples below write `quacky` as the command. From a local build, run it as `./output/quacky`;
installing the module (see `make module`) puts `quacky` on a phone's `PATH`.

## Your first script

Put this in `hello.qk`:

```
typeln Hello from Quacky!
typeln Typed by a script.
```

`typeln` types a line and presses Enter, so these two land one above the other. Use `type` when you
want text without a trailing Enter.

## Rehearse it (dry run)

Run the script with no flags and Quacky prints a readable transcript of what it would do, without
pressing a single key. This is the safe way to see a script before it touches a keyboard.

```sh
quacky hello.qk
```

You will see the typed text, with chords shown in brackets like `[ctrl + s]`. You can also check a
script for syntax errors without running it at all:

```sh
quacky --check hello.qk     # prints nothing and exits 0 if the script is valid
```

A syntax error always types nothing: the whole script is parsed before the first key is pressed.

## Type it for real

With a `/dev/hidgN` node available, point Quacky at it. Focus the window you want typed into, then:

```sh
quacky --hid /dev/hidg0 hello.qk
```

## Command-line reference

```
quacky [--hid DEV | -o FILE] [--check] [-d MS] [--fast] SCRIPT.qk
```

| Flag | What it does |
|---|---|
| *(none)* | Dry run: write a readable transcript to standard output. Nothing is typed. |
| `--hid DEV` | Type for real, sending keyboard reports to a `/dev/hidgN` node. Cannot be combined with `-o`. |
| `-o FILE` | Write the transcript to a file instead of standard output (appended). |
| `--check` | Parse only. Report the first syntax error, write no output, exit 0 if the script is valid. |
| `-d MS` | Milliseconds per key event. Default is 5. `0` types as fast as possible. |
| `--fast` | Roll each typed key into the next for roughly double the typing speed. Only affects `--hid`. |
| `--ultra` | Pack a run of distinct, same-shift characters into one keyboard report (up to six keys), for up to ~6x on such runs. Only affects `--hid`; takes precedence over `--fast`. See the note below. |
| `--help`, `-h` | Print usage and exit. |

`--ultra` is the fastest mode on favourable text, but it is not a free upgrade over `--fast`. A single
keyboard report carries six key slots and one modifier, and the host registers a character as each
slot's key goes down. So `--ultra` can send up to six characters in one report only when they are all
different and all need the same shift state; a repeated letter (`ll`, `ss`), a change of case, or a
shifted symbol starts a new report. On ordinary prose it lands close to `--fast`; its headline speed
shows on runs of distinct lowercase or unshifted characters. One caveat: it relies on the receiving
computer honouring the order of keys within a report, which Linux does and some other systems may not,
so prefer `--fast` when correctness on an unknown host matters more than peak speed.

## Recommended settings for everyday use

The peak-speed modes are for benchmarks. In real use the limit is the program receiving the
keystrokes, not Quacky: a browser text field falls behind around 2400 WPM, and even a native
terminal can be overrun and drop input. So the goal for a working script is not raw speed, it is
that every character lands. The default pacing is tuned for exactly that.

For most scripts, use no speed flags and the default tick:

```sh
quacky --hid /dev/hidg0 script.qk     # default mode, -d 5
```

That types at a steady, reliable cadence on any host. Reach for the knobs only when a specific
target asks for it:

| Target | Suggested flags | Why |
|---|---|---|
| General, or an unknown host | *(none)*, default `-d 5` | Full press-and-release per key, works everywhere. The safe default. |
| A target that drops characters (a VM console, remote desktop, a field that validates as you type) | `-d 15` or higher | A slower cadence gives the receiver time to keep up. Raise it until the input is clean. |
| A fast local target (a native terminal or editor on the same machine) | `--fast -d 2`, or `--fast -d 0` | `--fast` only adds speed once `-d` is no longer the limit, so pair it with a small or zero tick. |
| A benchmark on a known Linux host | `--ultra -d 0` | Peak numbers only. Not for real scripts; see the `--fast` and `--ultra` notes above. |

About the two speed flags: with the default `-d 5`, `--fast` and `--ultra` make little practical
difference, because the per-key pause, not the number of reports, sets the pace. They only pull
ahead at a very small or zero `-d`, which is also where a host is most likely to miss keys. For
everyday automation, leave them off.

Pace between actions, not only between keys:

- Start a script with a `sleep` when you need a moment to focus the right window first, for example
  `sleep 2000` to switch windows after launching.
- Add a `sleep` after an action the host needs time to react to: opening a run dialog, launching an
  app, loading a page. `type notepad\n` then `sleep 500` is far more reliable than typing into a
  window that is not ready yet.
- Use the `delay` statement (off by default) to add a fixed gap *between* the keystrokes a routine
  sends when it needs room to breathe, instead of inflating `-d`. It never pauses on assignments or
  control flow, and a `sleep` adds to it. `-d` paces individual keys; `delay` paces the steps
  between them.
- Use the `keydelay` statement to change that individual-key pacing from inside the script: it is
  the in-script `-d`, so you can type one passage slowly and the rest at full speed without touching
  the command line.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | Success, or `--check` found no errors. |
| 1 | A runtime error while the script ran (for example, divide by zero). |
| 2 | A syntax error. The script is rejected before anything is typed. |
| 64 | Command-line usage error (a bad flag or missing script). |
| 66 | Could not open the script file. |
| 73 | Could not create the `-o` output file. |
| 74 | Could not open the `--hid` device node. |

## Next steps

- [language.md](language.md): the full language, feature by feature.
- [keys.md](keys.md): every key name and how chords work.
- [examples.md](examples.md): worked scripts from a one-liner to a complete routine.
