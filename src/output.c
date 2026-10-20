/* The output module: one file descriptor, one clock, two backends (05). The
   transcript writes human-readable text; the HID backend writes 8-byte USB
   boot-keyboard reports. Raw write(2) only, never stdio, so the cadence is not
   batched away. The US keymap lives in keys.c, shared with the parser. */

#include "output.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "keys.h"

/* Write every byte or fail; a short write is real on a pipe when a signal lands
   mid-write, and EINTR is the only benign error (05 §3). */
static Status write_bytes(Output *o, const void *buf, size_t n, const char **err) {
    const char *p = buf;
    while (n) {
        ssize_t w = write(o->fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            snprintf(o->errbuf, sizeof o->errbuf, "output error: %s", strerror(errno));
            *err = o->errbuf;
            return ST_ERR;
        }
        p += w; n -= (size_t)w;
    }
    return ST_OK;
}

/* The clock: a running absolute deadline so wake-up latency never accumulates
   (05 §5). A zero tick disables pacing outright, which is what the tests use. */
static void ts_add_ns(struct timespec *t, long ns) {
    t->tv_nsec += ns;
    while (t->tv_nsec >= 1000000000L) { t->tv_nsec -= 1000000000L; t->tv_sec++; }
}
static int64_t ts_diff_ns(const struct timespec *a, const struct timespec *b) {
    return (int64_t)(a->tv_sec - b->tv_sec) * 1000000000LL + (a->tv_nsec - b->tv_nsec);
}
static void wait_tick(Output *o) {
    if (o->tick_ns == 0) return;
    if (!o->started) { clock_gettime(CLOCK_MONOTONIC, &o->next); o->started = true; return; }
    ts_add_ns(&o->next, o->tick_ns);
    int rc;
    do { rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &o->next, NULL); } while (rc == EINTR);
    /* After a suspend or a long stop the deadline is far in the past; resnap once
       instead of firing a burst of catch-up events. */
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    if (ts_diff_ns(&now, &o->next) > 50 * o->tick_ns) o->next = now;
}

bool out_open(Output *o, OutMode mode, const char *path, int64_t tick_ms) {
    signal(SIGPIPE, SIG_IGN);   /* a dead transcript reader becomes EPIPE, handled as a runtime error */
    memset(o, 0, sizeof *o);
    o->mode = mode;
    if (tick_ms < 0) tick_ms = 0;
    if (tick_ms > OUT_MS_MAX) tick_ms = OUT_MS_MAX;
    o->tick_ns = (long)tick_ms * 1000000L;

    if (mode == OUT_HID) {
        o->fd = open(path, O_WRONLY);   /* a device node: USB Arsenal created it, we do not */
    } else if (path) {
        o->fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0644);   /* O_APPEND makes concurrent runs interleave cleanly */
    } else {
        o->fd = STDOUT_FILENO;
    }
    return o->fd >= 0;
}

void out_close(Output *o) {
    if (o->fd >= 0 && o->fd != STDOUT_FILENO) close(o->fd);
    o->fd = -1;
}

/* HID report builders (05 §2). Every report starts from the held set; a press
   adds this event's keys on top, a release report is the held set alone. */
static Status write_report(Output *o, uint8_t mods, const uint8_t *usages, int n, const char **err) {
    uint8_t rep[8] = {0};
    rep[0] = (uint8_t)(o->held_mod | mods);
    int slot = 0;
    for (int k = 0; k < 6; k++) if (o->held_key[k]) rep[2 + slot++] = o->held_key[k];
    for (int k = 0; k < n; k++) {
        if (slot >= 6) { *err = "too many keys held (max 6)"; return ST_ERR; }
        rep[2 + slot++] = usages[k];
    }
    return write_bytes(o, rep, sizeof rep, err);
}
static Status write_held(Output *o, const char **err) {   /* the current held set as one report */
    return write_report(o, 0, NULL, 0, err);
}

static Status tag(Output *o, const char *verb, const Str *text, const char **err) {
    char line[256];
    int n = text ? snprintf(line, sizeof line, "[%s%.*s]\n", verb, (int)text->len, text->p)
                 : snprintf(line, sizeof line, "[%s]\n", verb);
    if (n < 0) { *err = "output error: formatting"; return ST_ERR; }
    size_t len = (size_t)n < sizeof line ? (size_t)n : sizeof line - 1;
    return write_bytes(o, line, len, err);
}

Status out_type(Output *o, const char *bytes, size_t len, const char **err) {
    size_t i = 0;
    while (i < len) {
        unsigned char b = (unsigned char)bytes[i];
        int cplen; uint32_t cp;
        if      (b < 0x80)               { cplen = 1; cp = b; }
        else if (b >= 0xC0 && b <= 0xDF) { cplen = 2; cp = b & 0x1Fu; }
        else if (b >= 0xE0 && b <= 0xEF) { cplen = 3; cp = b & 0x0Fu; }
        else if (b >= 0xF0 && b <= 0xF7) { cplen = 4; cp = b & 0x07u; }
        else                             { cplen = 1; cp = b; }   /* malformed lead, pass singly */
        if (i + (size_t)cplen > len) { cplen = 1; cp = b; }        /* truncated at the end */
        for (int k = 1; k < cplen; k++) {
            unsigned char c = (unsigned char)bytes[i + (size_t)k];
            if ((c & 0xC0u) != 0x80u) { cplen = 1; cp = b; break; }   /* bad continuation */
            cp = (cp << 6) | (c & 0x3Fu);
        }

        wait_tick(o);
        if (o->mode == OUT_TRANSCRIPT) {
            Status s = write_bytes(o, bytes + i, (size_t)cplen, err);
            if (s != ST_OK) return s;
        } else {
            CharKey ck = char_key((int)cp);
            if (!ck.ok) {
                snprintf(o->errbuf, sizeof o->errbuf, "no US-layout key for U+%04" PRIX32, cp);
                *err = o->errbuf;
                return ST_ERR;
            }
            uint8_t usage = ck.usage;
            Status s = write_report(o, ck.mod, &usage, 1, err);   /* press */
            if (s != ST_OK) return s;
            s = write_held(o, err);                                /* release back to the held set */
            if (s != ST_OK) return s;
        }
        i += (size_t)cplen;
    }
    return ST_OK;
}

Status out_chord(Output *o, const Str *text, uint8_t mods, const uint8_t *usages, int n, const char **err) {
    wait_tick(o);
    if (o->mode == OUT_TRANSCRIPT) return tag(o, "", text, err);
    Status s = write_report(o, mods, usages, n, err);   /* press */
    if (s != ST_OK) return s;
    return write_held(o, err);                           /* release */
}

/* hold: add the keys to the persistent held set (idempotent), then emit the set
   so the host sees them go down and keep them down until released. */
Status out_hold(Output *o, const Str *text, uint8_t mods, const uint8_t *usages, int n, const char **err) {
    for (int k = 0; k < n; k++) {
        bool present = false;
        int free_slot = -1;
        for (int j = 0; j < 6; j++) {
            if (o->held_key[j] == usages[k]) { present = true; break; }
            if (o->held_key[j] == 0 && free_slot < 0) free_slot = j;
        }
        if (present) continue;
        if (free_slot < 0) { *err = "too many keys held (max 6)"; return ST_ERR; }
        o->held_key[free_slot] = usages[k];
    }
    o->held_mod = (uint8_t)(o->held_mod | mods);

    wait_tick(o);
    if (o->mode == OUT_TRANSCRIPT) return tag(o, "hold ", text, err);
    return write_held(o, err);
}

Status out_release(Output *o, const Str *text, uint8_t mods, const uint8_t *usages, int n, bool all, const char **err) {
    if (all) {
        o->held_mod = 0;
        memset(o->held_key, 0, sizeof o->held_key);
    } else {
        /* Validate first so a half-done release never reaches the held set. */
        if ((o->held_mod & mods) != mods) { *err = "release of key that is not held"; return ST_ERR; }
        for (int k = 0; k < n; k++) {
            bool found = false;
            for (int j = 0; j < 6; j++) if (o->held_key[j] == usages[k]) { found = true; break; }
            if (!found) { *err = "release of key that is not held"; return ST_ERR; }
        }
        o->held_mod = (uint8_t)(o->held_mod & ~mods);
        for (int k = 0; k < n; k++)
            for (int j = 0; j < 6; j++) if (o->held_key[j] == usages[k]) { o->held_key[j] = 0; break; }
    }

    wait_tick(o);
    if (o->mode == OUT_TRANSCRIPT) return all ? tag(o, "release all", NULL, err) : tag(o, "release ", text, err);
    return write_held(o, err);
}

void out_sleep_ms(Output *o, int64_t ms) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    ts_add_ns(&t, (long)ms * 1000000L);   /* ms validated >= 0 and capped by the evaluator */
    int rc; do { rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL); } while (rc == EINTR);
    o->next = t;        /* the next key event is due right after the pause, not a tick later */
    o->started = true;
}

void out_set_default_delay(Output *o, int64_t ms) { o->default_delay = ms; }

void out_post_statement(Output *o) {
    if (o->default_delay > 0) out_sleep_ms(o, o->default_delay);
}
