/* The frontend's shared infrastructure: the arena all AST nodes come from, and
   the diagnostic renderer both error classes use. The recursive-descent and
   Pratt parser join this file in Phase 2. */

#include "parse.h"
#include "keys.h"
#include "lex.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARENA_CHUNK (64 * 1024)

static Chunk *new_chunk(size_t cap) {
    Chunk *c = malloc(sizeof *c + cap);
    if (!c) {
        /* No recovery path: out of memory is fatal (02 section 6). */
        fputs("quacky: out of memory\n", stderr);
        exit(1);
    }
    c->next = NULL;
    c->used = 0;
    c->cap  = cap;
    return c;
}

void *arena_alloc(Arena *a, size_t n) {
    size_t align = _Alignof(max_align_t);
    n = (n + align - 1) & ~(align - 1);
    if (!a->head || a->head->cap - a->head->used < n) {
        /* An allocation larger than a chunk gets its own exact chunk. */
        Chunk *c = new_chunk(n > ARENA_CHUNK ? n : ARENA_CHUNK);
        c->next = a->head;
        a->head = c;
    }
    void *p = (char *)a->head->data + a->head->used;
    a->head->used += n;
    return p;
}

void arena_free(Arena *a) {
    Chunk *c = a->head;
    while (c) {
        Chunk *next = c->next;
        free(c);
        c = next;
    }
    a->head = NULL;
}

void print_diagnostic(const char *file, const char *src,
                      int line, int col, const char *msg) {
    fprintf(stderr, "%s:%d:%d: error: %s\n", file, line, col, msg);

    /* Walk to the start of the offending line by counting newlines. */
    const char *ls = src;
    for (int n = 1; n < line && *ls; ls++) {
        if (*ls == '\n') n++;
    }
    /* Print the line, tabs collapsed to one space so the caret lines up. */
    fputs("  ", stderr);
    for (const char *p = ls; *p && *p != '\n'; p++)
        fputc(*p == '\t' ? ' ' : *p, stderr);
    fputc('\n', stderr);

    /* The caret sits under the column, matching the two-space indent above. */
    fputs("  ", stderr);
    for (int i = 1; i < col; i++) fputc(' ', stderr);
    fputs("^\n", stderr);
}

_Noreturn void syntax_error(const char *file, const char *src,
                           int line, int col, const char *fmt, ...) {
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    print_diagnostic(file, src, line, col, msg);
    exit(2);
}


/* Phase 2: recursive-descent statements and a Pratt expression parser.
   Everything below allocates AST nodes from the arena; the only malloc is the
   scratch vectors that grow during parsing and are copied into the arena and
   freed (02 section 6). A syntax error prints and exits 2 from inside perr. */

typedef enum {
    P_NONE, P_OR, P_AND, P_BOR, P_BXOR, P_BAND,
    P_EQ, P_CMP, P_SHIFT, P_TERM, P_FACTOR, P_UNARY, P_CALL
} Prec;

typedef struct {
    Lexer       lx;
    Arena      *a;
    const char *filename;
    char       *src;
    Token       cur, prev;
    int         loop_depth;     /* >0 inside a while/for body */
    int         func_depth;     /* >0 inside a function body */
    int         expr_depth;     /* bounds expression nesting (03 section 7) */
    Func      **funcs;          /* scratch, copied to the arena at the end */
    int         nfuncs, funcs_cap;
    int         for_counter;    /* makes hidden loop-end names unique */
    /* Set only on the throwaway sub-parser for a `${...}` body, so its errors
       map back to the original source instead of the extracted snippet. */
    const char *origin_src;
    int         origin_line, origin_col;
} Parser;

static void *xrealloc(void *ptr, size_t n) {
    void *q = realloc(ptr, n);
    if (!q) { fputs("quacky: out of memory\n", stderr); exit(1); }
    return q;
}

/* Grow a scratch vector and append one element. vecp points at the vector
   pointer; the element type is whatever that vector holds (pointers or structs). */
#define PUSH(vecp, np, capp, item) do {                                   \
    if (*(np) == *(capp)) {                                               \
        *(capp) = *(capp) ? *(capp) * 2 : 8;                              \
        *(vecp) = xrealloc(*(vecp), (size_t)*(capp) * sizeof *(*(vecp))); \
    }                                                                     \
    (*(vecp))[(*(np))++] = (item);                                        \
} while (0)

static _Noreturn void perr(Parser *p, Token t, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static _Noreturn void perr(Parser *p, Token t, const char *fmt, ...) {
    char msg[256];
    va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    if (p->origin_src) {
        /* A `${...}` body is one physical line (a raw line carries no newline),
           so the snippet token is always on line 1 and only the column shifts. */
        print_diagnostic(p->filename, p->origin_src, p->origin_line,
                         p->origin_col + (t.col - 1), msg);
    } else {
        print_diagnostic(p->filename, p->src, t.line, t.col, msg);
    }
    exit(2);
}

static Str  tok_str(Token t) { return (Str){t.start, (size_t)t.len}; }
static bool str_eq(Str a, Str b) { return a.len == b.len && memcmp(a.p, b.p, a.len) == 0; }

static bool ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static bool ident_cont(char c) { return ident_start(c) || (c >= '0' && c <= '9'); }
static bool is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

static void p_advance(Parser *p) { p->prev = p->cur; p->cur = lex_next(&p->lx); }
static bool p_check(Parser *p, TokKind k) { return p->cur.kind == k; }
static bool p_match(Parser *p, TokKind k) { if (p_check(p, k)) { p_advance(p); return true; } return false; }
static void p_expect(Parser *p, TokKind k, const char *what) {
    if (!p_match(p, k)) perr(p, p->cur, "expected %s", what);
}
static void skip_seps(Parser *p) { while (p_check(p, T_NEWLINE) || p_check(p, T_SEMI)) p_advance(p); }

static bool is_closer_kw(TokKind k) {
    return k == T_FI || k == T_DONE || k == T_END || k == T_ELIF || k == T_ELSE;
}
static bool in_closers(TokKind k, const TokKind *closers, int nc) {
    for (int i = 0; i < nc; i++) if (closers[i] == k) return true;
    return false;
}
static void expect_term(Parser *p, const TokKind *closers, int nc) {
    if (p_check(p, T_NEWLINE) || p_check(p, T_SEMI) || p_check(p, T_EOF)) return;
    if (in_closers(p->cur.kind, closers, nc)) return;
    perr(p, p->cur, "expected a newline or ';' after the statement");
}

static void *arena_dup(Parser *p, const void *src, size_t bytes) {
    if (!bytes) return NULL;
    void *out = arena_alloc(p->a, bytes);
    memcpy(out, src, bytes);
    return out;
}
static Expr *mk_expr(Parser *p, ExprKind k, Token t) {
    Expr *e = arena_alloc(p->a, sizeof *e);
    memset(e, 0, sizeof *e);
    e->kind = k; e->line = t.line; e->col = t.col;
    return e;
}
static Stmt *mk_stmt(Parser *p, StmtKind k, Token t) {
    Stmt *s = arena_alloc(p->a, sizeof *s);
    memset(s, 0, sizeof *s);
    s->kind = k; s->line = t.line; s->col = t.col;
    return s;
}
static Expr *mk_bin(Parser *p, TokKind op, Expr *a, Expr *b, Token t) {
    Expr *e = mk_expr(p, E_BINARY, t);
    e->bin.op = op; e->bin.a = a; e->bin.b = b;
    return e;
}
static Token raw_at(Token raw, int off) { Token t = raw; t.col = raw.col + off; return t; }

/* forward declarations for the mutually recursive descent */
static Expr *parse_expr(Parser *p);
static Expr *parse_precedence(Parser *p, Prec prec);
static Expr *pf_literal(Parser *p);
static Expr *pf_variable(Parser *p);
static Expr *pf_grouping(Parser *p);
static Expr *pf_unary(Parser *p);
static Expr *in_binary(Parser *p, Expr *left);
static Expr *in_and(Parser *p, Expr *left);
static Expr *in_or(Parser *p, Expr *left);
static Expr *finish_call(Parser *p, Token name);
static Stmt *parse_statement(Parser *p);
static void  parse_statement_into(Parser *p, Stmt ***stmts, int *n, int *cap);
static void  desugar_for_into(Parser *p, Stmt ***stmts, int *n, int *cap);
static Block *parse_block(Parser *p, const TokKind *closers, int nc);
static Chord parse_chord_body(Parser *p, Token first);
static void  parse_raw_line(Parser *p, Token raw, Stmt *s);
static QStr *str_literal_value(Parser *p, Token t);

typedef Expr *(*PrefixFn)(Parser *);
typedef Expr *(*InfixFn)(Parser *, Expr *);
typedef struct { PrefixFn prefix; InfixFn infix; Prec prec; } ParseRule;

/* One row per token kind; sized to cover every kind so lookups never run off
   the end. Unset rows are {NULL, NULL, P_NONE}, which is what makes an
   expression stop in front of a keyword with no special casing (02 section 4.2). */
static const ParseRule rules[T_ERROR + 1] = {
    [T_INT]     = { pf_literal,  NULL,      P_NONE },
    [T_STRING]  = { pf_literal,  NULL,      P_NONE },
    [T_TRUE]    = { pf_literal,  NULL,      P_NONE },
    [T_FALSE]   = { pf_literal,  NULL,      P_NONE },
    [T_IDENT]   = { pf_variable, NULL,      P_NONE },
    [T_LPAREN]  = { pf_grouping, NULL,      P_NONE },
    [T_MINUS]   = { pf_unary,    in_binary, P_TERM },
    [T_BANG]    = { pf_unary,    NULL,      P_NONE },
    [T_TILDE]   = { pf_unary,    NULL,      P_NONE },
    [T_PLUS]    = { NULL,        in_binary, P_TERM },
    [T_STAR]    = { NULL,        in_binary, P_FACTOR },
    [T_SLASH]   = { NULL,        in_binary, P_FACTOR },
    [T_PERCENT] = { NULL,        in_binary, P_FACTOR },
    [T_SHL]     = { NULL,        in_binary, P_SHIFT },
    [T_SHR]     = { NULL,        in_binary, P_SHIFT },
    [T_LT]      = { NULL,        in_binary, P_CMP },
    [T_LE]      = { NULL,        in_binary, P_CMP },
    [T_GT]      = { NULL,        in_binary, P_CMP },
    [T_GE]      = { NULL,        in_binary, P_CMP },
    [T_EQ]      = { NULL,        in_binary, P_EQ },
    [T_NE]      = { NULL,        in_binary, P_EQ },
    [T_AMP]     = { NULL,        in_binary, P_BAND },
    [T_CARET]   = { NULL,        in_binary, P_BXOR },
    [T_PIPE]    = { NULL,        in_binary, P_BOR },
    [T_ANDAND]  = { NULL,        in_and,    P_AND },
    [T_OROR]    = { NULL,        in_or,     P_OR },
};

static Expr *parse_precedence(Parser *p, Prec prec) {
    if (++p->expr_depth > 256) perr(p, p->cur, "expression nested too deeply (max 256)");
    PrefixFn pre = rules[p->cur.kind].prefix;
    if (!pre) perr(p, p->cur, "expected an expression");
    Expr *left = pre(p);
    while (prec <= rules[p->cur.kind].prec) {
        InfixFn in = rules[p->cur.kind].infix;
        left = in(p, left);
    }
    p->expr_depth--;
    return left;
}
static Expr *parse_expr(Parser *p) { return parse_precedence(p, P_OR); }

static int64_t parse_int_token(Token t) {
    const char *s = t.start; int base = 10;
    if (t.len >= 2 && s[0] == '0' && s[1] == 'x') { base = 16; s += 2; }
    else if (t.len >= 2 && s[0] == '0' && s[1] == 'b') { base = 2; s += 2; }
    return (int64_t)strtoull(s, NULL, base);   /* range already checked by the lexer */
}
static Expr *pf_literal(Parser *p) {
    Token t = p->cur; p_advance(p);
    Expr *e = NULL;
    switch (t.kind) {
        case T_INT:    e = mk_expr(p, E_INT, t);  e->ival = parse_int_token(t); break;
        case T_STRING: e = mk_expr(p, E_STR, t);  e->sval = str_literal_value(p, t); break;
        case T_TRUE:   e = mk_expr(p, E_BOOL, t); e->bval = true; break;
        case T_FALSE:  e = mk_expr(p, E_BOOL, t); e->bval = false; break;
        default:       perr(p, t, "expected a literal");
    }
    return e;
}
static Expr *finish_call(Parser *p, Token name) {
    p_advance(p);   /* consume '(' */
    Expr **args = NULL; int n = 0, cap = 0;
    if (!p_check(p, T_RPAREN)) {
        do {
            Expr *arg = parse_expr(p);
            PUSH(&args, &n, &cap, arg);
        } while (p_match(p, T_COMMA));
    }
    p_expect(p, T_RPAREN, "')' after arguments");
    Expr *e = mk_expr(p, E_CALL, name);
    e->call.name = tok_str(name);
    e->call.args = arena_dup(p, args, (size_t)n * sizeof *args);
    e->call.nargs = n;
    free(args);
    return e;
}
static Expr *pf_variable(Parser *p) {
    Token t = p->cur; p_advance(p);
    if (p_check(p, T_LPAREN)) return finish_call(p, t);
    Expr *e = mk_expr(p, E_VAR, t);
    e->name = tok_str(t);
    return e;
}
static Expr *pf_grouping(Parser *p) {
    p_advance(p);   /* '(' */
    Expr *e = parse_expr(p);
    p_expect(p, T_RPAREN, "')' after expression");
    return e;
}
static Expr *pf_unary(Parser *p) {
    Token op = p->cur; p_advance(p);
    Expr *operand = parse_precedence(p, P_UNARY);
    Expr *e = mk_expr(p, E_UNARY, op);
    e->bin.op = op.kind; e->bin.a = operand; e->bin.b = NULL;
    return e;
}
static Expr *in_binary(Parser *p, Expr *left) {
    Token op = p->cur; p_advance(p);
    Prec prec = rules[op.kind].prec;
    Expr *right = parse_precedence(p, (Prec)(prec + 1));   /* left-associative */
    return mk_bin(p, op.kind, left, right, op);
}
static Expr *in_and(Parser *p, Expr *left) {
    Token op = p->cur; p_advance(p);
    Expr *right = parse_precedence(p, (Prec)(P_AND + 1));
    Expr *e = mk_expr(p, E_AND, op); e->bin.a = left; e->bin.b = right; return e;
}
static Expr *in_or(Parser *p, Expr *left) {
    Token op = p->cur; p_advance(p);
    Expr *right = parse_precedence(p, (Prec)(P_OR + 1));
    Expr *e = mk_expr(p, E_OR, op); e->bin.a = left; e->bin.b = right; return e;
}

/* The `type` raw line, split into text and interpolation segments (02 section 4.3). */
static void txt_push(char **txt, size_t *tn, size_t *tcap, char c) {
    if (*tn == *tcap) { *tcap = *tcap ? *tcap * 2 : 32; *txt = xrealloc(*txt, *tcap); }
    (*txt)[(*tn)++] = c;
}
static void flush_text(Parser *p, Seg **segs, int *nseg, int *segcap, char **txt, size_t *tn) {
    if (*tn == 0) return;
    Seg seg;
    seg.kind = SEG_TEXT;
    seg.text.p = arena_dup(p, *txt, *tn);
    seg.text.len = *tn;
    seg.expr = NULL;
    PUSH(segs, nseg, segcap, seg);
    *tn = 0;
}
static void seg_push_expr(Seg **segs, int *nseg, int *segcap, Expr *e) {
    Seg seg;
    seg.kind = SEG_EXPR; seg.text.p = NULL; seg.text.len = 0; seg.expr = e;
    PUSH(segs, nseg, segcap, seg);
}
static int decode_escape(Parser *p, Token raw, const char *r, int len, int i, int *adv) {
    char e = r[i];
    switch (e) {
        case '\\': *adv = 1; return '\\';
        case '"':  *adv = 1; return '"';
        case 'n':  *adv = 1; return '\n';
        case 't':  *adv = 1; return '\t';
        case 'r':  *adv = 1; return '\r';
        case 'e':  *adv = 1; return 0x1b;
        case 'a':  *adv = 1; return '\a';
        case 'b':  *adv = 1; return '\b';
        case 'v':  *adv = 1; return '\v';
        case 'f':  *adv = 1; return '\f';
        case 'x': {
            int j = i + 1, v = 0, ndig = 0;
            while (j < len && ndig < 2 && is_hex_digit(r[j])) { v = v * 16 + hexval(r[j]); j++; ndig++; }
            if (ndig == 0) perr(p, raw_at(raw, i - 1), "\\x used with no hex digits");
            *adv = j - i;
            return v & 0xff;
        }
        case '0': {
            int j = i + 1, v = 0, ndig = 0;    /* \0NNN, up to three octal digits */
            while (j < len && ndig < 3 && r[j] >= '0' && r[j] <= '7') { v = v * 8 + (r[j] - '0'); j++; ndig++; }
            *adv = j - i;
            return v & 0xff;
        }
        default:
            perr(p, raw_at(raw, i - 1), "unknown escape sequence");
    }
}
/* A string literal's value: the same escape set as a `type` line (no `$`
   interpolation, no `\c`), decoded once into an immortal arena QStr (04 §2), so
   the evaluator wraps it with no copy and no refcount. The lexer guarantees a
   byte always follows a backslash, so decode_escape never reads past the slice.
   Decoded text is no longer than the source, so one arena QStr sized to the raw
   length holds it; the few bytes an escape saves are left unused. */
static QStr *str_literal_value(Parser *p, Token t) {
    QStr *q = arena_alloc(p->a, sizeof *q + (size_t)t.len);
    q->rc = -1;
    size_t n = 0;
    Token esc = t; esc.col = t.col + 1;   /* maps a slice offset to the inner column, past the quote */
    for (int i = 0; i < t.len; ) {
        if (t.start[i] == '\\') {
            int adv;
            int byte = decode_escape(p, esc, t.start, t.len, i + 1, &adv);
            q->data[n++] = (char)byte;
            i += 1 + adv;
        } else {
            q->data[n++] = t.start[i++];
        }
    }
    q->len = n;
    return q;
}
static Expr *subparse_expr(Parser *parent, Token raw, int at, const char *inner, int innerlen) {
    /* Copy the ${...} body to a NUL-terminated arena buffer and run a fresh
       lexer+parser over it. origin_* makes errors inside the body report at the
       real source column: the body starts `at` bytes into the raw slice, whose
       first byte is at raw.col, so column raw.col + at. */
    char *buf = arena_alloc(parent->a, (size_t)innerlen + 1);
    memcpy(buf, inner, (size_t)innerlen);
    buf[innerlen] = '\0';
    Parser sp;
    memset(&sp, 0, sizeof sp);
    sp.a = parent->a; sp.filename = parent->filename; sp.src = buf;
    sp.origin_src = parent->src; sp.origin_line = raw.line; sp.origin_col = raw.col + at;
    sp.lx.filename = parent->filename; sp.lx.src = buf; sp.lx.p = buf;
    sp.lx.line = 1; sp.lx.col = 1; sp.lx.after_type = false;
    sp.cur = lex_next(&sp.lx);
    Expr *e = parse_expr(&sp);
    if (!p_check(&sp, T_EOF)) perr(&sp, sp.cur, "unexpected text after the expression in '${...}'");
    return e;
}
static void parse_raw_line(Parser *p, Token raw, Stmt *s) {
    const char *r = raw.start;
    int len = raw.len;
    Seg *segs = NULL; int nseg = 0, segcap = 0;
    char *txt = NULL; size_t tn = 0, tcap = 0;
    bool stop = false;

    for (int i = 0; i < len; ) {
        char ch = r[i];
        if (ch == '\\') {
            if (i + 1 >= len) perr(p, raw_at(raw, i), "trailing backslash in a 'type' line");
            char e = r[i + 1];
            if (e == 'c') { stop = true; break; }               /* \c stops output */
            if (e == '$') { txt_push(&txt, &tn, &tcap, '$'); i += 2; continue; }
            int adv;
            int byte = decode_escape(p, raw, r, len, i + 1, &adv);
            txt_push(&txt, &tn, &tcap, (char)byte);
            i += 1 + adv;
            continue;
        }
        if (ch == '$') {
            if (i + 1 < len && ident_start(r[i + 1])) {
                flush_text(p, &segs, &nseg, &segcap, &txt, &tn);
                int j = i + 1; while (j < len && ident_cont(r[j])) j++;
                Str nm = { arena_dup(p, r + i + 1, (size_t)(j - i - 1)), (size_t)(j - i - 1) };
                Expr *ve = mk_expr(p, E_VAR, raw_at(raw, i));
                ve->name = nm;
                seg_push_expr(&segs, &nseg, &segcap, ve);
                i = j; continue;
            }
            if (i + 1 < len && r[i + 1] == '{') {
                flush_text(p, &segs, &nseg, &segcap, &txt, &tn);
                int depth = 1, j = i + 2;
                while (j < len && depth > 0) {
                    if (r[j] == '{') depth++;
                    else if (r[j] == '}') { depth--; if (depth == 0) break; }
                    j++;
                }
                if (depth != 0) perr(p, raw_at(raw, i), "unbalanced '${' in a 'type' line");
                int innerlen = j - (i + 2);
                Expr *e = subparse_expr(p, raw, i + 2, r + i + 2, innerlen);
                seg_push_expr(&segs, &nseg, &segcap, e);
                i = j + 1; continue;
            }
            txt_push(&txt, &tn, &tcap, '$'); i++; continue;    /* lone $ is literal */
        }
        txt_push(&txt, &tn, &tcap, ch); i++;
    }

    if (!stop) txt_push(&txt, &tn, &tcap, '\n');   /* output is the text plus a newline */
    flush_text(p, &segs, &nseg, &segcap, &txt, &tn);

    s->type.segs = arena_dup(p, segs, (size_t)nseg * sizeof *segs);
    s->type.nsegs = nseg;
    s->type.stop_output = stop;
    free(segs);
    free(txt);
}

/* Chords (02 section 4.4): each key resolves through keys.c at parse time. */
static void chord_add_usage(Parser *p, Chord *c, Token k, uint8_t usage) {
    if (c->n >= 6) perr(p, k, "too many keys in a chord (max 6)");
    c->usages[c->n++] = usage;
}
static void resolve_key(Parser *p, Token k, Chord *c) {
    if (k.kind == T_IDENT) {
        KeyRef r = key_lookup(tok_str(k));
        if (r.kind == KEY_MODIFIER) { c->mod = (uint8_t)(c->mod | r.value); return; }
        if (r.kind == KEY_NAMED)    { chord_add_usage(p, c, k, r.value); return; }
        if (k.len == 1) {
            CharKey ck = char_key((unsigned char)k.start[0]);
            if (ck.ok) { c->mod = (uint8_t)(c->mod | ck.mod); chord_add_usage(p, c, k, ck.usage); return; }
        }
    } else if (k.kind == T_INT && k.len == 1) {
        CharKey ck = char_key((unsigned char)k.start[0]);
        if (ck.ok) { c->mod = (uint8_t)(c->mod | ck.mod); chord_add_usage(p, c, k, ck.usage); return; }
    }
    perr(p, k, "unknown key '%.*s'", (int)k.len, k.start);
}
static bool is_key_ident(Token t) {
    if (t.kind != T_IDENT) return false;
    if (key_lookup(tok_str(t)).kind != KEY_NONE) return true;
    if (t.len == 1 && char_key((unsigned char)t.start[0]).ok) return true;
    return false;
}
static Chord parse_chord_body(Parser *p, Token first) {
    Chord c; memset(&c, 0, sizeof c);
    Token k = first;                       /* `first` was already consumed by the caller */
    for (;;) {
        resolve_key(p, k, &c);
        if (!p_match(p, T_PLUS)) break;
        k = p->cur; p_advance(p);
    }
    /* The source span from the first key to the last, for the transcript tag. */
    c.text = (Str){ first.start, (size_t)((k.start + k.len) - first.start) };
    return c;
}

static Stmt *parse_const(Parser *p) {
    Token t = p->cur; p_advance(p);
    if (!p_check(p, T_IDENT)) perr(p, p->cur, "expected a name after 'const'");
    Token name = p->cur; p_advance(p);
    p_expect(p, T_ASSIGN, "'=' in a const declaration");
    Stmt *s = mk_stmt(p, S_CONST, t);
    s->assign.name = tok_str(name);
    s->assign.value = parse_expr(p);
    return s;
}
static Stmt *parse_expr_stmt(Parser *p, StmtKind k) {
    Token t = p->cur; p_advance(p);
    Stmt *s = mk_stmt(p, k, t);
    s->expr = parse_expr(p);
    return s;
}
static Stmt *parse_return(Parser *p) {
    Token t = p->cur;
    if (p->func_depth == 0) perr(p, t, "'return' outside a function");
    p_advance(p);
    Stmt *s = mk_stmt(p, S_RETURN, t);
    if (p_check(p, T_NEWLINE) || p_check(p, T_SEMI) || p_check(p, T_EOF) || is_closer_kw(p->cur.kind))
        s->expr = NULL;
    else
        s->expr = parse_expr(p);
    return s;
}
static Stmt *parse_if_chain(Parser *p, Token at) {
    Expr *cond = parse_expr(p);
    p_expect(p, T_THEN, "'then'");
    static const TokKind cl[] = { T_ELIF, T_ELSE, T_FI };
    Block *then_b = parse_block(p, cl, 3);
    Stmt *s = mk_stmt(p, S_IF, at);
    s->iff.cond = cond; s->iff.then_b = then_b; s->iff.else_s = NULL;
    if (p_check(p, T_ELIF)) {
        Token et = p->cur; p_advance(p);
        s->iff.else_s = parse_if_chain(p, et);
    } else if (p_check(p, T_ELSE)) {
        Token et = p->cur; p_advance(p);
        static const TokKind cl2[] = { T_FI };
        Block *eb = parse_block(p, cl2, 1);
        /* a bare else becomes a final if-true so else_s is always an S_IF chain */
        Stmt *es = mk_stmt(p, S_IF, et);
        Expr *tru = mk_expr(p, E_BOOL, et); tru->bval = true;
        es->iff.cond = tru; es->iff.then_b = eb; es->iff.else_s = NULL;
        s->iff.else_s = es;
    }
    return s;
}
static Stmt *parse_if(Parser *p) {
    Token t = p->cur; p_advance(p);
    Stmt *s = parse_if_chain(p, t);
    p_expect(p, T_FI, "'fi'");
    return s;
}
static Stmt *parse_while(Parser *p) {
    Token t = p->cur; p_advance(p);
    Expr *cond = parse_expr(p);
    p_expect(p, T_DO, "'do'");
    static const TokKind cl[] = { T_DONE };
    p->loop_depth++;
    Block *body = parse_block(p, cl, 1);
    p->loop_depth--;
    p_expect(p, T_DONE, "'done'");
    Stmt *s = mk_stmt(p, S_WHILE, t);
    s->loop.cond = cond; s->loop.body = body;
    return s;
}
static Stmt *parse_hold(Parser *p) {
    Token t = p->cur; p_advance(p);
    if (!p_check(p, T_IDENT) && !p_check(p, T_INT)) perr(p, p->cur, "expected a key after 'hold'");
    Token first = p->cur; p_advance(p);
    Stmt *s = mk_stmt(p, S_HOLD, t);
    s->chord = parse_chord_body(p, first);
    return s;
}
static Stmt *parse_release(Parser *p) {
    Token t = p->cur; p_advance(p);
    Stmt *s = mk_stmt(p, S_RELEASE, t);
    if (p_check(p, T_IDENT) && p->cur.len == 3 && memcmp(p->cur.start, "all", 3) == 0) {
        p_advance(p);
        s->chord.all = true;        /* mod and n already zero from mk_stmt */
        return s;
    }
    if (!p_check(p, T_IDENT) && !p_check(p, T_INT)) perr(p, p->cur, "expected a key or 'all' after 'release'");
    Token first = p->cur; p_advance(p);
    s->chord = parse_chord_body(p, first);
    return s;
}
static Stmt *parse_type(Parser *p) {
    Token t = p->cur; p_advance(p);
    if (!p_check(p, T_RAW_LINE)) perr(p, p->cur, "expected the text of a 'type' line");
    Token raw = p->cur; p_advance(p);
    Stmt *s = mk_stmt(p, S_TYPE, t);
    parse_raw_line(p, raw, s);
    return s;
}
static Stmt *parse_ident_stmt(Parser *p) {
    Token name = p->cur; p_advance(p);     /* consume the identifier, then dispatch */
    if (p_check(p, T_ASSIGN)) {
        p_advance(p);
        Stmt *s = mk_stmt(p, S_ASSIGN, name);
        s->assign.name = tok_str(name);
        s->assign.value = parse_expr(p);
        return s;
    }
    if (p_check(p, T_LPAREN)) {
        Stmt *s = mk_stmt(p, S_CALL, name);
        s->expr = finish_call(p, name);
        return s;
    }
    if (is_key_ident(name)) {
        Stmt *s = mk_stmt(p, S_CHORD, name);
        s->chord = parse_chord_body(p, name);
        return s;
    }
    perr(p, name, "expected assignment or call");
}
static Stmt *parse_statement(Parser *p) {
    Token t = p->cur;
    switch (t.kind) {
        case T_CONST:    return parse_const(p);
        case T_IF:       return parse_if(p);
        case T_WHILE:    return parse_while(p);
        case T_FUNC:     perr(p, t, "functions must be top level");
        case T_RETURN:   return parse_return(p);
        case T_BREAK:    if (p->loop_depth == 0) perr(p, t, "'break' outside a loop");
                         p_advance(p); return mk_stmt(p, S_BREAK, t);
        case T_CONTINUE: if (p->loop_depth == 0) perr(p, t, "'continue' outside a loop");
                         p_advance(p); return mk_stmt(p, S_CONTINUE, t);
        case T_SLEEP:    return parse_expr_stmt(p, S_SLEEP);
        case T_DELAY:    return parse_expr_stmt(p, S_DELAY);
        case T_REPEAT:   return parse_expr_stmt(p, S_REPEAT);
        case T_HOLD:     return parse_hold(p);
        case T_RELEASE:  return parse_release(p);
        case T_TYPE:     return parse_type(p);
        case T_IDENT:    return parse_ident_stmt(p);
        default:         perr(p, t, "expected a statement");
    }
}
static void desugar_for_into(Parser *p, Stmt ***stmts, int *n, int *cap) {
    Token t = p->cur; p_advance(p);   /* 'for' */
    if (!p_check(p, T_IDENT)) perr(p, p->cur, "expected a loop variable after 'for'");
    Token var = p->cur; p_advance(p);
    p_expect(p, T_IN, "'in'");
    Expr *lo = parse_expr(p);
    p_expect(p, T_DOTDOT, "'..'");
    Expr *hi = parse_expr(p);
    p_expect(p, T_DO, "'do'");
    static const TokKind cl[] = { T_DONE };
    p->loop_depth++;
    Block *body = parse_block(p, cl, 1);
    p->loop_depth--;
    p_expect(p, T_DONE, "'done'");

    char buf[32];
    int ln = snprintf(buf, sizeof buf, "__for_end_%d", p->for_counter++);
    if (ln < 0) ln = 0;
    Str endstr = { arena_dup(p, buf, (size_t)ln + 1), (size_t)ln };

    Stmt *init = mk_stmt(p, S_ASSIGN, t);
    init->assign.name = tok_str(var); init->assign.value = lo;
    Stmt *setend = mk_stmt(p, S_ASSIGN, t);
    setend->assign.name = endstr; setend->assign.value = hi;

    Expr *vref = mk_expr(p, E_VAR, var); vref->name = tok_str(var);
    Expr *eref = mk_expr(p, E_VAR, t);   eref->name = endstr;
    Expr *cond = mk_bin(p, T_LE, vref, eref, t);

    Expr *vref2 = mk_expr(p, E_VAR, var); vref2->name = tok_str(var);
    Expr *one = mk_expr(p, E_INT, var);   one->ival = 1;
    Stmt *incs = mk_stmt(p, S_ASSIGN, t);
    incs->assign.name = tok_str(var); incs->assign.value = mk_bin(p, T_PLUS, vref2, one, t);

    int bn = body->n;                 /* append `v = v + 1` to the loop body */
    Stmt **bs = arena_alloc(p->a, (size_t)(bn + 1) * sizeof *bs);
    if (bn) memcpy(bs, body->stmts, (size_t)bn * sizeof *bs);
    bs[bn] = incs;
    Block *body2 = arena_alloc(p->a, sizeof *body2);
    body2->stmts = bs; body2->n = bn + 1;

    Stmt *wh = mk_stmt(p, S_WHILE, t);
    wh->loop.cond = cond; wh->loop.body = body2;

    PUSH(stmts, n, cap, init);
    PUSH(stmts, n, cap, setend);
    PUSH(stmts, n, cap, wh);
}
static void parse_statement_into(Parser *p, Stmt ***stmts, int *n, int *cap) {
    if (p_check(p, T_FOR)) { desugar_for_into(p, stmts, n, cap); return; }
    Stmt *s = parse_statement(p);
    PUSH(stmts, n, cap, s);
}
static Block *parse_block(Parser *p, const TokKind *closers, int nc) {
    Stmt **stmts = NULL; int n = 0, cap = 0;
    skip_seps(p);
    while (!p_check(p, T_EOF) && !in_closers(p->cur.kind, closers, nc)) {
        parse_statement_into(p, &stmts, &n, &cap);
        expect_term(p, closers, nc);
        skip_seps(p);
    }
    Block *b = arena_alloc(p->a, sizeof *b);
    b->stmts = arena_dup(p, stmts, (size_t)n * sizeof *stmts);
    b->n = n;
    free(stmts);
    return b;
}
static void push_func(Parser *p, Func *f) {
    if (p->nfuncs == p->funcs_cap) {
        p->funcs_cap = p->funcs_cap ? p->funcs_cap * 2 : 8;
        p->funcs = xrealloc(p->funcs, (size_t)p->funcs_cap * sizeof *p->funcs);
    }
    p->funcs[p->nfuncs++] = f;
}
static void parse_func(Parser *p) {
    p_advance(p);   /* 'func' */
    if (!p_check(p, T_IDENT)) perr(p, p->cur, "expected a function name");
    Token name = p->cur; p_advance(p);

    Str *params = NULL; int np = 0, pcap = 0;
    while (p_check(p, T_IDENT)) {
        Str s = tok_str(p->cur); p_advance(p);
        PUSH(&params, &np, &pcap, s);
    }
    if (!(p_check(p, T_NEWLINE) || p_check(p, T_SEMI)))
        perr(p, p->cur, "expected a newline before the function body");

    static const TokKind cl[] = { T_END };
    int saved_loop = p->loop_depth;
    p->loop_depth = 0;                /* break/continue cannot cross a function boundary */
    p->func_depth++;
    Block *body = parse_block(p, cl, 1);
    p->func_depth--;
    p->loop_depth = saved_loop;
    p_expect(p, T_END, "'end' to close the function");

    for (int i = 0; i < p->nfuncs; i++)
        if (str_eq(p->funcs[i]->name, tok_str(name)))
            perr(p, name, "function '%.*s' already declared", (int)name.len, name.start);

    Func *f = arena_alloc(p->a, sizeof *f);
    f->name = tok_str(name);
    f->params = arena_dup(p, params, (size_t)np * sizeof *params);
    f->nparams = np;
    f->body = body;
    free(params);
    push_func(p, f);
}

Program *parse(Arena *a, const char *filename, char *src) {
    Parser p;
    memset(&p, 0, sizeof p);
    p.a = a; p.filename = filename; p.src = src;
    lex_init(&p.lx, filename, src);
    p.cur = lex_next(&p.lx);

    Stmt **stmts = NULL; int n = 0, cap = 0;
    skip_seps(&p);
    while (!p_check(&p, T_EOF)) {
        if (p_check(&p, T_FUNC)) parse_func(&p);
        else                     parse_statement_into(&p, &stmts, &n, &cap);
        expect_term(&p, NULL, 0);   /* top level: a separator or EOF ends a statement */
        skip_seps(&p);
    }

    Block *top = arena_alloc(a, sizeof *top);
    top->stmts = arena_dup(&p, stmts, (size_t)n * sizeof *stmts);
    top->n = n;
    free(stmts);

    Program *prog = arena_alloc(a, sizeof *prog);
    prog->top = top;
    prog->funcs = arena_dup(&p, p.funcs, (size_t)p.nfuncs * sizeof *p.funcs);
    prog->nfuncs = p.nfuncs;
    free(p.funcs);
    return prog;
}
