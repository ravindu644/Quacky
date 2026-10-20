/* TEMPORARY Phase 2 driver. It parses a script and pretty-prints the AST so the
   parser can be exercised before the evaluator (Phase 4) and the real CLI
   (Phase 7) exist, and it carries main() so `make native` links. A syntax error
   prints a diagnostic and exits 2 before any dump. Delete this file in Phase 7
   when src/main.c arrives. */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "lex.h"
#include "parse.h"

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(66); }
    if (fseek(f, 0, SEEK_END) != 0) { perror(path); exit(66); }
    long n = ftell(f);
    if (n < 0) { perror(path); exit(66); }
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fputs("out of memory\n", stderr); exit(1); }
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

static void indent(int d) { for (int i = 0; i < d; i++) fputs("  ", stdout); }

static const char *op_sym(TokKind k) {
    switch (k) {
        case T_PLUS: return "+";   case T_MINUS: return "-";  case T_STAR: return "*";
        case T_SLASH: return "/";  case T_PERCENT: return "%"; case T_SHL: return "<<";
        case T_SHR: return ">>";   case T_LT: return "<";     case T_LE: return "<=";
        case T_GT: return ">";     case T_GE: return ">=";    case T_EQ: return "==";
        case T_NE: return "!=";    case T_AMP: return "&";    case T_PIPE: return "|";
        case T_CARET: return "^";  case T_TILDE: return "~";  case T_BANG: return "!";
        default: return tok_kind_name(k);
    }
}

static void print_expr(const Expr *e) {
    switch (e->kind) {
        case E_INT:  printf("%" PRId64, e->ival); break;
        case E_BOOL: fputs(e->bval ? "true" : "false", stdout); break;
        case E_STR:  printf("\"%.*s\"", (int)e->sval.len, e->sval.p); break;
        case E_VAR:  printf("%.*s", (int)e->name.len, e->name.p); break;
        case E_UNARY:
            printf("(%s ", op_sym(e->bin.op)); print_expr(e->bin.a); putchar(')'); break;
        case E_BINARY:
            printf("(%s ", op_sym(e->bin.op));
            print_expr(e->bin.a); putchar(' '); print_expr(e->bin.b); putchar(')'); break;
        case E_AND:
            fputs("(&& ", stdout); print_expr(e->bin.a); putchar(' '); print_expr(e->bin.b); putchar(')'); break;
        case E_OR:
            fputs("(|| ", stdout); print_expr(e->bin.a); putchar(' '); print_expr(e->bin.b); putchar(')'); break;
        case E_CALL:
            printf("%.*s(", (int)e->call.name.len, e->call.name.p);
            for (int i = 0; i < e->call.nargs; i++) {
                if (i) fputs(", ", stdout);
                print_expr(e->call.args[i]);
            }
            putchar(')'); break;
    }
}

static void print_chord(const Chord *c) {
    if (c->all) { fputs("all", stdout); return; }
    printf("mod=0x%02x", c->mod);
    for (int i = 0; i < c->n; i++) printf(" 0x%02x", c->usages[i]);
}

static void print_block(const Block *b, int d);

static void print_stmt(const Stmt *s, int d) {
    indent(d);
    switch (s->kind) {
        case S_ASSIGN:
            printf("assign %.*s = ", (int)s->assign.name.len, s->assign.name.p);
            print_expr(s->assign.value); putchar('\n'); break;
        case S_CONST:
            printf("const %.*s = ", (int)s->assign.name.len, s->assign.name.p);
            print_expr(s->assign.value); putchar('\n'); break;
        case S_IF:
            fputs("if ", stdout); print_expr(s->iff.cond); fputs(":\n", stdout);
            print_block(s->iff.then_b, d + 1);
            if (s->iff.else_s) { indent(d); fputs("else:\n", stdout); print_stmt(s->iff.else_s, d + 1); }
            break;
        case S_WHILE:
            fputs("while ", stdout); print_expr(s->loop.cond); fputs(":\n", stdout);
            print_block(s->loop.body, d + 1); break;
        case S_RETURN:
            fputs("return", stdout);
            if (s->expr) { putchar(' '); print_expr(s->expr); }
            putchar('\n'); break;
        case S_BREAK:    puts("break"); break;
        case S_CONTINUE: puts("continue"); break;
        case S_SLEEP:    fputs("sleep ", stdout);  print_expr(s->expr); putchar('\n'); break;
        case S_DELAY:    fputs("delay ", stdout);  print_expr(s->expr); putchar('\n'); break;
        case S_REPEAT:   fputs("repeat ", stdout); print_expr(s->expr); putchar('\n'); break;
        case S_CALL:     fputs("call ", stdout);   print_expr(s->expr); putchar('\n'); break;
        case S_CHORD:    fputs("chord [", stdout);   print_chord(&s->chord); puts("]"); break;
        case S_HOLD:     fputs("hold [", stdout);    print_chord(&s->chord); puts("]"); break;
        case S_RELEASE:  fputs("release [", stdout); print_chord(&s->chord); puts("]"); break;
        case S_TYPE:
            printf("type (stop=%d):\n", s->type.stop_output);
            for (int i = 0; i < s->type.nsegs; i++) {
                indent(d + 1);
                if (s->type.segs[i].kind == SEG_TEXT)
                    printf("text \"%.*s\"\n", (int)s->type.segs[i].text.len, s->type.segs[i].text.p);
                else { fputs("expr ", stdout); print_expr(s->type.segs[i].expr); putchar('\n'); }
            }
            break;
    }
}

static void print_block(const Block *b, int d) {
    for (int i = 0; i < b->n; i++) print_stmt(b->stmts[i], d);
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s FILE.qk\n", argv[0]); return 64; }

    char *src = read_file(argv[1]);
    Arena arena = {0};
    Program *prog = parse(&arena, argv[1], src);

    for (int i = 0; i < prog->nfuncs; i++) {
        const Func *f = prog->funcs[i];
        printf("func %.*s(", (int)f->name.len, f->name.p);
        for (int j = 0; j < f->nparams; j++) {
            if (j) fputs(", ", stdout);
            printf("%.*s", (int)f->params[j].len, f->params[j].p);
        }
        puts("):");
        print_block(f->body, 1);
    }
    puts("top:");
    print_block(prog->top, 1);

    arena_free(&arena);
    free(src);
    return 0;
}
