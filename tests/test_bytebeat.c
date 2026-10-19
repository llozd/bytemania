/* SPDX-License-Identifier: GPL-3.0-only */
/* bytebeat.c against the C compiler: each formula is compiled by both and run over a range of t.
 * Build with -fwrapv, so C's overflow wraps as bytebeat.c's does (tests/run_tests.sh). */
#include <stdio.h>
#include <string.h>
#include "bytebeat.c"

#define VARS int32_t a = v[0], b = v[1], c = v[2], d = v[3], e = v[4], f = v[5], g = v[6], h = v[7]; \
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h
#define FORMULAS(X) \
    X(f0, t) \
    X(f1, t*((t>>12|t>>8)&63&t>>4)) \
    X(f2, t*(42&t>>10)) \
    X(f3, t*(t>>9|t>>13)&16) \
    X(f4, t*5&(t>>7)|t*3&(t*4>>10)) \
    X(f5, t*9&t>>4|t*5&t>>7|t*3&t/1024) \
    X(f6, t*(0xCA98>>(t>>9&14)&15)|t>>8) \
    X(f7, t>>6&1?t>>5:-t>>4) \
    X(f8, t*((t>>a|t>>b)&c&t>>d)) \
    X(f9, (t%e+f*g)-h/3) \
    X(f10, ~t^!t+(t<a)+(t<=b)+(t>c)+(t>=d)+(t==e)+(t!=f)) \
    X(f11, t&&a||b&&!c) \
    X(f12, t?a?1:2:3) \
    X(f13, -(-t)*+3-t%7)

#define FN(name, expr) static int32_t name(int32_t t, const int32_t *v) { VARS; return (expr); }
FORMULAS(FN)
#define ROW(name, expr) {#expr, name},
static const struct { const char *s; int32_t (*fn)(int32_t, const int32_t *); } CASES[] = { FORMULAS(ROW) };

static int fails;

static void expect_value(const char *s, uint32_t t, int32_t want)
{
    static const int32_t v[BB_VARS] = {0};
    bb_prog_t p;
    const char *err = bb_compile(s, &p, 0);
    int32_t got = err ? 0 : bb_run(&p, t, v);
    if (err || got != want) { printf("FAIL %s at t=%u: %d, want %d %s\n", s, t, got, want, err ? err : ""); fails++; }
}

static void expect_error(const char *s, const char *want)
{
    bb_prog_t p;
    const char *err = bb_compile(s, &p, 0);
    if (!err || strcmp(err, want)) { printf("FAIL \"%s\": %s, want %s\n", s, err ? err : "compiled", want); fails++; }
}

int main(void)
{
    static const int32_t v[BB_VARS] = {12, 8, 63, 4, 7, 3, 5, 100};
    char deep[64], longf[256];
    uint32_t i, k;
    bb_prog_t p;

    for (i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        uint32_t errpos;
        const char *err = bb_compile(CASES[i].s, &p, &errpos);
        if (err) { printf("FAIL %s: %s at %u\n", CASES[i].s, err, errpos); fails++; continue; }
        for (k = 0; k < 300000u; k += 7u) {
            int32_t want = CASES[i].fn((int32_t)k, v), got = bb_run(&p, k, v);
            if (got != want) { printf("FAIL %s at t=%u: %d, want %d\n", CASES[i].s, k, got, want); fails++; break; }
        }
    }

    /* where C is undefined */
    expect_value("t/0", 5, 0);
    expect_value("t%0", 5, 0);
    expect_value("(0-2147483647-1)/-1", 0, (int32_t)0x80000000u);
    expect_value("(0-2147483647-1)%-1", 0, 0);
    expect_value("t<<33", 3, 6);
    expect_value("t>>32", 3, 3);
    expect_value("0xFFFFFFFF", 0, -1);
    expect_value("  t +  1 ", 1, 2);

    expect_error("", "unexpected end");
    expect_error("t+", "unexpected end");
    expect_error("(t", "missing )");
    expect_error("t?1", "missing :");
    expect_error("x", "unexpected character");
    expect_error("tt", "unknown name");
    expect_error("t)", "unexpected character");
    memset(deep, 0, sizeof deep);
    for (i = 0; i < 17u; i++) strcat(deep, "t+(");
    strcat(deep, "t");
    expect_error(deep, "too deep");
    memset(longf, 0, sizeof longf);
    strcpy(longf, "t");
    for (i = 0; i < 40u; i++) strcat(longf, "+t");
    expect_error(longf, "too long");

    printf("%s\n", fails ? "bytebeat: FAILED" : "bytebeat: ok");
    return fails != 0;
}
