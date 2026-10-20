/* qtype: a typing-speed meter for a keyboard that is not a person.
 *
 * A browser typing test re-renders on every keystroke, so it measures the
 * browser, not the keyboard. qtype reads a target .txt, puts the terminal in
 * raw mode, and times the first byte to the last as a USB HID gadget types into
 * it. Two modes:
 *
 *   qtype FILE            interactive: a dim hint with your typing overlaid in
 *                         colour and live stats (the number includes the cost
 *                         of drawing each frame).
 *   qtype --bench FILE    no echo, no drawing: drains stdin as fast as the
 *                         kernel delivers it. This is the raw input ceiling, the
 *                         fair number for a gadget typing at full tilt.
 *
 * Build:  cc -O2 -Wall -Wextra -o qtype qtype.c   (zero dependencies)
 */

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define IDLE_FINISH_US 400000L   /* finish this long after input stops */

static struct termios g_saved;
static int g_raw = 0, g_altscreen = 0;

static void restore_term(void) {
    if (g_altscreen) { fputs("\033[?25h\033[?1049l", stdout); fflush(stdout); g_altscreen = 0; }
    if (g_raw) { tcsetattr(STDIN_FILENO, TCSANOW, &g_saved); g_raw = 0; }
}

static volatile sig_atomic_t g_winch = 0, g_quit = 0;
static void on_winch(int s) { (void)s; g_winch = 1; }
static void on_quit(int s)  { (void)s; g_quit = 1; }

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    if (n < 0) { perror(path); exit(1); }
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fputs("out of memory\n", stderr); exit(1); }
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    while (got > 0 && (buf[got - 1] == '\n' || buf[got - 1] == '\r')) buf[--got] = '\0';
    *out_len = got;
    return buf;
}

static double secs_between(struct timespec a, struct timespec b) {
    return (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) / 1e9;
}
static long us_between(struct timespec a, struct timespec b) {
    return (b.tv_sec - a.tv_sec) * 1000000L + (b.tv_nsec - a.tv_nsec) / 1000L;
}

static void enter_raw(int altscreen) {
    if (tcgetattr(STDIN_FILENO, &g_saved) != 0) { perror("tcgetattr (is stdin a terminal?)"); exit(1); }
    atexit(restore_term);
    struct termios raw = g_saved;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG | IEXTEN);
    raw.c_iflag &= (tcflag_t)~(IXON | ICRNL | INPCK | ISTRIP);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) { perror("tcsetattr"); exit(1); }
    g_raw = 1;
    if (altscreen) { fputs("\033[?1049h\033[?25l\033[2J", stdout); fflush(stdout); g_altscreen = 1; }
}

/* Stats shared by both modes. */
typedef struct { size_t typed, correct, target; double secs; } Stats;

static Stats compute(const char *target, const char *typed, size_t i, size_t len, double secs) {
    size_t correct = 0;
    for (size_t j = 0; j < i; j++) if (typed[j] == target[j]) correct++;
    Stats s = { i, correct, len, secs };
    return s;
}

static void print_result(Stats s, int bench) {
    double mins = s.secs / 60.0;
    double net = mins > 0 ? ((double)s.correct / 5.0) / mins : 0.0;
    double raw = mins > 0 ? ((double)s.typed / 5.0) / mins : 0.0;
    double cps = s.secs > 0 ? (double)s.typed / s.secs : 0.0;
    double acc = s.typed > 0 ? (double)s.correct / (double)s.typed * 100.0 : 0.0;
    printf("\n");
    printf("  chars read : %zu / %zu\n", s.typed, s.target);
    printf("  correct    : %zu  (%.2f%% accuracy)\n", s.correct, acc);
    printf("  elapsed    : %.4f s\n", s.secs);
    printf("  chars/sec  : %.0f\n", cps);
    printf("  net  WPM   : %.0f\n", net);
    printf("  raw  WPM   : %.0f\n", raw);
    if (s.typed < s.target) printf("  note: %zu characters never arrived\n", s.target - s.typed);
    if (!bench) printf("  (this is interactive mode; --bench measures the raw ceiling without drawing)\n");
}

/* ---- bench mode: the raw ceiling, no echo, no drawing ---- */
static int run_bench(const char *target, size_t len) {
    char *typed = malloc(len);
    if (!typed) { fputs("out of memory\n", stderr); return 1; }
    enter_raw(0);
    printf("qtype --bench: %zu characters. Focus this window; input is captured silently.\r\n", len);
    printf("Target:\r\n\033[2m%s\033[0m\r\n\r\nwaiting for input (Ctrl-C to abort)...\r\n", target);
    fflush(stdout);

    struct timespec t0 = {0, 0}, t1 = {0, 0};
    size_t i = 0;
    int started = 0, aborted = 0;
    char buf[4096];
    while (i < len) {
        fd_set fds; FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
        struct timeval tv = { 0, IDLE_FINISH_US };
        int sr = select(STDIN_FILENO + 1, &fds, NULL, NULL, started ? &tv : NULL);
        if (sr == 0) break;
        if (sr < 0) { if (errno == EINTR) continue; break; }
        ssize_t r = read(STDIN_FILENO, buf, sizeof buf);
        if (r <= 0) break;
        if (!started) { clock_gettime(CLOCK_MONOTONIC, &t0); started = 1; }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        for (ssize_t k = 0; k < r && i < len; k++) {
            unsigned char c = (unsigned char)buf[k];
            if (c == 3) { aborted = 1; break; }
            if (c == 127 || c == 8) { if (i > 0) i--; continue; }
            typed[i++] = (char)c;
        }
        if (aborted) break;
    }
    restore_term();
    if (aborted || !started) { printf("\naborted.\n"); free(typed); return 1; }
    print_result(compute(target, typed, i, len, secs_between(t0, t1)), 1);
    free(typed);
    return 0;
}

/* ---- interactive TUI ---- */
typedef struct { char *p; size_t len, cap; } Buf;
static void bgrow(Buf *b, size_t extra) {
    if (b->len + extra <= b->cap) return;
    size_t nc = b->cap ? b->cap * 2 : 8192;
    while (nc < b->len + extra) nc *= 2;
    b->p = realloc(b->p, nc);
    if (!b->p) { restore_term(); fputs("out of memory\n", stderr); exit(1); }
    b->cap = nc;
}
static void bputs(Buf *b, const char *s) { size_t n = strlen(s); bgrow(b, n); memcpy(b->p + b->len, s, n); b->len += n; }
static void bputc_(Buf *b, char c) { bgrow(b, 1); b->p[b->len++] = c; }
static void bprintf(Buf *b, const char *fmt, ...) {
    char tmp[256]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
    if (n > 0) bputs(b, tmp);
}

static int term_cols(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
    return 80;
}
static int term_rows(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) return ws.ws_row;
    return 24;
}

/* Word-wrap the target into line start indices. line_start[k]..line_start[k+1]
   is line k; the array has nline+1 entries. Rebuilt whenever the width changes. */
static void relayout(const char *target, size_t len, int width, size_t *line_start, size_t *nline) {
    if (width < 10) width = 10;
    size_t n = 0, col = 0, i = 0;
    line_start[0] = 0;
    while (i < len) {
        if (target[i] != ' ' && (i == 0 || target[i - 1] == ' ')) {
            size_t wlen = 0, j = i;
            while (j < len && target[j] != ' ') { wlen++; j++; }
            if (col > 0 && col + wlen > (size_t)width) { line_start[++n] = i; col = 0; }
        }
        col++;
        i++;
        if (col >= (size_t)width && i < len) { line_start[++n] = i; col = 0; }
    }
    *nline = n + 1;
}

static void render(Buf *b, const char *target, const char *typed, size_t i, size_t len,
                   const size_t *line_start, size_t nline, int rows,
                   double live_secs, int finished) {
    size_t correct = 0;
    for (size_t j = 0; j < i; j++) if (typed[j] == target[j]) correct++;
    double mins = live_secs / 60.0;
    double wpm = mins > 0 ? ((double)correct / 5.0) / mins : 0.0;
    double acc = i > 0 ? (double)correct / (double)i * 100.0 : 100.0;
    double prog = len ? (double)i / (double)len : 0.0;

    b->len = 0;
    bputs(b, "\033[H");   /* home */

    /* header */
    bputs(b, "  \033[1mquacky\033[0m\033[2m speed test\033[0m\033[K\r\n");
    bputs(b, "  \033[2m");
    for (int k = 0; k < 44; k++) bputc_(b, '-');
    bputs(b, "\033[0m\033[K\r\n");
    bprintf(b, "  \033[93mwpm\033[0m %-6.0f \033[93macc\033[0m %-4.0f%% \033[93mt\033[0m %-6.2fs  ",
            wpm, acc, live_secs);
    int bw = 16, fill = (int)(prog * bw + 0.5);
    bputs(b, "\033[93m");
    for (int k = 0; k < bw; k++) bputs(b, k < fill ? "#" : "\033[2m.\033[93m");
    bprintf(b, "\033[0m %3.0f%%\033[K\r\n\033[K\r\n", prog * 100.0);

    /* text window: keep the caret line visible */
    int avail = rows - 6;
    if (avail < 3) avail = 3;
    size_t caret_line = 0;
    while (caret_line + 1 < nline && line_start[caret_line + 1] <= i) caret_line++;
    size_t top = 0;
    if (nline > (size_t)avail && caret_line > (size_t)(avail - 2))
        top = caret_line - (size_t)(avail - 2);
    size_t bottom = top + (size_t)avail;
    if (bottom > nline) bottom = nline;

    for (size_t ln = top; ln < bottom; ln++) {
        bputs(b, "  ");
        size_t a = line_start[ln], z = (ln + 1 < nline) ? line_start[ln + 1] : len;
        for (size_t idx = a; idx < z; idx++) {
            char ch = target[idx];
            if (idx < i) {                                   /* already typed */
                if (typed[idx] == ch) { bputs(b, "\033[0m"); bputc_(b, ch); }
                else { bputs(b, "\033[91m"); bputc_(b, ch); bputs(b, "\033[0m"); }   /* wrong: expected glyph, red */
            } else if (idx == i && !finished) {              /* caret */
                bputs(b, "\033[7m"); bputc_(b, ch == ' ' ? ' ' : ch); bputs(b, "\033[0m");
            } else {                                         /* untyped hint */
                bputs(b, "\033[2m"); bputc_(b, ch); bputs(b, "\033[0m");
            }
        }
        bputs(b, "\033[K\r\n");
    }
    bputs(b, "\033[J");   /* clear below */
}

static int run_tui(const char *target, size_t len) {
    char *typed = calloc(len, 1);   /* calloc, so a render before the first key reads defined bytes */
    size_t *line_start = malloc((len + 1) * sizeof *line_start);
    if (!typed || !line_start) { fputs("out of memory\n", stderr); return 1; }

    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_winch; sigaction(SIGWINCH, &sa, NULL);
    sa.sa_handler = on_quit;  sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);

    enter_raw(1);
    int cols = term_cols(), rows = term_rows();
    size_t nline = 0;
    relayout(target, len, cols - 4, line_start, &nline);

    Buf b = {0};
    struct timespec t0 = {0, 0}, t1 = {0, 0}, last_in = {0, 0};
    size_t i = 0;
    int started = 0, finished = 0, aborted = 0;
    char buf[4096];

    render(&b, target, typed, i, len, line_start, nline, rows, 0.0, 0);
    (void)!write(STDOUT_FILENO, b.p, b.len);

    while (!finished && !g_quit) {
        if (g_winch) {
            g_winch = 0; cols = term_cols(); rows = term_rows();
            relayout(target, len, cols - 4, line_start, &nline);
        }
        fd_set fds; FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
        struct timeval tv = { 0, 100000 };   /* 10 Hz: refresh the clock / check idle */
        int sr = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);

        if (sr > 0 && FD_ISSET(STDIN_FILENO, &fds)) {
            ssize_t r = read(STDIN_FILENO, buf, sizeof buf);
            if (r <= 0) break;
            if (!started) { t0 = now; started = 1; }
            t1 = now; last_in = now;
            for (ssize_t k = 0; k < r && i < len; k++) {
                unsigned char c = (unsigned char)buf[k];
                if (c == 3) { aborted = 1; break; }
                if (c == 127 || c == 8) { if (i > 0) i--; continue; }
                typed[i++] = (char)c;
            }
            if (aborted || i >= len) finished = 1;
        } else if (sr < 0 && errno != EINTR) {
            break;
        } else if (started && us_between(last_in, now) > IDLE_FINISH_US) {
            finished = 1;   /* input stopped; t1 already holds the last byte's time */
        }

        double live = started ? secs_between(t0, finished ? t1 : now) : 0.0;
        render(&b, target, typed, i, len, line_start, nline, rows, live, finished);
        (void)!write(STDOUT_FILENO, b.p, b.len);
    }

    restore_term();
    free(b.p); free(line_start);
    if (aborted || g_quit || !started) { printf("aborted.\n"); free(typed); return 1; }
    print_result(compute(target, typed, i, len, secs_between(t0, t1)), 0);
    free(typed);
    return 0;
}

int main(int argc, char **argv) {
    int bench = 0; const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bench") == 0 || strcmp(argv[i], "-b") == 0) bench = 1;
        else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "usage: %s [--bench] TARGET.txt\n", argv[0]); return 2;
        } else if (!path) path = argv[i];
        else { fprintf(stderr, "usage: %s [--bench] TARGET.txt\n", argv[0]); return 2; }
    }
    if (!path) { fprintf(stderr, "usage: %s [--bench] TARGET.txt\n", argv[0]); return 2; }

    size_t len;
    char *target = read_file(path, &len);
    if (len == 0) { fputs("target file is empty\n", stderr); return 1; }

    int rc = bench ? run_bench(target, len) : run_tui(target, len);
    free(target);
    return rc;
}
