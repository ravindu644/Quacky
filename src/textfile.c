/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "textfile.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A keyboard types a file a character at a time, so a few seconds of typing is
   already a large file. Cap the read so a stray path cannot pull a huge file
   into memory; raise it here if a real passage ever needs more. */
#define TF_MAX (1UL << 20) /* 1 MiB */

/* Strict UTF-8, used as the "is this text, not a blob" test. Rejects a NUL
   byte, overlong encodings, UTF-16 surrogates, code points past U+10FFFF, and
   any truncated or stray continuation byte. out_type's decoder is deliberately
   lenient, so it cannot stand in for this. */
static bool valid_utf8_text(const unsigned char *s, size_t n) {
  size_t i = 0;
  while (i < n) {
    unsigned char c = s[i];
    if (c == 0x00)
      return false; /* NUL: this is a blob */
    if (c < 0x80) {
      i++;
      continue;
    } /* plain ASCII */

    int len;
    unsigned long cp;
    if ((c & 0xE0u) == 0xC0u) {
      if (c < 0xC2u)
        return false; /* 0xC0/0xC1 are overlong */
      len = 2;
      cp = (unsigned long)(c & 0x1Fu);
    } else if ((c & 0xF0u) == 0xE0u) {
      len = 3;
      cp = (unsigned long)(c & 0x0Fu);
    } else if ((c & 0xF8u) == 0xF0u) {
      if (c > 0xF4u)
        return false; /* past U+10FFFF */
      len = 4;
      cp = (unsigned long)(c & 0x07u);
    } else {
      return false; /* stray continuation or bad lead */
    }

    if (i + (size_t)len > n)
      return false; /* truncated at end of buffer */
    for (int k = 1; k < len; k++) {
      unsigned char cc = s[i + (size_t)k];
      if ((cc & 0xC0u) != 0x80u)
        return false; /* not a continuation byte */
      cp = (cp << 6) | (unsigned long)(cc & 0x3Fu);
    }
    if (len == 3 && cp < 0x800UL)
      return false; /* overlong 3-byte */
    if (len == 4 && cp < 0x10000UL)
      return false; /* overlong 4-byte */
    if (cp >= 0xD800UL && cp <= 0xDFFFUL)
      return false; /* surrogate */
    if (cp > 0x10FFFFUL)
      return false;

    i += (size_t)len;
  }
  return true;
}

/* Join data_path to the directory of script_path (dirname computed by hand to
   avoid the mutate-in-place and static-storage quirks of libgen dirname). An
   absolute data_path is copied through unchanged. Returns a malloc'd string. */
static char *resolve_path(const char *script_path, const char *data_path) {
  if (data_path[0] == '/') {
    size_t n = strlen(data_path);
    char *r = malloc(n + 1);
    if (r)
      memcpy(r, data_path, n + 1);
    return r;
  }
  const char *slash = strrchr(script_path, '/');
  const char *dir = slash ? script_path : ".";
  size_t dlen = slash ? (size_t)(slash - script_path) : 1; /* "." */
  size_t plen = strlen(data_path);
  char *r = malloc(dlen + 1 + plen + 1);
  if (!r)
    return NULL;
  memcpy(r, dir, dlen);
  r[dlen] = '/';
  memcpy(r + dlen + 1, data_path, plen + 1);
  return r;
}

bool tf_load(const char *script_path, const char *data_path, size_t data_len,
             char **out, size_t *outlen, char *errbuf, size_t errcap) {
  *out = NULL;
  *outlen = 0;

  /* The path is raw bytes; make a C string once, rejecting an embedded NUL. */
  if (memchr(data_path, '\0', data_len)) {
    snprintf(errbuf, errcap, "file path contains a NUL byte");
    return false;
  }
  char *cpath = malloc(data_len + 1);
  if (!cpath) {
    snprintf(errbuf, errcap, "out of memory");
    return false;
  }
  memcpy(cpath, data_path, data_len);
  cpath[data_len] = '\0';

  char *path = resolve_path(script_path, cpath);
  free(cpath);
  if (!path) {
    snprintf(errbuf, errcap, "out of memory resolving the path");
    return false;
  }

  /* fseek/ftell sizes a regular file. A special file that reports size 0 (some
     /proc entries) reads as empty and types nothing; that matches how the
     interpreter reads the script itself and never typing /proc is no real
     loss. A FIFO or unseekable file makes fseek fail and is reported below. */
  FILE *f = fopen(path, "rb");
  if (!f) {
    int e = errno;
    snprintf(errbuf, errcap, "cannot open \"%s\": %s", path, strerror(e));
    free(path);
    return false;
  }
  if (fseek(f, 0, SEEK_END) != 0) {
    int e = errno;
    snprintf(errbuf, errcap, "cannot read \"%s\": %s", path, strerror(e));
    fclose(f);
    free(path);
    return false;
  }
  long sz = ftell(f);
  if (sz < 0) {
    int e = errno;
    snprintf(errbuf, errcap, "cannot read \"%s\": %s", path, strerror(e));
    fclose(f);
    free(path);
    return false;
  }
  if ((unsigned long)sz > TF_MAX) {
    snprintf(errbuf, errcap, "file \"%s\" is too large", path);
    fclose(f);
    free(path);
    return false;
  }
  rewind(f);

  size_t n = (size_t)sz;
  char *buf = malloc(n ? n : 1);
  if (!buf) {
    snprintf(errbuf, errcap, "out of memory reading \"%s\"", path);
    fclose(f);
    free(path);
    return false;
  }
  size_t got = fread(buf, 1, n, f);
  fclose(f);
  if (got != n) {
    snprintf(errbuf, errcap, "cannot read \"%s\"", path);
    free(buf);
    free(path);
    return false;
  }
  if (!valid_utf8_text((const unsigned char *)buf, got)) {
    snprintf(errbuf, errcap, "file \"%s\" is not valid UTF-8 text", path);
    free(buf);
    free(path);
    return false;
  }

  free(path);
  *out = buf;
  *outlen = got;
  return true;
}
