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
 *   LFO        tap: the voice mode: MONO, DUAL (left and right, the right detuned and offset), POLY (4 voices, no
 *              glide); kept with the patch. Held, the knobs are DUAL's detune and offset
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
 *   ENV        a blank patch: t alone, the variables 0, no loop, no glide
 *
 * The save screen: SELECT or PRESETS picks a slot, SAVE goes on to name it (the keys type A..Z, the top one a
 * space; OCT- deletes; SAVE saves), ENV twice clears it, HOME goes back.
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
enum { P_A, P_B, P_C, P_D, P_E, P_F, P_G, P_H, P_GLIDE, P_VOL, P_LOOP, P_LSTART, P_LFINE, P_LLEN, P_DETUNE, P_OFFSET,
       P_N };                                  /* (the CC mappings keep these numbers: new ones go at the end) */
enum { LOOP_OFF, LOOP_ON, LOOP_PING };
static const struct { int32_t min, max; } PARAM[P_N] = {
    {0, 255}, {0, 255}, {0, 255}, {0, 255}, {0, 255}, {0, 255}, {0, 255}, {0, 255}, {0, 10}, {0, 127},
    {0, 2}, {0, 255}, {0, 255}, {0, 12},        /* the loop: mode, start (rows of 256), + fine, length 16 << n */
    {0, 100}, {0, 255},                         /* DUAL: the right side's detune (cents), t offset (16 steps) */
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

/* ---- the voices: the notes the main loop plays. MONO plays voice 0, the last held note; POLY all four */
#define NV 4u
enum { MODE_MONO, MODE_DUAL, MODE_POLY, MODE_MULTI, MODES };
static struct {
    volatile uint32_t inc;                      /* t steps per frame for its note, Q16 */
    volatile int32_t gate;                      /* target level, Q15: 0 = released */
    volatile uint32_t seq;                      /* + 1 for a note from silence: t starts over, no glide */
    uint32_t note, age;                         /* (the main loop's) */
} voice[NV];
static volatile uint32_t vmode;                 /* MODE_..., the patch's */
static volatile uint32_t bend_mul = 65536u;     /* pitch bend, Q16: on top of the glide, so it is immediate */
static volatile uint32_t dsp_us;                /* the last render's time */
static volatile uint32_t t_now;                 /* where lane 0's t is, for the views */

/* ---- the sound (runs in the audio ISR: keep it short, no flash, no screen). It renders lanes: each plays a voice's
 * note through a formula into the left, the right or both channels. MONO is 1 lane, POLY 4. */
static uint32_t loop_t(uint32_t pos, uint32_t mode, uint32_t start, uint32_t len)   /* len: a power of 2 */
{
    uint32_t off;
    if (mode == LOOP_OFF || pos < start) return pos;
    off = (pos - start) & (mode == LOOP_PING ? 2u * len - 1u : len - 1u);
    return start + (off < len ? off : 2u * len - 1u - off);      /* ping: the second half runs back */
}

static void app_audio(int32_t *out, uint32_t n)
{
    static struct { uint32_t pos, t, frac, inc, seq; int32_t amp, s; } ln[NV];   /* pos: steps since the note began;
                                                                                    t: where the loop puts it */
    const bb_prog_t *p = prog_live;
    int32_t var[BB_VARS], vol = param[P_VOL];
    uint32_t mode = vmode, lanes = mode == MODE_POLY ? NV : mode == MODE_MONO ? 1u : 2u, k, i, t0 = fm1_ticks();
    uint32_t bm_r = (uint32_t)((uint64_t)bend_mul * (65536u + 38u * (uint32_t)param[P_DETUNE]) >> 16);   /* + 1 cent:
                                                                                   x 1.000578 (~ 38 / 65536) */
    uint32_t glide = mode == MODE_POLY ? 0u : (uint32_t)param[P_GLIDE], bm = bend_mul;
    uint32_t lmode = (uint32_t)param[P_LOOP], lstart = (uint32_t)(param[P_LSTART] * 256 + param[P_LFINE]);
    uint32_t llen = 16u << param[P_LLEN];
    prog_used = p;
    for (i = 0; i < BB_VARS; i++) var[i] = param[P_A + i];
    for (k = 0; k < lanes; k++) {
        uint32_t src = mode == MODE_POLY ? k : 0u, target = voice[src].inc, pos, t, frac, inc, step;
        uint32_t pan = lanes == 2u ? 1u << k : 3u, mul = mode == MODE_DUAL && k ? bm_r : bm;   /* bit 0 L, bit 1 R */
        int32_t g = voice[src].gate, amp, s;
        if (ln[k].seq != voice[src].seq) {     /* t from 0; DUAL's right side ahead by its offset */
            ln[k].seq = voice[src].seq;
            ln[k].pos = ln[k].t = mode == MODE_DUAL && k ? 16u * (uint32_t)param[P_OFFSET] : 0u;
            ln[k].frac = 0;
            ln[k].inc = target;
        }
        pos = ln[k].pos; t = ln[k].t; frac = ln[k].frac; inc = ln[k].inc; amp = ln[k].amp; s = ln[k].s;
        if (!amp && !g) continue;               /* silent: costs nothing */
        step = (uint32_t)((uint64_t)inc * mul >> 16);
        for (i = 0; i < n; i++) {
            int32_t v;
            if (!(i & 15u) && inc != target) {  /* the glide, every 16 frames: 1 / 2^glide of the way */
                int32_t d = (int32_t)(target - inc) >> glide;
                inc = d ? inc + (uint32_t)d : target;
                step = (uint32_t)((uint64_t)inc * mul >> 16);
            }
            frac += step;
            if (frac >> 16) {                   /* t moved: a new sample, held until it moves again */
                pos += frac >> 16;
                t = loop_t(pos, lmode, lstart, llen);
                frac &= 0xFFFFu;
                s = ((bb_run(p, t, var) & 255) - 128) * 256;
            }
            if (amp < g) { amp += 48; if (amp > g) amp = g; }        /* attack ~10 ms */
            else if (amp > g) { amp -= 6; if (amp < g) amp = g; }    /* release ~100 ms */
            v = (s * ((amp * vol) >> 7)) >> 15;
            if (pan & 1u) out[2u * i] += v;     /* L */
            if (pan & 2u) out[2u * i + 1u] += v;   /* R */
        }
        ln[k].pos = pos; ln[k].t = t; ln[k].frac = frac; ln[k].inc = inc; ln[k].amp = amp; ln[k].s = s;
    }
    t_now = ln[0].t;
    dsp_us = (fm1_ticks() - t0) / FM1_TICKS_PER_US;
}

/* ---- notes (main loop). MONO: the last held note sounds, letting go goes back to the one before (legato: no new
 * t, and the glide). POLY: each note a voice: the same note again, else a free one, else the oldest is taken. */
static const uint32_t INC_C4[12] = {            /* MIDI 60..71: 8 kHz * 2^(i/12) * 65536 / 44100; other octaves shift */
    11889u, 12596u, 13345u, 14138u, 14979u, 15869u, 16813u, 17813u, 18872u, 19994u, 21183u, 22443u};
static uint8_t held[16];
static uint32_t nheld, cur_note = 255, ages;

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

static int note_sounds(uint32_t note)           /* (the key LEDs) */
{
    uint32_t k;
    for (k = 0; k < NV; k++)
        if (voice[k].gate && voice[k].note == note) return 1;
    return 0;
}

static int silent(void)                         /* no note held: flash may be written (it stalls the sound) */
{
    uint32_t k;
    for (k = 0; k < NV; k++)
        if (voice[k].gate) return 0;
    return 1;
}

static void note_on(uint32_t note, uint32_t vel)
{
    if (note > 127u || !vel) return;
    cur_note = note;
    if (vmode == MODE_POLY) {
        uint32_t k, pick = 0, best = ~0u;
        for (k = 0; k < NV; k++) {
            uint32_t score = voice[k].gate && voice[k].note == note ? 0u
                           : voice[k].gate ? 0x80000000u + voice[k].age : 1u + voice[k].age;
            if (score < best) { best = score; pick = k; }
        }
        voice[pick].note = note;
        voice[pick].age = ++ages;
        voice[pick].inc = inc_of(note);         /* the pitch, then t from 0, then the gate (RULES.md) */
        voice[pick].seq++;
        voice[pick].gate = (int32_t)(vel * 100u);   /* half a MONO note: four together clip less */
        return;
    }
    held_drop(note);
    if (nheld == sizeof held) held_drop(held[0]);
    held[nheld++] = (uint8_t)note;
    voice[0].note = note;
    voice[0].inc = inc_of(note);
    if (!voice[0].gate) voice[0].seq++;         /* from silence: t from 0, no glide */
    voice[0].gate = (int32_t)(vel * 200u);      /* 127 -> 25400 */
}

static void note_off(uint32_t note)
{
    uint32_t k;
    if (vmode == MODE_POLY) {
        for (k = 0; k < NV; k++)
            if (voice[k].note == note) voice[k].gate = 0;
        return;
    }
    held_drop(note);
    if (note != voice[0].note || !voice[0].gate) return;
    if (nheld) { voice[0].note = cur_note = held[nheld - 1u]; voice[0].inc = inc_of(cur_note); }
    else voice[0].gate = 0;
}

static void mode_set(uint32_t m)                /* all notes off first: the voices change hands */
{
    uint32_t k;
    for (k = 0; k < NV; k++) voice[k].gate = 0;
    nheld = 0;
    vmode = m;
}

/* ---- the patches: the built-in formulas, and 64 user slots of a whole patch (formula, a..h, loop, glide, voice
 * mode, and room for DUAL's spread and MULTI's right side), 16 to a flash sector (platform.c), kept in RAM */
#define SLOTS 64u
#define SLOTS_PER (SLOTS / LIB_SECTORS)
#define TEXT_MAX 90u                            /* a formula: what the main screen's three lines show */
#define USER 128u                               /* a patch number: 0..NF-1 built in, USER + k slot k, */
#define BLANK 254u                              /* the blank one (INIT), */
#define RANDOM 255u                             /* or one randbeat.c made */
#define LIB_V2 0x32545942u                      /* "BYT2": the library's format */
typedef struct {
    char name[12], text[TEXT_MAX + 2u];         /* both 0-terminated */
    uint8_t var[BB_VARS], loop, lstart, lfine, llen, glide, used, mode, detune, offset;
    char r_name[12], r_text[TEXT_MAX + 2u];     /* MULTI: the right side */
    uint8_t r_var[BB_VARS], pad[7];
} slot_t;
typedef char slot_size_is_240[sizeof(slot_t) == 240u ? 1 : -1];   /* (16 to a sector; the format on flash) */
static slot_t lib[SLOTS] __attribute__((section(".pool")));

/* the first format (v0.1): 2 sectors of 32 slots of 120 bytes, "BYTM". lib_init moves it over */
#define LIB_V1 0x4D545942u
typedef struct {
    char name[12], text[TEXT_MAX + 2u];
    uint8_t var[BB_VARS], loop, lstart, lfine, llen, glide, used, pad[2];
} slot_v1_t;
static uint32_t lib_dirty;                      /* bit k: sector k to save, when nothing sounds */
#define FIRST_PATCH 10u                         /* what plays when nothing was saved: viznut's FIRST */
static uint32_t cur = FIRST_PATCH;              /* the patch playing */
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

/* The library from flash. A slot comes from its sector in this format; else, from the first format's sector that
 * held it (v0.1's sector j had slots 32 j .. 32 j + 31, where sectors 2 j and 2 j + 1 now go), and its sector is
 * written in this format when nothing sounds. lib_save writes the highest sector first: 3 and 2, then 1 over v0.1's
 * sector 1, then 0 over its sector 0. A save cut halfway leaves each slot readable from one format or the other. */
static void lib_init(void)
{
    static bb_prog_t check;
    static slot_v1_t old[32] __attribute__((section(".pool")));
    uint32_t k, i, ok = 0;
    for (k = 0; k < LIB_SECTORS; k++) {
        if (!lib_load(k, LIB_V2, &lib[k * SLOTS_PER], SLOTS_PER * sizeof(slot_t))) ok |= 1u << k;
        else memset(&lib[k * SLOTS_PER], 0, SLOTS_PER * sizeof(slot_t));
    }
    for (k = 0; k < 2u; k++) {
        if ((ok >> (2u * k) & 3u) == 3u || lib_load(k, LIB_V1, old, sizeof old)) continue;
        for (i = 0; i < 32u; i++) {
            slot_t *s = &lib[32u * k + i];
            if (ok >> ((32u * k + i) / SLOTS_PER) & 1u) continue;
            memcpy(s->name, old[i].name, sizeof s->name);
            memcpy(s->text, old[i].text, sizeof s->text);
            memcpy(s->var, old[i].var, sizeof s->var);
            s->loop = old[i].loop; s->lstart = old[i].lstart; s->lfine = old[i].lfine; s->llen = old[i].llen;
            s->glide = old[i].glide; s->used = old[i].used;
        }
        lib_dirty |= ~ok & (3u << (2u * k));
    }
    for (k = 0; k < SLOTS; k++) {               /* a slot that cannot be shown or played reads as empty */
        slot_t *s = &lib[k];
        s->name[sizeof s->name - 1u] = s->text[TEXT_MAX] = s->r_name[sizeof s->r_name - 1u] = s->r_text[TEXT_MAX] = 0;
        if (s->used != 1u || bb_compile(s->text, &check, 0)) memset(s, 0, sizeof *s);
        if (s->mode >= MODES) s->mode = MODE_MONO;
    }
}

static int patch_ok(uint32_t p) { return p < NF || (p >= USER && p - USER < SLOTS && lib[p - USER].used); }
static const char *patch_name(uint32_t p)
{
    return p < USER ? FORMULA[p].name : p == RANDOM ? "RANDOM" : p == BLANK ? "INIT" : lib[p - USER].name;
}

static void patch_pick(uint32_t p)
{
    uint32_t i;
    if (p < USER) {                             /* (the bank compiles: tests/test_bytebeat.c) */
        copy_str(work, FORMULA[p].text, TEXT_MAX);
        for (i = 0; i < BB_VARS; i++) param_set(P_A + i, i < 4u ? FORMULA[p].def[i] : 0);
        param_set(P_DETUNE, 0); param_set(P_OFFSET, 0);
        if (vmode != MODE_MONO) mode_set(MODE_MONO);
    } else {
        const slot_t *s = &lib[p - USER];
        copy_str(work, s->text, TEXT_MAX);
        for (i = 0; i < BB_VARS; i++) param_set(P_A + i, s->var[i]);
        param_set(P_LOOP, s->loop); param_set(P_LSTART, s->lstart); param_set(P_LFINE, s->lfine);
        param_set(P_LLEN, s->llen); param_set(P_GLIDE, s->glide);
        param_set(P_DETUNE, s->detune); param_set(P_OFFSET, s->offset);
        if (vmode != s->mode) mode_set(s->mode);
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

static void patch_blank(void)                   /* INIT: t alone, the variables 0, no loop, no glide */
{
    uint32_t i;
    copy_str(work, "t", TEXT_MAX);
    for (i = 0; i < BB_VARS; i++) param_set(P_A + i, 0);
    param_set(P_LOOP, LOOP_OFF);
    param_set(P_GLIDE, 0);
    if (vmode != MODE_MONO) mode_set(MODE_MONO);
    formula_load(work, 0);
    cur = BLANK;
    edited = 0;
    work_err = 0;
    patch_seq++;
}

static uint32_t patch_step(uint32_t p, int32_t n) /* n patches on: the built-ins, then the used slots, round */
{
    uint32_t all = NF + SLOTS, i = p < USER ? p : NF + (p - USER);
    if (p >= BLANK) i = n > 0 ? all - 1u : 0u;  /* (from a made one: on to the first, or back to the last) */
    while (n) {
        i = n > 0 ? (i + 1u) % all : (i + all - 1u) % all;
        if (i < NF || lib[i - NF].used) n += n > 0 ? -1 : 1;
    }
    return i < NF ? i : USER + (i - NF);
}

static void patch_save(uint32_t k, const char *name)   /* what plays now, into slot k, as name */
{
    slot_t *s = &lib[k];
    uint32_t i;
    memset(s, 0, sizeof *s);
    copy_str(s->name, name, sizeof s->name - 1u);
    copy_str(s->text, work, TEXT_MAX);
    for (i = 0; i < BB_VARS; i++) s->var[i] = (uint8_t)param[P_A + i];
    s->loop = (uint8_t)param[P_LOOP]; s->lstart = (uint8_t)param[P_LSTART]; s->lfine = (uint8_t)param[P_LFINE];
    s->llen = (uint8_t)param[P_LLEN]; s->glide = (uint8_t)param[P_GLIDE];
    s->mode = (uint8_t)vmode; s->detune = (uint8_t)param[P_DETUNE]; s->offset = (uint8_t)param[P_OFFSET];
    s->used = 1;
    lib_dirty |= 1u << (k / SLOTS_PER);
    cur = USER + k;
    edited = 0;
    patch_seq++;
}

static void patch_clear(uint32_t k)             /* slot k empty; if it plays, it plays on as an unsaved edit */
{
    if (cur == USER + k) {
        cur = BLANK;
        edited = 1;
        patch_seq++;
    }
    memset(&lib[k], 0, sizeof lib[k]);
    lib_dirty |= 1u << (k / SLOTS_PER);
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

/* ---- the screen, NEON: deep indigo, pink and cyan, and each variable its own colour, on its knob and in the formula.
 * Titles and key caps in Bytemania Pixel (pixel_font.h), the rest in Terminus. gfx.c's draw_text_box clears to
 * black: text_box here clears to the background. */
#include "pixel_font.h"
#define C_BG RGB(13, 2, 33)
#define C_PINK RGB(255, 42, 109)
#define C_CYAN RGB(5, 217, 232)
#define C_SOFT RGB(122, 95, 168)
#define C_TEXT RGB(240, 235, 255)
#define C_FRAME RGB(60, 30, 100)
#define C_ERR RGB(255, 90, 90)
static const uint16_t C_VAR[BB_VARS] = {        /* a..h */
    RGB(255, 42, 109), RGB(5, 217, 232), RGB(249, 200, 14), RGB(123, 255, 107),
    RGB(255, 150, 50), RGB(190, 120, 255), RGB(90, 170, 255), RGB(255, 120, 220)};

static void text_box(uint32_t x, uint32_t y, uint32_t w, const felucca_font_t *f, const char *s, uint16_t c, int align)
{
    int32_t tw = text_w(f, s);
    cv_begin(w, f->h, C_BG);
    cv_text(align == 1 ? ((int32_t)w - tw) / 2 : align == 2 ? (int32_t)w - tw : 0, 0, f, s, c);
    cv_blit(x, y);
}

static int32_t cv_title(int32_t x, int32_t y, const char *s, uint16_t c)   /* big, with a cyan shadow; the end x */
{
    cv_text(x + 1, y + 1, &FONT_P16, s, C_CYAN);
    return cv_text(x, y, &FONT_P16, s, c);
}

static uint32_t put(char *s, uint32_t i, const char *p) { while (*p) s[i++] = *p++; return i; }
static uint32_t put_num(char *s, uint32_t i, uint32_t v)
{
    if (v >= 100u) s[i++] = (char)('0' + v / 100u % 10u);
    if (v >= 10u) s[i++] = (char)('0' + v / 10u % 10u);
    s[i++] = (char)('0' + v % 10u);
    return i;
}

/* the help: key caps (the button's name, as on the panel) and what they do, two to a 16 px row */
typedef struct { const char *key, *what; } legend_t;

static int32_t cap_w(const char *key, const char *what) { return text_w(&FONT_P8, key) + 10 + text_w(&FONT_S, what); }

static void cv_cap(int32_t x, int32_t y, const char *key, const char *what)   /* y: the top of a 16 px line */
{
    int32_t w = text_w(&FONT_P8, key) + 6;
    cv_rect(x, y + 3, w, 11, C_PINK);
    cv_text(x + 3, y + 5, &FONT_P8, key, C_BG);
    cv_text(x + w + 4, y, &FONT_S, what, C_TEXT);
}

static void draw_legend(uint32_t y, const legend_t *it, uint32_t n)
{
    uint32_t i, k;
    for (i = 0; i < n; i += 2u) {
        cv_begin(240, 16, C_BG);
        for (k = i; k < n && k < i + 2u; k++) cv_cap((int32_t)(k - i) * 120 + 2, 0, it[k].key, it[k].what);
        cv_blit(0, y + 8u * i);
    }
}

static uint16_t char_colour(const char *s, uint32_t i)   /* a..h in its variable's colour, but not in 0x1a */
{
    uint32_t j = i;
    if (s[i] < 'a' || s[i] > 'h') return C_TEXT;
    while (j && ((s[j - 1u] >= '0' && s[j - 1u] <= '9') || ((s[j - 1u] | 32) >= 'a' && (s[j - 1u] | 32) <= 'z'))) j--;
    return s[j] >= '0' && s[j] <= '9' ? C_TEXT : C_VAR[s[i] - 'a'];
}

static void cv_formula_char(int32_t x, int32_t y, const char *s, uint32_t i)
{
    char c[2] = {s[i], 0};
    cv_text(x, y, &FONT_S, c, char_colour(s, i));
}

static void draw_patch(void)                    /* number and name, author, the formula 30 characters a line */
{
    char s[32];
    uint32_t n = 0, i, len = text_len(work);
    if (cur < USER) { s[0] = (char)('0' + (cur + 1u) / 10u); s[1] = (char)('0' + (cur + 1u) % 10u); n = 2; }
    else if (cur < BLANK) { s[0] = 'U'; s[1] = (char)('0' + (cur - USER + 1u) / 10u); s[2] = (char)('0' + (cur - USER + 1u) % 10u); n = 3; }
    if (n) s[n++] = ' ';
    copy_str(s + n, patch_name(cur), edited ? 10 : 11);
    if (edited) { n = text_len(s); s[n++] = '*'; s[n] = 0; }   /* changed in the editor, not saved */
    cv_begin(240, 18, C_BG);
    cv_title((240 - text_w(&FONT_P16, s)) / 2, 0, s, C_PINK);
    cv_blit(0, 4);
    text_box(0, 26, 240, &FONT_P8, cur < USER ? FORMULA[cur].by : cur == RANDOM ? "made at random"
             : cur == BLANK ? "a blank start" : "your patch", C_SOFT, 1);
    cv_begin(240, 48, C_BG);
    for (i = 0; i < len && i < 90u; i++) {      /* each line centred */
        uint32_t line = i / 30u, w = len - line * 30u > 30u ? 30u : len - line * 30u;
        cv_formula_char((int32_t)(240u - w * 8u) / 2 + (int32_t)(i % 30u) * 8, (int32_t)line * 16, work, i);
    }
    cv_blit(0, 38);
}

enum { KNOBS_AD, KNOBS_EH, KNOBS_LOOP, KNOBS_VOICE };   /* what the knobs turn: a..d, e..h (HOME held), the loop (SEQ
                                                          held), DUAL's spread (LFO held) */
static const uint8_t BANK_KNOB[2][4] = {{P_LSTART, P_LFINE, P_LLEN, P_LOOP}, {P_DETUNE, P_OFFSET, P_N, P_N}};

static uint32_t knob_param(uint32_t bank, uint32_t i)   /* P_N: none */
{
    return bank >= KNOBS_LOOP ? BANK_KNOB[bank - KNOBS_LOOP][i] : P_A + 4u * bank + i;
}

static void knob_text(uint32_t bank, uint32_t i, char *name, char *d)   /* the knob's label and its value */
{
    static const char *const MODE[3] = {"OFF", "LOOP", "PING"};
    name[0] = (char)(bank == KNOBS_LOOP ? "SFLM"[i] : bank == KNOBS_VOICE ? "DO  "[i] : (int)('A' + 4u * bank + i));
    name[1] = 0;
    if (knob_param(bank, i) == P_N) d[0] = 0;
    else if (bank == KNOBS_VOICE) d[put(d, put_num(d, 0, (uint32_t)param[knob_param(bank, i)]), i ? "" : "c")] = 0;
    else if (bank != KNOBS_LOOP) d[put_num(d, 0, (uint32_t)param[P_A + 4u * bank + i])] = 0;
    else if (i == 3u) d[put(d, 0, MODE[param[P_LOOP]])] = 0;
    else if (i == 2u && param[P_LLEN] >= 6) d[put(d, put_num(d, 0, 16u << param[P_LLEN] >> 10), "K")] = 0;
    else d[put_num(d, 0, i == 2u ? 16u << param[P_LLEN] : (uint32_t)param[i ? P_LFINE : P_LSTART])] = 0;
}

static uint16_t knob_colour(uint32_t bank, uint32_t i)
{
    return bank == KNOBS_LOOP ? C_CYAN : bank == KNOBS_VOICE ? C_PINK : C_VAR[4u * bank + i];
}

static void draw_knobs(uint32_t bank, uint32_t y)   /* the main screen's: four cells, each framed in its colour */
{
    uint32_t i;
    cv_begin(240, 32, C_BG);
    for (i = 0; i < 4u; i++) {
        char name[2], d[8];
        int32_t x = (int32_t)i * 60 + 3;
        uint16_t c = knob_colour(bank, i);
        knob_text(bank, i, name, d);
        cv_rect(x, 0, 54, 1, c); cv_rect(x, 31, 54, 1, c); cv_rect(x, 0, 1, 32, c); cv_rect(x + 53, 0, 1, 32, c);
        cv_text(x + 4, 4, &FONT_P8, name, c);
        cv_text(x + 50 - text_w(&FONT_S, d), 14, &FONT_S, d, C_TEXT);
    }
    cv_blit(0, y);
}

static void draw_knobs_line(uint32_t bank, uint32_t y)   /* the editor's: one 16 px line */
{
    uint32_t i;
    cv_begin(240, 16, C_BG);
    for (i = 0; i < 4u; i++) {
        char name[2], d[8];
        int32_t x = (int32_t)i * 60 + 6;
        knob_text(bank, i, name, d);
        cv_text(x, 5, &FONT_P8, name, knob_colour(bank, i));
        cv_text(x + 12, 0, &FONT_S, d, C_TEXT);
    }
    cv_blit(0, y);
}

/* ---- the views: the formula run again here, in a frame 240 x 72 (inside: 232 x 64) */
enum { VIEW_SCOPE, VIEW_RASTER, VIEWS };
static uint16_t shade[256];                     /* sample value -> colour: background, pink, cyan */
static uint16_t ramp_x[16];                     /* the scope, left to right: pink to cyan */

static uint16_t mix(uint32_t r0, uint32_t g0, uint32_t b0, uint32_t r1, uint32_t g1, uint32_t b1, uint32_t k)   /* k / 255 */
{
    return RGB((r0 * (255u - k) + r1 * k) / 255u, (g0 * (255u - k) + g1 * k) / 255u, (b0 * (255u - k) + b1 * k) / 255u);
}

static void shade_init(void)
{
    uint32_t v;
    for (v = 0; v < 256u; v++)
        shade[v] = v < 128u ? mix(13, 2, 33, 255, 42, 109, v * 2u) : mix(255, 42, 109, 5, 217, 232, (v - 128u) * 2u);
    for (v = 0; v < 16u; v++)
        ramp_x[v] = mix(255, 42, 109, 5, 217, 232, v * 17u);
}

static uint8_t rast[64][232] __attribute__((section(".pool")));   /* the raster's rows, kept: a row of t is done once */
static uint32_t rast_row[64];                   /* the row of t each holds (row + 1, 0 = none) */

static void draw_view(uint32_t view, int fresh) /* fresh: the formula or a variable changed, the kept rows are old */
{
    const bb_prog_t *p = prog_live;
    int32_t var[BB_VARS];
    uint32_t i, x, t = t_now;
    for (i = 0; i < BB_VARS; i++) var[i] = param[P_A + i];
    cv_begin(240, 72, C_BG);
    cv_rect(2, 0, 236, 1, C_FRAME); cv_rect(2, 71, 236, 1, C_FRAME);
    cv_rect(2, 0, 1, 72, C_FRAME); cv_rect(237, 0, 1, 72, C_FRAME);
    if (view == VIEW_SCOPE) {                   /* the last 232 samples */
        int32_t y0 = 0, y;
        for (x = 0; x < 232u; x++, y0 = y) {
            y = 67 - ((bb_run(p, t - 231u + x, var) & 255) >> 2);
            if (x) cv_line(3 + (int32_t)x, y0, 4 + (int32_t)x, y, ramp_x[x * 16u / 232u]);
        }
    } else {                                    /* a row per 256 steps of t, the newest at the bottom */
        uint32_t row0 = (t >> 8) - 63u, r;
        if (fresh) for (r = 0; r < 64u; r++) rast_row[r] = 0;
        for (r = 0; r < 64u; r++) {
            uint32_t row = row0 + r, k = row & 63u;
            if (row > t >> 8) continue;         /* before t = 0 (row0 wrapped) */
            if (rast_row[k] != row + 1u) {      /* the row now still fills: computed again next time */
                for (x = 0; x < 232u; x++) {
                    uint32_t tt = row << 8 | (x * 256u / 232u);   /* 232 px over 256 steps */
                    rast[k][x] = tt <= t ? (uint8_t)bb_run(p, tt, var) : 0;
                }
                rast_row[k] = row < t >> 8 ? row + 1u : 0u;
            }
            for (x = 0; x < 232u; x++) cv_pset(4 + (int32_t)x, 4 + (int32_t)r, shade[rast[k][x]]);
        }
    }
    cv_blit(0, 88);
}

static const char *const MODE_NAME[MODES] = {"MONO", "DUAL", "POLY", "MULTI"};

static void draw_status(uint32_t note, int32_t oct, uint32_t glide, uint32_t load, int pick, uint32_t mode)
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
    i = put(s, i, "  ");
    i = put(s, i, MODE_NAME[mode]);
    s[i] = 0;
    text_box(0, 204, 240, &FONT_S, s, C_SOFT, 1);
    i = put(s, 0, pick ? "KEYS PICK  DSP " : "KEYS PLAY  DSP ");
    i = put_num(s, i, load);
    s[i++] = '%';
    s[i] = 0;
    text_box(0, 222, 240, &FONT_S, s, pick ? C_CYAN : C_SOFT, 1);
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

/* ---- CC mappings: a CC moves a parameter between min and max, on any channel */
#define NMAP 12u
#define CC_OFF 128u
typedef struct { uint8_t cc, target; int16_t min, max; } ccmap_t;
static const char *const PNAME[P_N] = {"A", "B", "C", "D", "E", "F", "G", "H", "GLIDE", "VOL", "LOOP", "START", "FINE", "LEN",
                                       "DETUN", "OFFS"};

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

/* ---- the list screens (the CC map, save): a title, rows of 16 px from y = 40, each drawn again only when it changed */
enum { ROW_PLAIN, ROW_DIM, ROW_SEL, ROW_HOT };
static char rows_drawn[12][34];

static void draw_head(const char *title, const char *key, const char *what, const char *cols)
{
    cv_begin(240, 18, C_BG);                    /* the title, and a key cap for what the screen's own button does */
    cv_title(4, 0, title, C_PINK);
    if (key) cv_cap(236 - cap_w(key, what), 2, key, what);
    cv_blit(0, 4);
    if (cols) text_box(0, 24, 240, &FONT_S, cols, C_SOFT, 0);
}

static void draw_row(uint32_t r, const char *s, uint32_t look)
{
    char k[34] = {0};
    k[0] = (char)('0' + look);
    copy_str(k + 1, s, 32);
    if (!memcmp(k, rows_drawn[r], sizeof k)) return;
    memcpy(rows_drawn[r], k, sizeof k);
    cv_begin(240, 16, look == ROW_SEL ? C_PINK : look == ROW_HOT ? C_CYAN : C_BG);
    cv_text(8, 0, &FONT_S, s, look >= ROW_SEL ? C_BG : look == ROW_DIM ? C_SOFT : C_TEXT);
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

#define SAVE_ROWS 10u                           /* (the legend below them) */

static void draw_save_rows(uint32_t sel, int clearing)   /* the slots around sel; clearing: sel asks first */
{
    uint32_t r, top = sel < SAVE_ROWS / 2u ? 0u : sel - SAVE_ROWS / 2u > SLOTS - SAVE_ROWS ? SLOTS - SAVE_ROWS : sel - SAVE_ROWS / 2u;
    for (r = 0; r < SAVE_ROWS; r++) {
        uint32_t k = top + r;
        char s[32] = {'U', (char)('0' + (k + 1u) / 10u), (char)('0' + (k + 1u) % 10u), ' '};
        if (k == sel && clearing) copy_str(s + 4, "CLEAR? ENV AGAIN", 20);
        else copy_str(s + 4, lib[k].used ? lib[k].name : "-", 11);
        draw_row(r, s, k == sel ? (clearing ? ROW_HOT : ROW_SEL) : lib[k].used ? ROW_PLAIN : ROW_DIM);
    }
}

static void draw_name(const char *name)         /* the name screen: the name as it is typed, big, a cursor after it */
{
    uint32_t n = text_len(name);
    cv_begin(240, 40, C_BG);
    cv_text(8, 4, &FONT_L, name, C_TEXT);
    cv_rect(8 + (int32_t)n * 16, 4, 3, 32, C_PINK);
    cv_blit(0, 96);
}

/* ---- the editor's screen: the formula with the cursor, whether it compiles, the keys' tokens, the knobs, help */
static void draw_edit_title(void)
{
    cv_begin(240, 18, C_BG);
    cv_text(cv_title(4, 0, "EDIT", C_PINK) + 8, 6, &FONT_P8, patch_name(cur), C_CYAN);
    cv_blit(0, 2);
}

static void draw_edit_text(void)                /* 30 characters a line from the left, 3 lines from y = 22 */
{
    uint32_t i, len = text_len(work);
    cv_begin(240, 48, C_BG);
    for (i = 0; i <= len && i < 90u; i++) {
        int32_t x = (int32_t)(i % 30u) * 8, y = (int32_t)(i / 30u) * 16;
        if (i < len) cv_formula_char(x, y, work, i);
        if (work_err && i == (work_err_at < len ? work_err_at : len)) cv_rect(x, y + 14, 8, 2, C_ERR);
        if (i == cpos) cv_rect(x, y, 2, 16, C_PINK);
    }
    if (cpos == 90u) cv_rect(238, 32, 2, 16, C_PINK);   /* (full: the cursor at the end of the last line) */
    cv_blit(0, 22);
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
    text_box(0, 72, 240, &FONT_S, s, work_err ? C_ERR : C_SOFT, 1);
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
    cv_begin(240, 64, C_BG);
    for (k = 0, w = 0; k < 27u; k++)            /* 16 white keys of 15 px, the token low on the key */
        if (!key_black(k)) {
            const char *tok = key_token(k, layer);
            cv_rect((int32_t)w * 15, 0, 14, 64, RGB(225, 215, 255));
            cv_token((int32_t)w * 15 + 3, tok[1] ? 32 : 46, tok, C_BG);
            w++;
        }
    for (k = 0, w = 0; k < 27u; k++)            /* the black ones on the line between two white */
        if (key_black(k)) {
            cv_rect((int32_t)w * 15 - 5, 0, 10, 34, RGB(45, 20, 80));
            cv_token((int32_t)w * 15 - 4, 1, key_token(k, layer), C_CYAN);
        } else
            w++;
    cv_blit(0, 90);
}

static const legend_t EDIT_HELP[8] = {
    {"FX", "more keys"}, {"OCT-", "del back"}, {"OCT+", "del fwd"}, {"ARP", "random"},
    {"ENV", "blank"}, {"PLAY", "hear C4"}, {"SAVE", "save"}, {"EDIT", "close"}};
static const legend_t SAVE_HELP[3] = {{"SAVE", "name it"}, {"ENV", "clear"}, {"HOME", "back"}};
static const legend_t NAME_HELP[3] = {{"OCT-", "delete"}, {"SAVE", "save"}, {"HOME", "back"}};

/* ---- the settings: what survives a power-off */
typedef struct { uint8_t magic, patch, glide, pick, view; int8_t oct; ccmap_t map[NMAP]; } settings_t;

static void settings_default(settings_t *s)
{
    uint32_t i;
    memset(s, 0, sizeof *s);
    s->magic = 'C';
    s->patch = FIRST_PATCH;
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
    if (!patch_ok(s->patch)) s->patch = FIRST_PATCH;
    if (s->glide > 10u) s->glide = 0;
    if (s->oct < -2 || s->oct > 2) s->oct = 0;
    s->pick = !!s->pick;
    if (s->view >= VIEWS) s->view = VIEW_SCOPE;
}

/* ---- the panel and the screens */
enum { SCR_MAIN, SCR_CC, SCR_SAVE, SCR_EDIT, SCR_NAME };
static settings_t set;
static uint32_t dirty, t_change;                /* the settings changed, when (saved 2 s later, when silent) */
static uint32_t scr = SCR_MAIN, scr_fresh = 1;  /* the screen showing; to draw whole */
static uint32_t cc_sel, learn, save_sel;        /* the CC screen's slot, learning; the save screen's slot */
static uint32_t clearing;                       /* the save screen asks before it clears save_sel */
static char name[12];                           /* the name screen's name */
static uint32_t audition;                       /* the editor's C4 sounds */
static uint32_t bank, held_turned, held_down;   /* the knobs' bank; SEQ, LFO: a knob turned while held, when pressed
                                                   (a tap, or a hold) */
static uint32_t loop_last = LOOP_ON;            /* the loop mode a tap on SEQ turns on */
static const uint8_t MODE_NEXT[MODES] = {MODE_DUAL, MODE_POLY, MODE_MONO, MODE_MONO};   /* a tap on LFO */

static void changed(void) { dirty = 1; t_change = fm1_ms; }

static void screen(uint32_t s)
{
    if (audition && s != SCR_EDIT) { audition = 0; note_off(60); }
    scr = s;
    learn = clearing = 0;
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

static uint32_t knobs_input(uint32_t b, uint32_t rel)   /* the four knobs, in the bank HOME, SEQ or LFO hold. Taps
                                                           (< 0.3 s, nothing turned) of SEQ and LFO: the bits back */
{
    const uint32_t HOLDS = 1u << BTN_SEQ | 1u << BTN_LFO;
    int32_t n;
    uint32_t k, id;
    if (b & HOLDS) { held_turned = 0; held_down = fm1_ms; }
    for (k = 0; k < 4u; k++) {
        id = knob_param(bank, k);
        if (!(n = id == P_N || id == P_LOOP || id == P_LLEN ? fm1_enc_take(ENC_KNOB1 + k) : knob(ENC_KNOB1 + k)) || id == P_N)
            continue;
        param_set(id, param[id] + n);
        if (bank >= KNOBS_LOOP) held_turned = 1;
        if (id == P_LOOP && param[P_LOOP]) loop_last = (uint32_t)param[P_LOOP];
    }
    if (rel & 1u << BTN_SEQ && !held_turned && fm1_ms - held_down < 300u)
        param_set(P_LOOP, param[P_LOOP] ? LOOP_OFF : (int32_t)loop_last);
    return held_turned || fm1_ms - held_down >= 300u ? 0u : rel & HOLDS;
}

static void main_input(uint32_t b, uint32_t rel)
{
    int32_t n;
    if (b & (1u << BTN_SEL)) { set.pick ^= 1u; changed(); }
    if (b & (1u << BTN_FX)) { set.view = (uint8_t)((set.view + 1u) % VIEWS); changed(); }
    if (b & (1u << BTN_SAVE)) save_open();
    if (b & (1u << BTN_EDIT)) { cpos = text_len(work); screen(SCR_EDIT); }
    if ((n = fm1_enc_take(ENC_PRESETS))) pick(patch_step(cur, n));
    if (knobs_input(b, rel) & 1u << BTN_LFO) mode_set(MODE_NEXT[vmode]);   /* (the patch's: saved with it) */
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
    if (b & (1u << BTN_ARP)) { patch_random(); cpos = text_len(work); }   /* (random and blank are not kept in the */
    if (b & (1u << BTN_ENV)) { patch_blank(); cpos = text_len(work); }    /* settings: a power-off goes back to the
                                                                              last patch picked) */
    knobs_input(b, rel);
    if (b & (1u << BTN_SAVE)) save_open();
    else if (b & (1u << BTN_EDIT)) screen(SCR_MAIN);
}

static void save_input(uint32_t b)              /* pick a slot: SAVE names it, ENV twice clears it, HOME goes back */
{
    int32_t n = fm1_enc_take(ENC_SELECT) + fm1_enc_take(ENC_PRESETS), c;
    if (n) {
        c = (int32_t)save_sel + n;
        save_sel = c < 0 ? 0u : c >= (int32_t)SLOTS ? SLOTS - 1u : (uint32_t)c;
        clearing = 0;
    }
    if (b & (1u << BTN_ENV) && lib[save_sel].used) {
        if (clearing) { patch_clear(save_sel); clearing = 0; }
        else clearing = 1;
    }
    if (b & (1u << BTN_SAVE)) {                 /* to the name screen, with the name it plays under */
        copy_str(name, patch_name(cur), sizeof name - 1u);
        screen(SCR_NAME);
    } else if (b & (1u << BTN_HOME))
        screen(SCR_MAIN);
}

static void name_input(uint32_t b, uint32_t keys_down)   /* the keys type A..Z and a space; OCT- deletes */
{
    uint32_t k, n = text_len(name);
    for (k = 0; k < 27u; k++)
        if (keys_down >> k & 1u && n < sizeof name - 1u) { name[n++] = k < 26u ? (char)('A' + k) : ' '; name[n] = 0; }
    if (b & (1u << BTN_OCT_DOWN) && n) name[--n] = 0;
    if (b & (1u << BTN_SAVE)) {
        if (!n) copy_str(name, "NO NAME", sizeof name - 1u);
        patch_save(save_sel, name);
        set.patch = (uint8_t)cur;
        changed();
        screen(SCR_MAIN);
    } else if (b & (1u << BTN_HOME))
        screen(SCR_SAVE);
}

static void app_main(void)
{
    uint32_t prev = 0, sounding = 0, k, t_load = 0, load = 0;
    uint32_t p_drawn = ~0u, note_drawn = ~0u, status_drawn = ~0u, head_drawn = ~0u, bank_drawn = ~0u;
    uint32_t t_view = 0, view_t = ~0u, view_shown = ~0u, view_fresh = 1;
    uint32_t cpos_drawn = ~0u, layer_drawn = ~0u;   /* the editor's */
    char text_drawn[TEXT_MAX + 1u] = {0}, name_drawn[sizeof name];   /* (name_drawn: set apart on each screen) */
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
            if (!(down >> k & 1u) || scr == SCR_EDIT || scr == SCR_NAME) continue;   /* (those type with them) */
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
        bank = fm1_in.buttons >> BTN_SEQ & 1u ? KNOBS_LOOP : fm1_in.buttons >> BTN_LFO & 1u ? KNOBS_VOICE
             : fm1_in.buttons >> BTN_HOME & 1u ? KNOBS_EH : KNOBS_AD;
        if (scr != SCR_EDIT && scr != SCR_NAME) {   /* (those delete with them) */
            if (b & (1u << BTN_OCT_DOWN) && set.oct > -2) { set.oct--; changed(); }
            if (b & (1u << BTN_OCT_UP) && set.oct < 2) { set.oct++; changed(); }
        }
        if (b & (1u << BTN_GLO)) screen(scr == SCR_CC ? SCR_MAIN : SCR_CC);
        else if (scr == SCR_MAIN) main_input(b, rel);
        else if (scr == SCR_CC) cc_input(b);
        else if (scr == SCR_EDIT) edit_input(b, rel, down);
        else if (scr == SCR_NAME) name_input(b, down);
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
            fm1_led_key(KEY_LED(k), scr == SCR_EDIT ? keys >> k & 1u : set.pick ? k == cur : note_sounds(base + k));
        fm1_led_key(BTN_SEL, set.pick);
        fm1_led_key(BTN_GLO, scr == SCR_CC);
        fm1_led_key(BTN_SAVE, scr == SCR_SAVE || scr == SCR_NAME);
        fm1_led_key(BTN_ENV, clearing);
        fm1_led_key(BTN_EDIT, scr == SCR_EDIT);
        fm1_led_key(BTN_PLAY, audition);
        fm1_led_key(BTN_FX, scr == SCR_EDIT && fm1_in.buttons >> BTN_FX & 1u);
        fm1_led_key(BTN_REC, learn);
        fm1_led_key(BTN_HOME, bank == KNOBS_EH);
        fm1_led_key(BTN_SEQ, param[P_LOOP] != LOOP_OFF);
        fm1_led_key(BTN_LFO, vmode != MODE_MONO);
        fm1_led_key(BTN_OCT_DOWN, set.oct < 0);
        fm1_led_key(BTN_OCT_UP, set.oct > 0);

        if (fm1_ms - t_load >= 250u) {          /* the render's share of its 5.8 ms */
            t_load = fm1_ms;
            load = dsp_us * 100u / 5805u;
        }
        if (scr_fresh) {                        /* a screen comes up: everything on it is drawn again */
            scr_fresh = 0;
            lcd_fill(0, 0, 240, 240, C_BG);
            p_drawn = note_drawn = status_drawn = head_drawn = view_shown = cpos_drawn = layer_drawn = ~0u;
            for (k = 0; k < 4u; k++) v_drawn[k] = -1;
            memset(rows_drawn, 0, sizeof rows_drawn);
            memset(name_drawn, 0xFF, sizeof name_drawn);   /* (no name looks like that: drawn whatever it is) */
        }
        for (k = 0; k < BB_VARS; k++)           /* any of a..h changed: the views show something else */
            if (param[P_A + k] != v_seen[k]) { v_seen[k] = param[P_A + k]; view_fresh = 1; view_t = ~0u; }
        if (scr == SCR_CC) {
            if (learn != head_drawn) {
                head_drawn = learn;
                if (learn) draw_head("LEARN", 0, 0, " move a control to map it");
                else draw_head("CC MAP", "REC", "learn", " #  CC TO    MIN-MAX NOW");
            }
            draw_cc_rows(set.map, cc_sel, (int)learn);
        } else if (scr == SCR_SAVE) {
            if (head_drawn) {
                head_drawn = 0;
                draw_head("SAVE TO", 0, 0, " pick a slot");
                draw_legend(206, SAVE_HELP, 3);
            }
            draw_save_rows(save_sel, (int)clearing);
        } else if (scr == SCR_NAME) {
            if (head_drawn) {
                head_drawn = 0;
                draw_head("NAME", 0, 0, " the keys type A to Z,");
                text_box(0, 40, 240, &FONT_S, " the top key a space", C_SOFT, 0);
                draw_legend(206, NAME_HELP, 3);
            }
            if (memcmp(name, name_drawn, sizeof name)) { memcpy(name_drawn, name, sizeof name); draw_name(name); }
        } else if (scr == SCR_EDIT) {
            uint32_t layer = fm1_in.buttons >> BTN_FX & 1u;
            if (head_drawn || patch_seq != p_drawn) {   /* (the patch changes here with ARP and ENV) */
                head_drawn = 0;
                p_drawn = patch_seq;
                draw_edit_title();
                draw_legend(176, EDIT_HELP, 8);
            }
            if (memcmp(work, text_drawn, sizeof work) || cpos != cpos_drawn) {   /* (work_err follows work) */
                memcpy(text_drawn, work, sizeof work);
                cpos_drawn = cpos;
                draw_edit_text();
                draw_edit_status();
            }
            if (layer != layer_drawn) { layer_drawn = layer; draw_palette(layer); }
            for (k = 0; k < 4u; k++) v[k] = knob_param(bank, k) == P_N ? -2 : param[knob_param(bank, k)];
            if (v[0] != v_drawn[0] || v[1] != v_drawn[1] || v[2] != v_drawn[2] || v[3] != v_drawn[3] || bank != bank_drawn) {
                for (k = 0; k < 4u; k++) v_drawn[k] = v[k];
                bank_drawn = bank;
                draw_knobs_line(bank, 156);
            }
        } else {
            for (k = 0; k < 4u; k++) v[k] = knob_param(bank, k) == P_N ? -2 : param[knob_param(bank, k)];
            st_now = (uint32_t)(set.oct + 2) + 5u * (uint32_t)param[P_GLIDE] + 55u * set.pick + 110u * load
                   + 11110u * vmode;
            if (patch_seq != p_drawn) { p_drawn = patch_seq; draw_patch(); view_fresh = 1; view_t = ~0u; }
            if (v[0] != v_drawn[0] || v[1] != v_drawn[1] || v[2] != v_drawn[2] || v[3] != v_drawn[3] || bank != bank_drawn) {
                for (k = 0; k < 4u; k++) v_drawn[k] = v[k];
                bank_drawn = bank;
                draw_knobs(bank, 164);
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
                draw_status(cur_note, set.oct, (uint32_t)param[P_GLIDE], load, set.pick, vmode);
            }
        }

        if (silent() && dirty && fm1_ms - t_change > 2000u) {   /* 2 s after the change, when silent (an erase stalls the sound) */
            settings_save(&set, sizeof set);
            dirty = 0;
        }
        if (silent() && lib_dirty) {            /* a slot saved: its sector, when silent; the highest first (lib_init) */
            for (k = LIB_SECTORS; k-- > 0; )
                if (lib_dirty >> k & 1u) lib_save(k, LIB_V2, &lib[k * SLOTS_PER], SLOTS_PER * sizeof(slot_t));
            lib_dirty = 0;
        }
    }
}
