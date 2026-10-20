/* The value model: refcounted strings, to_string, string `+`, and int_op with
   every integer UB case handled in one place (04). No I/O, no evaluator state;
   this is a leaf module the evaluator computes over. */

#include "value.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

void val_retain(Value v)  { if (v.type == VAL_STR) str_retain(v.s); }
void val_release(Value v) { if (v.type == VAL_STR) str_release(v.s); }

/* The one runtime-string allocator: header plus len bytes in a single malloc,
   rc starts at 1, bytes left for the caller to fill. */
static QStr *qstr_alloc(size_t len) {
    QStr *s = malloc(sizeof *s + len);
    if (!s) { fputs("quacky: out of memory\n", stderr); exit(1); }
    s->rc = 1;
    s->len = len;
    return s;
}

QStr *to_string(Value v) {
    switch (v.type) {
        case VAL_STR:
            str_retain(v.s);
            return v.s;
        case VAL_BOOL: {
            const char *t = v.b ? "true" : "false";
            size_t n = strlen(t);
            QStr *s = qstr_alloc(n);
            memcpy(s->data, t, n);
            return s;
        }
        case VAL_INT: {
            /* INT64_MIN is 20 digits plus a sign; 24 leaves room to spare. */
            char buf[24];
            int n = snprintf(buf, sizeof buf, "%" PRId64, v.i);
            QStr *s = qstr_alloc((size_t)n);
            memcpy(s->data, buf, (size_t)n);
            return s;
        }
        case VAL_NONE:
            break;
    }
    assert(0 && "to_string on VAL_NONE");
    return qstr_alloc(0);
}

QStr *str_concat(const QStr *a, const QStr *b) {
    QStr *s = qstr_alloc(a->len + b->len);
    memcpy(s->data, a->data, a->len);
    memcpy(s->data + a->len, b->data, b->len);
    return s;
}

Status int_op(TokKind op, int64_t a, int64_t b, int64_t *out, const char **err) {
    switch (op) {
        /* Wraparound is the defined behaviour; do the maths unsigned so signed
           overflow never happens (Lua's intop idiom, lvm.c). */
        case T_PLUS:  *out = (int64_t)((uint64_t)a + (uint64_t)b); return ST_OK;
        case T_MINUS: *out = (int64_t)((uint64_t)a - (uint64_t)b); return ST_OK;
        case T_STAR:  *out = (int64_t)((uint64_t)a * (uint64_t)b); return ST_OK;

        case T_SLASH:
            if (b == 0)  { *err = "division by zero"; return ST_ERR; }
            /* INT64_MIN / -1 overflows and traps on x86; compute it unsigned. */
            if (b == -1) { *out = (int64_t)(0u - (uint64_t)a); return ST_OK; }
            *out = a / b;
            return ST_OK;

        case T_PERCENT:
            if (b == 0)  { *err = "modulo by zero"; return ST_ERR; }
            if (b == -1) { *out = 0; return ST_OK; }   /* a % -1 is 0, and dodges the trap */
            *out = a % b;
            return ST_OK;

        /* A shift amount outside 0..63 is undefined in C; define it ourselves. */
        case T_SHL:
            *out = (b < 0 || b >= 64) ? 0 : (int64_t)((uint64_t)a << b);
            return ST_OK;
        case T_SHR:
            if (b < 0 || b >= 64) { *out = a < 0 ? -1 : 0; return ST_OK; }
            /* Arithmetic right shift spelled out, since C leaves `>>` of a
               negative implementation-defined. */
            *out = a >= 0 ? (int64_t)((uint64_t)a >> b) : ~(int64_t)(~(uint64_t)a >> b);
            return ST_OK;

        case T_AMP:   *out = a & b; return ST_OK;
        case T_PIPE:  *out = a | b; return ST_OK;
        case T_CARET: *out = a ^ b; return ST_OK;

        default:
            assert(0 && "int_op on a non-integer operator");
            return ST_ERR;
    }
}

bool equal(Value a, Value b) {
    if (a.type != b.type) return false;
    switch (a.type) {
        case VAL_INT:  return a.i == b.i;
        case VAL_BOOL: return a.b == b.b;
        case VAL_STR:  return a.s->len == b.s->len &&
                              memcmp(a.s->data, b.s->data, a.s->len) == 0;
        default:       return false;   /* VAL_NONE never compares equal */
    }
}

Status compare(Value a, Value b, int *out, const char **err) {
    if (a.type == VAL_INT && b.type == VAL_INT) {
        *out = (a.i > b.i) - (a.i < b.i);
        return ST_OK;
    }
    if (a.type == VAL_STR && b.type == VAL_STR) {
        size_t n = a.s->len < b.s->len ? a.s->len : b.s->len;
        int c = n ? memcmp(a.s->data, b.s->data, n) : 0;
        if (c != 0) *out = c < 0 ? -1 : 1;                       /* differ within the common run */
        else *out = (a.s->len > b.s->len) - (a.s->len < b.s->len); /* otherwise shorter sorts first */
        return ST_OK;
    }
    *err = "cannot compare values of different types";
    return ST_ERR;
}
