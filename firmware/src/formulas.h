/* SPDX-License-Identifier: GPL-3.0-only */
/* The formulas. First 8 basic waveforms, Bytemania's own: on a phase t*a>>b that a = 67, b = 3 tune to the note
 * (8.375 a step: 261.7 Hz at middle C, 0.6 cents sharp). Then classic bytebeats with some of their numbers made
 * variables (the defaults give the original): 12 from viznut's "Some deep analysis of one-line music programs" (2011),
 * the rest from the bytebeat composer's library (dollchan.net/bytebeat), credited as there. */
static const struct { const char *name, *by, *text; int16_t def[4]; } FORMULA[] = {
    {"SAW", "basic wave", "t*a>>b", {67, 3}},
    {"SQUARE", "basic wave, c: width", "(t*a>>b&255)<c?255:0", {67, 3, 128}},
    {"TRIANGLE", "basic wave", "((t*a>>b)^-(t*a>>b>>7&1))*2", {67, 3}},
    {"SINE", "basic wave", "128+((t*a>>b&128)?-1:1)*((t*a>>b&127)*(127-(t*a>>b&127))>>5)", {67, 3}},
    {"PWM", "basic wave, c: sweep", "(t*a>>b&255)<(t>>c&255)?255:0", {67, 3, 6}},
    {"SYNC", "basic wave, c: ratio", "(t*a>>b&255)*c>>3", {67, 3, 12}},
    {"XOR", "basic wave, c: 2nd saw", "(t*a>>b)^(t*c>>b)", {67, 3, 101}},
    {"STEPS", "basic wave, c: mask", "t*a>>b&c", {67, 3, 224}},
    {"PLAIN T", "viznut", "t*a", {1}},
    {"MUNCHING", "viznut", "t&t>>a", {8}},
    {"FIRST", "viznut", "t*((t>>a|t>>b)&c&t>>d)", {12, 8, 63, 4}},
    {"FORTY-TWO", "several people", "t*(a&t>>b)", {42, 10}},
    {"ROY", "Niklas Roy", "t*(t>>a|t>>b)&c", {9, 13, 16}},
    {"MIIRO", "miiro", "t*a&(t>>b)|t*c&(t*4>>d)", {5, 7, 3, 10}},
    {"VIZNUT", "viznut", "(t*a&t>>b)|(t*c&t>>d)", {5, 7, 3, 10}},
    {"STEPHTH", "stephth", "t*a&t>>b|t*c&t>>d|t*3&t/1024", {9, 4, 5, 7}},
    {"RRROLA", "Rrrola", "t*(0xCA98>>(t>>a&14)&15)|t>>b", {9, 8}},
    {"DROID", "droid", "t>>a&1?t>>b:-t>>c", {6, 5, 4}},
    {"SPLIT", "unknown", "t*(a+((t>>b)&c))", {5, 11, 5}},
    {"MU6K LEAD", "viznut, after Mu6k", "t>>a^t>>b|t>>c|t&d", {6, 8, 12, 63}},
    {"MODEM", "krcko", "(t&t>>a)*(t>>b|t>>c)", {12, 4, 8}},
    {"FRACTAL", "Blueberry", "(t>>a^(t>>a)-1^1)%b*t", {9, 13}},
    {"RYG TRI", "ryg", "(t<<a)^-(t>>b&1)", {4, 4}},
    {"JACQUES", "Lord Graga", "a*(t>>b|3*t|t>>(t>>15))+(t>>8&5)", {10, 7}},
    {"LEMMINGS", "marmakoide", "t>>a|t<<b|t&1023^1981|t-67>>c", {5, 4, 4}},
    {"SURFER", "skurk, raer", "t&4096?t/2*(t^t%255)|t>>a:t/8|(t&8192?4*t:t)", {5}},
    {"CROWD", "erlehmann", "2*t^2*t+(t>>a)&t>>b|t>>4-(1^7&t>>19)|t>>a", {7, 12}},
    {"LATE PARTY", "rrrola", "t*(a+(t>>b)%3)&t/3>>7&c", {4, 9, 224}},
    {"C64 GAME", "skurk", "t*(t>>(t&4096?t*t>>12:t>>12))|t<<(t>>a)|t>>b", {8, 4}},
    {"JAUNTY", "ryg", "t*(t>>a)*64+(t>>1)*(t>>b)*(t>>c)*48>>((t>>16|t>>17)&1)", {12, 10, 11}},
};
#define NF (sizeof FORMULA / sizeof FORMULA[0])
