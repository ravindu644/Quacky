# Quacky - build system, modelled on Droidspaces and USB Arsenal.
# Copyright (C) 2026 ravindu644 <droidcasts@protonmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

BINARY_NAME = quacky
SRC_DIR     = src
OUT_DIR     = output

CC ?= cc

# QUACKY_VERSION in src/include/quacky.h is the one place the version lives;
# `make bump` rewrites it. versionCode follows the siblings: major*1000 + minor*100 + patch*10.
VERSION := $(shell grep "define QUACKY_VERSION" $(SRC_DIR)/include/quacky.h | awk '{print $$3}' | tr -d '"')
VERSION_CODE := $(shell echo $(VERSION) | awk -F. '{print $$1*1000 + $$2*100 + $$3*10}')
NPROC   := $(shell nproc 2>/dev/null || echo 4)

# V=1 shows full commands, V=0 (default) shows kernel-style short logs.
V ?= 0
ifeq ($(V),1)
  Q      =
  msg_cc =
  msg_ld =
else
  Q      = @
  msg_cc = @printf "  CC      %s\n" $<
  msg_ld = @printf "  LD      %s\n" $@
endif

# A new .c under src/ is picked up with no edit here. Headers live in src/include/;
# there is no header-dependency tracking, so run `make clean` after editing a header.
SRCS = $(wildcard $(SRC_DIR)/*.c)
INC  = -I$(SRC_DIR)/include

# Pinned: C11, every warning an error, static. Do not loosen to silence a warning.
CSTD  := -std=c11 -D_POSIX_C_SOURCE=200809L
COPT  := -O2 -ffunction-sections -fdata-sections
CWARN := -Wall -Wextra -Wshadow -Wconversion -Wvla -Werror
CFLAGS   = $(CSTD) $(COPT) $(CWARN) $(INC)
LDFLAGS := -static -Wl,--gc-sections

# Normalise the arch for the per-arch object dir (so builds do not collide).
ARCH := $(shell $(CC) -dumpmachine 2>/dev/null | cut -d'-' -f1 | \
        sed 's/i.86/x86/; s/armv7l/armhf/; s/^arm.*/armhf/' || echo "x86_64")

OBJ_DIR = $(OUT_DIR)/.obj/$(ARCH)
OBJS    = $(SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

# Find a musl cross-compiler for a triplet, in order of preference.
HOME_VAR := $(shell echo $$HOME)
find-cc = $(shell \
	if [ -n "$(MUSL_CROSS)" ] && [ -f "$(MUSL_CROSS)/$(1)-gcc" ]; then \
		echo "$(MUSL_CROSS)/$(1)-gcc"; \
	elif [ -f "$(HOME_VAR)/toolchains/$(1)-cross/bin/$(1)-gcc" ]; then \
		echo "$(HOME_VAR)/toolchains/$(1)-cross/bin/$(1)-gcc"; \
	elif command -v $(1)-gcc >/dev/null 2>&1; then \
		echo "$(1)-gcc"; \
	elif [ -f "/opt/cross/bin/$(1)-gcc" ]; then \
		echo "/opt/cross/bin/$(1)-gcc"; \
	fi)

.PHONY: all help clean native x86_64 aarch64 armhf x86 riscv64 \
        all-build tarball all-tarball debug-hardened module bump format

all: help

help:
	@echo "Quacky v$(VERSION) build system"
	@echo ""
	@echo "Build (static, musl):"
	@echo "  make native    - Build for this machine's architecture"
	@echo "  make aarch64   - Build for 64-bit ARM (most phones)"
	@echo "  make armhf     - Build for 32-bit ARM (hard-float)"
	@echo "  make x86_64    - Build for 64-bit x86"
	@echo "  make x86       - Build for 32-bit x86"
	@echo "  make riscv64   - Build for 64-bit RISC-V"
	@echo ""
	@echo "Advanced:"
	@echo "  make all-build   - Build every architecture into $(OUT_DIR)/"
	@echo "  make tarball     - Package the current binary"
	@echo "  make all-tarball - Build all and package one distribution"
	@echo "  make module      - Build the phone archs into a KernelSU/Magisk zip"
	@echo ""
	@echo "Options:"
	@echo "  V=1            - Show full compiler commands"
	@echo "  MUSL_CROSS=dir - Directory holding <triplet>-gcc"
	@echo "  NO_STRIP=1     - Keep symbols"
	@echo ""
	@echo "Other:"
	@echo "  make debug-hardened - Host build with ASan/UBSan for the tests"
	@echo "  make bump NEW=x.y.z - Set the version (quacky.h and module.prop)"
	@echo "  make format    - Run clang-format over src/"
	@echo "  make clean     - Remove $(OUT_DIR)/ (run after editing a header)"

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	$(msg_cc)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) -c $< -o $@

$(BINARY_NAME): $(OBJS)
	$(msg_ld)
	$(Q)mkdir -p $(OUT_DIR)
	$(Q)$(CC) $(OBJS) -o $(OUT_DIR)/$(BINARY_NAME) $(LDFLAGS)
	@if [ -z "$(NO_STRIP)" ]; then \
		STRIP_CMD=$$(echo $(CC) | sed 's/-gcc$$/-strip/; s/gcc$$/strip/'); \
		if command -v $$STRIP_CMD >/dev/null 2>&1 || [ -f "$$STRIP_CMD" ]; then \
			$(if $(filter 0,$(V)),printf "  STRIP   $(OUT_DIR)/$(BINARY_NAME)\n";) \
			$$STRIP_CMD -s $(OUT_DIR)/$(BINARY_NAME) 2>/dev/null || true; \
		fi; \
	fi
	@echo "[+] Built: $(OUT_DIR)/$(BINARY_NAME)"

# native: pick the host arch's musl toolchain, or use CC if it is already musl.
NATIVE_TRIPLET := $(shell $(CC) -dumpmachine 2>/dev/null)
NATIVE_ARCH    := $(shell echo "$(NATIVE_TRIPLET)" | cut -d'-' -f1 | sed 's/i.86/i686/')
ifneq ($(findstring musl,$(NATIVE_TRIPLET)),)
  NATIVE_CC := $(CC)
else
  NATIVE_CC := $(call find-cc,$(NATIVE_ARCH)-linux-musl)
endif

native:
	@if [ -n "$(NATIVE_CC)" ]; then $(MAKE) -j$(NPROC) $(BINARY_NAME) CC="$(NATIVE_CC)"; \
	else echo "Error: $(NATIVE_ARCH)-linux-musl-gcc not found (set MUSL_CROSS)"; exit 1; fi

x86_64:
	@CROSS_CC="$(call find-cc,x86_64-linux-musl)"; \
	if [ -n "$$CROSS_CC" ]; then $(MAKE) -j$(NPROC) $(BINARY_NAME) CC=$$CROSS_CC; \
	else echo "Error: x86_64-linux-musl-gcc not found (set MUSL_CROSS)"; exit 1; fi

aarch64:
	@CROSS_CC="$(call find-cc,aarch64-linux-musl)"; \
	if [ -n "$$CROSS_CC" ]; then $(MAKE) -j$(NPROC) $(BINARY_NAME) CC=$$CROSS_CC; \
	else echo "Error: aarch64-linux-musl-gcc not found (set MUSL_CROSS)"; exit 1; fi

armhf:
	@CROSS_CC="$(call find-cc,arm-linux-musleabihf)"; \
	if [ -n "$$CROSS_CC" ]; then $(MAKE) -j$(NPROC) $(BINARY_NAME) CC=$$CROSS_CC; \
	else echo "Error: arm-linux-musleabihf-gcc not found (set MUSL_CROSS)"; exit 1; fi

x86:
	@CROSS_CC="$(call find-cc,i686-linux-musl)"; \
	if [ -n "$$CROSS_CC" ]; then $(MAKE) -j$(NPROC) $(BINARY_NAME) CC=$$CROSS_CC; \
	else echo "Error: i686-linux-musl-gcc not found (set MUSL_CROSS)"; exit 1; fi

riscv64:
	@CROSS_CC="$(call find-cc,riscv64-linux-musl)"; \
	if [ -n "$$CROSS_CC" ]; then $(MAKE) -j$(NPROC) $(BINARY_NAME) CC=$$CROSS_CC; \
	else echo "Error: riscv64-linux-musl-gcc not found (set MUSL_CROSS)"; exit 1; fi

# A host build (glibc) with sanitizers, for the test suite. Not static musl.
# No -fsanitize=leak: a syntax error exits without freeing the arena by design,
# which the leak checker would flag and fail the error-case tests.
debug-hardened:
	@echo "[*] Building hardened debug binary..."
	@mkdir -p $(OUT_DIR)
	$(Q)$(CC) $(CSTD) $(CWARN) $(INC) -g -O0 -fsanitize=address,undefined \
		$(SRCS) -o $(OUT_DIR)/$(BINARY_NAME)-hardened
	@echo "[+] Hardened binary built: $(OUT_DIR)/$(BINARY_NAME)-hardened"

# all-build: fail immediately if any architecture fails - no || fallback.
all-build:
	@echo "[*] Building for all architectures ($(NPROC) jobs each)..."
	@rm -rf $(OUT_DIR)
	@$(MAKE) --no-print-directory x86_64  && mv $(OUT_DIR)/$(BINARY_NAME) $(OUT_DIR)/$(BINARY_NAME)-x86_64
	@$(MAKE) --no-print-directory aarch64 && mv $(OUT_DIR)/$(BINARY_NAME) $(OUT_DIR)/$(BINARY_NAME)-aarch64
	@$(MAKE) --no-print-directory armhf   && mv $(OUT_DIR)/$(BINARY_NAME) $(OUT_DIR)/$(BINARY_NAME)-armhf
	@$(MAKE) --no-print-directory x86     && mv $(OUT_DIR)/$(BINARY_NAME) $(OUT_DIR)/$(BINARY_NAME)-x86
	@$(MAKE) --no-print-directory riscv64 && mv $(OUT_DIR)/$(BINARY_NAME) $(OUT_DIR)/$(BINARY_NAME)-riscv64
	@echo "[+] All architectures built in $(OUT_DIR)/"

tarball:
	@if [ ! -f $(OUT_DIR)/$(BINARY_NAME) ]; then \
		echo "Error: $(OUT_DIR)/$(BINARY_NAME) not found. Build it first."; exit 1; fi
	@DETECTED_ARCH=$$(file $(OUT_DIR)/$(BINARY_NAME) | grep -oP '(x86-64|ARM aarch64|UCB RISC-V|i386|ARM)' | sed 's/x86-64/amd64/; s/ARM aarch64/arm64/; s/UCB RISC-V/riscv64/; s/i386/x86/; s/ARM/armhf/'); \
	TARBALL="$(BINARY_NAME)-$$DETECTED_ARCH-static-v$(VERSION).tar.gz"; \
	echo "[*] Creating $$TARBALL..."; \
	tar -czf $$TARBALL -C $(OUT_DIR) $(BINARY_NAME); \
	echo "[+] Created: $$TARBALL ($$(du -h $$TARBALL | cut -f1))"

all-tarball: all-build
	@COMMIT_HASH=$$(git rev-parse --short HEAD 2>/dev/null || date +%Y-%m-%d); \
	TARBALL="$(BINARY_NAME)-v$(VERSION)-$$COMMIT_HASH.tar.gz"; \
	ROOT_DIR="$(BINARY_NAME)-v$(VERSION)"; \
	TEMP_DIR="/tmp/quacky-tarball-$$$$"; \
	mkdir -p $$TEMP_DIR/$$ROOT_DIR; \
	for arch in x86_64 aarch64 armhf x86 riscv64; do \
		if [ -f $(OUT_DIR)/$(BINARY_NAME)-$$arch ]; then \
			mkdir -p $$TEMP_DIR/$$ROOT_DIR/$$arch; \
			cp $(OUT_DIR)/$(BINARY_NAME)-$$arch $$TEMP_DIR/$$ROOT_DIR/$$arch/$(BINARY_NAME); \
		fi; \
	done; \
	echo "[*] Creating unified distribution: $$TARBALL..."; \
	tar -czf $$TARBALL -C $$TEMP_DIR $$ROOT_DIR; \
	rm -rf $$TEMP_DIR; \
	echo "[+] Created: $$TARBALL ($$(du -h $$TARBALL | cut -f1))"

# One zip for every phone: customize.sh keeps the binary for the device ABI.
module:
	@rm -rf $(OUT_DIR)/module
	@mkdir -p $(OUT_DIR)/module/bin
	@# objects live per arch under .obj/, so the four builds do not collide
	@for a in aarch64 armhf x86_64 x86; do \
		$(MAKE) --no-print-directory $$a || exit 1; \
		mv $(OUT_DIR)/$(BINARY_NAME) $(OUT_DIR)/module/bin/$(BINARY_NAME)-$$a || exit 1; \
	done
	@cp module/module.prop module/customize.sh module/uninstall.sh $(OUT_DIR)/module/
	@sed -i 's/^version=.*/version=v$(VERSION)/; s/^versionCode=.*/versionCode=$(VERSION_CODE)/' $(OUT_DIR)/module/module.prop
	@cd $(OUT_DIR)/module && rm -f ../$(BINARY_NAME)-v$(VERSION).zip && zip -qr9 ../$(BINARY_NAME)-v$(VERSION).zip .
	@echo "[+] Built: $(OUT_DIR)/$(BINARY_NAME)-v$(VERSION).zip"

bump:
	@echo "$(NEW)" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$$' || { echo "Usage: make bump NEW=x.y.z"; exit 1; }
	@sed -i 's/define QUACKY_VERSION ".*"/define QUACKY_VERSION "$(NEW)"/' $(SRC_DIR)/include/quacky.h
	@code=$$(echo $(NEW) | awk -F. '{print $$1*1000 + $$2*100 + $$3*10}'); \
		sed -i "s/^version=.*/version=v$(NEW)/; s/^versionCode=.*/versionCode=$$code/" module/module.prop
	@echo "[+] Version $(VERSION) -> $(NEW). Commit it as: build: bump v$(NEW)"

format:
	@command -v clang-format >/dev/null 2>&1 || { echo "Error: clang-format not found"; exit 1; }
	@find $(SRC_DIR) -name '*.c' -o -name '*.h' | xargs clang-format -i
	@echo "[+] Formatted all source files"

clean:
	@rm -rf $(OUT_DIR) $(BINARY_NAME)-*.tar.gz
	@echo "[+] Cleaned build artifacts"
