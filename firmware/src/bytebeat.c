/* SPDX-License-Identifier: GPL-3.0-only */
/* Bytebeat formulas: C expressions of t and the variables a..h, compiled in the main loop into a small
 * stack program that the audio interrupt runs once per step of t.
 *
 *   t  a..h  123  0x7F  ( )
 *   ?:  ||  &&  |  ^  &  == !=  < <= > >=  << >>  + -  * / %  unary - ~ !
 *
 * 32-bit signed integers with C's precedence, except where C would be undefined: overflow wraps, a shift
 * takes its count & 31, x / 0 and x % 0 are 0. No side effects, so ?: && || evaluate both sides.
 * Included by bytemania.c and by tests/test_bytebeat.c; it needs nothing from the platform. */
#include <stdint.h>

#define BB_MAX 64u                              /* ops in a program */
#define BB_STACK 16u                            /* its deepest stack */
#define BB_VARS 8u                              /* a..h */

enum {
    OP_NUM, OP_T, OP_VAR,
    OP_NEG, OP_NOT, OP_LNOT,
    OP_MUL, OP_DIV, OP_MOD, OP_ADD, OP_SUB, OP_SHL, OP_SHR,
    OP_LT, OP_LE, OP_GT, OP_GE, OP_EQ, OP_NE,
    OP_AND, OP_XOR, OP_OR, OP_LAND, OP_LOR, OP_SEL
};

typedef struct {
    uint8_t op[BB_MAX];
    int32_t arg[BB_MAX];                        /* OP_NUM: the number, OP_VAR: 0..7 */
    uint32_t n;
} bb_prog_t;

/* ---- run (the audio interrupt: no calls out, no division by zero) */
static int32_t bb_run(const bb_prog_t *p, uint32_t t, const int32_t *var)
{
    int32_t s[BB_STACK], x, y;
    uint32_t i, sp = 0;
    for (i = 0; i < p->n; i++) {
        switch (p->op[i]) {
        case OP_NUM: s[sp++] = p->arg[i]; continue;
        case OP_T: s[sp++] = (int32_t)t; continue;
        case OP_VAR: s[sp++] = var[p->arg[i]]; continue;
        case OP_NEG: s[sp - 1] = (int32_t)(0u - (uint32_t)s[sp - 1]); continue;
        case OP_NOT: s[sp - 1] = ~s[sp - 1]; continue;
        case OP_LNOT: s[sp - 1] = !s[sp - 1]; continue;
        case OP_SEL: sp -= 2; s[sp - 1] = s[sp - 1] ? s[sp] : s[sp + 1]; continue;
        default: break;
        }
        y = s[--sp];                            /* a binary op: x op y */
        x = s[sp - 1];
        switch (p->op[i]) {
        case OP_MUL: x = (int32_t)((uint32_t)x * (uint32_t)y); break;
        case OP_DIV: x = !y ? 0 : y == -1 ? (int32_t)(0u - (uint32_t)x) : x / y; break;
        case OP_MOD: x = !y || y == -1 ? 0 : x % y; break;
        case OP_ADD: x = (int32_t)((uint32_t)x + (uint32_t)y); break;
        case OP_SUB: x = (int32_t)((uint32_t)x - (uint32_t)y); break;
        case OP_SHL: x = (int32_t)((uint32_t)x << (y & 31)); break;
        case OP_SHR: x = x >> (y & 31); break;
        case OP_LT: x = x < y; break;
        case OP_LE: x = x <= y; break;
        case OP_GT: x = x > y; break;
        case OP_GE: x = x >= y; break;
        case OP_EQ: x = x == y; break;
        case OP_NE: x = x != y; break;
        case OP_AND: x &= y; break;
        case OP_XOR: x ^= y; break;
        case OP_OR: x |= y; break;
        case OP_LAND: x = x && y; break;
        case OP_LOR: x = x || y; break;
        }
        s[sp - 1] = x;
    }
    return sp ? s[0] : 0;
}

/* ---- compile (the main loop) */
typedef struct {
    const char *s;
    uint32_t pos, depth;
    bb_prog_t *p;
    const char *err;                            /* the first error, 0 = none */
} bb_cc_t;

static const struct { char s[3]; uint8_t prec, op; } BB_BIN[] = {   /* two-character ones first */
    {"||", 1, OP_LOR}, {"&&", 2, OP_LAND}, {"==", 6, OP_EQ}, {"!=", 6, OP_NE}, {"<=", 7, OP_LE}, {">=", 7, OP_GE},
    {"<<", 8, OP_SHL}, {">>", 8, OP_SHR}, {"|", 3, OP_OR}, {"^", 4, OP_XOR}, {"&", 5, OP_AND}, {"<", 7, OP_LT},
    {">", 7, OP_GT}, {"+", 9, OP_ADD}, {"-", 9, OP_SUB}, {"*", 10, OP_MUL}, {"/", 10, OP_DIV}, {"%", 10, OP_MOD},
};

static void cc_fail(bb_cc_t *c, const char *msg) { if (!c->err) c->err = msg; }

static char cc_peek(bb_cc_t *c)
{
    while (c->s[c->pos] == ' ') c->pos++;
    return c->s[c->pos];
}

static void cc_emit(bb_cc_t *c, uint32_t op, int32_t arg)
{
    if (c->err) return;
    if (c->p->n >= BB_MAX) { cc_fail(c, "too long"); return; }
    if (op <= OP_VAR) c->depth++;
    else if (op == OP_SEL) c->depth -= 2;
    else if (op >= OP_MUL) c->depth--;
    if (c->depth > BB_STACK) { cc_fail(c, "too deep"); return; }
    c->p->op[c->p->n] = (uint8_t)op;
    c->p->arg[c->p->n++] = arg;
}

static void cc_cond(bb_cc_t *c);

static void cc_primary(bb_cc_t *c)
{
    char ch = cc_peek(c);
    if (ch >= '0' && ch <= '9') {
        uint32_t v = 0, base = 10, d;
        if (ch == '0' && (c->s[c->pos + 1] == 'x' || c->s[c->pos + 1] == 'X')) { base = 16; c->pos += 2; }
        for (;; c->pos++) {
            ch = c->s[c->pos];
            if (ch >= '0' && ch <= '9') d = (uint32_t)(ch - '0');
            else if (base == 16 && ch >= 'a' && ch <= 'f') d = (uint32_t)(ch - 'a' + 10);
            else if (base == 16 && ch >= 'A' && ch <= 'F') d = (uint32_t)(ch - 'A' + 10);
            else break;
            v = v * base + d;                   /* (wraps, as a too-big constant does in C) */
        }
        cc_emit(c, OP_NUM, (int32_t)v);
    } else if (ch == 't') {
        c->pos++;
        cc_emit(c, OP_T, 0);
    } else if (ch >= 'a' && ch <= 'h') {
        c->pos++;
        cc_emit(c, OP_VAR, ch - 'a');
    } else if (ch == '(') {
        c->pos++;
        cc_cond(c);
        if (cc_peek(c) != ')') { cc_fail(c, "missing )"); return; }
        c->pos++;
    } else
        cc_fail(c, ch ? "unexpected character" : "unexpected end");
    if (!c->err && ((ch = c->s[c->pos]) >= 'a' && ch <= 'z')) cc_fail(c, "unknown name");
}

static void cc_unary(bb_cc_t *c)
{
    char ch = cc_peek(c);
    uint32_t op = ch == '-' ? OP_NEG : ch == '~' ? OP_NOT : ch == '!' ? OP_LNOT : 0;
    if (ch == '+') { c->pos++; cc_unary(c); return; }
    if (!op) { cc_primary(c); return; }
    c->pos++;
    cc_unary(c);
    cc_emit(c, op, 0);
}

static void cc_binary(bb_cc_t *c, uint32_t min)    /* precedence climbing over BB_BIN */
{
    cc_unary(c);
    while (!c->err) {
        uint32_t i, k = sizeof BB_BIN / sizeof BB_BIN[0];
        cc_peek(c);
        for (i = 0; i < k; i++) {
            const char *o = BB_BIN[i].s;
            if (c->s[c->pos] == o[0] && (!o[1] || c->s[c->pos + 1] == o[1])) break;
        }
        if (i == k || BB_BIN[i].prec < min) return;
        c->pos += BB_BIN[i].s[1] ? 2u : 1u;
        cc_binary(c, BB_BIN[i].prec + 1u);
        cc_emit(c, BB_BIN[i].op, 0);
    }
}

static void cc_cond(bb_cc_t *c)
{
    cc_binary(c, 1);
    if (c->err || cc_peek(c) != '?') return;
    c->pos++;
    cc_cond(c);
    if (cc_peek(c) != ':') { cc_fail(c, "missing :"); return; }
    c->pos++;
    cc_cond(c);
    cc_emit(c, OP_SEL, 0);
}

/* 0 = compiled into *p; else the error, and *errpos where it is. *p is changed either way. */
static const char *bb_compile(const char *s, bb_prog_t *p, uint32_t *errpos)
{
    bb_cc_t c = {s, 0, 0, p, 0};
    p->n = 0;
    cc_cond(&c);
    if (!c.err && cc_peek(&c)) cc_fail(&c, "unexpected character");
    if (errpos) *errpos = c.pos;
    return c.err;
}
