/* The output module: one file descriptor, one clock, two backends (05). The
   transcript writes human-readable text; the HID backend writes 8-byte USB
   boot-keyboard reports. Raw write(2) only, never stdio, so the cadence is not
   batched away. The US keymap lives in keys.c, shared with the parser. */

#include "output.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "keys.h"

/* Write every byte or fail; a short write is real on a pipe when a signal lands
   mid-write, and EINTR is the only benign error (05 §3). */
static Status write_bytes(Output *o, const void *buf, size_t n,
                          const char **err) {
  const char *p = buf;
  while (n) {
    ssize_t w = write(o->fd, p, n);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      o->write_failed = true; /* environmental, not a script fault */
      /* The hidg endpoint is only writable once the host has set a USB
         configuration. Until then write(2) gives ESHUTDOWN, which says nothing
         about the real cause: an unplugged cable, or a host not talking to us
         yet. USB Arsenal owns the gadget, so we only report, never recover. */
      if (errno == ESHUTDOWN)
        *err = "the host has not configured the gadget yet "
               "(is the cable plugged into a computer?)";
      else {
        snprintf(o->errbuf, sizeof o->errbuf, "output error: %s",
                 strerror(errno));
        *err = o->errbuf;
      }
      return ST_ERR;
    }
    p += w;
    n -= (size_t)w;
  }
  return ST_OK;
}

/* The clock: a running absolute deadline so wake-up latency never accumulates
   (05 §5). A zero tick disables pacing outright, which is what the tests use.
 */
static void ts_add_ns(struct timespec *t, int64_t ns) {
  /* Split first: tv_nsec is a 32-bit long on armhf/i686, so a whole `sleep
     3000` in ns (3e9) would overflow it. Each part is now < 1e9, so one carry
     suffices. */
  t->tv_sec += (time_t)(ns / 1000000000);
  t->tv_nsec += (long)(ns % 1000000000);
  if (t->tv_nsec >= 1000000000L) {
    t->tv_nsec -= 1000000000L;
    t->tv_sec++;
  }
}
static int64_t ts_diff_ns(const struct timespec *a, const struct timespec *b) {
  return (int64_t)(a->tv_sec - b->tv_sec) * 1000000000LL +
         (a->tv_nsec - b->tv_nsec);
}
static void wait_tick(Output *o) {
  if (o->tick_ns == 0)
    return;
  if (!o->started) {
    clock_gettime(CLOCK_MONOTONIC, &o->next);
    o->started = true;
    return;
  }
  ts_add_ns(&o->next, o->tick_ns);
  int rc;
  do {
    rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &o->next, NULL);
  } while (rc == EINTR);
  /* After a suspend or a long stop the deadline is far in the past; resnap once
     instead of firing a burst of catch-up events. */
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  if (ts_diff_ns(&now, &o->next) > 50 * o->tick_ns)
    o->next = now;
}

/* Transcript-only marker so a dry run shows the pauses it would insert. Best
   effort: a failed write here (a dead pipe) is caught by the next real emit, so
   pacing keeps its "cannot fail" contract. Nothing is printed for a zero pause
   or in --hid mode. */
static void pause_mark(Output *o, const char *verb, int64_t ms) {
  if (o->mode != OUT_TRANSCRIPT || ms <= 0)
    return;
  const char *err = NULL;
  char line[64];
  int n = snprintf(line, sizeof line, "[%s %" PRId64 "ms]\n", verb, ms);
  if (n > 0)
    (void)write_bytes(
        o, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1, &err);
}

/* The raw absolute-deadline pause (05 §5). Clearing `started` makes the next
   key re-snap to now rather than add a tick to the old deadline (05 §6; a sleep
   500 then a key lands at 500 ms, not 520). */
static void pause_ms(Output *o, int64_t ms) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  ts_add_ns(&t,
            ms * 1000000); /* ms validated >= 0 and capped by the evaluator */
  int rc;
  do {
    rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
  } while (rc == EINTR);
  o->started = false;
}

/* The scripted default delay (the `delay` stmt): a gap inserted only BETWEEN
   output actions, before each action but the first, so it never pads the start
   or the end of a script. An explicit `sleep` is separate and adds on top of
   this (03 §4.1). The evaluator calls this once at the start of each output
   statement, so a multi-segment `type` is still one gap; a dry run shows it as
   a [delay Nms] marker. */
void out_action_gap(Output *o) {
  if (o->seen_action && o->default_delay > 0) {
    pause_mark(o, "delay", o->default_delay);
    pause_ms(o, o->default_delay);
  }
  o->seen_action = true;
}

bool out_open(Output *o, OutMode mode, const char *path, int64_t tick_ms) {
  signal(SIGPIPE, SIG_IGN); /* a dead transcript reader becomes EPIPE, handled
                               as a runtime error */
  memset(o, 0, sizeof *o);
  o->mode = mode;
  if (tick_ms < 0)
    tick_ms = 0;
  if (tick_ms > OUT_MS_MAX)
    tick_ms = OUT_MS_MAX;
  o->tick_ns = tick_ms * 1000000;

  if (mode == OUT_HID) {
    /* A device node: USB Arsenal created it, we do not. Open read-write so the
       host's LED output report can be read back (the `*_on` reads); fall back
       to write-only if the node is not readable, since reliable typing matters
       more than the reads, which then fail loudly at the reading statement. */
    o->fd = open(path, O_RDWR);
    o->can_read = (o->fd >= 0);
    if (o->fd < 0)
      o->fd = open(path, O_WRONLY);
  } else if (path) {
    o->fd = open(path, O_WRONLY | O_APPEND | O_CREAT,
                 0644); /* O_APPEND makes concurrent runs interleave cleanly */
  } else {
    o->fd = STDOUT_FILENO;
  }
  return o->fd >= 0;
}

void out_close(Output *o) {
  /* Never leave the host with a key stuck down: a script that ends (or dies on
     a runtime error) while something is held gets one all-up report. Best
     effort; a dead gadget has nothing left to release. Signals still skip this
     (05 §7). */
  bool held = o->held_mod != 0;
  for (int k = 0; k < 6; k++)
    held = held || o->held_key[k] != 0;
  if (o->mode == OUT_HID && o->fd >= 0 && held) {
    static const uint8_t up[8] = {0};
    const char *err;
    (void)write_bytes(o, up, sizeof up, &err);
  }
  if (o->fd >= 0 && o->fd != STDOUT_FILENO)
    close(o->fd);
  o->fd = -1;
}

/* HID report builders (05 §2). Every report starts from the held set; a press
   adds this event's keys on top, a release report is the held set alone. */
static Status write_report(Output *o, uint8_t mods, const uint8_t *usages,
                           int n, const char **err) {
  uint8_t rep[8] = {0};
  rep[0] = (uint8_t)(o->held_mod | mods);
  int slot = 0;
  for (int k = 0; k < 6; k++)
    if (o->held_key[k])
      rep[2 + slot++] = o->held_key[k];
  for (int k = 0; k < n; k++) {
    if (slot >= 6) {
      *err = "too many keys held (max 6)";
      return ST_ERR;
    }
    rep[2 + slot++] = usages[k];
  }
  return write_bytes(o, rep, sizeof rep, err);
}
static Status
write_held(Output *o,
           const char **err) { /* the current held set as one report */
  return write_report(o, 0, NULL, 0, err);
}

static Status tag(Output *o, const char *verb, const Str *text,
                  const char **err) {
  char line[256];
  int n = text ? snprintf(line, sizeof line, "[%s%.*s]\n", verb, (int)text->len,
                          text->p)
               : snprintf(line, sizeof line, "[%s]\n", verb);
  if (n < 0) {
    *err = "output error: formatting";
    return ST_ERR;
  }
  size_t len = (size_t)n < sizeof line ? (size_t)n : sizeof line - 1;
  return write_bytes(o, line, len, err);
}

/* Decode one UTF-8 scalar at bytes[i] (i < len). Returns the byte length
   consumed
   (>= 1) and sets *cp. A malformed lead, a bad continuation, or a scalar
   truncated at the end passes through as a single raw byte: this layer never
   errors on bytes, only the keymap does. */
static int decode_utf8(const char *bytes, size_t len, size_t i, uint32_t *cp) {
  unsigned char b = (unsigned char)bytes[i];
  int cplen;
  uint32_t c;
  if (b < 0x80) {
    cplen = 1;
    c = b;
  } else if (b >= 0xC0 && b <= 0xDF) {
    cplen = 2;
    c = b & 0x1Fu;
  } else if (b >= 0xE0 && b <= 0xEF) {
    cplen = 3;
    c = b & 0x0Fu;
  } else if (b >= 0xF0 && b <= 0xF7) {
    cplen = 4;
    c = b & 0x07u;
  } else {
    cplen = 1;
    c = b;
  } /* malformed lead, pass singly */
  if (i + (size_t)cplen > len) {
    *cp = b;
    return 1;
  } /* truncated at the end */
  for (int k = 1; k < cplen; k++) {
    unsigned char cc = (unsigned char)bytes[i + (size_t)k];
    if ((cc & 0xC0u) != 0x80u) {
      *cp = b;
      return 1;
    } /* bad continuation */
    c = (c << 6) | (cc & 0x3Fu);
  }
  *cp = c;
  return cplen;
}

static bool usage_in(const uint8_t *set, int n, uint8_t u) {
  for (int k = 0; k < n; k++)
    if (set[k] == u)
      return true;
  return false;
}

/* --ultra (HID, typed characters only). A boot-keyboard report can carry six
   key usages, and the host generates a character on each usage's down-edge. So
   a run of distinct characters that share one modifier can ride in a single
   report: the host sees up to six down-edges in one USB poll instead of one. A
   frame breaks on a modifier change (the report has one modifier byte), on a
   repeated usage (a key present twice is still one key), or at six keys.
   Consecutive frames roll with no release between them when they share no
   usage: the next report drops the old keys (up-edges) and adds the new ones
   (down-edges) in the same poll. A shared usage forces a release first, so
   every character keeps its own down-edge. This reaches ~6x on runs of distinct
   unshifted keys and falls back to the --fast rate on text full of repeats or
   shifted keys. Order within a report is honoured by Linux evdev; some hosts
   may reorder a multi-key frame, so the gain is a benchmark, not a promise. */
static Status out_type_ultra(Output *o, const char *bytes, size_t len,
                             const char **err) {
  uint8_t prev[6];
  int prev_n = 0; /* the typed keys the last report left pressed */
  size_t i = 0;
  while (i < len) {
    uint32_t cp;
    int cplen = decode_utf8(bytes, len, i, &cp);
    CharKey ck = char_key((int)cp);
    if (!ck.ok) {
      if (prev_n) {
        Status s = write_held(o, err);
        if (s != ST_OK)
          return s;
      }
      snprintf(o->errbuf, sizeof o->errbuf, "no US-layout key for U+%04" PRIX32,
               cp);
      *err = o->errbuf;
      return ST_ERR; /* same message and partial-output behaviour as the slow
                        path */
    }

    uint8_t frame[6];
    int frame_n = 0;
    uint8_t frame_mod = ck.mod;
    do {
      frame[frame_n++] = ck.usage;
      i += (size_t)cplen;
      if (frame_n == 6 || i >= len)
        break;
      cplen = decode_utf8(bytes, len, i, &cp);
      ck = char_key((int)cp);
    } while (ck.ok && ck.mod == frame_mod &&
             !usage_in(frame, frame_n, ck.usage));

    if (prev_n) {
      bool share = false;
      for (int k = 0; k < frame_n && !share; k++)
        share = usage_in(prev, prev_n, frame[k]);
      if (share) {
        Status s = write_held(o, err);
        if (s != ST_OK)
          return s;
      }
    }
    wait_tick(o);
    Status s = write_report(o, frame_mod, frame, frame_n, err);
    if (s != ST_OK)
      return s;
    memcpy(prev, frame, (size_t)frame_n);
    prev_n = frame_n;
  }
  if (prev_n)
    return write_held(o, err); /* let go of the last frame */
  return ST_OK;
}

Status out_type(Output *o, const char *bytes, size_t len, const char **err) {
  if (o->mode == OUT_HID && o->ultra)
    return out_type_ultra(o, bytes, len, err);
  size_t i = 0;
  uint8_t cur_usage =
      0; /* --fast: the typed key still held from the previous char */
  bool pressed = false;
  while (i < len) {
    uint32_t cp;
    int cplen = decode_utf8(bytes, len, i, &cp);

    wait_tick(o);
    if (o->mode == OUT_TRANSCRIPT) {
      Status s = write_bytes(o, bytes + i, (size_t)cplen, err);
      if (s != ST_OK)
        return s;
    } else {
      CharKey ck = char_key((int)cp);
      if (!ck.ok) {
        if (pressed)
          write_held(o, err); /* do not leave a key reported as held */
        snprintf(o->errbuf, sizeof o->errbuf,
                 "no US-layout key for U+%04" PRIX32, cp);
        *err = o->errbuf;
        return ST_ERR;
      }
      uint8_t usage = ck.usage;
      if (o->fast) {
        /* Roll one key into the next: a report that drops the old usage
           and adds the new one is a release and a press in a single poll.
           Only a repeat of the same usage needs its own release first, so
           the host sees a fresh down-edge (its own key never lifted). */
        if (pressed && usage == cur_usage) {
          Status s = write_held(o, err);
          if (s != ST_OK)
            return s;
        }
        Status s = write_report(o, ck.mod, &usage, 1, err);
        if (s != ST_OK)
          return s;
        cur_usage = usage;
        pressed = true;
      } else {
        Status s = write_report(o, ck.mod, &usage, 1, err); /* press */
        if (s != ST_OK)
          return s;
        s = write_held(o, err); /* release back to the held set */
        if (s != ST_OK)
          return s;
      }
    }
    i += (size_t)cplen;
  }
  if (pressed)
    return write_held(o, err); /* release the last typed key (fast mode) */
  return ST_OK;
}

Status out_chord(Output *o, const Str *text, uint8_t mods,
                 const uint8_t *usages, int n, const char **err) {
  wait_tick(o);
  if (o->mode == OUT_TRANSCRIPT)
    return tag(o, "", text, err);
  Status s = write_report(o, mods, usages, n, err); /* press */
  if (s != ST_OK)
    return s;
  return write_held(o, err); /* release */
}

/* hold: add the keys to the persistent held set (idempotent), then emit the set
   so the host sees them go down and keep them down until released. */
Status out_hold(Output *o, const Str *text, uint8_t mods, const uint8_t *usages,
                int n, const char **err) {
  for (int k = 0; k < n; k++) {
    bool present = false;
    int free_slot = -1;
    for (int j = 0; j < 6; j++) {
      if (o->held_key[j] == usages[k]) {
        present = true;
        break;
      }
      if (o->held_key[j] == 0 && free_slot < 0)
        free_slot = j;
    }
    if (present)
      continue;
    if (free_slot < 0) {
      *err = "too many keys held (max 6)";
      return ST_ERR;
    }
    o->held_key[free_slot] = usages[k];
  }
  o->held_mod = (uint8_t)(o->held_mod | mods);

  wait_tick(o);
  if (o->mode == OUT_TRANSCRIPT)
    return tag(o, "hold ", text, err);
  return write_held(o, err);
}

Status out_release(Output *o, const Str *text, uint8_t mods,
                   const uint8_t *usages, int n, bool all, const char **err) {
  if (all) {
    o->held_mod = 0;
    memset(o->held_key, 0, sizeof o->held_key);
  } else {
    /* Validate first so a half-done release never reaches the held set. */
    if ((o->held_mod & mods) != mods) {
      *err = "release of key that is not held";
      return ST_ERR;
    }
    for (int k = 0; k < n; k++) {
      bool found = false;
      for (int j = 0; j < 6; j++)
        if (o->held_key[j] == usages[k]) {
          found = true;
          break;
        }
      if (!found) {
        *err = "release of key that is not held";
        return ST_ERR;
      }
    }
    o->held_mod = (uint8_t)(o->held_mod & ~mods);
    for (int k = 0; k < n; k++)
      for (int j = 0; j < 6; j++)
        if (o->held_key[j] == usages[k]) {
          o->held_key[j] = 0;
          break;
        }
  }

  wait_tick(o);
  if (o->mode == OUT_TRANSCRIPT)
    return all ? tag(o, "release all", NULL, err)
               : tag(o, "release ", text, err);
  return write_held(o, err);
}

void out_sleep_ms(Output *o, int64_t ms) {
  pause_mark(o, "sleep", ms); /* a dry run shows the explicit pause too */
  pause_ms(o, ms);
}

void out_set_default_delay(Output *o, int64_t ms) { o->default_delay = ms; }

/* The in-script twin of -d: ms is capped by the evaluator, so ms * 1e6 stays in
   range (05 §8). Zero disables the per-event tick, exactly like -d 0. */
void out_set_tick(Output *o, int64_t ms) { o->tick_ns = ms * 1000000; }

/* The in-script twins of --fast / --ultra. Only the HID backend reads these;
   ultra still wins over fast in out_type. */
void out_set_fast(Output *o, bool on) { o->fast = on; }
void out_set_ultra(Output *o, bool on) { o->ultra = on; }

/* Read the live settings back so a script can snapshot and restore them (the
   mirror of the statements above). tick is stored as ns but only ever set as a
   whole number of ms, so the division is exact; the modes report 0/1 to match
   how they are written. */
int64_t out_delay_ms(const Output *o) { return o->default_delay; }
int64_t out_tick_ms(const Output *o) { return o->tick_ns / 1000000; }
int64_t out_fast(const Output *o) { return o->fast ? 1 : 0; }
int64_t out_ultra(const Output *o) { return o->ultra ? 1 : 0; }

/* Drain every output report the host has queued, keeping the last one as the
   current LED state. The host sends a report only when a lock key's LED
   changes, so between changes there is nothing to read and the cache stands. A
   zero poll timeout makes this a non-blocking peek, so it never stalls the
   script; the write path stays blocking and untouched. The LED bitmap is the
   last byte of the report, which also skips a leading report-ID byte if the
   descriptor has one. */
static void drain_led(Output *o) {
  struct pollfd pfd = {.fd = o->fd, .events = POLLIN};
  while (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
    uint8_t buf[8];
    ssize_t n = read(o->fd, buf, sizeof buf);
    if (n <= 0)
      break;
    o->led_state = buf[n - 1];
  }
}

Status out_led(Output *o, int bit, bool *val, const char **err) {
  if (o->mode != OUT_HID) {
    *err = "cannot read host LEDs in a dry run (use --hid)";
    return ST_ERR;
  }
  if (!o->can_read) {
    *err = "the gadget node opened write-only, so its LEDs cannot be read";
    return ST_ERR;
  }
  drain_led(o);
  *val = (((unsigned)o->led_state >> bit) & 1u) != 0;
  return ST_OK;
}
