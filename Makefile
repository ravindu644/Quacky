# Quacky build. Bare `make` prints help. Details: docs/developer/07-build-and-cross.md.
# SRC is a wildcard, so dropping a new .c into src/ builds it with no edit here.
# There is no header-dependency tracking, so run `make clean` after editing a header.

TC      := $(HOME)/toolchains
TARGETS := x86_64-linux-musl i686-linux-musl aarch64-linux-musl arm-linux-musleabihf riscv64-linux-musl
SRC     := $(wildcard src/*.c)
HDR     := $(wildcard src/*.h)

CSTD    := -std=c11 -D_POSIX_C_SOURCE=200809L
COPT    := -O2 -ffunction-sections -fdata-sections
CWARN   := -Wall -Wextra -Wshadow -Wconversion -Wvla -Werror
LDFLAGS := -static -s -Wl,--gc-sections

.DEFAULT_GOAL := help
.PHONY: help all native clean

help:
	@echo 'Quacky build targets:'
	@echo '  make all       cross-compile the five static musl binaries into bin/'
	@echo '  make native    host build with ASan+UBSan for tests (bin/quacky)'
	@echo '  make clean     remove bin/'

all: $(TARGETS:%=bin/quacky-%)

bin/quacky-%: $(SRC) $(HDR) | bin
	$(TC)/$*-cross/bin/$*-gcc $(CSTD) $(COPT) $(CWARN) $(SRC) $(LDFLAGS) -o $@

native: $(SRC) $(HDR) | bin
	gcc $(CSTD) -O0 -g -fsanitize=address,undefined $(CWARN) $(SRC) -o bin/quacky

bin:
	mkdir -p bin

clean:
	rm -rf bin
