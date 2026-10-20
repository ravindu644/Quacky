#ifndef QUACKY_VALUE_H
#define QUACKY_VALUE_H

#include <stddef.h>

/* A borrowed slice into the source buffer. The AST uses it for names and string
   literals so nothing is copied until a value is actually needed (04 section 1).
   The rest of the value model (Value, QStr, int_op) arrives in Phase 3. */
typedef struct { const char *p; size_t len; } Str;

#endif
