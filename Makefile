# Quacky build. Bare `make` prints help. Details: docs/developer/07-build-and-cross.md.
# SRC is a wildcard, so dropping a new .c into src/ builds it with no edit here.
# There is no header-dependency tracking, so run `make clean` after editing a header.

TC      := $(HOME)/toolchains
TARGETS := x86_64-linux-musl i686-linux-musl aarch64-linux-musl arm-linux-musleabihf riscv64-linux-musl
SRC     := $(wildcard src/*.c)
HDR     := $(wildcard src/include/*.h)
INC     := -Isrc/include

CSTD    := -std=c11 -D_POSIX_C_SOURCE=200809L
COPT    := -O2 -ffunction-sections -fdata-sections
CWARN   := -Wall -Wextra -Wshadow -Wconversion -Wvla -Werror
LDFLAGS := -static -s -Wl,--gc-sections

.DEFAULT_GOAL := help
.PHONY: help all native qtype clean

help:
	@echo 'Quacky build targets:'
	@echo '  make all       cross-compile the five static musl binaries into bin/'
	@echo '  make native    host build with ASan+UBSan for tests (bin/quacky)'
	@echo '  make qtype     host build of the typing-speed meter (tools/qtype)'
	@echo '  make clean     remove bin/'

all: $(TARGETS:%=bin/quacky-%)

bin/quacky-%: $(SRC) $(HDR) | bin
	$(TC)/$*-cross/bin/$*-gcc $(CSTD) $(COPT) $(CWARN) $(INC) $(SRC) $(LDFLAGS) -o $@

native: $(SRC) $(HDR) | bin
	gcc $(CSTD) -O0 -g -fsanitize=address,undefined $(CWARN) $(INC) $(SRC) -o bin/quacky

# A host-only companion (termios), built with the host cc, kept out of $(SRC).
qtype: tools/qtype
tools/qtype: tools/qtype.c
	cc -O2 -Wall -Wextra -Wconversion -o $@ tools/qtype.c

bin:
	mkdir -p bin

clean:
	rm -rf bin
