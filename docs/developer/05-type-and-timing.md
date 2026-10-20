# 05 · Output and timing

All I/O lives in one module, `output.c`. It owns the file descriptor, the clock, the US
keymap, and the persistent held-key state. The language layer (03) never touches bytes; it
calls `out_type`, `out_chord`, `out_hold`, `out_release`, `out_sleep_ms` and
`out_set_default_delay`, and the module decides whether that becomes HID reports or a
transcript.

## 1. Two backends, one interface

The semantics are identical in both backends; only the bytes differ.

| Backend | Selected by | Writes |
|---|---|---|
| **transcript** | default, or `-o FILE` | human-readable text: `type` output verbatim, every key action as a tag |
| **HID** | `--hid /dev/hidgN` | 8-byte USB boot-keyboard reports |

A third mode, `--check`, runs the lexer and parser only, prints the first error or nothing,
and exits 2 or 0 without opening any output. The ordinary "syntax error ⇒ no output" guarantee
(06) means a check is also implied by every normal run.

```c
typedef enum { OUT_TRANSCRIPT, OUT_HID } OutMode;

typedef struct {
    int             fd;            // -1 until opened
    OutMode         mode;
    struct timespec next;          // absolute deadline of the next key event, CLOCK_MONOTONIC
    long            tick_ns;       // default 5 * 1000 * 1000, from -d
    int64_t         default_delay; // ms inserted after every statement, from `delay`; 0 = off
    bool            started;       // first emit initialises `next`
    uint8_t         held_mod;      // modifier bits currently held by `hold`
    uint8_t         held_key[6];   // non-modifier usages currently held; 0 = empty slot
} Output;
```

### 1.1 Opening

```
--hid DEV : fd = open(DEV, O_WRONLY)                           // a device node: no O_APPEND, no O_CREAT
            failure → usage-level error, exit 64+ (06)
-o FILE   : fd = open(FILE, O_WRONLY | O_APPEND | O_CREAT, 0644), mode = transcript
no -o     : fd = STDOUT_FILENO, mode = transcript
```

- Open once at startup, before the first statement runs, so a bad path or an absent gadget
  node fails before any key is pressed.
- `O_APPEND` on a transcript file makes "seek to end + write" atomic per `write(2)`, so a
  second Quacky process appending to the same file interleaves cleanly. A device node is not
  opened `O_APPEND`: reports must land at the position the gadget expects, and the node is not
  created if missing (that is USB Arsenal's job).
- Never wrap the fd in `FILE*`. stdio buffering would batch reports and destroy the cadence;
  raw `write(2)` gives ordered, unbuffered delivery for free.

## 2. The HID report

The USB boot-keyboard report is 8 bytes:

```
byte 0 : modifier bitmap (OR of the held modifiers and this event's modifiers)
byte 1 : reserved, always 0
byte 2 : key usage, slot 1
byte 3 : key usage, slot 2
 …
byte 7 : key usage, slot 6
```

**Modifier bitmap (byte 0):** `ctrl` 0x01, `shift` 0x02, `alt` 0x04, `super` 0x08,
`altgr` 0x40. These are the left-side bits plus right-alt; aliases collapse onto the same bit
(§6.1 of 01).

A key event is **two writes**: the press report, then a release report. The release report is
*not* always zero: it is whatever the `hold` set still holds (`held_mod` + `held_key`). With an
empty held set the release is eight zero bytes, matching a normal keyboard letting go.

```c
static uint8_t base_mod(const Output *o) { return o->held_mod; }                 // start every report from the held set
// press(mods, usages[n]):  report[0] = base_mod|mods; copy held_key then usages into slots 2..7; write(8)
// release():               report[0] = base_mod;      copy held_key into slots 2..7;             write(8)
```

- **Six-key rollover.** Slots 2..7 hold at most six non-modifier usages. A chord (plus the held
  set) that needs more than six is a runtime error (`too many keys held (max 6)`), not a
  silently dropped key.
- There is no inter-report dwell by default; back-to-back press and release writes give the
  host a distinct down then up event. A small fixed gap is a one-line tuning knob if a target
  drops fast events, but the reference protocol uses none.
- **`--fast` rollover (opt-in, typed characters only).** Each character is normally two reports, press
  then release, and the USB interrupt endpoint polls about once per millisecond, so ~2 ms per
  character is the wall. With `--fast`, `out_type` rolls one key into the next: a report that
  drops the old usage and adds the new one is a release and a press in a single poll, so a run of
  distinct keys costs one report each. A repeat of the same usage (including the same key with a
  different shift, like `aA`) still gets its own release first, so the host sees a fresh
  down-edge, and the last key is released when the run ends. This roughly halves the reports
  (measured ~2x throughput) and is off by default, so the default byte stream is unchanged.
- **`--ultra` frame packing (opt-in, typed characters only).** A report has six key slots and one
  modifier byte, and the host generates a character on each slot's down-edge. So a run of distinct
  characters that share a modifier can ride in one report: `out_type_ultra` fills a frame greedily
  and emits up to six keys at once, and the host sees up to six down-edges in a single USB poll. A
  frame breaks on a modifier change, a repeated usage (a key present twice is still one key), or at
  six slots. Consecutive frames roll with no release between them when they share no usage; the next
  report drops the old slots (up-edges) and adds the new ones (down-edges) in the same poll. A shared
  usage forces a release report first so the repeated character keeps its own down-edge. The result
  is up to ~6x on runs of distinct unshifted characters (the typing-test alphabet
  `abc…xyz0-9 -=;',./` is the best case) and degrades to the `--fast` rate on prose full of repeats
  or shifted keys. `--ultra` takes precedence over `--fast`. The one assumption it adds: the host
  must process the key slots in report order, which Linux evdev does; a host that reorders a
  multi-key report would type the frame scrambled, so this is a benchmark mode, not the safe default.
  Correctness is pinned by the `rollover.ultra.hid` golden (08), which captures the shift-break,
  the roll, and the repeat-break in one line.

## 3. What each call emits

```c
Status out_type   (Output *o, const char *bytes, size_t len, const char **err);
Status out_chord  (Output *o, const Str *text, uint8_t mods, const uint8_t *usages, int n, const char **err);
Status out_hold   (Output *o, const Str *text, uint8_t mods, const uint8_t *usages, int n, const char **err);
Status out_release(Output *o, const Str *text, uint8_t mods, const uint8_t *usages, int n, bool all, const char **err);
```

The fallible calls return a `Status` and set `*err` (mirroring `int_op`) rather than reaching
into the evaluator, so `output.c` depends on nothing above it: the evaluator turns an `ST_ERR`
into a runtime error at the offending statement's line/col, including I/O failures (EPIPE/EIO),
the unmappable-character and six-key-rollover errors, and `release` of an unheld key. The `text`
argument is the chord's source span (e.g. "ctrl + c"), carried on the `Chord` so the transcript
can tag the action without reverse-mapping usages to names.

`out_type` splits `bytes` into UTF-8 code points (a byte `< 0x80` is one char; `0xC0..0xDF`
leads 2, `0xE0..0xEF` 3, `0xF0..0xF7` 4; malformed bytes are treated singly, never an error at
this layer). For each code point it waits one tick, then:

| Backend | `out_type` code point | `out_chord` / `out_hold` / `out_release` |
|---|---|---|
| **HID** | look up (mod, usage) in the US map (§4); press then release. Unmapped → runtime error `no US-layout key for U+XXXX` | build the report from `mods` + `usages`, press then release; `hold`/`release` mutate the held set and re-emit it |
| **transcript** | write the code point's bytes verbatim (so existing `type` golden files are unchanged) | write a tag: `[ctrl + c]`, `[hold shift]`, `[release shift]`, `[release all]`, each on its own line |

```c
static void write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; die_io(errno); }   // EPIPE → runtime error path, §7
        p += w; n -= (size_t)w;
    }
}
```

Short writes are real on pipes/FIFOs when a signal arrives mid-write; a device node or regular
file short-writes only on error. The loop costs nothing and removes both cases. musl's `write`
is a direct syscall wrapper (`musl/src/unistd/write.c:4-7`), nothing hidden.

## 4. The US keymap and HID usages

A static table maps each printable US-ASCII code point to `{ modifier, usage }`. Uppercase and
shifted symbols carry the Shift bit in the modifier field; there is no runtime shift logic. A
few control characters map to named keys so `type` works end to end: `\n` → `enter` (0x28),
`\t` → `tab` (0x2b), `\b` → `backspace` (0x2a). Everything else with no US key is the runtime
error above. The table lives in `keys.c` next to the key-name table; a second layout would be a
second table behind the same lookup (the documented hook; v1 is US only).

**Representative usages (USB HID Usage Page 0x07):**

| Key | Usage | Key | Usage |
|---|---|---|---|
| `a`–`z` | 0x04–0x1d | `enter` | 0x28 |
| `1`–`9` | 0x1e–0x26 | `esc` | 0x29 |
| `0` | 0x27 | `backspace` | 0x2a |
| `tab` | 0x2b | `space` | 0x2c |
| `capslock` | 0x39 | `f1`–`f12` | 0x3a–0x45 |
| `printscreen` | 0x46 | `scrolllock` | 0x47 |
| `pause` | 0x48 | `insert` | 0x49 |
| `home` | 0x4a | `pageup` | 0x4b |
| `delete` | 0x4c | `endkey` | 0x4d |
| `pagedown` | 0x4e | `right` | 0x4f |
| `left` | 0x50 | `down` | 0x51 |
| `up` | 0x52 | `numlock` | 0x53 |
| `f13`–`f24` | 0x68–0x73 | `menu` | 0x65 |

These are the standard boot-keyboard usages (USB HID Usage Tables, page 0x07). Shifted glyphs
reuse the base letter/number usage plus the Shift bit: `A` = {0x02, 0x04}, `!` = {0x02, 0x1e}.

## 5. The clock: absolute deadlines

One key event per tick, paced on a running absolute deadline so the error never accumulates.

```c
static void ts_add_ns(struct timespec *t, long ns) {
    t->tv_nsec += ns;
    while (t->tv_nsec >= 1000000000L) { t->tv_nsec -= 1000000000L; t->tv_sec++; }
}

static void wait_tick(Output *o) {
    if (!o->started) { clock_gettime(CLOCK_MONOTONIC, &o->next); o->started = true; return; }  // first event: immediate
    ts_add_ns(&o->next, o->tick_ns);
    int rc;
    do { rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &o->next, NULL); } while (rc == EINTR);
    // catch-up: if we are far behind (suspend/resume, SIGSTOP), resnap instead of bursting
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    if (ts_diff_ns(&now, &o->next) > 50 * o->tick_ns) o->next = now;
}
```

Why this shape, with evidence:

| Choice | Reason | Source |
|---|---|---|
| `next = next + tick`, not `now + tick` | relative sleeps accumulate the wake-up latency on every event (a 20 ms tick becomes 20.1 ms, 1000 events drift 100 ms). Absolute deadlines make the error non-cumulative. | cyclictest `cyclictest.c:736-739`, `:895-902`; man `clock_nanosleep(2)`: "Using an absolute timer is useful for preventing timer drift problems" |
| `TIMER_ABSTIME` + `rem = NULL` + loop on `EINTR` | with an absolute deadline the kernel does not fill `rem` and the call "can be restarted using the same t argument"; `clock_nanosleep` is never auto-restarted by `SA_RESTART`. musl writes `rem` only for relative sleeps (`musl/src/time/clock_nanosleep.c:35`). | man page §1; cyclictest `:777-784` treats `EINTR` as the only benign error |
| `CLOCK_MONOTONIC` | immune to `settimeofday`/NTP jumps that would fire a `CLOCK_REALTIME` deadline early or late. Does not advance during suspend, hence the catch-up rule. | man page §3 |
| Deadline in the past returns immediately | makes the catch-up logic cheap: no negative-sleep handling. | man page §2 |
| Catch-up threshold 50 ticks (1 s) | after a laptop resume we resnap once rather than burst 50 key events. | cyclictest `:904-905` |

Precision: Linux `clock_nanosleep` wakes within tens of microseconds on an idle box, up to a
few ms under load; at a 20 ms tick that is invisible, and it is far faster than a USB host's
1–8 ms polling interval anyway.

## 6. `sleep ms` and `delay ms`

```c
void out_sleep_ms(Output *o, int64_t ms);               // one-shot pause (the `sleep` statement)
void out_set_default_delay(Output *o, int64_t ms);      // the `delay` statement: store in o->default_delay
void out_post_statement(Output *o);                     // 03 calls this after each statement; sleeps default_delay if > 0
```

`out_sleep_ms` shares the clock so the script's `sleep` and the key cadence agree:

```c
void out_sleep_ms(Output *o, int64_t ms) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    ts_add_ns(&t, ms * 1000000L);                 // ms validated ≥ 0 by the evaluator; capped (§8) so the ns math stays in range
    int rc; do { rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL); } while (rc == EINTR);
    o->next = t;                                   // the next key event is due right after the pause, not a tick later
}
```

`delay` and `sleep` are additive and predictable: `delay` fires once after each statement via
`out_post_statement`, `sleep` fires where it is written. `repeat` (01 §6.5) is handled in the
evaluator (03) by re-running the previous action node; it needs nothing here.

## 7. Signals and failure

| Event | Policy |
|---|---|
| `SIGPIPE` (a transcript reader went away) | `signal(SIGPIPE, SIG_IGN)` at startup; `write` returns `EPIPE`, which becomes the runtime error "output error: Broken pipe" and exit 1. |
| write to `/dev/hidgX` fails (`ESHUTDOWN`, `EIO`: host unplugged, gadget not bound) | runtime error with `strerror`, exit 1. Quacky never tries to re-compose the gadget; that is USB Arsenal's job. |
| `SIGINT` / `SIGTERM` during a pause | default action, process dies. Nothing is buffered; writes already landed. No handlers installed. On HID this can leave a key reported as held; a cautious script ends with `release all`. |
| `ENOSPC` on a transcript file | runtime error with `strerror`, exit 1. |
| `fsync` | not called. The guarantee is "bytes reached the kernel in order", not "on disk". Documented in the README. |

## 8. Tick knob

`-d MS` (default 5) sets the per-event tick. `-d 0` disables the sleep entirely (`wait_tick`
returns early), which is what the golden-file tests use so a long expected output does not take
tens of seconds. Internally `tick_ns = MS * 1000000L`; `long` is 32-bit on i686/armhf, so cap
`MS` (and any `sleep`/`delay` argument) at 2 000 000 (≈33 min) at option/eval time to keep the
nanosecond multiplication in range.

## 9. Size of this module

About 200 lines of C in `output.c` plus the two static tables in `keys.c`. It is the one place
the language touches the OS besides reading the script, so it is also the one place the test
suite (08) exercises with a stopwatch and with a byte-for-byte capture of the HID reports.
