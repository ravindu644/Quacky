# 09 · References

Quacky borrows ideas from a handful of open interpreters and the Linux timing and standards docs.
This page records what came from where, so you can read the originals yourself. Each reference
points into the named project's own source tree; clone it with `git clone --depth 1 <url>` to follow
along. The line numbers are a snapshot taken while Quacky was written and may have drifted as those
projects changed, so treat them as a starting point, not an exact address.

## Cloned projects

### craftinginterpreters (clox) — github.com/munificent/craftinginterpreters
| What we took | Where |
|---|---|
| Scanner struct `{start, current, line}` and tokens as source slices | `c/scanner.c:8-12`, `:65-72` |
| Error token convention | `c/scanner.c:75-82` |
| `skipWhitespace` with newline counting and line comments | `c/scanner.c:85-114` |
| Keyword matching by length + `memcmp` | `c/scanner.c:117-168` |
| Unterminated string detection | `c/scanner.c:191-202` |
| `scanToken` dispatch shape | `c/scanner.c:205-255` |
| Precedence enum | `c/compiler.c:36-47` |
| Pratt `rules[]` table (prefix, infix, precedence) | `c/compiler.c:862-977` |
| `parsePrecedence` loop | `c/compiler.c:980-1002` |
| `resolveLocal` (the slot-resolver we chose **not** to build) | `c/compiler.c:426-432` |
| `ObjString` layout and `takeString`/`copyString` ownership split | `c/object.h:123-128`, `c/object.c:146-169` |
| `runtimeError` with line number | `c/vm.c:46-64`; result enum `c/vm.h:85-88` |

### picol — github.com/antirez/picol
| What we took | Where |
|---|---|
| Status enum `OK, ERR, RETURN, BREAK, CONTINUE` | `picol.c:63` |
| Recursion limit as a runtime error | `picol.c:61`, `:389-393` |
| Linked-list variables and call frames | `picol.c:84-87`, `:102-105`, `:322-330` |
| Newline **and** `;` both end a command | `picol.c:131-141` |
| `$name` scanning, lone `$` is literal | `picol.c:173-191` |
| `#` comment only at command start (we differ: `#` anywhere except raw/strings) | `picol.c:262-267`, `:289-294` |
| Status propagation out of the eval loop | `picol.c:457-458`, `:484-487` |
| `while` consuming BREAK/CONTINUE | `picol.c:657-668` |
| Frame push on proc call (dynamic scope, which we deliberately avoid) | `picol.c:707-714` |
| Abort-on-OOM allocators | `picol.c:39-55` |

### chibicc — github.com/rui314/chibicc
| What we took | Where |
|---|---|
| `Token` with `loc`, `len`, `line_no`, `at_bol`, `has_space` | `chibicc.h:74-91` |
| `verror_at`: print `file:line:`, the source line, caret | `tokenize.c:28-50` |
| `error_at` computing the line by scanning (we precompute instead) | `tokenize.c:52-60` |
| `new_token` setting `at_bol` | `tokenize.c:100-112` |
| `read_escaped_char`: octal, hex, named escapes, `\e` | `tokenize.c:180-231` |
| Unclosed string literal check | `tokenize.c:234-240` |
| Main lexer loop: line/block comments, newline sets `at_bol` | `tokenize.c:490-533` |
| CRLF normalisation before lexing | `tokenize.c:691` |
| Directive consumes to end of line via `at_bol` (model for `type`) | `preprocess.c:76-78`, `:82-89`, `:233` |

### lua — github.com/lua/lua
| What we took | Where |
|---|---|
| `lua_longjmp` chain, `LUAI_THROW`/`LUAI_TRY`, `luaD_throw`, `luaD_rawrunprotected` (the alternative we did not pick) | `ldo.c:61-65`, `:97-105`, `:125-128`, `:162-166` |
| Signed/unsigned casts for wraparound arithmetic | `llimits.h:145-155` |
| `luaV_idiv`: the `-1` and `0` special cases | `lvm.c:773-785` |
| `luaV_mod` | `lvm.c:793-805` |
| `luaV_shiftl`: shift counts outside range yield 0 | `lvm.c:825-834`, `NBITS` at `:819` |
| Integer type/format configuration pattern | `luaconf.h:482`, `:500-539` |

### rt-tests (cyclictest) — kernel.googlesource.com/pub/scm/utils/rt-tests/rt-tests
| What we took | Where |
|---|---|
| `next = now + interval`, normalised | `src/cyclictest/cyclictest.c:736-739` |
| `clock_nanosleep(clock, TIMER_ABSTIME, &next, NULL)` with EINTR as the only benign error | `src/cyclictest/cyclictest.c:777-784` |
| Advance `next += interval` after each cycle; catch-up loop when behind | `src/cyclictest/cyclictest.c:895-905` |

### musl — git.musl-libc.org/git/musl (clone is 1.2.6; toolchains ship 1.2.5)
| What we confirmed | Where |
|---|---|
| `rem` is written only for relative sleeps; `TIMER_ABSTIME` path | `src/time/clock_nanosleep.c:17-45` (line 35) |
| `write` is a bare cancellable syscall | `src/unistd/write.c:4-7` |
| `time_t`, `int64_t` are `_Int64` on every arch | `include/alltypes.h.in:12`, `:18` |
| `PRId64` is `"l"` on 64-bit, `"ll"` on 32-bit | `include/inttypes.h:25-31` |
| 32-bit arches use the time64 syscall | `arch/i386/bits/syscall.h.in:401`, `arch/arm/bits/syscall.h.in:364` |

### hid_gadget_test — github.com/aagallag/hid_gadget_test
The kernel's HID-gadget test tool, the model for reading the host's LED output report back from the
same `/dev/hidgN` node (the `capslock_on` / `numlock_on` / `scrolllock_on` reads).
| What we took | Where |
|---|---|
| Open the node `O_RDWR`, not write-only, so the device is readable | `hid_gadget_test.c:264` |
| `select()`/`poll()` the fd, then `read()` the host's output report | `hid_gadget_test.c:271-291` |

### flipper_suite — github.com/barkandbite/flipper_suite
`flipperpwn/os_detect.c`, the model for `host_os`: tell the OS apart by which lock-key LEDs the host
reflects, waiting for the echo rather than a fixed pause.
| What we took | Where |
|---|---|
| Toggle a lock key, wait for the host to echo the LED change, with a timeout | `flipperpwn/os_detect.c:24-50` |
| The heuristic: no NumLock echo -> macOS; ScrollLock echo -> Windows, else Linux; no CapsLock echo -> inactive | `flipperpwn/os_detect.c:82-131` |
| Fixed 1500 ms poll timeout (we calibrate from the measured CapsLock echo instead) | `flipperpwn/os_detect.c:14` |
| CDC-serial fallback (**not** taken: Quacky is keyboard-only) | `flipperpwn/os_detect.c:210-454` |

### SparkType — github.com/Yyax13/SparkType
`src/infra/OS/main.cpp`, a second take on the same LED-reflection idea that confirmed the macOS
discriminator.
| What we took | Where |
|---|---|
| macOS told apart by no LED reply to a lock-key toggle | `src/infra/OS/main.cpp:11-40` |
| Win-vs-Linux by counting USB host-config requests (**not** taken: not visible through `/dev/hidgN`; we use flipper's ScrollLock test) | `src/infra/OS/main.cpp:25-31` |

## Read online, not cloned
- **wren** `src/vm/wren_compiler.c`, `readString` / `TOKEN_INTERPOLATION`, `MAX_INTERPOLATION_NESTING 8`, paren counting in `nextToken`: the model for `${expr}` interpolation. https://github.com/wren-lang/wren/blob/main/src/vm/wren_compiler.c

## HID report layout and key usages
The 8-byte boot-keyboard report (`[modifier, reserved, k1..k6]`), the modifier bitmap bits
(ctrl 0x01, shift 0x02, alt 0x04, GUI 0x08, right-alt 0x40) and every key usage in 05 §4 come
straight from the public USB standards, not from any third-party tool:
- **USB HID Usage Tables**, Usage Page 0x07 (Keyboard/Keypad): the usage numbers for letters,
  digits, named keys and the keypad. https://usb.org/document-library/hid-usage-tables-15
- **Device Class Definition for HID 1.11**, Appendix B.1 (the boot-protocol keyboard report
  descriptor): the 8-byte report shape and the press-then-release convention.
  https://usb.org/document-library/device-class-definition-hid-111
- **Linux USB gadget / f_hid** (`/dev/hidgN`): a report is delivered by `write(2)`ing the
  descriptor's report length to the node; the host LED output report is read back for the
  `capslock_on` / `numlock_on` / `scrolllock_on` reads (05). The kernel's `hid_gadget_test`
  (`select`/`read` on the same fd) is the pattern. Documentation/usb/gadget_hid.rst in the kernel
  tree.

## Man pages and documents consulted
- `clock_nanosleep(2)`: https://man7.org/linux/man-pages/man2/clock_nanosleep.2.html — EINTR with `TIMER_ABSTIME`, no `rem`, never restarted by `SA_RESTART`, past deadline returns immediately, absolute timers avoid drift.
- `write(2)`: https://man7.org/linux/man-pages/man2/write.2.html — partial writes on pipes after signal, `O_APPEND` atomic seek+write, `EPIPE`/`SIGPIPE`.
- `echo(1)`: https://man7.org/linux/man-pages/man1/echo.1.html — escape list including `\c`, `\e`, `\0NNN`, `\xHH`; default trailing newline.
- musl time64: https://musl.libc.org/time64.html — 64-bit `time_t` on 32-bit arches since 1.2.0.
- GCC 9.4 overflow builtins: https://gcc.gnu.org/onlinedocs/gcc-9.4.0/gcc/Integer-Overflow-Builtins.html
- C operator precedence: https://en.cppreference.com/w/c/language/operator_precedence
- musl static linking and `dlopen`: https://www.openwall.com/lists/musl/2012/12/08/4 (no dynamic loading in static binaries; irrelevant to this project but recorded).

## Dropped candidates and why
| Project | Reason |
|---|---|
| wren (clone) | bytecode VM overlaps clox; only the interpolation lexer was useful, read online |
| mruby, quickjs, tcc | 10–100× the scope, nothing extractable that the above do not cover |
| bash `builtins/echo.def` | bash word-splits before `echo` sees arguments; does not model "rest of line raw" |
| `pv`, typing-simulator tools | relative `usleep` loops, worse than the cyclictest pattern |
