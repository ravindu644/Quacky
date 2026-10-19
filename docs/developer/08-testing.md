# 08 · Testing

Three layers, all driven by one shell script so there is no test framework dependency.

## 1. Golden-file tests (correctness)

```
tests/
  cases/
    arith.qk          arith.out
    strings.qk        strings.out
    control.qk        control.out
    funcs.qk          funcs.out
    type_escapes.qk   type_escapes.out
    errors/
      undef_var.qk    undef_var.err   (expected stderr, exit code in first line)
      div_zero.qk     ...
  run.sh
```

`run.sh`: for every `cases/*.qk`, run `quacky -d 0 -o $tmp case.qk`, `cmp $tmp case.out`.
For `errors/*.qk`, compare stderr and exit code. `-d 0` disables the tick so a 2 000-character
expected file finishes instantly.

Required coverage (one case each, mapped to the spec):

| Case | Checks |
|---|---|
| `arith` | every row of spec §4.2: wrap, `INT64_MIN / -1`, `% -1`, shift ≥ 64, negative shift, arithmetic `>>` |
| `precedence` | `1 + 2 * 3`, `a & b == 0` behaves as C, `!true \|\| true`, grouping |
| `strings` | concat with int and bool, `<` ordering, equality across types is `false` |
| `control` | `if/elif/else`, nested `while`, `break`/`continue`, `for` inclusive and empty range |
| `funcs` | forward call (hoisting), recursion (`fib 20`), arity error, no-value error, global write from function, const write error |
| `type_escapes` | `\n \t \\ \$ \xHH \0NNN \c`, `$var`, `${expr}`, lone `$`, `#` inside line, empty `type`, UTF-8 string with multi-byte characters |
| `comments` | `#` line, `#[ ]#` block across lines, `#` inside string literal kept |
| `errors/*` | one file per catalogue entry in 06 §5, plus syntax errors: `break` outside loop, unterminated string, unclosed `#[`, unbalanced `${` |

## 2. Timing test (the feature)

```sh
start=$(date +%s%N)
quacky -d 20 -o /dev/null tests/timing_100chars.qk    # script types exactly 100 characters + newline
elapsed_ms=$(( ($(date +%s%N) - start) / 1000000 ))
# 100 chars → 99 gaps (first char is immediate) + 1 newline gap = 100 ticks = 2000 ms
[ $elapsed_ms -ge 1990 ] && [ $elapsed_ms -le 2150 ]
```

Tolerance: lower bound tight (we must never be fast), upper bound +150 ms for scheduler noise
on a loaded CI box. Run on the **native** build only; under qemu-user timing is meaningless.

Second timing case: `sleep 500` then `type x` must land `x` at 500 ms ± 20, not 520 (05 §5).

Drift test: 500 characters at `-d 10` must finish within 5 000 ms + 60 ms. A relative-sleep
implementation accumulates ~50–100 µs per character and fails this; the absolute-deadline one
passes with margin.

## 3. Cross-architecture runs

`qemu-user-static` is **not installed** on this machine (checked). Install on Fedora:

```sh
sudo dnf install qemu-user-static        # registers binfmt_misc, so bin/quacky-aarch64-linux-musl runs directly
```

Then `run.sh` loops over all five binaries for the golden tests (not the timing tests). The
static musl binaries need no sysroot, which is the whole point of `-static`. Priority: i686 and
armhf, because they are the 32-bit targets where `long`/`size_t` bugs surface.

## 4. Sanitizers and fuzzing

- `make native` (07 §2) builds with ASan + UBSan; run the golden suite under it. UBSan is the proof that `int_op` has no signed overflow.
- Cheap fuzzing without tooling: a 30-line script that mutates `tests/cases/*.qk` bytes randomly and asserts the interpreter exits 0, 1 or 2 and never crashes (exit ≥ 128). Run with `-d 0` and a `timeout 2` wrapper (a mutated `while true` loop is expected and fine).
- `afl++`/`libFuzzer` are upgrades; the entry point is `parse(const char *src, size_t len)`, which is already a pure function.

## 5. What is deliberately not tested
- Behaviour when the output file disappears mid-run (`O_APPEND` to an unlinked inode keeps working; no spec).
- Timing under `qemu`.
- Memory leaks beyond what ASan reports on the golden suite; the arena is freed at exit and refcounts are exercised by `strings.qk` loops.
