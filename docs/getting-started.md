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
make            # cross-compile the static release binaries into bin/
make native     # build one binary for this machine (with sanitizers), into bin/quacky
make clean      # remove bin/
```

For trying things out, `make native` is the quickest. It produces `bin/quacky`. Run `make clean`
after editing a header, since the build does not track header dependencies.

## Your first script

Put this in `hello.qk`:

```
type Hello from Quacky!
type Typed by a script.
```

Each `type` ends its line by pressing Enter for you, so these two lines land one above the other.

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
| `--help`, `-h` | Print usage and exit. |

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
