/* The evaluator (03). A recursive tree-walker: eval_expr writes an owned Value
   through an out-parameter, every evaluator returns a Status, and the non-OK
   codes (ERR/RETURN/BREAK/CONTINUE) travel up the C stack until a loop, a call,
   or main consumes them. String ownership follows the five rules in 04 §2. */

#include "eval.h"
#include "textfile.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Status rt_error(Interp *I, int line, int col, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(I->err, sizeof I->err, fmt, ap);
    va_end(ap);
    I->err_line = line; I->err_col = col;
    return ST_ERR;
}

static const char *vt_name(ValType t) {
    switch (t) {
        case VAL_INT:  return "int";
        case VAL_BOOL: return "bool";
        case VAL_STR:  return "str";
        default:       return "none";
    }
}
static const char *op_sym(TokKind k) {
    switch (k) {
        case T_PLUS: return "+";  case T_MINUS: return "-"; case T_STAR: return "*";
        case T_SLASH: return "/"; case T_PERCENT: return "%"; case T_SHL: return "<<";
        case T_SHR: return ">>";  case T_LT: return "<";   case T_LE: return "<=";
        case T_GT: return ">";    case T_GE: return ">=";  case T_EQ: return "==";
        case T_NE: return "!=";   case T_AMP: return "&";  case T_PIPE: return "|";
        case T_CARET: return "^"; case T_TILDE: return "~"; case T_BANG: return "!";
        default: return "?";
    }
}

/* Frames: a 64-bucket chained hash map, FNV-1a over the name bytes. */
static size_t hash_name(Str n) {
    uint64_t h = 1469598103934665603ULL;   /* FNV-1a offset basis (64-bit even where size_t is 32) */
    for (size_t i = 0; i < n.len; i++) { h ^= (unsigned char)n.p[i]; h *= 1099511628211ULL; }
    return (size_t)(h & 63u);
}
static Frame *new_frame(Frame *parent) {
    Frame *f = calloc(1, sizeof *f);
    if (!f) { fputs("quacky: out of memory\n", stderr); exit(1); }
    f->parent = parent;
    return f;
}
static void free_frame(Frame *f) {
    for (int b = 0; b < 64; b++) {
        for (Var *v = f->buckets[b]; v; ) {
            Var *next = v->next;
            val_release(v->val);
            free(v);
            v = next;
        }
    }
    free(f);
}
static Var *find(Frame *f, Str name) {
    for (Var *v = f->buckets[hash_name(name)]; v; v = v->next)
        if (v->name.len == name.len && memcmp(v->name.p, name.p, name.len) == 0) return v;
    return NULL;
}
static void insert(Frame *f, Str name, Value val, bool is_const) {
    Var *v = malloc(sizeof *v);
    if (!v) { fputs("quacky: out of memory\n", stderr); exit(1); }
    v->name = name; v->val = val; v->is_const = is_const;
    val_retain(v->val);                 /* the variable holds its own reference (04 rule 2) */
    size_t b = hash_name(name);
    v->next = f->buckets[b];
    f->buckets[b] = v;
}

/* Forward declarations for the mutual recursion. */
static Status eval_expr (Interp *I, Expr *e, Value *out);
static Status eval_value(Interp *I, Expr *e, Value *out);   /* eval_expr + reject a no-value call */
static Status exec_block(Interp *I, Block *b);
static Status exec_stmt (Interp *I, Stmt *s);

static Var *lookup(Interp *I, Str name) {
    Var *v = find(I->frame, name);
    if (!v && I->frame != I->globals) v = find(I->globals, name);
    return v;
}

/* value passed in is owned by the caller; the variable takes its own reference. */
static Status do_assign(Interp *I, Stmt *s, Value val) {
    Var *v = lookup(I, s->assign.name);
    if (v && v->is_const)
        return rt_error(I, s->line, s->col, "cannot assign to constant '%.*s'",
                        (int)s->assign.name.len, s->assign.name.p);
    if (v) { val_retain(val); val_release(v->val); v->val = val; }
    else insert(I->frame, s->assign.name, val, false);
    return ST_OK;
}
static Status do_const(Interp *I, Stmt *s, Value val) {
    if (find(I->frame, s->assign.name))
        return rt_error(I, s->line, s->col, "constant '%.*s' already declared",
                        (int)s->assign.name.len, s->assign.name.p);
    insert(I->frame, s->assign.name, val, true);
    return ST_OK;
}

static Func *find_func(Interp *I, Str name) {
    for (int i = 0; i < I->nfuncs; i++)
        if (I->funcs[i]->name.len == name.len &&
            memcmp(I->funcs[i]->name.p, name.p, name.len) == 0) return I->funcs[i];
    return NULL;
}

static Status call_function(Interp *I, Func *f, Value *args, int nargs, Value *out, int line, int col) {
    if (nargs != f->nparams)
        return rt_error(I, line, col, "function '%.*s' expects %d arguments, got %d",
                        (int)f->name.len, f->name.p, f->nparams, nargs);
    if (++I->depth > 200) { I->depth--; return rt_error(I, line, col, "recursion too deep (limit 200)"); }

    Frame *fr = new_frame(I->globals);              /* parent is globals, not the caller: two-level scope */
    for (int i = 0; i < nargs; i++) insert(fr, f->params[i], args[i], false);

    /* `repeat` state is per frame: an action recorded inside the body names this
       frame's locals, so it must not leak out to a `repeat` in the caller. */
    Frame *saved_frame = I->frame; I->frame = fr;
    Stmt *saved_action = I->last_action; I->last_action = NULL;
    Value saved_ret = I->ret; I->ret = (Value){ VAL_NONE, .i = 0 };
    Status s = exec_block(I, f->body);
    I->frame = saved_frame;
    I->last_action = saved_action;

    Value result = (s == ST_RETURN) ? I->ret : (Value){ VAL_NONE, .i = 0 };
    I->ret = saved_ret;
    free_frame(fr);
    I->depth--;

    assert(s == ST_OK || s == ST_RETURN || s == ST_ERR);   /* break/continue can't escape a func body */
    if (s == ST_ERR) return ST_ERR;
    *out = result;
    return ST_OK;
}

static Status eval_call(Interp *I, Expr *e, Value *out) {
    Func *f = find_func(I, e->call.name);
    if (!f) return rt_error(I, e->line, e->col, "undefined function '%.*s'",
                            (int)e->call.name.len, e->call.name.p);
    int n = e->call.nargs;
    Value *args = n ? malloc((size_t)n * sizeof *args) : NULL;
    if (n && !args) { fputs("quacky: out of memory\n", stderr); exit(1); }
    Status s = ST_OK;
    int got = 0;
    for (; got < n; got++) {
        s = eval_value(I, e->call.args[got], &args[got]);   /* a no-value arg is an error here */
        if (s != ST_OK) break;
    }
    if (s == ST_OK) s = call_function(I, f, args, n, out, e->line, e->col);
    for (int i = 0; i < got; i++) val_release(args[i]);     /* insert() retained them; drop our refs */
    free(args);
    return s;
}

static Status eval_binary(Interp *I, Expr *e, Value *out) {
    Value a, b;
    Status s = eval_value(I, e->bin.a, &a); if (s != ST_OK) return s;
    s = eval_value(I, e->bin.b, &b); if (s != ST_OK) { val_release(a); return s; }
    TokKind op = e->bin.op;

    /* `+` with a string on either side concatenates, converting the other side. */
    if (op == T_PLUS && (a.type == VAL_STR || b.type == VAL_STR)) {
        QStr *sa = to_string(a), *sb = to_string(b);
        *out = (Value){ VAL_STR, .s = str_concat(sa, sb) };
        str_release(sa); str_release(sb);
        val_release(a); val_release(b);
        return ST_OK;
    }

    switch (op) {
        case T_PLUS: case T_MINUS: case T_STAR: case T_SLASH: case T_PERCENT:
        case T_SHL: case T_SHR: case T_AMP: case T_PIPE: case T_CARET: {
            if (a.type != VAL_INT || b.type != VAL_INT) {
                val_release(a); val_release(b);
                return rt_error(I, e->line, e->col, "operator '%s' expects int and int, got %s and %s",
                                op_sym(op), vt_name(a.type), vt_name(b.type));
            }
            int64_t r; const char *err = NULL;
            Status is = int_op(op, a.i, b.i, &r, &err);
            val_release(a); val_release(b);
            if (is != ST_OK) return rt_error(I, e->line, e->col, "%s", err);
            *out = (Value){ VAL_INT, .i = r };
            return ST_OK;
        }
        case T_LT: case T_LE: case T_GT: case T_GE: {
            bool ok_types = (a.type == VAL_INT && b.type == VAL_INT) ||
                            (a.type == VAL_STR && b.type == VAL_STR);
            if (!ok_types) {
                val_release(a); val_release(b);
                return rt_error(I, e->line, e->col, "operator '%s' expects int and int, got %s and %s",
                                op_sym(op), vt_name(a.type), vt_name(b.type));
            }
            int c; const char *err = NULL;
            compare(a, b, &c, &err);                 /* types validated above, cannot fail */
            val_release(a); val_release(b);
            bool r = (op == T_LT) ? c < 0 : (op == T_LE) ? c <= 0 : (op == T_GT) ? c > 0 : c >= 0;
            *out = (Value){ VAL_BOOL, .b = r };
            return ST_OK;
        }
        case T_EQ: case T_NE: {
            bool eq = equal(a, b);
            val_release(a); val_release(b);
            *out = (Value){ VAL_BOOL, .b = (op == T_EQ) ? eq : !eq };
            return ST_OK;
        }
        default:
            assert(0 && "unknown binary operator");
            val_release(a); val_release(b);
            return rt_error(I, e->line, e->col, "internal: bad binary operator");
    }
}

static Status eval_unary(Interp *I, Expr *e, Value *out) {
    Value a; Status s = eval_value(I, e->bin.a, &a); if (s != ST_OK) return s;
    TokKind op = e->bin.op;
    if (op == T_BANG) {
        if (a.type != VAL_BOOL) { val_release(a);
            return rt_error(I, e->line, e->col, "operator '!' expects bool, got %s", vt_name(a.type)); }
        *out = (Value){ VAL_BOOL, .b = !a.b };
    } else {   /* '-' or '~', both int */
        if (a.type != VAL_INT) { val_release(a);
            return rt_error(I, e->line, e->col, "operator '%s' expects int, got %s", op_sym(op), vt_name(a.type)); }
        *out = (Value){ VAL_INT, .i = (op == T_MINUS) ? int_neg(a.i) : ~a.i };
    }
    val_release(a);
    return ST_OK;
}

static Status eval_logical(Interp *I, Expr *e, Value *out) {
    Value a; Status s = eval_value(I, e->bin.a, &a); if (s != ST_OK) return s;
    if (a.type != VAL_BOOL) { val_release(a);
        return rt_error(I, e->line, e->col, "condition must be bool, got %s", vt_name(a.type)); }
    bool left = a.b; val_release(a);
    /* short-circuit: && stops on false, || stops on true */
    if (e->kind == E_AND && !left) { *out = (Value){ VAL_BOOL, .b = false }; return ST_OK; }
    if (e->kind == E_OR  &&  left) { *out = (Value){ VAL_BOOL, .b = true  }; return ST_OK; }
    Value b; s = eval_value(I, e->bin.b, &b); if (s != ST_OK) return s;
    if (b.type != VAL_BOOL) { val_release(b);
        return rt_error(I, e->line, e->col, "condition must be bool, got %s", vt_name(b.type)); }
    *out = (Value){ VAL_BOOL, .b = b.b };
    val_release(b);
    return ST_OK;
}

static Status eval_expr(Interp *I, Expr *e, Value *out) {
    switch (e->kind) {
        case E_INT:  *out = (Value){ VAL_INT,  .i = e->ival };  return ST_OK;
        case E_BOOL: *out = (Value){ VAL_BOOL, .b = e->bval };  return ST_OK;
        case E_STR:  *out = (Value){ VAL_STR,  .s = e->sval };  return ST_OK;   /* immortal, no refcount */
        case E_VAR: {
            Var *v = lookup(I, e->name);
            if (!v) return rt_error(I, e->line, e->col, "undefined variable '%.*s'",
                                    (int)e->name.len, e->name.p);
            *out = v->val; val_retain(*out);
            return ST_OK;
        }
        case E_UNARY:  return eval_unary(I, e, out);
        case E_BINARY: return eval_binary(I, e, out);
        case E_AND: case E_OR: return eval_logical(I, e, out);
        case E_CALL:   return eval_call(I, e, out);
    }
    assert(0 && "unknown expr kind");
    return ST_ERR;
}

static Status eval_value(Interp *I, Expr *e, Value *out) {
    Status s = eval_expr(I, e, out);
    if (s != ST_OK) return s;
    if (out->type == VAL_NONE) {   /* only an E_CALL can yield this */
        assert(e->kind == E_CALL);
        return rt_error(I, e->line, e->col, "function '%.*s' returned no value",
                        (int)e->call.name.len, e->call.name.p);
    }
    return ST_OK;
}

/* A millisecond argument for sleep/delay/repeat: must be a non-negative int. */
static Status eval_count(Interp *I, Expr *e, const char *what, int64_t *out) {
    Value v; Status s = eval_value(I, e, &v); if (s != ST_OK) return s;
    if (v.type != VAL_INT || v.i < 0) {
        val_release(v);
        return rt_error(I, e->line, e->col, "%s expects a non-negative int", what);
    }
    *out = v.i;
    return ST_OK;
}

static Status exec_type(Interp *I, Stmt *s) {
    const char *err = NULL;
    for (int i = 0; i < s->type.nsegs; i++) {
        Seg *seg = &s->type.segs[i];
        if (seg->kind == SEG_TEXT) {
            if (out_type(I->out, seg->text.p, seg->text.len, &err) != ST_OK)
                return rt_error(I, s->line, s->col, "%s", err);
        } else {
            Value v; Status st = eval_value(I, seg->expr, &v); if (st != ST_OK) return st;
            QStr *q = to_string(v);
            Status o = out_type(I->out, q->data, q->len, &err);
            str_release(q); val_release(v);
            if (o != ST_OK) return rt_error(I, s->line, s->col, "%s", err);
        }
    }
    /* The trailing newline (absent when the line ended with \c) is already baked
       into the final text segment by the parser, so nothing more to emit here. */
    return ST_OK;
}

static Status exec_type_file(Interp *I, Stmt *s) {
    const char *err = NULL;
    const char *bytes = s->typefile.bytes;   /* literal path: baked and validated at parse time */
    size_t len = s->typefile.len;
    char *tofree = NULL;

    if (s->typefile.path) {
        /* Computed path: resolve against the script's directory, read and
           validate now. Any failure is a run-time error (exit 1), reported at
           the path expression so the caret matches the literal case. */
        Expr *pe = s->typefile.path;
        Value v; Status st = eval_value(I, pe, &v);
        if (st != ST_OK) return st;
        if (v.type != VAL_STR) {
            Status e = rt_error(I, pe->line, pe->col,
                                "type_file path must be a string, got %s", vt_name(v.type));
            val_release(v);
            return e;
        }
        char errbuf[256];
        char *buf = NULL; size_t n = 0;
        bool ok = tf_load(I->script_path, v.s->data, v.s->len, &buf, &n, errbuf, sizeof errbuf);
        val_release(v);
        if (!ok) return rt_error(I, pe->line, pe->col, "%s", errbuf);
        bytes = buf; len = n; tofree = buf;
    }

    Status r = ST_OK;
    if (out_type(I->out, bytes, len, &err) != ST_OK)
        r = rt_error(I, s->line, s->col, "%s", err);
    else if (s->typefile.add_newline && out_type(I->out, "\n", 1, &err) != ST_OK)
        r = rt_error(I, s->line, s->col, "%s", err);
    free(tofree);
    return r;
}

static Status exec_stmt(Interp *I, Stmt *s) {
    Status st = ST_OK;
    const char *err = NULL;

    switch (s->kind) {
        case S_ASSIGN: case S_CONST: {
            Value v; st = eval_value(I, s->assign.value, &v); if (st != ST_OK) return st;
            st = (s->kind == S_ASSIGN) ? do_assign(I, s, v) : do_const(I, s, v);
            val_release(v);
            break;
        }
        case S_IF: {
            Value c; st = eval_value(I, s->iff.cond, &c); if (st != ST_OK) return st;
            if (c.type != VAL_BOOL) { val_release(c);
                return rt_error(I, s->line, s->col, "condition must be bool, got %s", vt_name(c.type)); }
            bool take = c.b; val_release(c);
            if (take) st = exec_block(I, s->iff.then_b);
            else if (s->iff.else_s) st = exec_stmt(I, s->iff.else_s);
            break;
        }
        case S_WHILE:
            for (;;) {
                Value c; st = eval_value(I, s->loop.cond, &c); if (st != ST_OK) return st;
                if (c.type != VAL_BOOL) { val_release(c);
                    return rt_error(I, s->line, s->col, "condition must be bool, got %s", vt_name(c.type)); }
                bool go = c.b; val_release(c);
                if (!go) break;
                Status b = exec_block(I, s->loop.body);
                if (b == ST_BREAK) break;
                if (b != ST_CONTINUE && b != ST_OK) return b;   /* ST_ERR or ST_RETURN propagate */
                if (s->loop.step && (st = exec_stmt(I, s->loop.step)) != ST_OK) return st;
            }
            break;
        case S_RETURN: {
            Value v = (Value){ VAL_NONE, .i = 0 };
            if (s->expr) { st = eval_value(I, s->expr, &v); if (st != ST_OK) return st; }
            I->ret = v;                                     /* ownership moves to the return slot */
            return ST_RETURN;
        }
        case S_BREAK:    return ST_BREAK;
        case S_CONTINUE: return ST_CONTINUE;
        case S_SLEEP: {
            int64_t ms; st = eval_count(I, s->expr, "sleep", &ms); if (st != ST_OK) return st;
            if (ms > OUT_MS_MAX) ms = OUT_MS_MAX;
            out_sleep_ms(I->out, ms);
            break;
        }
        case S_DELAY: {
            int64_t ms; st = eval_count(I, s->expr, "delay", &ms); if (st != ST_OK) return st;
            if (ms > OUT_MS_MAX) ms = OUT_MS_MAX;
            out_set_default_delay(I->out, ms);
            break;
        }
        case S_REPEAT: {
            int64_t n; st = eval_count(I, s->expr, "repeat", &n); if (st != ST_OK) return st;
            if (!I->last_action) return rt_error(I, s->line, s->col, "repeat with no preceding action");
            Stmt *action = I->last_action;
            for (int64_t k = 0; k < n; k++) {
                Status r = exec_stmt(I, action);            /* re-runs the action; keeps last_action */
                if (r != ST_OK) return r;
            }
            break;
        }
        case S_TYPE: st = exec_type(I, s); break;
        case S_TYPE_FILE: st = exec_type_file(I, s); break;
        case S_CALL: {
            Value v; st = eval_expr(I, s->expr, &v);        /* a no-value return is fine for a call statement */
            if (st != ST_OK) return st;
            val_release(v);
            break;
        }
        case S_CHORD:
            if (out_chord(I->out, &s->chord.text, s->chord.mod, s->chord.usages, s->chord.n, &err) != ST_OK)
                return rt_error(I, s->line, s->col, "%s", err);
            break;
        case S_HOLD:
            if (out_hold(I->out, &s->chord.text, s->chord.mod, s->chord.usages, s->chord.n, &err) != ST_OK)
                return rt_error(I, s->line, s->col, "%s", err);
            break;
        case S_RELEASE:
            if (out_release(I->out, &s->chord.text, s->chord.mod, s->chord.usages, s->chord.n,
                            s->chord.all, &err) != ST_OK)
                return rt_error(I, s->line, s->col, "%s", err);
            break;
    }
    if (st != ST_OK) return st;

    /* `repeat` bookkeeping (03 §4.1): remember the last action; clear it on any
       other statement; `repeat` itself preserves it, so two in a row both work. */
    switch (s->kind) {
        case S_TYPE: case S_TYPE_FILE: case S_CHORD: case S_HOLD: case S_RELEASE: I->last_action = s; break;
        case S_REPEAT: break;
        default: I->last_action = NULL;
    }
    return ST_OK;
}

static Status exec_block(Interp *I, Block *b) {
    for (int i = 0; i < b->n; i++) {
        Status s = exec_stmt(I, b->stmts[i]);
        if (s != ST_OK) return s;
        out_post_statement(I->out);     /* the default delay fires after every statement (03 §4.1) */
    }
    return ST_OK;
}

Status run(Interp *I, Program *prog) {
    I->prog = prog;
    I->funcs = prog->funcs; I->nfuncs = prog->nfuncs;   /* the parser already rejected duplicates */
    I->depth = 0;
    I->ret = (Value){ VAL_NONE, .i = 0 };
    I->last_action = NULL;
    I->globals = new_frame(NULL);
    I->frame = I->globals;

    Status s = exec_block(I, prog->top);
    assert(s == ST_OK || s == ST_ERR);   /* return/break/continue were rejected at top level by the parser */
    free_frame(I->globals);
    return s;
}
