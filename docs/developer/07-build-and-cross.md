# 07 · Build and cross-compilation

## 1. Toolchains (already on this machine)

`~/toolchains/<triple>-cross/bin/<triple>-gcc`, musl-cross-make style, all shipping **musl 1.2.5**
(read from the `libc.a` version string).

| Triple | GCC | Static hello world | Static probe* |
|---|---|---|---|
| `x86_64-linux-musl` | 14.2.0 | 9.4 KB | 21.7 KB |
| `i686-linux-musl` | 9.4.0 | 9.1 KB | 21.4 KB |
| `aarch64-linux-musl` | 9.4.0 | 66.6 KB | 66.6 KB |
| `arm-linux-musleabihf` | 9.4.0 | 9.1 KB | 25.5 KB |
| `riscv64-linux-musl` | 14.2.0 | 9.5 KB | 30.1 KB |

\*probe = `clock_nanosleep(TIMER_ABSTIME)`, `open/write`, `__builtin_add_overflow`,
`_Static_assert`, `_Alignof(max_align_t)`, `PRId64`, compiled with the exact flags below and
`-Werror`. All five passed; the x86_64 binary ran and confirmed 64-bit `time_t`.

The aarch64 toolchain's libc is not built with section GC, which is why its floor is 66 KB.
Expected final interpreter size: 60–120 KB on four targets, ~130 KB on aarch64.

## 2. Flags

```make
CSTD    := -std=c11 -D_POSIX_C_SOURCE=200809L
COPT    := -O2 -ffunction-sections -fdata-sections
CWARN   := -Wall -Wextra -Wshadow -Wconversion -Wvla -Werror
LDFLAGS := -static -Wl,--gc-sections
```

(The binary is stripped as a separate step after linking, so `-s` is not in `LDFLAGS`;
`NO_STRIP=1` keeps the symbols.)

- `-std=c11`, not `gnu11`: keeps the three GCC 9.4 targets honest. `_Static_assert`, `_Alignof`, anonymous unions, flexible array members are all C11.
- `_POSIX_C_SOURCE=200809L` exposes `clock_nanosleep`, `CLOCK_MONOTONIC`, `TIMER_ABSTIME`, `sigaction`. Without it `-std=c11` hides them.
- `-Wconversion` is what catches `size_t` → `int` and `int64_t` → `long` narrowing on the 32-bit targets at compile time, instead of at 3 a.m. on an armhf box.
- `-static` with musl has none of glibc's static pitfalls: no NSS/`dlopen` for name lookup (we do none anyway), no locale files needed. `dlopen` itself is unavailable in a static musl binary, which is irrelevant here.
- Debug build: `make debug-hardened` compiles with the host `cc` (glibc) and
  `-O0 -g -fsanitize=address,undefined`, into `output/quacky-hardened`; the tests run it. UBSan is
  the cheap way to prove the `int_op` wraparound claims in 04 §4. It omits `-fsanitize=leak` on
  purpose: a syntax error exits without freeing the arena by design, which the leak checker would
  flag. `make native` is a plain static musl build, not instrumented.

## 3. Makefile

The build system is modelled on the companion Droidspaces and USB Arsenal projects. All output lands
in `output/`; bare `make` prints the help. `SRCS` is a `wildcard src/*.c` (a new `.c` is compiled
with no Makefile edit), and objects go under a per-arch `output/.obj/<arch>/` so builds for different
architectures do not collide. Logs are kernel-style (`CC`/`LD`/`STRIP`); `V=1` shows full commands.

A `find-cc` function locates a musl cross-compiler for a triple (checking `MUSL_CROSS`, then
`~/toolchains/<triple>-cross/bin`, then `PATH`, then `/opt/cross`), and each per-arch target invokes
a recursive `make` with that `CC`:

```make
SRCS = $(wildcard src/*.c)
INC  = -Isrc/include

$(OBJ_DIR)/%.o: src/%.c
	$(Q)$(CC) $(CSTD) $(COPT) $(CWARN) $(INC) -c $< -o $@

quacky: $(OBJS)
	$(Q)$(CC) $(OBJS) -o output/quacky $(LDFLAGS)   # then strip, unless NO_STRIP=1

aarch64:
	@CROSS_CC="$(call find-cc,aarch64-linux-musl)"; \
	$(MAKE) -j$(NPROC) quacky CC=$$CROSS_CC
```

Targets: `make native | x86_64 | aarch64 | armhf | x86 | riscv64` build one binary; `make all-build`
builds all five; `make tarball` / `make all-tarball` package them; `make module` zips the phone
archs (aarch64, armhf, x86_64, x86) with the `module/` assets into a KernelSU/Magisk zip;
`make bump NEW=x.y.z` rewrites the version; `make debug-hardened` is the sanitizer build;
`make clean` removes `output/`.

## 4. Portability checklist (32-bit is the enemy)

| Rule | Why |
|---|---|
| Values are `int64_t`; lengths are `size_t`; never `long`, never `int` for either | `long` is 32-bit on i686 and armhf |
| Print with `PRId64` / `%zu` | musl picks `"l"` vs `"ll"` per target (`include/inttypes.h:25-31`) |
| `time_t` is 64-bit on all five with musl ≥ 1.2.0 (`include/alltypes.h.in:12`); i386 and arm use the `clock_nanosleep_time64` syscall (`arch/i386/bits/syscall.h.in:401`, `arch/arm/bits/syscall.h.in:364`) | no Y2038 issue, and no `#ifdef` needed |
| `struct timespec.tv_nsec` is `long`: keep nanosecond arithmetic under 2^31 on 32-bit by normalising after every add (05 §4) | `20 ms * 100` already overflows a 32-bit `long` in ns |
| `_Static_assert(sizeof(int64_t)==8)`, `_Static_assert(sizeof(void*) >= 4)` in one header | documents the assumption, costs nothing |
| No `__int128`, no `long double`, no `alloca`, no VLAs (`-Wvla`) | not all targets, not needed |
| Alignment via `_Alignof(max_align_t)` from `<stddef.h>` | arena correctness on armhf (8-byte `int64_t` alignment with 4-byte pointers) |
| Unaligned access is UB everywhere; the arena rounds every allocation, so no packed structs | armhf traps or silently misreads |

## 5. Alternative toolchain
If the `~/toolchains` set is unavailable, `zig cc -target <triple> -static` produces identical
musl static binaries for all five triples from one download, with the same flags. Not used
here because the GCC set already exists and is pinned.

## 6. Verifying a release
```
make all-build                                        # output/quacky-<arch> for all five
for f in output/quacky-*; do file $$f; done           # "statically linked", right arch
output/quacky-x86_64 -d 0 tests/cases/*.qk            # native run
qemu-<arch>-static output/quacky-<arch> ...            # others, see 08
```
