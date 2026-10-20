/* qtype: a raw-terminal typing-speed meter, the console answer to a browser
   typing test. It reads a target .txt, puts the tty in raw mode, then drains
   stdin as fast as the kernel delivers it, timing the first byte to the last.
   It does NOT echo during capture: echoing means the terminal renders every
   key, which is exactly the per-keystroke cost that makes a browser the
   bottleneck. So this measures how fast input actually arrives, which is what
   you want when a USB HID gadget is doing the typing.

   Build:  cc -O2 -Wall -Wextra -o qtype qtype.c
   Use:    ./qtype target.txt     # focus this terminal, then let the gadget type
*/

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static struct termios g_saved;
static int g_raw = 0;

static void restore_tty(void) {
    if (g_raw) { tcsetattr(STDIN_FILENO, TCSANOW, &g_saved); g_raw = 0; }
}

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
    /* A trailing newline in the file is not part of the text to type. */
    while (got > 0 && (buf[got - 1] == '\n' || buf[got - 1] == '\r')) buf[--got] = '\0';
    *out_len = got;
    return buf;
}

static double secs_between(struct timespec a, struct timespec b) {
    return (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) / 1e9;
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s TARGET.txt\n", argv[0]); return 2; }

    size_t len;
    char *target = read_file(argv[1], &len);
    if (len == 0) { fputs("target file is empty\n", stderr); return 1; }
    char *typed = malloc(len);
    if (!typed) { fputs("out of memory\n", stderr); return 1; }

    struct termios raw;
    if (tcgetattr(STDIN_FILENO, &g_saved) != 0) { perror("tcgetattr (is stdin a terminal?)"); return 1; }
    atexit(restore_tty);
    raw = g_saved;
    /* Raw: no line buffering, no echo, no signal keys. VMIN=1 so read returns as
       soon as a byte is there; we handle Ctrl-C (byte 3) ourselves. */
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG | IEXTEN);
    raw.c_iflag &= (tcflag_t)~(IXON | ICRNL | INPCK | ISTRIP);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) { perror("tcsetattr"); return 1; }
    g_raw = 1;

    printf("qtype: %zu characters to read. Focus this window; input is captured silently.\r\n", len);
    printf("Target:\r\n\033[2m%s\033[0m\r\n\r\n", target);
    printf("waiting for input (Ctrl-C to abort)...\r\n");
    fflush(stdout);

    struct timespec t0 = {0, 0}, t1 = {0, 0};
    size_t i = 0;
    int started = 0, aborted = 0;
    char buf[4096];

    /* Block for the first byte, then finish when input has been idle for 400 ms,
       so a run ends cleanly even if the host dropped the last few characters at
       high speed (otherwise a count-based loop would wait forever). t1 tracks the
       last byte's arrival, never the idle wait, so elapsed stays accurate. */
    while (i < len) {
        fd_set fds; FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
        struct timeval tv = { 0, 400000 };
        int sr = select(STDIN_FILENO + 1, &fds, NULL, NULL, started ? &tv : NULL);
        if (sr == 0) break;                      /* idle past the timeout: done */
        if (sr < 0) { if (errno == EINTR) continue; break; }

        ssize_t r = read(STDIN_FILENO, buf, sizeof buf);
        if (r <= 0) break;
        if (!started) { clock_gettime(CLOCK_MONOTONIC, &t0); started = 1; }
        clock_gettime(CLOCK_MONOTONIC, &t1);     /* last activity, not the idle wait */
        for (ssize_t k = 0; k < r && i < len; k++) {
            unsigned char c = (unsigned char)buf[k];
            if (c == 3) { aborted = 1; break; }          /* Ctrl-C */
            if (c == 127 || c == 8) { if (i > 0) i--; continue; }  /* backspace, for a human */
            typed[i++] = (char)c;
        }
        if (aborted) break;
    }
    restore_tty();

    if (aborted || !started) { printf("\naborted.\n"); return 1; }

    size_t correct = 0;
    for (size_t j = 0; j < i; j++) if (typed[j] == target[j]) correct++;

    double secs = secs_between(t0, t1);
    double mins = secs / 60.0;
    double net_wpm = mins > 0 ? ((double)correct / 5.0) / mins : 0.0;
    double raw_wpm = mins > 0 ? ((double)i / 5.0) / mins : 0.0;
    double cps = secs > 0 ? (double)i / secs : 0.0;
    double acc = i > 0 ? (double)correct / (double)i * 100.0 : 0.0;

    printf("\n");
    printf("  chars read : %zu / %zu\n", i, len);
    printf("  correct    : %zu  (%.2f%% accuracy)\n", correct, acc);
    printf("  elapsed    : %.4f s\n", secs);
    printf("  chars/sec  : %.0f\n", cps);
    printf("  net  WPM   : %.0f\n", net_wpm);
    printf("  raw  WPM   : %.0f\n", raw_wpm);
    if (i < len) printf("  note: %zu characters never arrived (dropped by the host at this speed)\n", len - i);

    free(target); free(typed);
    return 0;
}
