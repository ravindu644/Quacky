#ifndef QUACKY_OUTPUT_H
#define QUACKY_OUTPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "value.h"   /* Status */

/* The only module that touches the OS for output (05). The language layer deals
   in characters, chords and key names; everything below here is bytes, a file
   descriptor, and a clock. Two backends share one interface: a transcript for
   dry-runs and tests, and 8-byte USB boot-keyboard reports for the real gadget. */

typedef enum { OUT_TRANSCRIPT, OUT_HID } OutMode;

typedef struct {
    int             fd;             /* -1 until opened */
    OutMode         mode;
    struct timespec next;           /* absolute deadline of the next key event, CLOCK_MONOTONIC */
    long            tick_ns;        /* per-event tick, from -d; 0 disables the sleep */
    int64_t         default_delay;  /* ms inserted after every statement (the `delay` stmt); 0 = off */
    bool            started;        /* first emit snaps `next` to now */
    uint8_t         held_mod;       /* modifier bits held by `hold`, merged into every report */
    uint8_t         held_key[6];    /* non-modifier usages held; 0 = empty slot */
    char            errbuf[128];    /* holds a formatted message so a returned *err stays valid */
} Output;

/* Open the backend once at startup, before any statement runs, so a bad path or
   an absent gadget node fails before a single key is pressed. path is the device
   (OUT_HID) or the transcript file; a NULL path in transcript mode means stdout.
   tick_ms is the per-event tick (20 default, 0 off), capped so the ns math stays
   in range on 32-bit. Returns false with errno set on open failure; the caller
   maps that to the right usage exit code. Installs the SIGPIPE ignore. */
bool out_open(Output *o, OutMode mode, const char *path, int64_t tick_ms);
void out_close(Output *o);

/* The fallible calls write bytes, so they can hit EPIPE/EIO, and the HID backend
   can reject an unmappable character, a six-key overflow, or a release of a key
   that is not held. Each returns ST_OK or sets *err to a message (owned by the
   Output, valid until the next call) and returns ST_ERR, which the evaluator
   turns into a runtime error at the offending statement (06). */
Status out_type   (Output *o, const char *bytes, size_t len, const char **err);
Status out_chord  (Output *o, const Str *text, uint8_t mods, const uint8_t *usages, int n, const char **err);
Status out_hold   (Output *o, const Str *text, uint8_t mods, const uint8_t *usages, int n, const char **err);
Status out_release(Output *o, const Str *text, uint8_t mods, const uint8_t *usages, int n, bool all, const char **err);

/* Pacing (05 §6). These only sleep, so they cannot fail. The evaluator has
   already validated the millisecond argument as a non-negative, capped int. */
void out_sleep_ms         (Output *o, int64_t ms);
void out_set_default_delay(Output *o, int64_t ms);
void out_post_statement   (Output *o);   /* the evaluator calls this after each statement */

/* The largest sleep/delay/tick argument, so tv_nsec math stays in range on a
   32-bit long (05 §8). About 33 minutes. */
#define OUT_MS_MAX 2000000

#endif
