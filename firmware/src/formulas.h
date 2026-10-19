/* SPDX-License-Identifier: GPL-3.0-only */
/* The formulas: classic bytebeats with some of their numbers made variables (the defaults give the original).
 * The first 12 are from viznut's "Some deep analysis of one-line music programs" (2011), credited as there;
 * the rest from the bytebeat composer's library (dollchan.net/bytebeat), credited as there. */
static const struct { const char *name, *by, *text; int16_t def[4]; } FORMULA[] = {
    {"SAW", "viznut", "t*a", {1}},
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
    {"TRIANGLE", "ryg", "(t<<a)^-(t>>b&1)", {4, 4}},
    {"JACQUES", "Lord Graga", "a*(t>>b|3*t|t>>(t>>15))+(t>>8&5)", {10, 7}},
    {"LEMMINGS", "marmakoide", "t>>a|t<<b|t&1023^1981|t-67>>c", {5, 4, 4}},
    {"SURFER", "skurk, raer", "t&4096?t/2*(t^t%255)|t>>a:t/8|(t&8192?4*t:t)", {5}},
    {"CROWD", "erlehmann", "2*t^2*t+(t>>a)&t>>b|t>>4-(1^7&t>>19)|t>>a", {7, 12}},
    {"LATE PARTY", "rrrola", "t*(a+(t>>b)%3)&t/3>>7&c", {4, 9, 224}},
    {"C64 GAME", "skurk", "t*(t>>(t&4096?t*t>>12:t>>12))|t<<(t>>a)|t>>b", {8, 4}},
    {"JAUNTY", "ryg", "t*(t>>a)*64+(t>>1)*(t>>b)*(t>>c)*48>>((t>>16|t>>17)&1)", {12, 10, 11}},
};
#define NF (sizeof FORMULA / sizeof FORMULA[0])
