/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef QUACKY_QUACKY_H
#define QUACKY_QUACKY_H

/* The one place the version lives. `make bump` rewrites QUACKY_VERSION, and the
   Makefile greps it for the tarball and module names. */
#define QUACKY_VERSION "1.2.0"
#define QUACKY_TAGLINE "a tiny scripting language that types over USB HID"
#define QUACKY_AUTHOR "ravindu644"
#define QUACKY_URL "https://github.com/ravindu644/Quacky"

/* ANSI colours for the banner, same palette as the USB Arsenal sibling. */
#define C_RESET "\033[0m"
#define C_CYAN "\033[1;36m"
#define C_BLUE "\033[1;34m"
#define C_DIM "\033[2m"
#define C_BOLD "\033[1m"

#endif
