/* The command line: read a script, parse it, run it, map the result to an exit
   code (00, 06). A syntax error has already exited 2 from inside the parser
   before this file opens any output, which is the "no output on error" promise. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>

#include "eval.h"
#include "output.h"
#include "parse.h"

static const char USAGE[] =
    "usage: quacky [--hid DEV | -o FILE] [--check] [-d MS] SCRIPT.qk\n"
    "  default           write a readable transcript to stdout (a dry run)\n"
    "  --hid DEV         type for real: 8-byte HID reports to a /dev/hidgN node\n"
    "  -o FILE           write the transcript to FILE (appended)\n"
    "  --check           parse only, report the first syntax error, write nothing\n"
    "  -d MS             milliseconds per key event (default 20, 0 = no pacing)\n"
    "  --help            show this message\n";

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "quacky: cannot open %s: ", path); perror(NULL); exit(EX_NOINPUT); }
    if (fseek(f, 0, SEEK_END) != 0) { perror(path); exit(EX_NOINPUT); }
    long n = ftell(f);
    if (n < 0) { perror(path); exit(EX_NOINPUT); }
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fputs("quacky: out of memory\n", stderr); exit(1); }
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

static _Noreturn void usage_error(const char *msg) {
    if (msg) fprintf(stderr, "quacky: %s\n", msg);
    fputs(USAGE, stderr);
    exit(EX_USAGE);
}

int main(int argc, char **argv) {
    const char *script = NULL, *hid_dev = NULL, *out_file = NULL;
    bool check = false;
    int64_t tick_ms = 20;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) { fputs(USAGE, stdout); return 0; }
        else if (strcmp(a, "--check") == 0) check = true;
        else if (strcmp(a, "--hid") == 0) { if (++i >= argc) usage_error("--hid needs a device"); hid_dev = argv[i]; }
        else if (strcmp(a, "-o") == 0)    { if (++i >= argc) usage_error("-o needs a file"); out_file = argv[i]; }
        else if (strcmp(a, "-d") == 0) {
            if (++i >= argc) usage_error("-d needs a millisecond value");
            char *end; long long v = strtoll(argv[i], &end, 10);
            if (*end != '\0' || v < 0) usage_error("-d needs a non-negative integer");
            tick_ms = v;
        }
        else if (a[0] == '-' && a[1] != '\0') usage_error("unknown option");
        else { if (script) usage_error("more than one script given"); script = a; }
    }
    if (!script) usage_error("no script given");
    if (hid_dev && out_file) usage_error("--hid and -o cannot be combined");

    char *src = read_file(script);
    Arena arena = {0};
    Program *prog = parse(&arena, script, src);   /* a syntax error exits 2 from inside */

    if (check) { arena_free(&arena); free(src); return 0; }

    Output out;
    OutMode mode = hid_dev ? OUT_HID : OUT_TRANSCRIPT;
    const char *path = hid_dev ? hid_dev : out_file;   /* NULL path in transcript mode means stdout */
    if (!out_open(&out, mode, path, tick_ms)) {
        fprintf(stderr, "quacky: cannot open %s: ", path ? path : "stdout");
        perror(NULL);
        arena_free(&arena); free(src);
        /* opening the gadget node vs creating a transcript file are different failures */
        return hid_dev ? EX_IOERR : EX_CANTCREAT;
    }

    Interp I; memset(&I, 0, sizeof I);
    I.out = &out;
    Status s = run(&I, prog);

    int code = 0;
    if (s == ST_ERR) {
        print_diagnostic(script, src, I.err_line, I.err_col, I.err);
        code = 1;
    }
    out_close(&out);
    arena_free(&arena);
    free(src);
    return code;
}
