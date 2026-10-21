#ifndef QUACKY_TEXTFILE_H
#define QUACKY_TEXTFILE_H

#include <stdbool.h>
#include <stddef.h>

/* Load a text file for `type_file`/`typeln_file`. The path is given as bytes
   (data_path, data_len) because the language's strings are not NUL-terminated;
   tf_load does the NUL check and the C-string copy once, so both call sites do
   not. It is resolved against the directory of script_path (an absolute path is
   used as is), read, and checked to be properly encoded UTF-8 text with no NUL
   byte, so a binary blob is rejected rather than typed. This confirms the file
   is text; whether every character has a US-layout key is a separate check that
   only the HID backend can make, at type time, exactly as for `type`.
   On success *out is a malloc'd buffer of *outlen bytes that the caller frees,
   and it returns true. On failure it returns false and writes a one-line reason
   into errbuf. The same function serves the parser (a literal path, checked up
   front) and the evaluator (a computed path, checked at run time). */
bool tf_load(const char *script_path, const char *data_path, size_t data_len,
             char **out, size_t *outlen, char *errbuf, size_t errcap);

/* Resolve data_path against the directory of script_path (an absolute path is
   returned unchanged), as a malloc'd string the caller frees. `import` uses it
   to resolve a module path relative to the file that imports it. */
char *tf_resolve(const char *script_path, const char *data_path);

#endif
