/* SPDX-License-Identifier: GPL-3.0-only */
/* bytebeat.c against the C compiler: each formula is compiled by both and run over a range of t.
 * Build with -fwrapv, so C's overflow wraps as bytebeat.c's does (tests/run_tests.sh). */
#include <stdio.h>
#include <string.h>
#include "bytebeat.c"
#include "formulas.h"

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

#define ORIGINALS(X) /* formulas.h's bank, in order: the basic waveforms, then the classics as first published */ \
    X(w0, t*67>>3) \
    X(w1, (t*67>>3&255)<128?255:0) \
    X(w2, ((t*67>>3)^-(t*67>>3>>7&1))*2) \
    X(w3, 128+((t*67>>3&128)?-1:1)*((t*67>>3&127)*(127-(t*67>>3&127))>>5)) \
    X(w4, (t*67>>3&255)<(t>>6&255)?255:0) \
    X(w5, (t*67>>3&255)*12>>3) \
    X(w6, (t*67>>3)^(t*101>>3)) \
    X(w7, t*67>>3&224) \
    X(o0, t) \
    X(o1, t&t>>8) \
    X(o2, t*(((t>>12)|(t>>8))&(63&(t>>4)))) \
    X(o3, t*(42&t>>10)) \
    X(o4, t*(t>>9|t>>13)&16) \
    X(o5, t*5&(t>>7)|t*3&(t*4>>10)) \
    X(o6, (t*5&t>>7)|(t*3&t>>10)) \
    X(o7, t*9&t>>4|t*5&t>>7|t*3&t/1024) \
    X(o8, t*(0xCA98>>(t>>9&14)&15)|t>>8) \
    X(o9, t>>6&1?t>>5:-t>>4) \
    X(o10, t*(5+((t>>11)&5))) \
    X(o11, t>>6^t>>8|t>>12|t&63) \
    X(o12, (t&t>>12)*(t>>4|t>>8)) \
    X(o13, (t>>9^(t>>9)-1^1)%13*t) \
    X(o14, (t<<4)^-(t>>4&1)) \
    X(o15, 10*(t>>7|3*t|t>>(t>>15))+(t>>8&5)) \
    X(o16, t>>5|t<<4|t&1023^1981|t-67>>4) \
    X(o17, t&4096?t/2*(t^t%255)|t>>5:t/8|(t&8192?4*t:t)) \
    X(o18, 2*t^2*t+(t>>7)&t>>12|t>>4-(1^7&t>>19)|t>>7) \
    X(o19, t*(4+(t>>9)%3)&t/3>>7&224) \
    X(o20, t*(t>>(t&4096?t*t>>12:t>>12))|t<<(t>>8)|t>>4) \
    X(o21, t*(t>>12)*64+(t>>1)*(t>>10)*(t>>11)*48>>((t>>16|t>>17)&1))

#define FN(name, expr) static int32_t name(int32_t t, const int32_t *v) { VARS; return (expr); }
FORMULAS(FN)
ORIGINALS(FN)
#define ROW(name, expr) {#expr, name},
typedef struct { const char *s; int32_t (*fn)(int32_t, const int32_t *); } case_t;
static const case_t CASES[] = { FORMULAS(ROW) };
static const case_t ORIG[] = { ORIGINALS(ROW) };

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

    /* the bank with its defaults is the original */
    if (sizeof ORIG / sizeof ORIG[0] != NF) { printf("FAIL the bank has %u formulas, ORIGINALS %u\n", (unsigned)NF, (unsigned)(sizeof ORIG / sizeof ORIG[0])); fails++; }
    for (i = 0; i < NF && i < sizeof ORIG / sizeof ORIG[0]; i++) {
        int32_t dv[BB_VARS] = {0};
        const char *err = bb_compile(FORMULA[i].text, &p, 0);
        for (k = 0; k < 4u; k++) dv[k] = FORMULA[i].def[k];
        if (err) { printf("FAIL %s: %s\n", FORMULA[i].name, err); fails++; continue; }
        for (k = 0; k < 300000u; k += 7u)
            if (bb_run(&p, k, dv) != ORIG[i].fn((int32_t)k, dv)) { printf("FAIL %s is not %s at t=%u\n", FORMULA[i].name, ORIG[i].s, k); fails++; break; }
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
