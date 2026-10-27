# Quacky

Quacky is a tiny scripting language, and a single static C11 interpreter for it, that reads a
human-readable script and types it on another computer over USB. You write what to type and which
keys to press; Quacky turns that into USB HID keyboard reports, and the machine on the other end of
the cable sees an ordinary keyboard typing text and key combinations, very fast.

**One static binary, around 60 to 130 KB, with no dependency beyond libc**, and about 3,800 lines of
C you can read in an afternoon: a lexer, a Pratt parser, a tree-walking evaluator, and one module
that talks to the keyboard.

## Why

Quacky is a free, open-source replacement for commercial keystroke injection tools, built for
educational and experimental purposes only. Those sell for well
over $100, yet the microcontroller inside one costs under $10, and you do not need the gadget at all.
The Linux kernel has shipped USB HID gadget support
through configfs since 2015, and the phone already in your pocket is a full USB device: every Android
phone runs that kernel, and its USB-C port is dual-role, so it can present itself to a computer as an
ordinary keyboard. So why buy the hardware? Quacky is the software for it, its own language and its
own interpreter, portable across architectures, running on what you already own. The USB side is
handled by the companion [USB Arsenal](https://github.com/ravindu644/USB-Arsenal) project; Quacky
only speaks keyboard.

> [!CAUTION]
> Quacky types into whatever computer the cable is plugged into, as fast as a keyboard can go. Use it
> only on systems you own or are explicitly authorized to test. Keystroke injection against someone
> else's machine is illegal in most places.

## What you get

- **A readable interpreter to study.** A lexer, Pratt parser, tree-walker and output module, small
  enough to read end to end, with a design guide in [`docs/developer/`](docs/developer/).
- **Rehearse before anything types.** A dry run prints a readable transcript of what the keyboard
  would do, and `--check` validates a script without pressing a key. A syntax error types nothing.
- **A real little language.** Integers, booleans and strings; `if`/`while`/`for`; functions;
  variables and `const`; string interpolation inside typed text.
- **Keyboard control, not just text.** Chords like `ctrl + alt + delete`, keys held down across
  lines, repeats, and adjustable pacing.
- **Stay in control of the run.** `echo` prints progress to your terminal (never to the host), a
  real run mirrors its keystrokes to the terminal live, and `exit` stops a script cleanly when a
  branch decides it should not go on.
- **Read the host back.** `capslock_on` / `numlock_on` / `scrolllock_on` report the host's lock-key
  LEDs, and `host_os` detects Windows, macOS or Linux, so a script can branch on the live machine.
- **Type a whole file.** `type_file "notes.txt"` types a document verbatim, no escaping; a literal
  path is validated by `--check` before anything types.
- **Portable and dependency-free.** Static musl binaries for x86_64, i686, aarch64, armhf and
  riscv64, each tens of kilobytes, needing nothing but libc.

## The language in one look

```
delay 50                 # from here on, pause 50 ms after every step
super + r                # a chord: keys joined with + are pressed together
sleep 400
typeln notepad           # typeln types the line and presses Enter, launching it
sleep 600
typeln Hello from Quacky!
```

- **`type`** writes literal text (no Enter); **`typeln`** adds the Enter. Both take `$name` and
  `${expr}` interpolation.
- **Chords** are keys joined with `+` and pressed together: `enter`, `ctrl + c`, `super + r`.
- **`hold` / `release`** keep keys down across later lines; **`delay`** sets a default pause,
  **`sleep`** pauses once, **`repeat N`** re-runs the previous action.
- **`echo`** prints a message to your terminal (not the host), and **`exit`** stops the run, so a
  helper can report and bail instead of typing into the wrong place.

A full tour is in [docs/language.md](docs/language.md).

## Get started

```sh
make native                              # build output/quacky for this machine
./output/quacky script.qk                # dry run: print a transcript, type nothing
./output/quacky --check script.qk        # parse only, report syntax errors
./output/quacky --hid /dev/hidg0 script.qk  # type for real over USB
```

The `/dev/hidg0` node comes from [USB Arsenal](https://github.com/ravindu644/USB-Arsenal); you do
not need it for a dry run. See [docs/getting-started.md](docs/getting-started.md) for the full walk
through.

> [!NOTE]
> A plain run is a dry run: it prints a transcript and types nothing. Real typing happens only in
> `--hid /dev/hidgX` mode. A syntax error types nothing either, so you can rehearse safely.

> [!WARNING]
> In `--hid` mode there is no undo. A script that opens a terminal and runs a command does exactly
> that on the host, at full speed. Rehearse with a dry run and `--check` before you plug in.

## Measured speed

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/poster-dark.webp">
    <img alt="Quacky speed lab: net WPM across USB poll intervals, the -d pacing range, burst versus sustained, clock accuracy, and the --fast rollover" src="docs/assets/poster-light.webp" width="900">
  </picture>
</p>

Typed over USB HID into a desktop host at 100% accuracy on every run. The `qtype` meter, the method
and the raw sweep tables live in [`tools/README.md`](tools/README.md).

## Documentation

| Page | What it covers |
|---|---|
| [Getting started](docs/getting-started.md) | Build, run modes, the full command-line and exit-code reference. |
| [The language](docs/language.md) | Every feature, with a runnable example for each. |
| [Keys and chords](docs/keys.md) | Modifiers, named keys, chords, and holding keys down. |
| [Examples](docs/examples.md) | Worked scripts from a one-liner to a complete routine. |
| [Developer docs](docs/developer/) | How the interpreter is built, module by module. |

## Credits

The USB gadget side, the `/dev/hidgN` node Quacky writes to, is provided by
[USB Arsenal](https://github.com/ravindu644/USB-Arsenal).

## License

[GNU General Public License v3.0](./LICENSE). Copyright (C) 2026
[ravindu644](https://github.com/ravindu644).
