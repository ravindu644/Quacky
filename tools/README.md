# tools

Small companions to the interpreter. Not part of the `quacky` build (`src/*.c`), so they do not
ship in the static binaries.

## qtype

A raw-terminal typing-speed meter: the console answer to a browser typing test. A browser page
re-renders on every keystroke, so it measures the browser, not the keyboard. `qtype` puts the
terminal in raw mode and just drains stdin as fast as the kernel delivers it, timing the first
byte to the last, with no echo during capture. That makes it a fair stopwatch for a USB HID
gadget typing at full tilt.

```sh
make qtype                      # or: cc -O2 -Wall -Wextra -o tools/qtype tools/qtype.c
./tools/qtype passage.txt       # focus this terminal, then let the gadget type into it
```

It reads the target text, waits for input, and when typing stops it prints characters read,
accuracy against the target, elapsed time, chars/sec, and net/raw WPM. Feed the same text to
Quacky (`type <the passage>\c` on one line) over `--hid`, focus the terminal, and read the number.

Measured this way on a phone over USB HID into a Fedora host: 5490 WPM at the default rollover-off
typing, 9704 WPM with `--fast`, both at 100% accuracy. A browser capped at ~2444 on the same run,
which is what prompted this tool.
