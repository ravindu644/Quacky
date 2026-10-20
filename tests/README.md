# Tests

Golden-file tests driven by one POSIX shell script, no framework (08).

```sh
make debug-hardened  # build output/quacky-hardened with ASan + UBSan
tests/run.sh         # run everything
```

- `cases/*.qk` with a `.out` are transcript goldens: `run.sh` runs each with `-d 0 -o`
  and compares byte for byte, and also `--check`s it clean.
- `cases/keys.hid` is the captured 8-byte report stream: `run.sh` replays the case with
  `--hid` to a regular file and compares, so both backends are checked from one script.
- `cases/errors/*.qk` expect a diagnostic: the matching `.err` holds the exit code on line 1
  and the exact stderr after it. Syntax cases are also `--check`ed (exit 2, no output).
- `cases/errors_hid/*.qk` are runtime errors that only fire in the HID backend (an unmappable
  `type` character), so they run with `--hid`.
- Timing (the 100-character cadence, `sleep` landing on the deadline, the 500-character drift
  test) and a byte-mutation fuzzer run last; set `QK_SKIP_TIMING=1` to omit them.

`QUACKY` overrides the binary, so the same cases run on another architecture:

```sh
# on a phone or board over adb, against a static cross build
QUACKY=/data/local/tmp/quacky-aarch64 QK_SKIP_TIMING=1 sh run.sh
```

Timing measurements use the `-O2` x86_64 build from `make all-build` when present, since the ASan
build's start-up cost would blur the bounds.
