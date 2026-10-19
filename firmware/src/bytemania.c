/* SPDX-License-Identifier: GPL-3.0-only */
/* Bytemania: a bytebeat synth for the FM-1, built on Dinghy. A formula of t and the variables a..h makes
 * an 8-bit sample each step of t; the note sets how fast t runs (middle C = 8 kHz, the classic rate).
 *
 *   keys       play the note (mono, the last key wins); with SEL lit, pick a formula instead
 *   SEL        the keys: play / pick a formula
 *   FX         the view: scope / raster
 *   GLO        the CC screen (and back)
 *   EDIT       the editor (and back)
 *   SAVE       the save screen: what plays, into one of 64 user slots
 *   PRESETS    the patch: the built-in formulas, then the used slots
 *   KNOB1..4   the variables a..d (faster when turned fast); with HOME held, e..h
 *   SEQ        tap (< 0.3 s): the loop on / off; held, the knobs are the loop: start, fine, length, mode
 *   SELECT     the glide
 *   OCT- OCT+  the octave
 *   MASTER     the volume (platform.c)
 *   USB-MIDI   notes in play, with their velocity; pitch bend +-2 semitones
 *
 * The CC screen: 12 slots, each a CC that moves a parameter (a..h, glide, volume) between min and max.
 *   SELECT     the slot
 *   KNOB1..4   its CC (or off), its parameter, min, max (min > max turns it around)
 *   REC        learn: the next CC that comes in is the slot's
 *
 * The editor: the keys type the formula (the screen shows which key types what), compiled as it changes.
 *   FX         held: the second layer of the keys
 *   SELECT     the cursor
 *   OCT- OCT+  delete before / after the cursor
 *   PLAY       a C4 to hear it by, on / off
 *   ARP        a new random formula (randbeat.c) with values for its variables, to edit or save
 *   KNOB1..4   as on the main screen
 *
 * The formulas: bytebeat.c (compiled in the main loop, run in the audio interrupt). The views run the
 * formula again in the main loop, at the t the interrupt has reached.
 * What platform.c offers: docs/API.md. The chip's rules: docs/RULES.md. */
#define APP_NAME "BYTEMANIA"                    /* the rescue and the crash screen say this */
#include "platform.c"
#include "bytebeat.c"
#include "randbeat.c"
#include "formulas.h"

/* ---- the parameters: one table that the knobs, MIDI, the CC mappings and the patches all write */
enum { P_A, P_B, P_C, P_D, P_E, P_F, P_G, P_H, P_GLIDE, P_VOL, P_LOOP, P_LSTART, P_LFINE, P_LLEN, P_N };
enum { LOOP_OFF, LOOP_ON, LOOP_PING };
static const struct { int32_t min, max; } PARAM[P_N] = {
    {0, 255}, {0, 255}, {0, 255}, {0, 255}, {0, 255}, {0, 255}, {0, 255}, {0, 255}, {0, 10}, {0, 127},
    {0, 2}, {0, 255}, {0, 255}, {0, 12},        /* the loop: mode, start (rows of 256), + fine, length 16 << n */
};
static volatile int32_t param[P_N];

static void param_set(uint32_t id, int32_t v)
{
    param[id] = v < PARAM[id].min ? PARAM[id].min : v > PARAM[id].max ? PARAM[id].max : v;
}

/* ---- the programs: three, so the main loop always has one the interrupt is not running */
static bb_prog_t prog[3];
static const bb_prog_t *volatile prog_live = &prog[0], *volatile prog_used = &prog[0];

static const char *formula_load(const char *text, uint32_t *errpos)
{
    bb_prog_t *p = &prog[0];
    const char *err;
    while (p == prog_live || p == prog_used) p++;
    if ((err = bb_compile(text, p, errpos))) return err;
    prog_live = p;                              /* a 32-bit store: the interrupt sees the old one or this */
    return 0;
}

/* ---- the sound (runs in the audio ISR: keep it short, no flash, no screen) */
static volatile uint32_t inc_target;            /* t steps per frame, Q16 */
static volatile uint32_t bend_mul = 65536u;     /* pitch bend, Q16: on top of the glide, so it is immediate */
static volatile int32_t gate;                   /* target level, Q15: 0 = released */
static volatile uint32_t note_seq;              /* + 1 for a note from silence: t starts over, no glide */
static volatile uint32_t dsp_us;                /* the last render's time */
static volatile uint32_t t_now;                 /* where t is, for the views */

static uint32_t loop_t(uint32_t pos, uint32_t mode, uint32_t start, uint32_t len)   /* len: a power of 2 */
{
    uint32_t off;
    if (mode == LOOP_OFF || pos < start) return pos;
    off = (pos - start) & (mode == LOOP_PING ? 2u * len - 1u : len - 1u);
    return start + (off < len ? off : 2u * len - 1u - off);      /* ping: the second half runs back */
}

static void app_audio(int32_t *out, uint32_t n)
{
    static uint32_t pos, t, frac, inc, seq;     /* pos: steps since the note began; t: where the loop puts it */
    static int32_t amp, s;
    const bb_prog_t *p = prog_live;
    int32_t var[BB_VARS], g = gate;
    uint32_t i, t0 = fm1_ticks(), target = inc_target, glide = (uint32_t)param[P_GLIDE], bm = bend_mul, step;
    int32_t vol = param[P_VOL];
    uint32_t lmode = (uint32_t)param[P_LOOP], lstart = (uint32_t)(param[P_LSTART] * 256 + param[P_LFINE]);
    uint32_t llen = 16u << param[P_LLEN];
    prog_used = p;
    if (seq != note_seq) { seq = note_seq; pos = t = 0; frac = 0; inc = target; }
    if (!amp && !g) { dsp_us = 0; return; }     /* silent: costs nothing */
    for (i = 0; i < BB_VARS; i++) var[i] = param[P_A + i];
    step = (uint32_t)((uint64_t)inc * bm >> 16);
    for (i = 0; i < n; i++) {
        int32_t v;
        if (!(i & 15u) && inc != target) {      /* the glide, every 16 frames: 1 / 2^glide of the way */
            int32_t d = (int32_t)(target - inc) >> glide;
            inc = d ? inc + (uint32_t)d : target;
            step = (uint32_t)((uint64_t)inc * bm >> 16);
        }
        frac += step;
        if (frac >> 16) {                       /* t moved: a new sample, held until it moves again */
            pos += frac >> 16;
            t = loop_t(pos, lmode, lstart, llen);
            frac &= 0xFFFFu;
            s = ((bb_run(p, t, var) & 255) - 128) * 256;
        }
        if (amp < g) { amp += 48; if (amp > g) amp = g; }        /* attack ~10 ms */
        else if (amp > g) { amp -= 6; if (amp < g) amp = g; }    /* release ~100 ms */
        v = (s * ((amp * vol) >> 7)) >> 15;
        out[2u * i] += v;                       /* L */
        out[2u * i + 1u] += v;                  /* R */
    }
    t_now = t;
    dsp_us = (fm1_ticks() - t0) / FM1_TICKS_PER_US;
}

/* ---- notes (main loop): mono, the last held note sounds; releasing it goes back to the one before */
static const uint32_t INC_C4[12] = {            /* MIDI 60..71: 8 kHz * 2^(i/12) * 65536 / 44100; other octaves shift */
    11889u, 12596u, 13345u, 14138u, 14979u, 15869u, 16813u, 17813u, 18872u, 19994u, 21183u, 22443u};
static uint8_t held[16];
static uint32_t nheld, cur_note = 255;

static uint32_t inc_of(uint32_t note)
{
    uint32_t oct = note / 12u;
    return oct >= 5u ? INC_C4[note % 12u] << (oct - 5u) : INC_C4[note % 12u] >> (5u - oct);
}

static void bend_set(int32_t b)                 /* -8192..8191 -> +-2 semitones */
{
    uint32_t p = 60u * 256u + (uint32_t)(b / 16), lo = inc_of(p >> 8), hi = inc_of((p >> 8) + 1u);
    bend_mul = (lo + (hi - lo) * (p & 255u) / 256u) * 65536u / INC_C4[0];   /* linear within a semitone: < 0.3 % off */
}

static void held_drop(uint32_t note)
{
    uint32_t i, j;
    for (i = j = 0; i < nheld; i++)
        if (held[i] != note) held[j++] = held[i];
    nheld = j;
}

static void note_on(uint32_t note, uint32_t vel)
{
    int fresh = !gate;
    if (note > 127u || !vel) return;
    held_drop(note);
    if (nheld == sizeof held) held_drop(held[0]);
    held[nheld++] = (uint8_t)note;
    cur_note = note;
    inc_target = inc_of(note);                  /* the pitch before the gate (RULES.md) */
    if (fresh) note_seq++;
    gate = (int32_t)(vel * 200u);               /* 127 -> 25400 */
}

static void note_off(uint32_t note)
{
    held_drop(note);
    if (note != cur_note || !gate) return;
    if (nheld) { cur_note = held[nheld - 1u]; inc_target = inc_of(cur_note); }
    else gate = 0;
}

/* ---- the patches: the built-in formulas, and 64 user slots of a whole patch (formula, a..h, loop, glide),
 * 32 to a flash sector (platform.c), kept in RAM */
#define SLOTS 64u
#define SLOTS_PER (SLOTS / LIB_SECTORS)
#define TEXT_MAX 90u                            /* a formula: what the main screen's three lines show */
#define USER 128u                               /* a patch number: 0..NF-1 built in, USER + k slot k, */
#define RANDOM 255u                             /* or one randbeat.c made */
typedef struct {
    char name[12], text[TEXT_MAX + 2u];         /* both 0-terminated */
    uint8_t var[BB_VARS], loop, lstart, lfine, llen, glide, used, pad[2];
} slot_t;
static slot_t lib[SLOTS] __attribute__((section(".pool")));
static uint32_t lib_dirty;                      /* bit k: sector k to save, when nothing sounds */
static uint32_t cur = 2;                        /* the patch playing */
static uint32_t patch_seq;                      /* + 1 for each patch picked, made or saved: the screen shows it */
static char work[TEXT_MAX + 1u];                /* its formula */
static uint32_t edited;                         /* work is not the patch's own formula any more */
static const char *work_err;                    /* work does not compile (the last that did plays), and where */
static uint32_t work_err_at;

static void copy_str(char *d, const char *s, uint32_t max)   /* at most max characters, 0-terminated */
{
    uint32_t i;
    for (i = 0; s[i] && i < max; i++) d[i] = s[i];
    d[i] = 0;
}

static void lib_init(void)
{
    static bb_prog_t check;
    uint32_t k;
    for (k = 0; k < LIB_SECTORS; k++)
        if (lib_load(k, &lib[k * SLOTS_PER], SLOTS_PER * sizeof(slot_t)))
            memset(&lib[k * SLOTS_PER], 0, SLOTS_PER * sizeof(slot_t));
    for (k = 0; k < SLOTS; k++) {               /* a slot that cannot be shown or played reads as empty */
        lib[k].name[sizeof lib[k].name - 1u] = lib[k].text[TEXT_MAX] = 0;
        if (lib[k].used != 1u || bb_compile(lib[k].text, &check, 0)) memset(&lib[k], 0, sizeof lib[k]);
    }
}

static int patch_ok(uint32_t p) { return p < NF || (p >= USER && p - USER < SLOTS && lib[p - USER].used); }
static const char *patch_name(uint32_t p) { return p < USER ? FORMULA[p].name : p == RANDOM ? "RANDOM" : lib[p - USER].name; }

static void patch_pick(uint32_t p)
{
    uint32_t i;
    if (p < USER) {                             /* (the bank compiles: tests/test_bytebeat.c) */
        copy_str(work, FORMULA[p].text, TEXT_MAX);
        for (i = 0; i < BB_VARS; i++) param_set(P_A + i, i < 4u ? FORMULA[p].def[i] : 0);
    } else {
        const slot_t *s = &lib[p - USER];
        copy_str(work, s->text, TEXT_MAX);
        for (i = 0; i < BB_VARS; i++) param_set(P_A + i, s->var[i]);
        param_set(P_LOOP, s->loop); param_set(P_LSTART, s->lstart); param_set(P_LFINE, s->lfine);
        param_set(P_LLEN, s->llen); param_set(P_GLIDE, s->glide);
    }
    formula_load(work, 0);
    cur = p;
    edited = 0;
    work_err = 0;
    patch_seq++;
}

static void patch_random(void)                  /* a new one, with its variables (the loop and glide stay) */
{
    static uint32_t seed = 1;
    int32_t var[BB_VARS];
    uint32_t i;
    seed = seed * 1664525u + 1013904223u + fm1_ticks() + fm1_ms;   /* (when ARP was pressed: never the same) */
    rb_make(work, TEXT_MAX, var, seed);
    for (i = 0; i < BB_VARS; i++) param_set(P_A + i, var[i]);
    formula_load(work, 0);                      /* (it compiled in rb_make) */
    cur = RANDOM;
    edited = 0;
    work_err = 0;
    patch_seq++;
}

static uint32_t patch_step(uint32_t p, int32_t n) /* n patches on: the built-ins, then the used slots, round */
{
    uint32_t all = NF + SLOTS, i = p < USER ? p : NF + (p - USER);
    if (p == RANDOM) i = n > 0 ? all - 1u : 0u;  /* (from a random one: on to the first, or back to the last) */
    while (n) {
        i = n > 0 ? (i + 1u) % all : (i + all - 1u) % all;
        if (i < NF || lib[i - NF].used) n += n > 0 ? -1 : 1;
    }
    return i < NF ? i : USER + (i - NF);
}

static void patch_save(uint32_t k)              /* what plays now, into slot k, keeping its name */
{
    slot_t *s = &lib[k];
    char name[sizeof s->name];
    uint32_t i;
    copy_str(name, patch_name(cur), sizeof name - 1u);
    memset(s, 0, sizeof *s);
    copy_str(s->name, name, sizeof s->name - 1u);
    copy_str(s->text, work, TEXT_MAX);
    for (i = 0; i < BB_VARS; i++) s->var[i] = (uint8_t)param[P_A + i];
    s->loop = (uint8_t)param[P_LOOP]; s->lstart = (uint8_t)param[P_LSTART]; s->lfine = (uint8_t)param[P_LFINE];
    s->llen = (uint8_t)param[P_LLEN]; s->glide = (uint8_t)param[P_GLIDE];
    s->used = 1;
    lib_dirty |= 1u << (k / SLOTS_PER);
    cur = USER + k;
    edited = 0;
    patch_seq++;
}

/* ---- editing work: the keys' tokens go in at the cursor; each change compiles */
static uint32_t cpos;                           /* the cursor: before work[cpos] */

static uint32_t text_len(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }

static void work_changed(void)
{
    work_err = formula_load(work, &work_err_at);
    edited = 1;
}

static void work_insert(const char *tok)
{
    uint32_t len = text_len(work), n = text_len(tok), i;
    if (!n || len + n > TEXT_MAX) return;
    for (i = len + 1u; i-- > cpos; ) work[i + n] = work[i];    /* (the 0 too) */
    for (i = 0; i < n; i++) work[cpos + i] = tok[i];
    cpos += n;
    work_changed();
}

static void work_delete(uint32_t at)            /* the character at work[at] */
{
    uint32_t len = text_len(work), i;
    if (at >= len) return;
    for (i = at; i < len; i++) work[i] = work[i + 1u];
    if (cpos > at) cpos--;
    work_changed();
}

/* the keys' tokens: white keys and black keys, the second layer with FX held ("" = none) */
static const char *const TOK_WHITE[2][16] = {
    {"t", "a", "b", "c", "d", "e", "f", "g", "h", "(", ")", "+", "-", "*", "&", "|"},
    {"/", "%", "~", "!", "<", ">", "?", ":", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||"},
};
static const char *const TOK_BLACK[2][11] = {
    {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "^"},
    {"0x", "A", "B", "C", "D", "E", "F", " ", "", "", ""},
};

static int key_black(uint32_t k) { return (0x54Au >> ((53u + k) % 12u)) & 1u; }   /* key 0 = F3 */

static const char *key_token(uint32_t k, uint32_t layer)
{
    uint32_t i, w = 0, b = 0;
    for (i = 0; i < k; i++) { if (key_black(i)) b++; else w++; }
    return key_black(k) ? TOK_BLACK[layer][b] : TOK_WHITE[layer][w];
}

/* ---- the screen */
#define C_ACC RGB(180, 140, 255)                /* lilac */
#define C_OFF RGB(90, 90, 90)

static void draw_patch(void)                    /* number and name, author, the formula 30 characters a line */
{
    char s[32];
    const char *p = work;
    uint32_t n, y;
    n = 0;
    if (cur < USER) { s[0] = (char)('0' + (cur + 1u) / 10u); s[1] = (char)('0' + (cur + 1u) % 10u); n = 2; }
    else if (cur != RANDOM) { s[0] = 'U'; s[1] = (char)('0' + (cur - USER + 1u) / 10u); s[2] = (char)('0' + (cur - USER + 1u) % 10u); n = 3; }
    if (n) s[n++] = ' ';
    copy_str(s + n, patch_name(cur), edited ? 10 : 11);
    if (edited) { n = text_len(s); s[n++] = '*'; s[n] = 0; }   /* changed in the editor, not saved */
    draw_text_box(0, 4, 240, &FONT_L, s, C_ACC, 1);
    draw_text_box(0, 38, 240, &FONT_S, cur < USER ? FORMULA[cur].by : cur == RANDOM ? "made at random" : "your patch", C_OFF, 1);
    cv_begin(240, 48, C_BLACK);
    for (y = 0; *p && y < 48u; y += 16u) {
        for (n = 0; *p && n < 30u; n++) s[n] = *p++;
        s[n] = 0;
        cv_text((240 - text_w(&FONT_S, s)) / 2, (int32_t)y, &FONT_S, s, C_WHITE);
    }
    cv_blit(0, 56);
}

static uint32_t put(char *s, uint32_t i, const char *p) { while (*p) s[i++] = *p++; return i; }
static uint32_t put_num(char *s, uint32_t i, uint32_t v)
{
    if (v >= 100u) s[i++] = (char)('0' + v / 100u % 10u);
    if (v >= 10u) s[i++] = (char)('0' + v / 10u % 10u);
    s[i++] = (char)('0' + v % 10u);
    return i;
}

enum { KNOBS_AD, KNOBS_EH, KNOBS_LOOP };        /* what the knobs turn: a..d, e..h (HOME held), the loop (SEQ held) */

static void draw_knobs(uint32_t bank)           /* what the four knobs turn, 60 px each */
{
    static const char *const MODE[3] = {"OFF", "LOOP", "PING"};
    uint32_t i;
    cv_begin(240, 16, C_BLACK);
    for (i = 0; i < 4u; i++) {
        char name[2] = {(char)(bank == KNOBS_LOOP ? "SFLM"[i] : (int)('A' + 4u * bank + i)), 0}, d[8];
        int32_t x = (int32_t)i * 60 + 10;
        if (bank != KNOBS_LOOP) d[put_num(d, 0, (uint32_t)param[P_A + 4u * bank + i])] = 0;
        else if (i == 3u) d[put(d, 0, MODE[param[P_LOOP]])] = 0;
        else if (i == 2u && param[P_LLEN] >= 6) d[put(d, put_num(d, 0, 16u << param[P_LLEN] >> 10), "K")] = 0;
        else d[put_num(d, 0, i == 2u ? 16u << param[P_LLEN] : (uint32_t)param[i ? P_LFINE : P_LSTART])] = 0;
        cv_text(x, 0, &FONT_S, name, bank ? C_ACC : C_OFF);
        cv_text(x + 16, 0, &FONT_S, d, C_WHITE);
    }
    cv_blit(0, 178);
}

/* ---- the views: the formula run again here, 240 x 64 */
enum { VIEW_SCOPE, VIEW_RASTER, VIEWS };
static uint16_t shade[256];                     /* sample value -> colour: black, lilac, white */

static void shade_init(void)
{
    uint32_t v;
    for (v = 0; v < 256u; v++) {
        uint32_t a = v < 128u ? v * 2u : 255u, w = v < 128u ? 0u : (v - 128u) * 2u;
        shade[v] = RGB(180u * a / 255u + 75u * w / 255u, 140u * a / 255u + 115u * w / 255u, 255u * a / 255u);
    }
}

static uint8_t rast[64][240] __attribute__((section(".pool")));   /* the raster's rows, kept: a row of t is done once */
static uint32_t rast_row[64];                   /* the row of t each holds (row + 1, 0 = none) */

static void draw_view(uint32_t view, int fresh) /* fresh: the formula or a variable changed, the kept rows are old */
{
    const bb_prog_t *p = prog_live;
    int32_t var[BB_VARS];
    uint32_t i, x, t = t_now;
    for (i = 0; i < BB_VARS; i++) var[i] = param[P_A + i];
    cv_begin(240, 64, C_BLACK);
    if (view == VIEW_SCOPE) {                   /* the last 240 samples */
        int32_t y0 = 0, y;
        for (x = 0; x < 240u; x++, y0 = y) {
            y = 63 - ((bb_run(p, t - 239u + x, var) & 255) >> 2);
            if (x) cv_line((int32_t)x - 1, y0, (int32_t)x, y, C_ACC);
        }
    } else {                                    /* a row per 256 steps of t, the newest at the bottom */
        uint32_t row0 = (t >> 8) - 63u, r;
        if (fresh) for (r = 0; r < 64u; r++) rast_row[r] = 0;
        for (r = 0; r < 64u; r++) {
            uint32_t row = row0 + r, k = row & 63u;
            if (row > t >> 8) continue;         /* before t = 0 (row0 wrapped) */
            if (rast_row[k] != row + 1u) {      /* the row now still fills: computed again next time */
                for (x = 0; x < 240u; x++) {
                    uint32_t tt = row << 8 | (x + x / 15u);   /* 240 px over 256 steps */
                    rast[k][x] = tt <= t ? (uint8_t)bb_run(p, tt, var) : 0;
                }
                rast_row[k] = row < t >> 8 ? row + 1u : 0u;
            }
            for (x = 0; x < 240u; x++) cv_pset((int32_t)x, (int32_t)r, shade[rast[k][x]]);
        }
    }
    cv_blit(0, 108);
}

static void draw_status(uint32_t note, int32_t oct, uint32_t glide, uint32_t load, int pick)
{
    static const char *const NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    char s[40];
    uint32_t i = put(s, 0, note < 128u ? NAME[note % 12u] : "-");
    if (note < 128u) i = note < 12u ? put(s, i, "-1") : put_num(s, i, note / 12u - 1u);
    i = put(s, i, "  OCT ");
    s[i++] = oct < 0 ? '-' : '+';
    i = put_num(s, i, (uint32_t)(oct < 0 ? -oct : oct));
    i = put(s, i, "  GLIDE ");
    i = put_num(s, i, glide);
    s[i] = 0;
    draw_text_box(0, 200, 240, &FONT_S, s, C_OFF, 1);
    i = put(s, 0, pick ? "KEYS PICK  DSP " : "KEYS PLAY  DSP ");
    i = put_num(s, i, load);
    s[i++] = '%';
    s[i] = 0;
    draw_text_box(0, 220, 240, &FONT_S, s, pick ? C_ACC : C_OFF, 1);
}

/* ---- the panel */
static const uint8_t LOOP_KNOB[4] = {P_LSTART, P_LFINE, P_LLEN, P_LOOP};
static int32_t knob(uint32_t enc)               /* detents, 8x when turned fast */
{
    static uint32_t last[7];
    int32_t n = fm1_enc_take(enc);
    if (!n) return 0;
    if (fm1_ms - last[enc] < 40u) n *= 8;
    last[enc] = fm1_ms;
    return n;
}

/* ---- CC mappings: a CC moves a parameter between min and max, on any channel */
#define NMAP 12u
#define CC_OFF 128u
typedef struct { uint8_t cc, target; int16_t min, max; } ccmap_t;
static const char *const PNAME[P_N] = {"A", "B", "C", "D", "E", "F", "G", "H", "GLIDE", "VOL", "LOOP", "START", "FINE", "LEN"};

static void cc_in(const ccmap_t *map, uint32_t cc, uint32_t val)
{
    uint32_t i;
    for (i = 0; i < NMAP; i++)
        if (map[i].cc == cc)
            param_set(map[i].target, map[i].min + (map[i].max - map[i].min) * (int32_t)val / 127);
}

static void map_reset(ccmap_t *m, uint32_t target)   /* the parameter's whole range */
{
    m->target = (uint8_t)target;
    m->min = (int16_t)PARAM[target].min;
    m->max = (int16_t)PARAM[target].max;
}

static uint32_t put_pad(char *s, uint32_t i, uint32_t v, uint32_t w)   /* v right-aligned in w characters */
{
    char d[4];
    uint32_t n = put_num(d, 0, v), k;
    for (k = n; k < w; k++) s[i++] = ' ';
    for (k = 0; k < n; k++) s[i++] = d[k];
    return i;
}

/* ---- the list screens (the CC map, save): 12 rows of 16 px from y = 40, each drawn again only when it changed */
enum { ROW_PLAIN, ROW_DIM, ROW_SEL, ROW_HOT };
static char rows_drawn[12][34];

static void draw_head(const char *title, uint16_t c, const char *cols)
{
    draw_text_box(0, 4, 240, &FONT_S, title, c, 1);
    draw_text_box(0, 22, 240, &FONT_S, cols, C_OFF, 1);
}

static void draw_row(uint32_t r, const char *s, uint32_t look)
{
    char k[34] = {0};
    k[0] = (char)('0' + look);
    copy_str(k + 1, s, 32);
    if (!memcmp(k, rows_drawn[r], sizeof k)) return;
    memcpy(rows_drawn[r], k, sizeof k);
    cv_begin(240, 16, look == ROW_SEL ? C_ACC : look == ROW_HOT ? C_WHITE : C_BLACK);
    cv_text(24, 0, &FONT_S, s, look >= ROW_SEL ? C_BLACK : look == ROW_DIM ? C_OFF : C_WHITE);
    cv_blit(0, 40u + 16u * r);
}

static void draw_cc_rows(const ccmap_t *map, uint32_t sel, int learn)
{
    uint32_t r;
    for (r = 0; r < NMAP; r++) {
        const ccmap_t *m = &map[r];
        char s[32];
        uint32_t i = put_pad(s, 0, r + 1u, 2), k;
        s[i++] = ' ';
        i = m->cc == CC_OFF ? put(s, i, "OFF") : put_pad(s, i, m->cc, 3);
        s[i++] = ' ';
        for (k = put(s, i, PNAME[m->target]); k < i + 5u; k++) s[k] = ' ';
        i += 5u;
        s[i++] = ' ';
        i = put_pad(s, i, (uint32_t)m->min, 3);
        s[i++] = '-';
        i = put_pad(s, i, (uint32_t)m->max, 3);
        s[i++] = ' ';
        i = put_pad(s, i, (uint32_t)param[m->target], 3);
        s[i] = 0;
        draw_row(r, s, r == sel ? (learn ? ROW_HOT : ROW_SEL) : m->cc == CC_OFF ? ROW_DIM : ROW_PLAIN);
    }
}

static void draw_save_rows(uint32_t sel)        /* the 12 slots around sel */
{
    uint32_t r, top = sel < 6u ? 0u : sel - 6u > SLOTS - 12u ? SLOTS - 12u : sel - 6u;
    for (r = 0; r < 12u; r++) {
        uint32_t k = top + r;
        char s[20] = {'U', (char)('0' + (k + 1u) / 10u), (char)('0' + (k + 1u) % 10u), ' '};
        copy_str(s + 4, lib[k].used ? lib[k].name : "-", 11);
        draw_row(r, s, k == sel ? ROW_SEL : lib[k].used ? ROW_PLAIN : ROW_DIM);
    }
}

/* ---- the editor's screen: the formula with the cursor, whether it compiles, the keys' tokens, the knobs, help */
#define C_ERR RGB(255, 90, 90)

static void draw_edit_text(void)                /* 30 characters a line from the left, 4 lines from y = 24 */
{
    uint32_t i, len = text_len(work);
    cv_begin(240, 64, C_BLACK);
    for (i = 0; i <= len && i < 120u; i++) {
        int32_t x = (int32_t)(i % 30u) * 8, y = (int32_t)(i / 30u) * 16;
        if (i < len) { char c[2] = {work[i], 0}; cv_text(x, y, &FONT_S, c, C_WHITE); }
        if (work_err && i == (work_err_at < len ? work_err_at : len)) cv_rect(x, y + 14, 8, 2, C_ERR);
        if (i == cpos) cv_rect(x, y, 2, 16, C_ACC);
    }
    cv_blit(0, 24);
}

static void draw_edit_status(void)
{
    char s[40];
    uint32_t i = 0;
    if (work_err) {                             /* (the longest, "unexpected character at 90", fits the line) */
        i = put(s, 0, work_err);
        i = put(s, i, " at ");
        i = put_num(s, i, work_err_at + 1u);
    } else {
        i = put(s, 0, "ok  ");
        i = put_num(s, i, text_len(work));
        i = put(s, i, "/90");
    }
    s[i] = 0;
    draw_text_box(0, 92, 240, &FONT_S, s, work_err ? C_ERR : C_OFF, 1);
}

static void cv_token(int32_t x, int32_t y, const char *tok, uint16_t c)   /* one character, or two stacked */
{
    char s[2] = {tok[0] == ' ' ? '_' : tok[0], 0};
    if (!tok[0]) return;
    cv_text(x, y, &FONT_S, s, c);
    if (tok[1]) { s[0] = tok[1]; cv_text(x, y + 16, &FONT_S, s, c); }
}

static void draw_palette(uint32_t layer)        /* the keys as a keyboard, each with its token, 240 x 64 */
{
    uint32_t k, w;
    cv_begin(240, 64, C_BLACK);
    for (k = 0, w = 0; k < 27u; k++)            /* 16 white keys of 15 px, the token low on the key */
        if (!key_black(k)) {
            const char *tok = key_token(k, layer);
            cv_rect((int32_t)w * 15, 0, 14, 64, RGB(200, 200, 200));
            cv_token((int32_t)w * 15 + 3, tok[1] ? 32 : 46, tok, C_BLACK);
            w++;
        }
    for (k = 0, w = 0; k < 27u; k++)            /* the black ones on the line between two white */
        if (key_black(k)) {
            cv_rect((int32_t)w * 15 - 5, 0, 10, 34, RGB(40, 40, 40));
            cv_token((int32_t)w * 15 - 4, 1, key_token(k, layer), C_WHITE);
        } else
            w++;
    cv_blit(0, 112);
}

static void draw_edit_help(void)
{
    draw_text_box(0, 200, 240, &FONT_S, "FX MORE OCT-/+ DEL ARP RANDOM", C_OFF, 1);
    draw_text_box(0, 220, 240, &FONT_S, "PLAY NOTE   SAVE   EDIT DONE", C_OFF, 1);
}

/* ---- the settings: what survives a power-off */
typedef struct { uint8_t magic, patch, glide, pick, view; int8_t oct; ccmap_t map[NMAP]; } settings_t;

static void settings_default(settings_t *s)
{
    uint32_t i;
    memset(s, 0, sizeof *s);
    s->magic = 'C';
    s->patch = 2;
    s->view = VIEW_SCOPE;
    for (i = 0; i < NMAP; i++) {                /* off; given a CC, slot 1 moves a, 2 b ... 9 glide, 10 volume */
        s->map[i].cc = CC_OFF;
        map_reset(&s->map[i], i < P_N ? i : 0u);
    }
}

static void settings_check(settings_t *s)       /* what a power-off or another version may have left */
{
    uint32_t k;
    for (k = 0; k < NMAP; k++) {
        ccmap_t *m = &s->map[k];
        if (m->cc > 119u) m->cc = CC_OFF;
        if (m->target >= P_N) map_reset(m, 0);
        if (m->min < PARAM[m->target].min || m->min > PARAM[m->target].max ||
            m->max < PARAM[m->target].min || m->max > PARAM[m->target].max) map_reset(m, m->target);
    }
    if (!patch_ok(s->patch)) s->patch = 2;
    if (s->glide > 10u) s->glide = 0;
    if (s->oct < -2 || s->oct > 2) s->oct = 0;
    s->pick = !!s->pick;
    if (s->view >= VIEWS) s->view = VIEW_SCOPE;
}

/* ---- the panel and the screens */
enum { SCR_MAIN, SCR_CC, SCR_SAVE, SCR_EDIT };
static settings_t set;
static uint32_t dirty, t_change;                /* the settings changed, when (saved 2 s later, when silent) */
static uint32_t scr = SCR_MAIN, scr_fresh = 1;  /* the screen showing; to draw whole */
static uint32_t cc_sel, learn, save_sel;        /* the CC screen's slot, learning; the save screen's slot */
static uint32_t audition;                       /* the editor's C4 sounds */
static uint32_t bank, seq_turned, seq_down, loop_last = LOOP_ON;   /* the knobs' bank; SEQ: turned while held, when
                                                                       pressed (a tap, or a hold); the mode a tap turns on */

static void changed(void) { dirty = 1; t_change = fm1_ms; }

static void screen(uint32_t s)
{
    if (audition && s != SCR_EDIT) { audition = 0; note_off(60); }
    scr = s;
    learn = 0;
    scr_fresh = 1;
}

static void save_open(void)                     /* the save screen: on the slot playing, else the first free one */
{
    if (work_err) return;                       /* (what does not compile is not saved) */
    save_sel = 0;
    if (cur >= USER && cur - USER < SLOTS) save_sel = cur - USER;
    else while (save_sel < SLOTS - 1u && lib[save_sel].used) save_sel++;
    screen(SCR_SAVE);
}

static void pick(uint32_t p)
{
    patch_pick(p);
    set.patch = (uint8_t)p;
    set.glide = (uint8_t)param[P_GLIDE];
    changed();
}

static void knobs_input(uint32_t b, uint32_t rel)   /* the four knobs, in the bank HOME or SEQ hold; SEQ's tap */
{
    int32_t n;
    uint32_t k;
    if (b & (1u << BTN_SEQ)) { seq_turned = 0; seq_down = fm1_ms; }
    for (k = 0; k < 4u; k++) {
        uint32_t id = bank == KNOBS_LOOP ? (uint32_t)LOOP_KNOB[k] : P_A + 4u * bank + k;
        if (!(n = id == P_LOOP || id == P_LLEN ? fm1_enc_take(ENC_KNOB1 + k) : knob(ENC_KNOB1 + k))) continue;
        param_set(id, param[id] + n);
        if (bank == KNOBS_LOOP) seq_turned = 1;
        if (id == P_LOOP && param[P_LOOP]) loop_last = (uint32_t)param[P_LOOP];
    }
    if (rel & (1u << BTN_SEQ) && !seq_turned && fm1_ms - seq_down < 300u)   /* a tap, not a hold */
        param_set(P_LOOP, param[P_LOOP] ? LOOP_OFF : (int32_t)loop_last);
}

static void main_input(uint32_t b, uint32_t rel)
{
    int32_t n;
    if (b & (1u << BTN_SEL)) { set.pick ^= 1u; changed(); }
    if (b & (1u << BTN_FX)) { set.view = (uint8_t)((set.view + 1u) % VIEWS); changed(); }
    if (b & (1u << BTN_SAVE)) save_open();
    if (b & (1u << BTN_EDIT)) { cpos = text_len(work); screen(SCR_EDIT); }
    if ((n = fm1_enc_take(ENC_PRESETS))) pick(patch_step(cur, n));
    knobs_input(b, rel);
    if ((n = fm1_enc_take(ENC_SELECT))) {
        param_set(P_GLIDE, param[P_GLIDE] + n);
        set.glide = (uint8_t)param[P_GLIDE];
        changed();
    }
}

static void cc_input(uint32_t b)
{
    ccmap_t *m = &set.map[cc_sel];
    int32_t lo = PARAM[m->target].min, hi = PARAM[m->target].max, n, c;
    if (b & (1u << BTN_REC)) learn ^= 1u;
    if ((n = fm1_enc_take(ENC_SELECT))) {
        c = (int32_t)cc_sel + n;
        cc_sel = c < 0 ? 0u : c >= (int32_t)NMAP ? NMAP - 1u : (uint32_t)c;
        learn = 0;
    }
    if ((n = knob(ENC_KNOB1))) {                /* off, 0..119 (120..127 are MIDI's channel modes) */
        c = (m->cc == CC_OFF ? -1 : (int32_t)m->cc) + n;
        m->cc = c < 0 ? (uint8_t)CC_OFF : (uint8_t)(c > 119 ? 119 : c);
        changed();
    }
    if ((n = fm1_enc_take(ENC_KNOB2))) {
        c = (int32_t)m->target + n;
        map_reset(m, c < 0 ? 0u : c >= (int32_t)P_N ? P_N - 1u : (uint32_t)c);
        changed();
    }
    if ((n = knob(ENC_KNOB3))) { c = m->min + n; m->min = (int16_t)(c < lo ? lo : c > hi ? hi : c); changed(); }
    if ((n = knob(ENC_KNOB4))) { c = m->max + n; m->max = (int16_t)(c < lo ? lo : c > hi ? hi : c); changed(); }
}

static void edit_input(uint32_t b, uint32_t rel, uint32_t keys_down)
{
    uint32_t k, len = text_len(work), layer = fm1_in.buttons >> BTN_FX & 1u;
    int32_t n, c;
    for (k = 0; k < 27u; k++)
        if (keys_down >> k & 1u) work_insert(key_token(k, layer));
    if (b & (1u << BTN_OCT_DOWN) && cpos) work_delete(cpos - 1u);
    if (b & (1u << BTN_OCT_UP)) work_delete(cpos);
    if ((n = fm1_enc_take(ENC_SELECT))) {
        c = (int32_t)cpos + n;
        cpos = c < 0 ? 0u : c > (int32_t)len ? len : (uint32_t)c;
    }
    if (b & (1u << BTN_PLAY)) {
        audition ^= 1u;
        if (audition) note_on(60, 100); else note_off(60);
    }
    if (b & (1u << BTN_ARP)) { patch_random(); cpos = text_len(work); }   /* (not kept in the settings: a power-off
                                                                              goes back to the last patch picked) */
    knobs_input(b, rel);
    if (b & (1u << BTN_SAVE)) save_open();
    else if (b & (1u << BTN_EDIT)) screen(SCR_MAIN);
}

static void save_input(uint32_t b)
{
    int32_t n = fm1_enc_take(ENC_SELECT) + fm1_enc_take(ENC_PRESETS), c;
    if (n) {
        c = (int32_t)save_sel + n;
        save_sel = c < 0 ? 0u : c >= (int32_t)SLOTS ? SLOTS - 1u : (uint32_t)c;
    }
    if (b & (1u << BTN_SAVE)) {
        patch_save(save_sel);
        set.patch = (uint8_t)cur;
        changed();
        screen(SCR_MAIN);
    } else if (b & (1u << BTN_HOME))
        screen(SCR_MAIN);
}

static void app_main(void)
{
    uint32_t prev = 0, sounding = 0, k, t_load = 0, load = 0;
    uint32_t p_drawn = ~0u, note_drawn = ~0u, status_drawn = ~0u, head_drawn = ~0u, bank_drawn = ~0u;
    uint32_t t_view = 0, view_t = ~0u, view_shown = ~0u, view_fresh = 1;
    uint32_t cpos_drawn = ~0u, layer_drawn = ~0u;   /* the editor's */
    char text_drawn[TEXT_MAX + 1u] = {0};
    int32_t v_drawn[4] = {-1, -1, -1, -1}, v_seen[BB_VARS] = {0};
    uint8_t key_note[27] = {0};                 /* the note each key started */
    lib_init();
    settings_default(&set);
    if (settings_load(&set, sizeof set) || set.magic != 'C') settings_default(&set);
    settings_check(&set);
    shade_init();
    param_set(P_VOL, 127);
    param_set(P_LLEN, 8);                       /* 4096 steps, half a second at middle C */
    patch_pick(set.patch);
    if (set.patch < USER) param_set(P_GLIDE, set.glide);   /* (a slot brings its own) */

    for (;;) {
        uint32_t keys, down, up, rel, b, base = (uint32_t)(53 + 12 * set.oct), st_now;
        int32_t v[4];
        uint8_t st, d1, d2;
        platform_poll();

        keys = fm1_in.notes;                    /* bit k: key k (0 = F3 .. 26 = G5) */
        down = keys & ~prev;
        up = prev & ~keys;
        prev = keys;
        for (k = 0; k < 27u; k++) {
            if (up >> k & sounding >> k & 1u) {  /* only the keys that started a note stop one */
                sounding &= ~(1u << k);
                note_off(key_note[k]);
                midi_send(0x80, key_note[k], 0);
            }
            if (!(down >> k & 1u) || scr == SCR_EDIT) continue;   /* (the editor types with them) */
            if (set.pick) {
                if (k < NF) pick(k);
                continue;
            }
            key_note[k] = (uint8_t)(base + k);
            sounding |= 1u << k;
            note_on(key_note[k], 100);
            midi_send(0x90, key_note[k], 100);
        }

        b = fm1_input_edges(&rel);
        bank = fm1_in.buttons >> BTN_SEQ & 1u ? KNOBS_LOOP : fm1_in.buttons >> BTN_HOME & 1u ? KNOBS_EH : KNOBS_AD;
        if (scr != SCR_EDIT) {                  /* (the editor deletes with them) */
            if (b & (1u << BTN_OCT_DOWN) && set.oct > -2) { set.oct--; changed(); }
            if (b & (1u << BTN_OCT_UP) && set.oct < 2) { set.oct++; changed(); }
        }
        if (b & (1u << BTN_GLO)) screen(scr == SCR_CC ? SCR_MAIN : SCR_CC);
        else if (scr == SCR_MAIN) main_input(b, rel);
        else if (scr == SCR_CC) cc_input(b);
        else if (scr == SCR_EDIT) edit_input(b, rel, down);
        else save_input(b);

        while (midi_get(&st, &d1, &d2)) {       /* USB-MIDI in, any channel */
            if ((st & 0xF0u) == 0x90u && d2) note_on(d1, d2);
            else if ((st & 0xF0u) == 0x80u || (st & 0xF0u) == 0x90u) note_off(d1);
            else if ((st & 0xF0u) == 0xE0u) bend_set((int32_t)((uint32_t)d2 << 7 | d1) - 8192);
            else if ((st & 0xF0u) == 0xB0u && d1 < 120u) {
                if (learn) { set.map[cc_sel].cc = d1; learn = 0; changed(); }
                cc_in(set.map, d1, d2);
            }
        }

        for (k = 0; k < 27u; k++)               /* lit: the key held (editor), the formula picked, the note that sounds */
            fm1_led_key(KEY_LED(k), scr == SCR_EDIT ? keys >> k & 1u : set.pick ? k == cur : gate && cur_note == base + k);
        fm1_led_key(BTN_SEL, set.pick);
        fm1_led_key(BTN_GLO, scr == SCR_CC);
        fm1_led_key(BTN_SAVE, scr == SCR_SAVE);
        fm1_led_key(BTN_EDIT, scr == SCR_EDIT);
        fm1_led_key(BTN_PLAY, audition);
        fm1_led_key(BTN_FX, scr == SCR_EDIT && fm1_in.buttons >> BTN_FX & 1u);
        fm1_led_key(BTN_REC, learn);
        fm1_led_key(BTN_HOME, bank == KNOBS_EH);
        fm1_led_key(BTN_SEQ, param[P_LOOP] != LOOP_OFF);
        fm1_led_key(BTN_OCT_DOWN, set.oct < 0);
        fm1_led_key(BTN_OCT_UP, set.oct > 0);

        if (fm1_ms - t_load >= 250u) {          /* the render's share of its 5.8 ms */
            t_load = fm1_ms;
            load = dsp_us * 100u / 5805u;
        }
        if (scr_fresh) {                        /* a screen comes up: everything on it is drawn again */
            scr_fresh = 0;
            lcd_fill(0, 0, 240, 240, C_BLACK);
            p_drawn = note_drawn = status_drawn = head_drawn = view_shown = cpos_drawn = layer_drawn = ~0u;
            for (k = 0; k < 4u; k++) v_drawn[k] = -1;
            memset(rows_drawn, 0, sizeof rows_drawn);
        }
        for (k = 0; k < BB_VARS; k++)           /* any of a..h changed: the views show something else */
            if (param[P_A + k] != v_seen[k]) { v_seen[k] = param[P_A + k]; view_fresh = 1; view_t = ~0u; }
        if (scr == SCR_CC) {
            if (learn != head_drawn) {
                head_drawn = learn;
                draw_head(learn ? "LEARN: MOVE A CONTROL" : "CC MAP", learn ? C_WHITE : C_ACC, " #  CC TO    MIN-MAX NOW");
            }
            draw_cc_rows(set.map, cc_sel, (int)learn);
        } else if (scr == SCR_SAVE) {
            if (head_drawn) { head_drawn = 0; draw_head("SAVE TO", C_ACC, "SAVE: OK    HOME: CANCEL"); }
            draw_save_rows(save_sel);
        } else if (scr == SCR_EDIT) {
            uint32_t layer = fm1_in.buttons >> BTN_FX & 1u;
            if (head_drawn || patch_seq != p_drawn) {   /* (the patch changes here with ARP) */
                char t[24] = "EDIT ";
                head_drawn = 0;
                p_drawn = patch_seq;
                copy_str(t + 5, patch_name(cur), 11);
                draw_text_box(0, 4, 240, &FONT_S, t, C_ACC, 1);
                draw_edit_help();
            }
            if (memcmp(work, text_drawn, sizeof work) || cpos != cpos_drawn) {   /* (work_err follows work) */
                memcpy(text_drawn, work, sizeof work);
                cpos_drawn = cpos;
                draw_edit_text();
                draw_edit_status();
            }
            if (layer != layer_drawn) { layer_drawn = layer; draw_palette(layer); }
            for (k = 0; k < 4u; k++) v[k] = param[bank == KNOBS_LOOP ? (uint32_t)LOOP_KNOB[k] : P_A + 4u * bank + k];
            if (v[0] != v_drawn[0] || v[1] != v_drawn[1] || v[2] != v_drawn[2] || v[3] != v_drawn[3] || bank != bank_drawn) {
                for (k = 0; k < 4u; k++) v_drawn[k] = v[k];
                bank_drawn = bank;
                draw_knobs(bank);
            }
        } else {
            for (k = 0; k < 4u; k++) v[k] = param[bank == KNOBS_LOOP ? (uint32_t)LOOP_KNOB[k] : P_A + 4u * bank + k];
            st_now = (uint32_t)(set.oct + 2) + 5u * (uint32_t)param[P_GLIDE] + 55u * set.pick + 110u * load;
            if (patch_seq != p_drawn) { p_drawn = patch_seq; draw_patch(); view_fresh = 1; view_t = ~0u; }
            if (v[0] != v_drawn[0] || v[1] != v_drawn[1] || v[2] != v_drawn[2] || v[3] != v_drawn[3] || bank != bank_drawn) {
                for (k = 0; k < 4u; k++) v_drawn[k] = v[k];
                bank_drawn = bank;
                draw_knobs(bank);
            }
            if (set.view != view_shown) { view_shown = set.view; view_t = ~0u; }
            if (fm1_ms - t_view >= 40u && t_now != view_t) {
                t_view = fm1_ms;                /* 25 frames a second, while t moves or what it shows changed */
                view_t = t_now;
                draw_view(set.view, (int)view_fresh);
                view_fresh = 0;
            }
            if (cur_note != note_drawn || st_now != status_drawn) {
                note_drawn = cur_note; status_drawn = st_now;
                draw_status(cur_note, set.oct, (uint32_t)param[P_GLIDE], load, set.pick);
            }
        }

        if (!gate && dirty && fm1_ms - t_change > 2000u) {   /* 2 s after the change, when silent (an erase stalls the sound) */
            settings_save(&set, sizeof set);
            dirty = 0;
        }
        if (!gate && lib_dirty) {               /* a slot saved: its sector, when silent */
            for (k = 0; k < LIB_SECTORS; k++)
                if (lib_dirty >> k & 1u) lib_save(k, &lib[k * SLOTS_PER], SLOTS_PER * sizeof(slot_t));
            lib_dirty = 0;
        }
    }
}
