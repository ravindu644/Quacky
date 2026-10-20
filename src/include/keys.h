#ifndef QUACKY_KEYS_H
#define QUACKY_KEYS_H

#include <stdbool.h>
#include <stdint.h>

#include "value.h"

/* The single source of truth for key names and the US-QWERTY character map
   (05 section 4, 01 section 6.1). The parser resolves chords through these at
   parse time, and the output module reuses the same tables for `type` text in
   Phase 6, so there is one place each fact lives. */

typedef enum { KEY_NONE, KEY_MODIFIER, KEY_NAMED } KeyKind;

typedef struct {
    KeyKind kind;
    uint8_t value;   /* KEY_MODIFIER: the modifier bit; KEY_NAMED: the HID usage */
} KeyRef;

typedef struct {
    bool    ok;
    uint8_t mod;     /* modifier bits; Shift for uppercase and shifted glyphs */
    uint8_t usage;   /* HID usage of the base key */
} CharKey;

/* Resolve a lowercase key word: a modifier (with its aliases) or a named key. */
KeyRef  key_lookup(Str name);

/* Map a US-ASCII code point to its (modifier, usage); ok=false when there is no
   US-layout key. A few control characters map to named keys so `type` works:
   '\n' -> enter, '\t' -> tab, '\b' -> backspace. */
CharKey char_key(int codepoint);

#endif
