/* Key names and the US-QWERTY character map. Two pure lookups over two static
   tables (05 section 4, 01 section 6.1). No state, no I/O. */

#include "keys.h"

#include <string.h>

#define MOD_CTRL  0x01u
#define MOD_SHIFT 0x02u
#define MOD_ALT   0x04u
#define MOD_SUPER 0x08u
#define MOD_ALTGR 0x40u

/* Modifiers collapse their aliases onto one bit; named keys carry one HID usage.
   The End key is spelled `endkey` because `end` closes a function block. */
static const struct { const char *name; KeyKind kind; uint8_t value; } KEYS[] = {
    {"ctrl", KEY_MODIFIER, MOD_CTRL}, {"control", KEY_MODIFIER, MOD_CTRL},
    {"shift", KEY_MODIFIER, MOD_SHIFT},
    {"alt", KEY_MODIFIER, MOD_ALT}, {"option", KEY_MODIFIER, MOD_ALT},
    {"super", KEY_MODIFIER, MOD_SUPER}, {"win", KEY_MODIFIER, MOD_SUPER},
    {"gui", KEY_MODIFIER, MOD_SUPER}, {"meta", KEY_MODIFIER, MOD_SUPER},
    {"command", KEY_MODIFIER, MOD_SUPER},
    {"altgr", KEY_MODIFIER, MOD_ALTGR}, {"ralt", KEY_MODIFIER, MOD_ALTGR},

    {"enter", KEY_NAMED, 0x28}, {"esc", KEY_NAMED, 0x29},
    {"backspace", KEY_NAMED, 0x2a}, {"tab", KEY_NAMED, 0x2b},
    {"space", KEY_NAMED, 0x2c}, {"capslock", KEY_NAMED, 0x39},
    {"f1", KEY_NAMED, 0x3a}, {"f2", KEY_NAMED, 0x3b}, {"f3", KEY_NAMED, 0x3c},
    {"f4", KEY_NAMED, 0x3d}, {"f5", KEY_NAMED, 0x3e}, {"f6", KEY_NAMED, 0x3f},
    {"f7", KEY_NAMED, 0x40}, {"f8", KEY_NAMED, 0x41}, {"f9", KEY_NAMED, 0x42},
    {"f10", KEY_NAMED, 0x43}, {"f11", KEY_NAMED, 0x44}, {"f12", KEY_NAMED, 0x45},
    {"printscreen", KEY_NAMED, 0x46}, {"scrolllock", KEY_NAMED, 0x47},
    {"pause", KEY_NAMED, 0x48}, {"insert", KEY_NAMED, 0x49},
    {"home", KEY_NAMED, 0x4a}, {"pageup", KEY_NAMED, 0x4b},
    {"delete", KEY_NAMED, 0x4c}, {"endkey", KEY_NAMED, 0x4d},
    {"pagedown", KEY_NAMED, 0x4e}, {"right", KEY_NAMED, 0x4f},
    {"left", KEY_NAMED, 0x50}, {"down", KEY_NAMED, 0x51},
    {"up", KEY_NAMED, 0x52}, {"numlock", KEY_NAMED, 0x53},
    {"menu", KEY_NAMED, 0x65},
    {"f13", KEY_NAMED, 0x68}, {"f14", KEY_NAMED, 0x69}, {"f15", KEY_NAMED, 0x6a},
    {"f16", KEY_NAMED, 0x6b}, {"f17", KEY_NAMED, 0x6c}, {"f18", KEY_NAMED, 0x6d},
    {"f19", KEY_NAMED, 0x6e}, {"f20", KEY_NAMED, 0x6f}, {"f21", KEY_NAMED, 0x70},
    {"f22", KEY_NAMED, 0x71}, {"f23", KEY_NAMED, 0x72}, {"f24", KEY_NAMED, 0x73},
};

KeyRef key_lookup(Str name) {
    for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++) {
        if (strlen(KEYS[i].name) == name.len &&
            memcmp(KEYS[i].name, name.p, name.len) == 0)
            return (KeyRef){KEYS[i].kind, KEYS[i].value};
    }
    return (KeyRef){KEY_NONE, 0};
}

CharKey char_key(int cp) {
    if (cp >= 'a' && cp <= 'z') return (CharKey){true, 0, (uint8_t)(0x04 + (cp - 'a'))};
    if (cp >= 'A' && cp <= 'Z') return (CharKey){true, MOD_SHIFT, (uint8_t)(0x04 + (cp - 'A'))};
    if (cp >= '1' && cp <= '9') return (CharKey){true, 0, (uint8_t)(0x1e + (cp - '1'))};
    if (cp == '0')              return (CharKey){true, 0, 0x27};

    switch (cp) {
        /* a few control characters so `type` text works end to end */
        case '\n': return (CharKey){true, 0, 0x28};   /* enter */
        case '\t': return (CharKey){true, 0, 0x2b};   /* tab */
        case '\b': return (CharKey){true, 0, 0x2a};   /* backspace */

        /* unshifted punctuation */
        case ' ':  return (CharKey){true, 0, 0x2c};
        case '-':  return (CharKey){true, 0, 0x2d};
        case '=':  return (CharKey){true, 0, 0x2e};
        case '[':  return (CharKey){true, 0, 0x2f};
        case ']':  return (CharKey){true, 0, 0x30};
        case '\\': return (CharKey){true, 0, 0x31};
        case ';':  return (CharKey){true, 0, 0x33};
        case '\'': return (CharKey){true, 0, 0x34};
        case '`':  return (CharKey){true, 0, 0x35};
        case ',':  return (CharKey){true, 0, 0x36};
        case '.':  return (CharKey){true, 0, 0x37};
        case '/':  return (CharKey){true, 0, 0x38};

        /* shifted glyphs reuse the base usage plus the Shift bit */
        case '!':  return (CharKey){true, MOD_SHIFT, 0x1e};
        case '@':  return (CharKey){true, MOD_SHIFT, 0x1f};
        case '#':  return (CharKey){true, MOD_SHIFT, 0x20};
        case '$':  return (CharKey){true, MOD_SHIFT, 0x21};
        case '%':  return (CharKey){true, MOD_SHIFT, 0x22};
        case '^':  return (CharKey){true, MOD_SHIFT, 0x23};
        case '&':  return (CharKey){true, MOD_SHIFT, 0x24};
        case '*':  return (CharKey){true, MOD_SHIFT, 0x25};
        case '(':  return (CharKey){true, MOD_SHIFT, 0x26};
        case ')':  return (CharKey){true, MOD_SHIFT, 0x27};
        case '_':  return (CharKey){true, MOD_SHIFT, 0x2d};
        case '+':  return (CharKey){true, MOD_SHIFT, 0x2e};
        case '{':  return (CharKey){true, MOD_SHIFT, 0x2f};
        case '}':  return (CharKey){true, MOD_SHIFT, 0x30};
        case '|':  return (CharKey){true, MOD_SHIFT, 0x31};
        case ':':  return (CharKey){true, MOD_SHIFT, 0x33};
        case '"':  return (CharKey){true, MOD_SHIFT, 0x34};
        case '~':  return (CharKey){true, MOD_SHIFT, 0x35};
        case '<':  return (CharKey){true, MOD_SHIFT, 0x36};
        case '>':  return (CharKey){true, MOD_SHIFT, 0x37};
        case '?':  return (CharKey){true, MOD_SHIFT, 0x38};
    }
    return (CharKey){false, 0, 0};
}
