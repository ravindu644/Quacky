# 05 · `type` output and timing

The only I/O in the interpreter. One module, `output.c`, owns the file descriptor and the
clock.

## 1. State

```c
typedef struct {
    int             fd;          // -1 until opened
    struct timespec next;        // absolute deadline of the next character, CLOCK_MONOTONIC
    long            tick_ns;     // default 20 * 1000 * 1000, from -d
    bool            started;     // first emit initialises `next`
} Output;
```

## 2. Opening

```
-o FILE given:  fd = open(FILE, O_WRONLY | O_APPEND | O_CREAT, 0644)
                failure → usage-level error, exit 64+ (06)
no -o:          fd = STDOUT_FILENO
```

- `O_APPEND` makes "seek to end + write" atomic per `write(2)` (man page, NOTES), so a second quacky process appending to the same file interleaves at character granularity instead of clobbering.
- Never wrap the fd in `FILE*`. stdio buffering would batch the characters and destroy the effect; `fflush` per char is a slower way to get what raw `write(2)` gives for free.
- Open once at startup, before the first statement runs, so a bad path fails before any sleep.
- `/dev/stdout` and FIFOs work naturally since they are just paths.

## 3. Emitting characters

```c
void out_emit(Output *o, const char *bytes, size_t len);   // called per type-segment
```

1. Split `bytes` into UTF-8 code points: a byte `< 0x80` is one char; `0xC0..0xDF` leads 2, `0xE0..0xEF` 3, `0xF0..0xF7` 4; anything malformed is treated as single bytes (never error, never block on bad input).
2. For each code point: `wait_tick(o)`, then `write_all(o->fd, cp, cplen)`.

```c
static void write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; die_io(errno); }   // EPIPE → runtime error path, see §6
        p += w; n -= (size_t)w;
    }
}
```

Short writes are real on pipes and FIFOs when a signal arrives mid-write (`write(2)` NOTES);
a regular file only short-writes on disk-full. The loop costs nothing and removes both cases.
musl's `write` is a direct syscall wrapper with cancellation point (`musl/src/unistd/write.c:4-7`),
nothing hidden.

## 4. The clock: absolute deadlines

```c
static void ts_add_ns(struct timespec *t, long ns) {
    t->tv_nsec += ns;
    while (t->tv_nsec >= 1000000000L) { t->tv_nsec -= 1000000000L; t->tv_sec++; }
}

static void wait_tick(Output *o) {
    if (!o->started) { clock_gettime(CLOCK_MONOTONIC, &o->next); o->started = true; return; }  // first char: immediate
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
| `next = next + tick`, not `now + tick` | relative sleeps accumulate the wake-up latency on every character (a 20 ms tick becomes 20.1 ms, 1000 chars drift 100 ms). Absolute deadlines make the error non-cumulative. | cyclictest's main loop does exactly this: `cyclictest.c:736-739` (init `next = now + interval`), `:895-902` (advance `next` by `interval` each cycle); man `clock_nanosleep(2)`: "Using an absolute timer is useful for preventing timer drift problems" |
| `TIMER_ABSTIME` + `rem = NULL` + loop on `EINTR` | with an absolute deadline the kernel does not fill `rem` and the call "can be restarted using the same t argument"; `clock_nanosleep` is never auto-restarted by `SA_RESTART`. musl's implementation shows `rem` is only written for relative sleeps (`musl/src/time/clock_nanosleep.c:35`). | man page §1; cyclictest `:777-784` treats `EINTR` as the only benign error |
| `CLOCK_MONOTONIC` | immune to `settimeofday`/NTP jumps that would make a `CLOCK_REALTIME` absolute deadline fire early or late by the jump amount. Does not advance during suspend, hence the catch-up rule. | man page §3 |
| Deadline in the past returns immediately | this is what makes the catch-up logic cheap: no negative sleep handling. | man page §2 |
| Catch-up threshold 50 ticks (1 s) | cyclictest's `while (tsgreater(&now, &next)) next += interval` (`:904-905`) skips missed cycles one by one; for a typewriter we prefer to resnap once rather than burst 50 characters after a laptop resume. |

Precision: Linux `clock_nanosleep` wakes within tens of microseconds on an idle desktop, up to
a few ms under load; at a 20 ms tick that is invisible. The timer-slack default (50 µs) only
applies to the wake-up, not the deadline arithmetic.

## 5. `sleep ms`

Same mechanism, so the script's `sleep` and the typewriter cadence share one clock:

```c
void out_sleep_ms(Output *o, int64_t ms) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    ts_add_ns(&t, ms * 1000000L);                 // ms is validated ≥ 0 by the evaluator; cap at e.g. 1h to keep tv_nsec math in range
    int rc; do { rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL); } while (rc == EINTR);
    o->next = t;                                   // the next character is due right after the sleep, not 20 ms later
}
```

Setting `o->next = t` means `sleep 500` followed by `type x` prints `x` immediately after
the 500 ms, which is what a reader expects.

## 6. Signals and failure

| Event | Policy |
|---|---|
| `SIGPIPE` (reader of a FIFO/pipe went away) | `signal(SIGPIPE, SIG_IGN)` at startup; `write` then returns `EPIPE`, which becomes the runtime error "output closed" and exit 1. Letting the default kill the process would hide the error message. |
| `SIGINT` / `SIGTERM` during a sleep | default action, process dies. Nothing to flush (no stdio buffer), `O_APPEND` writes already landed. No handlers installed. |
| `ENOSPC`, `EIO` | runtime error with `strerror`, exit 1. |
| `fsync` | not called. The guarantee is "bytes reached the kernel in order", not "on disk". Document in README. |

## 7. Tick knob

`-d MS` (default 20). `-d 0` disables the sleep entirely (`wait_tick` returns early), which is
what the golden-file tests use so a 2 000-character expected output does not take 40 s.
Internally `tick_ns = MS * 1000000L`; `long` is 32-bit on i686/armhf, so cap `MS` at
2 000 000 (≈33 min) at option parsing to keep the multiplication in range.

## 8. Size of this module
About 120 lines of C. It is the one place where the language touches the OS besides reading
the script, so it is also the one place the test suite exercises with a stopwatch (08).
