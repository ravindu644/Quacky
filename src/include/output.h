#ifndef QUACKY_OUTPUT_H
#define QUACKY_OUTPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "value.h" /* Status */

/* The only module that touches the OS for output (05). The language layer deals
   in characters, chords and key names; everything below here is bytes, a file
   descriptor, and a clock. Two backends share one interface: a transcript for
   dry-runs and tests, and 8-byte USB boot-keyboard reports for the real gadget.
 */

typedef enum { OUT_TRANSCRIPT, OUT_HID } OutMode;

typedef struct {
  int fd; /* -1 until opened */
  OutMode mode;
  struct timespec
      next; /* absolute deadline of the next key event, CLOCK_MONOTONIC */
  int64_t tick_ns;       /* per-event tick, from -d; 0 disables the sleep */
  int64_t default_delay; /* ms gap inserted between output actions, before each
                            action but the first (the `delay` stmt); 0 = off */
  bool seen_action; /* an output action has run, so the next one owes the gap */
  bool started;     /* first emit snaps `next` to now */
  bool fast;        /* --fast: roll one typed key into the next (HID only) */
  bool ultra; /* --ultra: pack a run of distinct same-mod keys per report (HID
                 only) */
  uint8_t held_mod; /* modifier bits held by `hold`, merged into every report */
  uint8_t held_key[6]; /* non-modifier usages held; 0 = empty slot */
  bool can_read;     /* the hidg node opened readable (O_RDWR), so the host LED
                        output report can be read back; false = write-only */
  uint8_t led_state; /* last LED bitmap the host sent (bit 0 Num, 1 Caps, 2
                        Scroll); the host only sends on a change, so this caches
                        it between reads */
  const char *os_name; /* cached host_os result (a static literal), NULL until a
                          definite detection; probing toggles keys, so we do it
                          once (like DuckyScript's $_OS) */
  char errbuf[128];  /* holds a formatted message so a returned *err stays valid
                      */
  bool write_failed; /* set when an output write fails. Such a failure is
                        environmental (unplugged cable, host not configured,
                        broken pipe, full disk), never a fault in the script, so
                        eval reports it without a source caret. */
  bool quiet;       /* --quiet: suppress the live keystroke mirror in --hid mode
                       (`echo` still prints) */
  bool tbol;        /* transcript stream (o->fd) is at the start of a line, so a
                       marker like [delay 300ms] knows whether to open with a \n */
  bool sbol;        /* same, for stderr: the live mirror and `echo` share it */
  bool s_echo_open; /* stderr's current partial line was left by an `echo \c`,
                       so the next echo continues it instead of breaking to a
                       fresh line (a partial line left by the mirror does
                       break) */
} Output;

/* Open the backend once at startup, before any statement runs, so a bad path or
   an absent gadget node fails before a single key is pressed. path is the
   device (OUT_HID) or the transcript file; a NULL path in transcript mode means
   stdout. tick_ms is the per-event tick (5 default, 0 off), capped so the ns
   math stays in range on 32-bit. Returns false with errno set on open failure;
   the caller maps that to the right usage exit code. Installs the SIGPIPE
   ignore. */
bool out_open(Output *o, OutMode mode, const char *path, int64_t tick_ms);
void out_close(Output *o);

/* The fallible calls write bytes, so they can hit EPIPE/EIO, and the HID
   backend can reject an unmappable character, a six-key overflow, or a release
   of a key that is not held. Each returns ST_OK or sets *err to a message
   (owned by the Output, valid until the next call) and returns ST_ERR, which
   the evaluator turns into a runtime error at the offending statement (06). */
Status out_type(Output *o, const char *bytes, size_t len, const char **err);
Status out_chord(Output *o, const Str *text, uint8_t mods,
                 const uint8_t *usages, int n, const char **err);
Status out_hold(Output *o, const Str *text, uint8_t mods, const uint8_t *usages,
                int n, const char **err);
Status out_release(Output *o, const Str *text, uint8_t mods,
                   const uint8_t *usages, int n, bool all, const char **err);

/* Pacing (05 §6). These only sleep, so they cannot fail. The evaluator has
   already validated the millisecond argument as a non-negative, capped int. */
void out_sleep_ms(Output *o, int64_t ms);
void out_set_default_delay(Output *o, int64_t ms);
/* The `keydelay` statement: the per-key-event tick, the in-script twin of -d.
   ms is validated >= 0 and capped by the evaluator before it arrives here. */
void out_set_tick(Output *o, int64_t ms);
/* The `fastmode` / `ultramode` statements: the in-script twins of --fast /
   --ultra. HID-only; ultra still wins over fast in out_type. */
void out_set_fast(Output *o, bool on);
void out_set_ultra(Output *o, bool on);
/* Read the four settings back (the mirror of the setters above), so a script
   can snapshot and restore them. tick is returned in ms; the modes as 0/1. */
int64_t out_delay_ms(const Output *o);
int64_t out_tick_ms(const Output *o);
int64_t out_fast(const Output *o);
int64_t out_ultra(const Output *o);
/* Read one host lock-key LED back (bit 0 Num, 1 Caps, 2 Scroll). Unlike the
   settings above this can fail: there is no host in a dry run, and a node that
   opened write-only cannot be read. On failure *err gets a message (static, so
   it outlives the call) and the return is ST_ERR; the evaluator turns that into
   a runtime error at the reading expression. Drains any pending reports first,
   so *val is the current state even though the host only sends on a change. */
Status out_led(Output *o, int bit, bool *val, const char **err);
/* Detect the host OS by toggling lock keys and seeing which LEDs it reflects
   (NumLock ignored -> macOS; ScrollLock reflected -> Windows, else Linux; no
   CapsLock echo at all -> "unknown"). *name gets a static string
   ("windows"/"linux"/"macos"/"unknown"), never freed. The first definite
   result is cached, so repeated reads do not re-toggle the keys. Fails like
   out_led in a dry run or on a write-only node, and propagates a write error.
 */
Status out_detect_os(Output *o, const char **name, const char **err);
/* The evaluator calls this once at the start of each output statement; it
   pauses default_delay before every action but the first (the gap between
   keystrokes), so it never pads the start or end of a script. */
void out_action_gap(Output *o);

/* `echo`: print operator-facing text to stderr in every mode, so it shows on
   the terminal without ever reaching the host and without touching the
   transcript on stdout/-o or the HID report stream. Best effort: a closed
   stderr is dropped, never a reason to fail a real run. Call out_echo_begin
   once per echo statement (it moves to a fresh line if needed), then out_echo
   for each text or interpolated segment; the trailing newline is in the last
   segment. */
void out_echo_begin(Output *o);
void out_echo(Output *o, const char *bytes, size_t len);
/* Close any partial stderr line (from the live mirror or `echo \c`) so a
   following runtime-error diagnostic starts on its own line. */
void out_newline_guard(Output *o);

/* The largest sleep/delay/tick argument (about 33 minutes). The ns math is done
   in int64_t and split into seconds before it touches tv_nsec, since a 32-bit
   long overflows past ~2147 ms in nanoseconds (05 §8). */
#define OUT_MS_MAX 2000000

#endif
