/* SPDX-License-Identifier: GPL-3.0-only */
/* The formulas: classic bytebeats with some of their numbers made variables (the defaults give the original).
 * From viznut's "Some deep analysis of one-line music programs" (2011), credited as there. */
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
};
#define NF (sizeof FORMULA / sizeof FORMULA[0])
