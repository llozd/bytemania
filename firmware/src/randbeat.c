/* SPDX-License-Identifier: GPL-3.0-only */
/* Random bytebeats: one to three of the pieces classic formulas are made of (t times masked and shifted copies of
 * itself), joined by | ^ + &, with their numbers made the variables a..d. Each is run before anyone hears it: one
 * that is silent, stuck or noise is thrown away and another made. Needs bytebeat.c; nothing from the platform. */

typedef struct {
    char *s;
    uint32_t n, max, rng, nvar;
    int32_t *var;
} rb_t;

static uint32_t rb_rand(rb_t *r, uint32_t n)    /* 0..n-1, xorshift */
{
    r->rng ^= r->rng << 13;
    r->rng ^= r->rng >> 17;
    r->rng ^= r->rng << 5;
    return r->rng % n;
}

static void rb_put(rb_t *r, const char *p) { while (*p && r->n < r->max) r->s[r->n++] = *p++; }

static void rb_num(rb_t *r, uint32_t v)
{
    char d[12];
    uint32_t i = 0;
    do d[i++] = (char)('0' + v % 10u); while ((v /= 10u));
    while (i && r->n < r->max) r->s[r->n++] = d[--i];
}

enum { RB_SHIFT, RB_MUL, RB_MASK };

static void rb_val(rb_t *r, uint32_t role)      /* a number for its role: mostly a variable, while a..d last */
{
    static const uint8_t MASK[8] = {15, 31, 42, 63, 85, 127, 170, 255};
    int32_t v = role == RB_SHIFT ? 4 + (int32_t)rb_rand(r, 10) : role == RB_MUL ? 1 + (int32_t)rb_rand(r, 15)
              : MASK[rb_rand(r, 8)];
    if (r->nvar < 4u && rb_rand(r, 4)) {
        char c[2] = {(char)('a' + r->nvar), 0};
        r->var[r->nvar++] = v;
        rb_put(r, c);
    } else
        rb_num(r, (uint32_t)v);
}

static void rb_block(rb_t *r)
{
    switch (rb_rand(r, 6)) {
    case 0: rb_put(r, "t*((t>>"); rb_val(r, RB_SHIFT); rb_put(r, "|t>>"); rb_val(r, RB_SHIFT);
            rb_put(r, ")&"); rb_val(r, RB_MASK); rb_put(r, ")"); break;              /* t*((t>>a|t>>b)&c) */
    case 1: rb_put(r, "t*"); rb_val(r, RB_MUL); rb_put(r, "&t>>"); rb_val(r, RB_SHIFT); break;   /* t*a&t>>b */
    case 2: rb_put(r, "(t>>"); rb_val(r, RB_SHIFT); rb_put(r, ")*(t>>"); rb_val(r, RB_SHIFT); rb_put(r, ")"); break;
    case 3: rb_put(r, "t*("); rb_val(r, RB_MASK); rb_put(r, "&t>>"); rb_val(r, RB_SHIFT); rb_put(r, ")"); break;
    case 4: rb_put(r, "t>>"); rb_val(r, RB_SHIFT); rb_put(r, "^t>>"); rb_val(r, RB_SHIFT); break;
    default: rb_put(r, "t*(t>>"); rb_val(r, RB_SHIFT); rb_put(r, "|t>>"); rb_val(r, RB_SHIFT); rb_put(r, ")"); break;
    }
}

/* Heard in advance: enough different values, not mostly 0, moving, and not noise (uniform noise moves ~85 a step) */
static int rb_good(const bb_prog_t *p, const int32_t *var)
{
    uint32_t seen[8] = {0}, distinct = 0, zeros = 0, moves = 0, diff = 0, t, i, prev = 0;
    for (t = 0; t < 65536u; t += 16u) {         /* 4096 samples over 8 s at middle C */
        uint32_t v = (uint32_t)bb_run(p, t, var) & 255u;
        if (!(seen[v >> 5] >> (v & 31u) & 1u)) { seen[v >> 5] |= 1u << (v & 31u); distinct++; }
        if (!v) zeros++;
    }
    for (i = 0; i < 4096u; i++) {               /* half a second, step by step, from 1 s in */
        uint32_t v = (uint32_t)bb_run(p, 8192u + i, var) & 255u;
        if (i) { moves += v != prev; diff += v > prev ? v - prev : prev - v; }
        prev = v;
    }
    return distinct >= 8u && zeros < 3482u && moves >= 410u && diff / 4095u <= 80u;
}

/* A new formula into s (at most max characters), its variables' values into var[0..7]. 0 = made; 1 = none passed
 * in 64 tries, and s is the Forty-Two Melody instead. */
static int rb_make(char *s, uint32_t max, int32_t *var, uint32_t seed)
{
    static bb_prog_t p;
    static const char *const JOIN[4] = {"|", "^", "+", "&"};
    uint32_t tries, k, i;
    rb_t r;
    r.rng = seed ? seed : 1u;
    for (tries = 0; tries < 64u; tries++) {
        char buf[128];
        uint32_t blocks = 1u + rb_rand(&r, 3);
        r.s = buf; r.n = 0; r.max = sizeof buf - 1u; r.nvar = 0; r.var = var;
        for (i = 0; i < 8u; i++) var[i] = 0;
        for (k = 0; k < blocks; k++) {
            if (k) rb_put(&r, JOIN[rb_rand(&r, 4)]);
            if (blocks > 1u) rb_put(&r, "(");
            rb_block(&r);
            if (blocks > 1u) rb_put(&r, ")");
        }
        buf[r.n] = 0;
        if (r.n > max || bb_compile(buf, &p, 0) || !rb_good(&p, var)) continue;
        for (i = 0; i <= r.n; i++) s[i] = buf[i];
        return 0;
    }
    for (i = 0; i < 8u; i++) var[i] = 0;
    var[0] = 42; var[1] = 10;
    for (i = 0; "t*(a&t>>b)"[i]; i++) s[i] = "t*(a&t>>b)"[i];
    s[i] = 0;
    return 1;
}
