#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# KernelSU / Magisk install script: keep the one binary this device can run.

SKIPUNZIP=0

case "$ARCH" in
  arm64) BIN=quacky-aarch64 ;;
  arm)   BIN=quacky-armhf ;;
  x64)   BIN=quacky-x86_64 ;;
  x86)   BIN=quacky-x86 ;;
  *) abort "! Unsupported architecture: $ARCH" ;;
esac
[ -f "$MODPATH/bin/$BIN" ] || abort "! $BIN is missing from the zip"

mkdir -p "$MODPATH/system/bin"
mv "$MODPATH/bin/$BIN" "$MODPATH/system/bin/quacky"
rm -rf "$MODPATH/bin"
set_perm "$MODPATH/system/bin/quacky" 0 0 0755

ui_print "- Quacky for $ARCH installed"
ui_print "- Run 'quacky <script.qk>' from a root shell to type over USB HID."
ui_print "- The /dev/hidgN keyboard node is provided by the USB Arsenal module."
