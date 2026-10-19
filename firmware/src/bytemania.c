/* SPDX-License-Identifier: GPL-3.0-only */
/* Bytemania: a bytebeat synth for the FM-1, built on Dinghy. A formula of t and the variables a..h makes
 * an 8-bit sample each step of t; the note sets how fast t runs (middle C = 8 kHz, the classic rate).
 *
 *   keys       play the note (mono, the last key wins); with SEL lit, pick a formula instead
 *   SEL        the keys: play / pick a formula
 *   FX         the view: scope / raster
 *   PRESETS    the formula
 *   KNOB1..4   the variables a..d (faster when turned fast)
 *   SELECT     the glide
 *   OCT- OCT+  the octave
 *   MASTER     the volume (platform.c)
 *   USB-MIDI   notes in play, with their velocity; pitch bend +-2 semitones
 *
 * The formulas: bytebeat.c (compiled in the main loop, run in the audio interrupt). The views run the
 * formula again in the main loop, at the t the interrupt has reached.
 * What platform.c offers: docs/API.md. The chip's rules: docs/RULES.md. */
#define APP_NAME "BYTEMANIA"                    /* the rescue and the crash screen say this */
#include "platform.c"
#include "bytebeat.c"
#include "formulas.h"

/* ---- the parameters: one table that the knobs, MIDI and (later) the CC mappings all write */
enum { P_A, P_B, P_C, P_D, P_E, P_F, P_G, P_H, P_GLIDE, P_VOL, P_N };
static const struct { int32_t min, max; } PARAM[P_N] = {
    {0, 255}, {0, 255}, {0, 255}, {0, 255}, {0, 255}, {0, 255}, {0, 255}, {0, 255}, {0, 10}, {0, 127},
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

static void app_audio(int32_t *out, uint32_t n)
{
    static uint32_t t, frac, inc, seq;
    static int32_t amp, s;
    const bb_prog_t *p = prog_live;
    int32_t var[BB_VARS], g = gate;
    uint32_t i, t0 = fm1_ticks(), target = inc_target, glide = (uint32_t)param[P_GLIDE], bm = bend_mul, step;
    int32_t vol = param[P_VOL];
    prog_used = p;
    if (seq != note_seq) { seq = note_seq; t = 0; frac = 0; inc = target; }
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
            t += frac >> 16;
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

/* ---- the screen */
#define C_ACC RGB(180, 140, 255)                /* lilac */
#define C_OFF RGB(90, 90, 90)

static void draw_formula(uint32_t f)            /* number and name, author, the formula 30 characters a line */
{
    char s[32];
    const char *p = FORMULA[f].text;
    uint32_t n = 0, y;
    s[0] = (char)('0' + (f + 1u) / 10u);
    s[1] = (char)('0' + (f + 1u) % 10u);
    s[2] = ' ';
    while (FORMULA[f].name[n] && n < 12u) { s[3u + n] = FORMULA[f].name[n]; n++; }
    s[3u + n] = 0;
    draw_text_box(0, 4, 240, &FONT_L, s, C_ACC, 1);
    draw_text_box(0, 38, 240, &FONT_S, FORMULA[f].by, C_OFF, 1);
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

static void draw_vars(const int32_t *v)         /* a..d, 60 px each */
{
    uint32_t i;
    cv_begin(240, 16, C_BLACK);
    for (i = 0; i < 4u; i++) {
        char name[2] = {(char)('A' + i), 0}, d[4];
        int32_t x = (int32_t)i * 60 + 10;
        d[put_num(d, 0, (uint32_t)v[i])] = 0;
        cv_text(x, 0, &FONT_S, name, C_OFF);
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
static int32_t knob(uint32_t enc)               /* detents, 8x when turned fast */
{
    static uint32_t last[7];
    int32_t n = fm1_enc_take(enc);
    if (!n) return 0;
    if (fm1_ms - last[enc] < 40u) n *= 8;
    last[enc] = fm1_ms;
    return n;
}

static void formula_pick(uint32_t f)
{
    uint32_t i;
    formula_load(FORMULA[f].text, 0);           /* (the bank compiles: tests/test_bytebeat.c) */
    for (i = 0; i < BB_VARS; i++) param_set(P_A + i, i < 4u ? FORMULA[f].def[i] : 0);
}

typedef struct { uint8_t magic, formula, glide, pick, view; int8_t oct; } settings_t;   /* what survives a power-off */
static const settings_t SET0 = {'B', 2, 0, 0, VIEW_SCOPE, 0};

static void app_main(void)
{
    settings_t set = SET0;
    uint32_t prev = 0, sounding = 0, k, t_change = 0, dirty = 0, t_load = 0, load = 0;
    uint32_t f_drawn = ~0u, note_drawn = ~0u, status_drawn = ~0u, t_view = 0, view_t = ~0u, view_stale = 1, view_fresh = 1;
    int32_t v_drawn[4] = {-1, -1, -1, -1}, v_seen[BB_VARS] = {0};
    uint8_t key_note[27] = {0};                 /* the note each key started */
    if (settings_load(&set, sizeof set) || set.magic != SET0.magic) set = SET0;
    if (set.formula >= NF) set.formula = 0;
    if (set.glide > 10u) set.glide = 0;
    if (set.oct < -2 || set.oct > 2) set.oct = 0;
    set.pick = !!set.pick;
    if (set.view >= VIEWS) set.view = VIEW_SCOPE;
    shade_init();
    formula_pick(set.formula);
    param_set(P_GLIDE, set.glide);
    param_set(P_VOL, 127);

    lcd_fill(0, 0, 240, 240, C_BLACK);

    for (;;) {
        uint32_t keys, down, up, rel, b, base = (uint32_t)(53 + 12 * set.oct), st_now;
        int32_t n, v[4];
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
            if (!(down >> k & 1u)) continue;
            if (set.pick) {
                if (k < NF) { set.formula = (uint8_t)k; formula_pick(k); dirty = 1; t_change = fm1_ms; }
                continue;
            }
            key_note[k] = (uint8_t)(base + k);
            sounding |= 1u << k;
            note_on(key_note[k], 100);
            midi_send(0x90, key_note[k], 100);
        }

        b = fm1_input_edges(&rel);
        if (b & (1u << BTN_OCT_DOWN) && set.oct > -2) { set.oct--; dirty = 1; t_change = fm1_ms; }
        if (b & (1u << BTN_OCT_UP) && set.oct < 2) { set.oct++; dirty = 1; t_change = fm1_ms; }
        if (b & (1u << BTN_SEL)) { set.pick ^= 1u; dirty = 1; t_change = fm1_ms; }
        if (b & (1u << BTN_FX)) { set.view = (uint8_t)((set.view + 1u) % VIEWS); view_stale = 1; dirty = 1; t_change = fm1_ms; }

        if ((n = fm1_enc_take(ENC_PRESETS))) {
            set.formula = (uint8_t)(((int32_t)set.formula + n % (int32_t)NF + (int32_t)NF) % (int32_t)NF);
            formula_pick(set.formula); dirty = 1; t_change = fm1_ms;
        }
        for (k = 0; k < 4u; k++)
            if ((n = knob(ENC_KNOB1 + k))) param_set(P_A + k, param[P_A + k] + n);
        if ((n = fm1_enc_take(ENC_SELECT))) {
            param_set(P_GLIDE, param[P_GLIDE] + n);
            set.glide = (uint8_t)param[P_GLIDE]; dirty = 1; t_change = fm1_ms;
        }

        while (midi_get(&st, &d1, &d2)) {       /* USB-MIDI in, any channel */
            if ((st & 0xF0u) == 0x90u && d2) note_on(d1, d2);
            else if ((st & 0xF0u) == 0x80u || (st & 0xF0u) == 0x90u) note_off(d1);
            else if ((st & 0xF0u) == 0xE0u) bend_set((int32_t)((uint32_t)d2 << 7 | d1) - 8192);
        }

        for (k = 0; k < 27u; k++)               /* lit: the formula picked, or the note that sounds */
            fm1_led_key(KEY_LED(k), set.pick ? k == set.formula : gate && cur_note == base + k);
        fm1_led_key(BTN_SEL, set.pick);
        fm1_led_key(BTN_OCT_DOWN, set.oct < 0);
        fm1_led_key(BTN_OCT_UP, set.oct > 0);

        if (fm1_ms - t_load >= 250u) {          /* the render's share of its 5.8 ms */
            t_load = fm1_ms;
            load = dsp_us * 100u / 5805u;
        }
        for (k = 0; k < 4u; k++) v[k] = param[P_A + k];
        st_now = (uint32_t)(set.oct + 2) + 5u * (uint32_t)param[P_GLIDE] + 55u * set.pick + 110u * load;
        if (set.formula != f_drawn) { f_drawn = set.formula; draw_formula(set.formula); view_stale = view_fresh = 1; }
        if (v[0] != v_drawn[0] || v[1] != v_drawn[1] || v[2] != v_drawn[2] || v[3] != v_drawn[3]) {
            for (k = 0; k < 4u; k++) v_drawn[k] = v[k];
            draw_vars(v);
        }
        for (k = 0; k < BB_VARS; k++)           /* any of a..h changed: the views show something else */
            if (param[P_A + k] != v_seen[k]) { v_seen[k] = param[P_A + k]; view_stale = view_fresh = 1; }
        if (fm1_ms - t_view >= 40u && (t_now != view_t || view_stale)) {
            t_view = fm1_ms;                    /* 25 frames a second, while t moves or what it shows changed */
            view_t = t_now;
            draw_view(set.view, (int)view_fresh);
            view_stale = view_fresh = 0;
        }
        if (cur_note != note_drawn || st_now != status_drawn) {
            note_drawn = cur_note; status_drawn = st_now;
            draw_status(cur_note, set.oct, (uint32_t)param[P_GLIDE], load, set.pick);
        }

        if (dirty && !gate && fm1_ms - t_change > 2000u) {   /* 2 s after the change, when silent (an erase stalls the sound) */
            settings_save(&set, sizeof set);
            dirty = 0;
        }
    }
}
