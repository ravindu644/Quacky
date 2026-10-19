# Quacky

Quacky is a tiny scripting language, and a single static C11 interpreter for it, that reads a
human-readable script and writes USB HID keyboard reports to `/dev/hidgX`. The computer on the
other end of the cable sees an ordinary keyboard typing text and key combinations at supersonic
speed. The gadget node itself is provided by the companion
[USB Arsenal](https://github.com/ravindu644/USB-Arsenal) project, which owns the USB side
(configfs, the controller, the device identity); Quacky only speaks keyboard.

It is an educational, free and open-source project: a learning-friendly, no-cost alternative to
expensive commercial typing-automation hardware, and a small, readable interpreter (lexer,
Pratt parser, tree-walker, ~3 200 lines) to study.

> The blueprint for the implementation lives in [`docs/developer/`](docs/developer/). No
> interpreter code exists yet; this repository is the design.

## The language in one look

```
# open a run dialog on the host and type a line
delay 50                 # from here on, pause 50 ms after every step
super + r                # a chord: keys joined with + are pressed together
sleep 400                # a one-shot pause
type notepad
enter
sleep 600
type Hello from Quacky!
enter
hold shift               # keep a key down across the next lines
home
release shift            # (selects the line we just typed)
```

- **`type`** writes literal text, with `$name` / `${expr}` interpolation.
- **Chords** are key words joined with `+` and pressed together: `enter`, `ctrl + c`,
  `ctrl + alt + delete`, `super + r`.
- **`hold` / `release`** keep keys down across later lines; **`delay`** sets a default pause,
  **`sleep`** pauses once, **`repeat N`** re-runs the previous action.
- Variables, `const`, integer/boolean/string expressions, `if/elif/else/fi`, `while`, `for`,
  and `func` round out a small shell-and-Python-flavoured language with no indentation.

Full specification: [`docs/developer/01-language-spec.md`](docs/developer/01-language-spec.md).

## Running a script

```sh
quacky script.qk                 # dry run: print a readable transcript of what it would do
quacky --check script.qk         # parse only; report any syntax errors, type nothing
quacky --hid /dev/hidg0 script.qk  # the real thing: write HID reports to the keyboard node
quacky -o out.txt script.qk      # transcript to a file
quacky -d 0 script.qk            # set the per-key tick in ms (default 20; 0 = as fast as possible)
```

The transcript backend lets you rehearse a script safely before any key is pressed. A syntax
error produces **no** output at all.

## Status

Design stage. The interpreter targets x86_64, i686, aarch64, armhf and riscv64 as static musl
binaries, 60–130 KB, with no dependency beyond libc. See
[`docs/developer/00-overview.md`](docs/developer/00-overview.md) for the pipeline and
[`AGENTS.md`](AGENTS.md) for house rules.

Durability note: output is written in order to the kernel; Quacky does not call `fsync`, so the
guarantee is "the bytes reached the kernel", not "landed on disk".

## License

See the repository's license file.
