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
LDFLAGS := -static -s -Wl,--gc-sections
```

- `-std=c11`, not `gnu11`: keeps the three GCC 9.4 targets honest. `_Static_assert`, `_Alignof`, anonymous unions, flexible array members are all C11.
- `_POSIX_C_SOURCE=200809L` exposes `clock_nanosleep`, `CLOCK_MONOTONIC`, `TIMER_ABSTIME`, `sigaction`. Without it `-std=c11` hides them.
- `-Wconversion` is what catches `size_t` → `int` and `int64_t` → `long` narrowing on the 32-bit targets at compile time, instead of at 3 a.m. on an armhf box.
- `-static` with musl has none of glibc's static pitfalls: no NSS/`dlopen` for name lookup (we do none anyway), no locale files needed. `dlopen` itself is unavailable in a static musl binary, which is irrelevant here.
- Debug build: `-O0 -g -fsanitize=address,undefined` on the **native** toolchain only. UBSan is the cheap way to prove the `int_op` wraparound claims in 04 §4.

## 3. Makefile skeleton

```make
TC      := $(HOME)/toolchains
TARGETS := x86_64-linux-musl i686-linux-musl aarch64-linux-musl arm-linux-musleabihf riscv64-linux-musl
SRC     := $(wildcard src/*.c)

all: $(TARGETS:%=bin/quacky-%)

bin/quacky-%: $(SRC) src/*.h | bin
	$(TC)/$*-cross/bin/$*-gcc $(CSTD) $(COPT) $(CWARN) $(SRC) $(LDFLAGS) -o $@

bin: ; mkdir -p bin

native: $(SRC)                                   # host gcc, for tests and sanitizers
	gcc $(CSTD) -O0 -g -fsanitize=address,undefined $(CWARN) $(SRC) -o bin/quacky

clean: ; rm -rf bin
```

Whole-program compilation (all `.c` in one `gcc` invocation) is fine at this size, lets GCC
inline across files, and avoids object-file rules per target. `SRC` is a `wildcard`, so
`src/keys.c` (the key-name and US char→usage tables, 05 §4) is compiled with no Makefile
change; drop a new `.c` into `src/` and it builds.

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
for t in $TARGETS; do file bin/quacky-$t; done        # "statically linked", right arch
bin/quacky-x86_64-linux-musl -d 0 tests/*.qk          # native run
qemu-<arch>-static bin/quacky-<triple> ...             # others, see 08
```
