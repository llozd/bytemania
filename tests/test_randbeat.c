/* SPDX-License-Identifier: GPL-3.0-only */
/* randbeat.c over many seeds: each formula fits, compiles, uses only the variables it set, passes its own check,
 * and they differ (the text with its variables' values). Prints a few to read. */
#include <stdio.h>
#include <string.h>
#include "bytebeat.c"
#include "randbeat.c"

#define SEEDS 1000u

int main(void)
{
    static char seen[SEEDS][128];
    uint32_t seed, i, k, fails = 0, fallbacks = 0, same = 0;
    bb_prog_t p;
    for (seed = 1; seed <= SEEDS; seed++) {
        char s[96];
        int32_t var[8];
        int fb = rb_make(s, 90, var, seed * 2654435761u);
        const char *err = bb_compile(s, &p, 0);
        fallbacks += (uint32_t)fb;
        if (strlen(s) > 90 || err || !rb_good(&p, var)) { printf("FAIL seed %u: %s (%s)\n", seed, s, err ? err : "check"); fails++; }
        for (i = 0; s[i]; i++)                  /* a variable it uses has a value */
            if (s[i] >= 'e' && s[i] <= 'h') { printf("FAIL seed %u uses %c: %s\n", seed, s[i], s); fails++; }
        snprintf(seen[seed - 1u], sizeof seen[0], "%s %d %d %d %d", s, var[0], var[1], var[2], var[3]);
        for (k = 0; k < seed - 1u; k++) if (!strcmp(seen[k], seen[seed - 1u])) { same++; break; }
        if (seed <= 6) printf("  %s   a=%d b=%d c=%d d=%d\n", s, var[0], var[1], var[2], var[3]);
    }
    printf("randbeat: %u of %u fell back, %u repeats\n", fallbacks, SEEDS, same);
    if (fallbacks > SEEDS / 50u) { printf("FAIL too many fallbacks\n"); fails++; }
    if (same > SEEDS / 20u) { printf("FAIL too many repeats\n"); fails++; }
    printf("%s\n", fails ? "randbeat: FAILED" : "randbeat: ok");
    return fails != 0;
}
