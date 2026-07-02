/**
 * Magnéto — Schwung audio FX module
 * Stereo cassette/tape looper + recorder inspired by the Library of Congress C1
 * cassette player and the Tascam Portastudio.
 *
 * Author: Filliformes
 * License: MIT
 *
 * API: audio_fx_api_v2 (in-place stereo, int16 interleaved, 44100 Hz, 128 frames/block).
 * Records `audio_inout` (the chain signal at this slot). Put the stock "Line In" (LI)
 * sound generator upstream (Input Mode: Stereo) to loop external audio.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

/* ── host_api_v1_t — MUST match chain_host ABI exactly (order + types) ─────────── */
typedef int (*move_mod_emit_value_fn)(void *ctx, const char *source_id, const char *target,
                                      const char *param, float signal, float depth,
                                      float offset, int bipolar, int enabled);
typedef void (*move_mod_clear_source_fn)(void *ctx, const char *source_id);

typedef struct host_api_v1 {
    uint32_t api_version;
    int sample_rate;
    int frames_per_block;
    uint8_t *mapped_memory;
    int audio_out_offset;
    int audio_in_offset;
    void (*log)(const char *msg);
    int (*midi_send_internal)(const uint8_t *msg, int len);
    int (*midi_send_external)(const uint8_t *msg, int len);
    int (*get_clock_status)(void);
    move_mod_emit_value_fn mod_emit_value;
    move_mod_clear_source_fn mod_clear_source;
    void *mod_host_ctx;
} host_api_v1_t;

/* ── audio_fx_api_v2_t — process_block BEFORE set/get/on_midi ───────────────────── */
typedef struct audio_fx_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *config_json);
    void  (*destroy_instance)(void *instance);
    void  (*process_block)(void *instance, int16_t *audio_inout, int frames);
    void  (*set_param)(void *instance, const char *key, const char *val);
    int   (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    void  (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
} audio_fx_api_v2_t;

static const host_api_v1_t *g_host = NULL;
static int g_next_instance = 1;   /* hands each instance an M-number for exported filenames */

/* ── Constants (declare BEFORE functions that use them) ────────────────────────── */
#define SR              44100
#define MAX_BLOCK       128
#define MAX_LOOP_SEC    30                    /* buffer ceiling per side; 30s*2sides ≈ 10.1 MB int16 */
#define DEFAULT_REC_SEC 15.0f                 /* default Recording Length */
#define MAX_FRAMES      (MAX_LOOP_SEC * SR)
#define NUM_SIDES       2
#define DENORM          1e-25f
#define LOOP_ID_LEN     17                    /* 16 hex chars + NUL; namespaces saved loops */
#define XFADE_LEN       512                   /* ~11.6 ms click-free side crossfade */
#define POS_XFADE_LEN   882                   /* ~20 ms scan micro-loop crossfade */
#define JUMP_XFADE      300                   /* ~7 ms declick for Jump/Stutter position jumps */
#define SCAN_MIN_WIN    8000                  /* min micro-loop window (≥ crossfade even at 8× wind → no buzz) */
#define JOG_CHASE       0.0006                /* Scrub jog: slow chase → smooth mid-range pitch */
#define JOG_VEL_CAP     8.0                   /* Scrub jog: max playback speed (×3 oct) */
#define PLAY_GAIN_COEFF 0.0022f               /* ~10 ms loop fade on Play/Stop (declick) */
#define RECS_DIR        "/data/UserData/UserLibrary/Magneto Recs"  /* exported WAVs land here */
#define PATH_MAX_LEN    512
#define FAST_RATE       8.0     /* Fwd/Bwd fast-wind playback rate (×) */

/* Move transport / control CCs */
#define CC_JOG_CLICK    3
#define CC_JOG_ROTATE   14
#define CC_PLAY         85
#define CC_RECORD       86

/* ── Enum display names (get_param MUST return the name, not the index) ────────── */
#define NUM_SPEED   2
static const char *SPEED_NAMES[NUM_SPEED] = { "1 7/8", "15/16" };
static const float SPEED_RATIO[NUM_SPEED] = { 1.0f, 0.5f };

#define NUM_PANMODE 2
static const char *PANMODE_NAMES[NUM_PANMODE] = { "Mono", "Stereo" };

#define NUM_EQIN 3
static const char *EQIN_NAMES[NUM_EQIN] = { "Input", "Tape", "Off" };

#define NUM_RECMODE 2
static const char *RECMODE_NAMES[NUM_RECMODE] = { "Monitor", "Loop Only" };

/* Sync mode: tempo-synced loop lengths. In Sync, Rec Length becomes a musical division;
 * the fresh take auto-stops at exactly DIV_BEATS beats at the effective tempo. */
#define NUM_DIV 7
static const char  *DIV_NAMES[NUM_DIV] = { "1 Beat", "2 Beats", "1 Bar", "2 Bars", "4 Bars", "8 Bars", "16 Bars" };
static const double DIV_BEATS[NUM_DIV] = { 1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0 };
#define DEFAULT_SYNC_DIV 4                 /* 4 Bars */
#define SYNC_DEFAULT_BPM 120
#define CLK_PPQN         24                /* MIDI clock ticks per quarter note */
#define CLK_STALE_FRAMES (SR / 2)          /* no tick for ~0.5 s → clock stopped, fall back to manual */

#define NUM_SIDE_OPT 2
static const char *SIDE_NAMES[NUM_SIDE_OPT] = { "A 1-2", "B 3-4" };

/* Per-model tape profile. The first 7 fields seed the exposed Tape knobs when the model is
 * selected; the rest are *baked* (not user-editable) and give each machine a distinct EQ
 * fingerprint + noise colour. Researched from the Generation Loss MKII model set (CPR-3300
 * VCR, MS-WALKER, Model 12 reel, CAM-8 camcorder, DICTATRON, FISHY 60 toy) and classic
 * cassette/tape response (ferric/chrome/metal EQ, AM radio, reel head-bump). */
typedef struct {
    const char *name;
    /* exposed knob seeds (0..1) */
    float wow, flutter, saturation, rolloff, hiss, lowcut, generations;
    /* baked midrange peaking EQ */
    float mid_hz;       /* centre frequency of the machine's voice */
    float mid_gain_db;  /* + = honk/presence, − = scoop */
    float mid_q;        /* 0.5 wide … 2.5 narrow */
    /* baked tone */
    float tilt_bias;    /* −1 dark … +1 bright, on top of the user Tone */
    float noise_color;  /* hiss spectrum: 0 dark/rumbly … 1 bright/white */
} tape_model_t;

#define NUM_MODEL 9
static const tape_model_t MODELS[NUM_MODEL] = {
    /*               wow   flut  sat   roll  hiss  lcut  gen    mid_hz  midG   midQ  tilt   ncol */
    { "Type I",     0.18f,0.20f,0.30f,0.62f,0.20f,0.05f,0.12f,  1200.f, 2.5f, 0.8f, -0.10f, 0.55f },
    { "Type II",    0.10f,0.14f,0.20f,0.82f,0.10f,0.03f,0.06f,  3000.f, 1.5f, 0.7f,  0.20f, 0.70f },
    { "Type IV",    0.06f,0.09f,0.12f,0.93f,0.05f,0.02f,0.03f,  5000.f, 0.5f, 0.6f,  0.05f, 0.80f },
    { "Worn",       0.55f,0.58f,0.45f,0.32f,0.30f,0.28f,0.60f,   900.f, 4.0f, 1.4f, -0.45f, 0.25f },
    { "Radio",      0.25f,0.30f,0.55f,0.16f,0.35f,0.50f,0.45f,  1600.f, 6.0f, 1.8f, -0.20f, 0.60f },
    { "VCR",        0.33f,0.26f,0.40f,0.42f,0.25f,0.25f,0.45f,  2500.f, 3.0f, 1.0f, -0.05f, 0.65f },
    { "Dictaphone", 0.38f,0.55f,0.55f,0.20f,0.45f,0.55f,0.58f,  1500.f, 7.0f, 2.0f, -0.15f, 0.75f },
    { "Microcass",  0.28f,0.75f,0.45f,0.14f,0.50f,0.60f,0.62f,  2200.f, 5.0f, 1.6f,  0.10f, 0.85f },
    { "Studio",     0.05f,0.07f,0.35f,0.95f,0.06f,0.02f,0.02f,   200.f, 2.0f, 0.7f,  0.00f, 0.40f },
};

/* ── Helpers ───────────────────────────────────────────────────────────────────── */
static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline int   clampi(int x, int lo, int hi)       { return x < lo ? lo : (x > hi ? hi : x); }

static inline float tape_soft_clip(float x) {
    if (x >  1.5f) return  1.0f;
    if (x < -1.5f) return -1.0f;
    if (x >  1.0f) { float t = x - 1.0f; return  1.0f - t * t * 0.5f; }
    if (x < -1.0f) { float t = x + 1.0f; return -1.0f + t * t * 0.5f; }
    return x - x * x * x * (1.0f / 3.0f);
}

static inline float frand_bipolar(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return ((float)x / 2147483648.0f) - 1.0f;
}
static inline float frand01(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return (float)x / 4294967296.0f;
}

/* Interpolated stereo read from a loop buffer at fractional position rp. */
static inline void read_loop(const int16_t *lp, int len, double rp, float *out_l, float *out_r) {
    while (rp >= len) rp -= len;
    while (rp < 0)    rp += len;
    int i0 = (int)rp, i1 = (i0 + 1) % len;
    float fr = (float)(rp - i0);
    float a_l = lp[i0 * 2] / 32768.0f, a_r = lp[i0 * 2 + 1] / 32768.0f;
    float b_l = lp[i1 * 2] / 32768.0f, b_r = lp[i1 * 2 + 1] / 32768.0f;
    *out_l = a_l + (b_l - a_l) * fr;
    *out_r = a_r + (b_r - a_r) * fr;
}

/* RBJ peaking-EQ biquad coefficients (a0-normalised, TDF-II). */
static void peaking_coeffs(float f0, float gain_db, float q,
                           float *b0, float *b1, float *b2, float *a1, float *a2) {
    float A     = powf(10.0f, gain_db / 40.0f);
    float w0    = 6.2831853f * f0 / (float)SR;
    float cw    = cosf(w0), sw = sinf(w0);
    float alpha = sw / (2.0f * q);
    float a0    = 1.0f + alpha / A;
    *b0 = (1.0f + alpha * A) / a0;
    *b1 = (-2.0f * cw)       / a0;
    *b2 = (1.0f - alpha * A) / a0;
    *a1 = (-2.0f * cw)       / a0;
    *a2 = (1.0f - alpha / A) / a0;
}

/* RBJ low-shelf / high-shelf biquad coefficients (S=1, a0-normalised, TDF-II). */
static void lowshelf_coeffs(float f0, float dB, float *b0, float *b1, float *b2, float *a1, float *a2) {
    float A = powf(10.0f, dB / 40.0f);
    float w0 = 6.2831853f * f0 / (float)SR, cw = cosf(w0), sw = sinf(w0);
    float beta = 2.0f * sqrtf(A) * (sw * 0.5f * 1.41421356f);
    float a0 = (A + 1) + (A - 1) * cw + beta;
    *b0 = (A * ((A + 1) - (A - 1) * cw + beta)) / a0;
    *b1 = (2.0f * A * ((A - 1) - (A + 1) * cw)) / a0;
    *b2 = (A * ((A + 1) - (A - 1) * cw - beta)) / a0;
    *a1 = (-2.0f * ((A - 1) + (A + 1) * cw)) / a0;
    *a2 = ((A + 1) + (A - 1) * cw - beta) / a0;
}
static void highshelf_coeffs(float f0, float dB, float *b0, float *b1, float *b2, float *a1, float *a2) {
    float A = powf(10.0f, dB / 40.0f);
    float w0 = 6.2831853f * f0 / (float)SR, cw = cosf(w0), sw = sinf(w0);
    float beta = 2.0f * sqrtf(A) * (sw * 0.5f * 1.41421356f);
    float a0 = (A + 1) - (A - 1) * cw + beta;
    *b0 = (A * ((A + 1) + (A - 1) * cw + beta)) / a0;
    *b1 = (-2.0f * A * ((A - 1) + (A + 1) * cw)) / a0;
    *b2 = (A * ((A + 1) + (A - 1) * cw - beta)) / a0;
    *a1 = (2.0f * ((A - 1) - (A + 1) * cw)) / a0;
    *a2 = ((A + 1) - (A - 1) * cw - beta) / a0;
}
/* One TDF-II biquad step (advances z1/z2 in place). */
static inline float biquad(float x, float b0, float b1, float b2, float a1, float a2, float *z1, float *z2) {
    float y = b0 * x + *z1;
    *z1 = b1 * x - a1 * y + *z2 + DENORM;
    *z2 = b2 * x - a2 * y;
    return y;
}

/* Little-endian byte readers for WAV parsing. */
static inline uint16_t rd_u16le(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline int32_t rd_i24le(const uint8_t *p) {
    int32_t v = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16));
    if (v & 0x800000) v |= ~0xFFFFFF;   /* sign-extend 24→32 */
    return v;
}
static inline int16_t f2i16(float x) {
    x = clampf(x, -1.0f, 1.0f) * 32767.0f;
    return (int16_t)(x < 0.0f ? x - 0.5f : x + 0.5f);
}

/* Tape-coloration filter state — one set per signal path (loop vs input monitor). */
typedef struct {
    float lp_l, lp_r;                              /* HF rolloff one-pole */
    float hp_l, hp_r;                              /* Low Cut one-pole */
    float mid_z1_l, mid_z2_l, mid_z1_r, mid_z2_r;  /* baked per-model mid EQ biquad */
    float noise_lp_l, noise_lp_r;                  /* hiss colouring one-pole */
    float tilt_l, tilt_r;                          /* Tone tilt (post-tape) one-pole */
} tape_state_t;

/* ClipOnly2 (Airwindows, MIT — Chris Johnson). Transparent soft ceiling at −0.2 dBFS with a
 * 1-sample lookahead that softens the approach to the clip (tape-like, not hard digital clip).
 * Constants are refclip·hardness / refclip·softness from the original. */
typedef struct { double last; int wasPos, wasNeg; } cliponly_t;
static inline double clip_only2(double in, cliponly_t *c) {
    if (in > 4.0) in = 4.0; else if (in < -4.0) in = -4.0;
    if (c->wasPos) { if (in < c->last) c->last = 0.7058208 + in * 0.2609148; else c->last = 0.2491717 + c->last * 0.7390851; }
    c->wasPos = 0;
    if (in > 0.9549925859) { c->wasPos = 1; in = 0.7058208 + c->last * 0.2609148; }
    if (c->wasNeg) { if (in > c->last) c->last = -0.7058208 + in * 0.2609148; else c->last = -0.2491717 + c->last * 0.7390851; }
    c->wasNeg = 0;
    if (in < -0.9549925859) { c->wasNeg = 1; in = -0.7058208 + c->last * 0.2609148; }
    double out = c->last;
    c->last = in;
    return out;
}

/* ── Instance state ──────────────────────────────────────────────────────────────── */
typedef struct {
    /* Page 1 knobs */
    float varspeed; int speed_mode; int side;
    float tone; float volume; int model; float mix; float feedback;
    /* Page 2 (tape) */
    float wow; float flutter; float saturation; float rolloff; float hiss;
    float lowcut;       /* Low Cut (HP) amount 0..1 → 20..800 Hz */
    float generations;  /* "Wear" macro: band-narrowing + noise + softening 0..1 */

    /* Channel strip (Tascam-style input EQ + gain, pre-tape) — REAL units for display */
    float trim;         /* input gain dB, -12..+12 (0 = unity) */
    float low;          /* low shelf ~100 Hz gain dB, -12..+12 (0 = flat) */
    float mid;          /* mid peak gain dB, -12..+12 (0 = flat) */
    float mid_freq;     /* mid centre Hz, 250..5000 */
    float high;         /* high shelf gain dB, -12..+12 (0 = flat) */
    float high_freq;    /* high shelf Hz, 5000..12000 (default 10000) */
    float chan_vol;     /* channel output level 0..1 (post-EQ, pre-tape) */
    float input_pan;    /* 0..1, 0.5 = centre */
    int   pan_mode;     /* 0 = Mono (sum then pan), 1 = Stereo (balance) */

    /* Perform — playhead/glitch */
    float scrub;        /* 0..1 — playhead start position into the active side */
    float scrub_prev;   /* last applied scrub, to detect knob movement */
    float stutter;      /* 0..1 — playhead skip / back-and-forth */
    float failure;      /* 0..1 — sporadic loop dropouts (mutes) */
    float jump;         /* 0..1 — random playhead jumps; segment 1/2..1/1024 of loop */
    float scan;         /* 0..1 — second playhead (full-loop offset → micro-loop) */
    int   tape_stop;    /* 0/1 — engage tape-stop ramp */
    float stop_speed;   /* tape-stop time in ms, 60..10000 */
    float rec_length;   /* recording length cap in seconds, 1..30 */

    /* Deck transport */
    int   fwd, bwd;     /* 0/1 — fast-wind forward / backward */

    /* Recordings (sample I/O) */
    int  inst_num;                      /* per-session instance number (M1, M2, …) for filenames */
    char load_path[NUM_SIDES][PATH_MAX_LEN];   /* last file loaded into each side (UI display) */

    /* Transport */
    int play, rec, reverse;

    /* UI page for knob-overlay routing (0 = Main, 1 = Tape, 2 = Deck) */
    int current_page;

    /* Persistent per-instance ID — namespaces loop files so loops travel with the
     * saved Set and multiple instances never collide. Round-trips via the state string. */
    char loop_id[LOOP_ID_LEN];

    /* Loop buffers (stereo interleaved int16) per side */
    int16_t *loop[NUM_SIDES];
    int      loop_len[NUM_SIDES];
    double   play_pos;
    int      write_pos;

    /* DSP state */
    float wow_phase, flutter_phase;
    tape_state_t tape_main;   /* tape coloration state for the loop path */
    tape_state_t tape_mon;    /* separate state for the always-on input monitor */
    cliponly_t   clip_l, clip_r;   /* Airwindows ClipOnly2 master ceiling */
    /* channel-strip EQ biquads (low shelf / mid peak / high shelf, per channel) */
    float eq_lo_z1l, eq_lo_z2l, eq_lo_z1r, eq_lo_z2r;
    float eq_md_z1l, eq_md_z2l, eq_md_z1r, eq_md_z2r;
    float eq_hi_z1l, eq_hi_z2l, eq_hi_z1r, eq_hi_z2r;
    float dcb_x1l, dcb_y1l, dcb_x1r, dcb_y1r;   /* DC blocker on recorded input (no sub-DC buildup) */
    /* perform engines */
    int    jump_count;      /* samples left in the current jump segment */
    double scan_pos;        /* second-playhead position */
    double ts_mult;         /* tape-stop rate multiplier (1 = run, 0 = stopped) */
    /* click-free crossfades for Scan micro-loop wraps (~20 ms) */
    double scan_xpos;   int scan_xfade;
    double xj_pos;      int xj_count;   /* Jump/Stutter position-jump declick crossfade */
    double jog_target;  int jog_hold;   /* Scrub jog: chase target + active-hold window */
    float  play_gain;   /* loop fade-in/out envelope on Play/Stop (declick) */
    float  scan_gain;   /* scan fade-in envelope on engage (declick) */
    uint32_t rng;
    double scrub_frames;

    /* Rec Mode + EQ routing */
    int eq_in;          /* 0 = Input (pre-tape), 1 = Tape (on loop), 2 = Off */
    int rec_mode;       /* 0 = Monitor (input always audible), 1 = Loop Only */

    /* 20 ms parameter smoothing (Perform + Channel knobs) */
    float sm_trim, sm_low, sm_mid, sm_mid_freq, sm_high, sm_high_freq, sm_input_pan, sm_chan_vol;
    float sm_jump, sm_scan, sm_stutter, sm_failure;

    /* Click-free side switch + direction smoothing */
    int    render_side;     /* side currently being rendered */
    int    prev_side;       /* side fading out during a crossfade */
    int    xfade_count;     /* samples left in the side crossfade */
    double rate_smooth;     /* smoothed playback rate (reverse/varspeed = no click) */

    /* Stutter engine */
    int    st_count;        /* samples left in the current slice */
    double st_anchor;       /* loop position where the current slice began */
    int    st_dir;          /* +1 / -1 temporary playback direction */

    /* Failure (dropout) engine */
    int    fail_count;      /* samples left in the current play/mute span */
    int    fail_state;      /* 0 = playing, 1 = muted */
    float  fail_gain;       /* smoothed gate gain (click-free) */

    /* Sync mode (tempo-synced loop lengths) */
    int      sync_mode;     /* 0 = Free (Rec Length in seconds), 1 = Sync (bars/beats) */
    int      sync_div;      /* division index into DIV_BEATS[] */
    int      tempo_bpm;     /* manual tempo 20..999 — used when no MIDI clock is present */
    /* MIDI-clock tempo measurement (0xF8 ticks, 24 PPQN) */
    uint64_t clk_frame;         /* free-running sample counter (advanced each block) */
    uint64_t clk_last_tick;     /* clk_frame at the previous 0xF8 tick */
    uint64_t clk_last_activity; /* clk_frame at the last clock message (staleness) */
    double   clk_interval;      /* smoothed samples/tick (block-jitter averaged) */
    double   clk_bpm;           /* measured BPM, valid while clock is live */
    /* Tick-locked recording: when clock is live at record-start, a fresh Sync take stops
     * after exactly rec_target_ticks 0xF8 ticks — locked to the clock grid, not a measured
     * length (immune to BPM jitter / tempo drift, like the arp's clock-counted stepping). */
    int      rec_tick_lock;     /* 1 = this take is counting clock ticks */
    int      rec_tick_count;    /* ticks received since record-start */
    int      rec_target_ticks;  /* division beats × 24 PPQN */

    char module_dir[512];
} plugin_instance_t;

/* ── Knob maps (page-aware overlay) ──────────────────────────────────────────────── */
typedef struct { const char *key; const char *label; float min, max, step; int is_enum; } knob_def_t;

static const knob_def_t KNOB_MAP[8] = {            /* Page 0 — Magneto (Main) */
    { "play",     "Play",      0, 1, 1.0f,  1 },
    { "rec",      "Rec",       0, 1, 1.0f,  1 },
    { "varspeed", "Var Speed", 0, 1, 0.01f, 0 },
    { "speed",    "Speed",     0, 1, 1.0f,  1 },
    { "side",     "Side",      0, 1, 1.0f,  1 },
    { "tone",     "Tone",      0, 1, 0.01f, 0 },
    { "model",    "Tape",      0, NUM_MODEL - 1, 1.0f, 1 },
    { "volume",   "Volume",    0, 1, 0.01f, 0 },
};
static const knob_def_t KNOB_MAP_PERF[8] = {       /* Page 1 — Perform */
    { "scrub",     "Scrub",     0, 1, 0.01f, 0 },
    { "jump",      "Jump",      0, 1, 0.01f, 0 },
    { "scan",      "Scan",      0, 1, 0.01f, 0 },
    { "reverse",   "Reverse",   0, 1, 1.0f,  1 },
    { "stutter",   "Stutter",   0, 1, 0.01f, 0 },
    { "failure",   "Failure",   0, 1, 0.01f, 0 },
    { "tape_stop", "Tape Stop", 0, 1, 1.0f,  1 },
    { "stop_speed","Stop Speed",60, 10000, 50.0f, 0 },
};
static const knob_def_t KNOB_MAP_CHAN[8] = {       /* Page 2 — Channel (Tascam strip, real units) */
    { "trim",      "Trim",      -12,    12,   0.5f,  0 },
    { "high",      "High",      -12,    12,   0.5f,  0 },
    { "high_freq", "High Freq", 5000,   12000, 50.0f, 0 },
    { "mid_freq",  "Mid Freq",  250,    5000, 25.0f, 0 },
    { "mid",       "Mid",       -12,    12,   0.5f,  0 },
    { "low",       "Low",       -12,    12,   0.5f,  0 },
    { "input_pan", "Pan",       0,      1,    0.01f, 0 },
    { "chan_vol",  "Volume",    0,      1,    0.01f, 0 },
};
static const knob_def_t KNOB_MAP_P2[8] = {         /* Page 2 — Tape (signal-path order) */
    { "model",       "Tape",        0, NUM_MODEL - 1, 1.0f, 1 },
    { "wow",         "Wow",         0, 1, 0.01f, 0 },
    { "flutter",     "Flutter",     0, 1, 0.01f, 0 },
    { "saturation",  "Saturation",  0, 1, 0.01f, 0 },
    { "rolloff",     "HF Rolloff",  0, 1, 0.01f, 0 },
    { "lowcut",      "Low Cut",     0, 1, 0.01f, 0 },
    { "hiss",        "Hiss",        0, 1, 0.01f, 0 },
    { "generations", "Generations", 0, 1, 0.01f, 0 },
};
static const knob_def_t KNOB_MAP_DECK[8] = {       /* Page 4 — Deck */
    { "play",      "Play",      0, 1, 1.0f,  1 },
    { "fwd",       "Fwd",       0, 1, 1.0f,  1 },
    { "bwd",       "Bwd",       0, 1, 1.0f,  1 },
    { "rec",       "Rec",       0, 1, 1.0f,  1 },
    { "clear",     "Clear",     0, 1, 1.0f,  1 },
    { "input_pan", "Pan",       0, 1, 0.01f, 0 },
    { "pan_mode",  "Pan Mode",  0, 1, 1.0f,  1 },
    { "mix",       "Mix",       0, 1, 0.01f, 0 },
};
static const knob_def_t KNOB_MAP_RECS[8] = {       /* Page 5 — Recordings: menu-only */
    { NULL, NULL, 0, 0, 0, 0 }, { NULL, NULL, 0, 0, 0, 0 }, { NULL, NULL, 0, 0, 0, 0 },
    { NULL, NULL, 0, 0, 0, 0 }, { NULL, NULL, 0, 0, 0, 0 }, { NULL, NULL, 0, 0, 0, 0 },
    { NULL, NULL, 0, 0, 0, 0 }, { NULL, NULL, 0, 0, 0, 0 },
};
static const knob_def_t *active_map(const plugin_instance_t *p) {
    switch (p->current_page) {
        case 1:  return KNOB_MAP_PERF;
        case 2:  return KNOB_MAP_CHAN;
        case 3:  return KNOB_MAP_P2;     /* Tape */
        case 4:  return KNOB_MAP_DECK;
        case 5:  return KNOB_MAP_RECS;
        default: return KNOB_MAP;        /* Magneto */
    }
}

/* ── Param routing ───────────────────────────────────────────────────────────────── */
static float *float_param(plugin_instance_t *p, const char *key) {
    if (!strcmp(key, "varspeed"))   return &p->varspeed;
    if (!strcmp(key, "tone"))       return &p->tone;
    if (!strcmp(key, "volume"))     return &p->volume;
    if (!strcmp(key, "mix"))        return &p->mix;
    if (!strcmp(key, "feedback"))   return &p->feedback;
    if (!strcmp(key, "wow"))        return &p->wow;
    if (!strcmp(key, "flutter"))    return &p->flutter;
    if (!strcmp(key, "saturation"))  return &p->saturation;
    if (!strcmp(key, "rolloff"))     return &p->rolloff;
    if (!strcmp(key, "hiss"))        return &p->hiss;
    if (!strcmp(key, "lowcut"))      return &p->lowcut;
    if (!strcmp(key, "generations")) return &p->generations;
    if (!strcmp(key, "input_pan"))   return &p->input_pan;
    if (!strcmp(key, "stutter"))     return &p->stutter;
    if (!strcmp(key, "failure"))     return &p->failure;
    if (!strcmp(key, "scrub"))       return &p->scrub;
    if (!strcmp(key, "trim"))        return &p->trim;
    if (!strcmp(key, "low"))         return &p->low;
    if (!strcmp(key, "mid"))         return &p->mid;
    if (!strcmp(key, "mid_freq"))    return &p->mid_freq;
    if (!strcmp(key, "high"))        return &p->high;
    if (!strcmp(key, "high_freq"))   return &p->high_freq;
    if (!strcmp(key, "chan_vol"))    return &p->chan_vol;
    if (!strcmp(key, "jump"))        return &p->jump;
    if (!strcmp(key, "scan"))        return &p->scan;
    if (!strcmp(key, "stop_speed"))  return &p->stop_speed;
    if (!strcmp(key, "rec_length"))  return &p->rec_length;
    return NULL;
}

static void apply_model(plugin_instance_t *p, int m) {
    m = clampi(m, 0, NUM_MODEL - 1);
    const tape_model_t *t = &MODELS[m];
    p->model       = m;
    p->wow         = t->wow;
    p->flutter     = t->flutter;
    p->saturation  = t->saturation;
    p->rolloff     = t->rolloff;
    p->hiss        = t->hiss;
    p->lowcut      = t->lowcut;
    p->generations = t->generations;
    /* new model = new mid-EQ coefficients; clear the biquad state so old state doesn't
     * ring through the new filter (avoids a transient on model change). */
    p->tape_main.mid_z1_l = p->tape_main.mid_z2_l = p->tape_main.mid_z1_r = p->tape_main.mid_z2_r = 0.0f;
    p->tape_mon.mid_z1_l  = p->tape_mon.mid_z2_l  = p->tape_mon.mid_z1_r  = p->tape_mon.mid_z2_r  = 0.0f;
    /* baked EQ/noise (mid_hz, tilt_bias, noise_color) are read live from MODELS[model] */
}

/* ── Loop persistence ─────────────────────────────────────────────────────────────
 * Loops are written to module_dir as files namespaced by the instance's loop_id.
 * The loop_id round-trips through the state string (get_param/set_param "state"), so a
 * saved Set recalls its own loops and concurrent instances never overwrite each other.
 * A blank instance gets a fresh id (no files yet) and starts empty. */
static void gen_loop_id(char *out) {
    /* 64 bits of entropy from /dev/urandom → 16 hex chars; counter fallback. */
    static uint32_t fallback = 0x9E3779B9u;
    unsigned char raw[8];
    int ok = 0;
    FILE *u = fopen("/dev/urandom", "rb");
    if (u) { ok = (fread(raw, 1, sizeof(raw), u) == sizeof(raw)); fclose(u); }
    if (!ok) {
        fallback = fallback * 1664525u + 1013904223u;
        for (int i = 0; i < 8; i++) { raw[i] = (unsigned char)(fallback >> ((i & 3) * 8)); fallback ^= fallback << 7; }
    }
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) { out[i * 2] = hex[raw[i] >> 4]; out[i * 2 + 1] = hex[raw[i] & 0xF]; }
    out[16] = '\0';
}
static void loops_path(const plugin_instance_t *p, int side, char *out, int n) {
    snprintf(out, n, "%s/magneto_%s_%c.raw", p->module_dir, p->loop_id, side == 0 ? 'A' : 'B');
}
static void save_loops(plugin_instance_t *p) {
    if (!p->module_dir[0] || !p->loop_id[0]) return;
    char path[640];
    for (int s = 0; s < NUM_SIDES; s++) {
        if (p->loop_len[s] <= 0) continue;
        loops_path(p, s, path, sizeof(path));
        FILE *f = fopen(path, "wb");
        if (!f) continue;
        fwrite(p->loop[s], sizeof(int16_t), (size_t)p->loop_len[s] * 2, f);
        fclose(f);
    }
    snprintf(path, sizeof(path), "%s/magneto_%s.meta", p->module_dir, p->loop_id);
    FILE *m = fopen(path, "w");
    if (m) { fprintf(m, "%d %d\n", p->loop_len[0], p->loop_len[1]); fclose(m); }
}
static void load_loops(plugin_instance_t *p) {
    if (!p->module_dir[0] || !p->loop_id[0]) return;
    char path[640];
    int len[NUM_SIDES] = { 0, 0 };
    snprintf(path, sizeof(path), "%s/magneto_%s.meta", p->module_dir, p->loop_id);
    FILE *m = fopen(path, "r");
    if (!m) return;                         /* no save under this id → stay blank */
    if (fscanf(m, "%d %d", &len[0], &len[1]) != 2) { len[0] = len[1] = 0; }
    fclose(m);
    for (int s = 0; s < NUM_SIDES; s++) {
        p->loop_len[s] = 0;
        len[s] = clampi(len[s], 0, MAX_FRAMES);
        if (len[s] <= 0) continue;
        loops_path(p, s, path, sizeof(path));
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        size_t got = fread(p->loop[s], sizeof(int16_t), (size_t)len[s] * 2, f);
        fclose(f);
        p->loop_len[s] = (int)(got / 2);
    }
}

/* Recover the most recently saved loop in module_dir, even if it was orphaned by an
 * accidental module swap (the loop_id link in the slot state is lost on swap, but the
 * .raw/.meta files survive). Scans for magneto_<id>.meta, picks the newest, adopts that
 * id, and reloads. Control-thread only (blocking dir + file I/O). */
#define META_NAME_LEN (8 + 16 + 5)   /* "magneto_" + 16 hex + ".meta" */
static void recover_loop(plugin_instance_t *p) {
    if (!p->module_dir[0]) return;
    DIR *d = opendir(p->module_dir);
    if (!d) return;
    char   best_id[LOOP_ID_LEN] = { 0 };
    time_t best_mtime = 0;
    char   path[640];
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *nm = e->d_name;
        if (strlen(nm) != META_NAME_LEN) continue;
        if (strncmp(nm, "magneto_", 8) != 0) continue;
        if (strcmp(nm + 8 + 16, ".meta") != 0) continue;
        snprintf(path, sizeof(path), "%s/%s", p->module_dir, nm);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        /* skip empty saves (both sides 0) so Recover lands on real audio */
        FILE *mf = fopen(path, "r");
        if (!mf) continue;
        int la = 0, lb = 0;
        int got = fscanf(mf, "%d %d", &la, &lb);
        fclose(mf);
        if (got != 2 || (la <= 0 && lb <= 0)) continue;
        if (best_id[0] == 0 || st.st_mtime >= best_mtime) {
            best_mtime = st.st_mtime;
            memcpy(best_id, nm + 8, 16);
            best_id[16] = '\0';
        }
    }
    closedir(d);
    if (!best_id[0]) return;                 /* nothing on disk to recover */
    memcpy(p->loop_id, best_id, LOOP_ID_LEN);
    p->loop_len[0] = p->loop_len[1] = 0;
    p->play_pos = 0.0; p->write_pos = 0; p->rec = 0;
    load_loops(p);                           /* pulls in the adopted id's .raw pair */
}

/* ── Sample I/O ───────────────────────────────────────────────────────────────────────
 * Load any WAV (PCM 8/16/24/32 or float32, mono/stereo) into a tape side: decode →
 * resample to 44.1k → truncate to the tape length → int16 into loop[side]. Control-thread
 * only (blocking file I/O). Adapted from Granny's RIFF parser. */
static int load_wav_into_side(plugin_instance_t *p, int side, const char *path) {
    if (!path || !path[0]) return -1;
    side = clampi(side, 0, NUM_SIDES - 1);

    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    uint8_t riff[12];
    if (fread(riff, 1, 12, fp) != 12 ||
        memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) { fclose(fp); return -1; }

    int have_fmt = 0, have_data = 0;
    uint16_t fmt_tag = 0, channels = 0, block_align = 0, bits = 0;
    uint32_t srate = 0, data_size = 0;
    long data_off = 0;
    while (!feof(fp)) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, fp) != 8) break;
        uint32_t csz = rd_u32le(ch + 4);
        long here = ftell(fp);
        if (memcmp(ch, "fmt ", 4) == 0) {
            uint8_t f[40];
            uint32_t want = csz < sizeof(f) ? csz : (uint32_t)sizeof(f);
            if (fread(f, 1, want, fp) != want || want < 16) { fclose(fp); return -1; }
            fmt_tag = rd_u16le(f + 0); channels = rd_u16le(f + 2);
            srate = rd_u32le(f + 4); block_align = rd_u16le(f + 12); bits = rd_u16le(f + 14);
            have_fmt = 1;
        } else if (memcmp(ch, "data", 4) == 0) {
            data_size = csz; data_off = here; have_data = 1;
        }
        if (fseek(fp, here + (long)csz + (csz & 1u), SEEK_SET) != 0) break;
    }
    if (!have_fmt || !have_data || channels < 1 || block_align < 1 || srate < 1) { fclose(fp); return -1; }
    int is_float = (fmt_tag == 3);
    if (!((fmt_tag == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) ||
          (is_float && bits == 32))) { fclose(fp); return -1; }

    int total_frames = (int)(data_size / block_align);
    if (total_frames <= 0) { fclose(fp); return -1; }
    long src_cap = (long)((double)(MAX_LOOP_SEC + 1) * (double)srate);   /* decode ≤ tape length */
    int src_frames = (total_frames < (int)src_cap) ? total_frames : (int)src_cap;

    int bps = bits / 8;
    size_t need = (size_t)src_frames * block_align;
    uint8_t *raw = (uint8_t *)malloc(need);
    if (!raw) { fclose(fp); return -1; }
    if (fseek(fp, data_off, SEEK_SET) != 0 || fread(raw, 1, need, fp) != need) { free(raw); fclose(fp); return -1; }
    fclose(fp);

    float *sl = (float *)malloc((size_t)src_frames * sizeof(float));
    float *sr = (float *)malloc((size_t)src_frames * sizeof(float));
    if (!sl || !sr) { free(sl); free(sr); free(raw); return -1; }
    for (int i = 0; i < src_frames; i++) {
        const uint8_t *frp = raw + (size_t)i * block_align;
        float cv[2] = { 0.0f, 0.0f };
        for (int c = 0; c < channels && c < 2; c++) {
            const uint8_t *sp = frp + c * bps;
            float v = 0.0f;
            if (is_float)        memcpy(&v, sp, sizeof(float));
            else if (bits == 8)  v = ((int)sp[0] - 128) / 128.0f;
            else if (bits == 16) v = (int16_t)rd_u16le(sp) / 32768.0f;
            else if (bits == 24) v = rd_i24le(sp) / 8388608.0f;
            else                 v = (int32_t)rd_u32le(sp) / 2147483648.0f;
            cv[c] = v;
        }
        sl[i] = cv[0];
        sr[i] = (channels >= 2) ? cv[1] : cv[0];   /* mono → both sides */
    }
    free(raw);

    int16_t *dst = p->loop[side];
    double step = (double)srate / (double)SR;
    int out_len = (int)((double)src_frames / step);
    if (out_len > MAX_FRAMES) out_len = MAX_FRAMES;
    if (out_len < 1) { free(sl); free(sr); return -1; }
    for (int j = 0; j < out_len; j++) {
        double sp = (double)j * step;
        int i0 = (int)sp, i1 = i0 + 1;
        if (i1 >= src_frames) i1 = src_frames - 1;
        float fr = (float)(sp - i0);
        dst[j * 2]     = f2i16(sl[i0] + (sl[i1] - sl[i0]) * fr);
        dst[j * 2 + 1] = f2i16(sr[i0] + (sr[i1] - sr[i0]) * fr);
    }
    free(sl); free(sr);

    p->loop_len[side] = out_len;
    if (side == clampi(p->side, 0, NUM_SIDES - 1)) { p->play_pos = 0.0; p->write_pos = 0; p->rec = 0; }
    snprintf(p->load_path[side], PATH_MAX_LEN, "%s", path);
    save_loops(p);   /* persist the imported audio + travel with the Set */
    return 0;
}

/* Export a side's loop to Magneto Recs as M<n><Side>_YYYYMMDD_HHMM.wav (stereo 16-bit 44.1k). */
static void export_wav(plugin_instance_t *p, int side) {
    side = clampi(side, 0, NUM_SIDES - 1);
    int frames = p->loop_len[side];
    if (frames <= 0) return;
    mkdir(RECS_DIR, 0777);                       /* idempotent */

    char stamp[32]; time_t t = time(NULL); struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M", &tmv);
    char path[PATH_MAX_LEN];
    snprintf(path, sizeof(path), "%s/M%d%c_%s.wav", RECS_DIR, p->inst_num, side == 0 ? 'A' : 'B', stamp);

    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t data_bytes = (uint32_t)frames * 2u * 2u;
    uint32_t riff_size  = 36u + data_bytes;
    uint32_t byte_rate  = (uint32_t)SR * 2u * 2u;
    uint8_t h[44];
    memcpy(h + 0, "RIFF", 4);
    h[4]=riff_size&0xFF; h[5]=(riff_size>>8)&0xFF; h[6]=(riff_size>>16)&0xFF; h[7]=(riff_size>>24)&0xFF;
    memcpy(h + 8, "WAVE", 4); memcpy(h + 12, "fmt ", 4);
    h[16]=16; h[17]=0; h[18]=0; h[19]=0;  h[20]=1; h[21]=0;  h[22]=2; h[23]=0;
    h[24]=(uint8_t)(SR&0xFF); h[25]=(uint8_t)((SR>>8)&0xFF); h[26]=(uint8_t)((SR>>16)&0xFF); h[27]=(uint8_t)((SR>>24)&0xFF);
    h[28]=byte_rate&0xFF; h[29]=(byte_rate>>8)&0xFF; h[30]=(byte_rate>>16)&0xFF; h[31]=(byte_rate>>24)&0xFF;
    h[32]=4; h[33]=0;  h[34]=16; h[35]=0;
    memcpy(h + 36, "data", 4);
    h[40]=data_bytes&0xFF; h[41]=(data_bytes>>8)&0xFF; h[42]=(data_bytes>>16)&0xFF; h[43]=(data_bytes>>24)&0xFF;
    fwrite(h, 1, 44, f);
    fwrite(p->loop[side], sizeof(int16_t), (size_t)frames * 2, f);
    fclose(f);
}

/* ── Lifecycle ─────────────────────────────────────────────────────────────────────── */
static void *create_instance(const char *module_dir, const char *config_json) {
    (void)config_json;   /* host passes NULL here (confirmed on-device); state arrives via
                          * set_param("state", …). No slot id available → see Recover Loop. */
    plugin_instance_t *p = calloc(1, sizeof(plugin_instance_t));
    if (!p) return NULL;

    for (int s = 0; s < NUM_SIDES; s++) {
        p->loop[s] = calloc((size_t)MAX_FRAMES * 2, sizeof(int16_t));
        if (!p->loop[s]) { for (int j = 0; j < s; j++) free(p->loop[j]); free(p); return NULL; }
    }

    p->varspeed = 0.5f; p->speed_mode = 0; p->side = 0;
    p->tone = 0.5f; p->volume = 0.8f; p->mix = 1.0f; p->feedback = 0.5f;   /* Mix 100% by default */
    apply_model(p, 0);                  /* seeds wow..hiss + lowcut + generations */
    p->input_pan = 0.5f; p->pan_mode = 1;                                  /* Pan Mode = Stereo */
    p->stutter = 0.0f; p->failure = 0.0f;
    p->scrub = 0.0f; p->scrub_prev = 0.0f;
    /* channel strip — flat / unity (real units) */
    p->trim = 0.0f; p->low = 0.0f; p->mid = 0.0f; p->mid_freq = 1000.0f;  /* 0 dB, 1 kHz */
    p->high = 0.0f; p->high_freq = 10000.0f; p->chan_vol = 1.0f;          /* 0 dB, 10 kHz */
    p->eq_in = 0;       /* EQ on Input */
    p->rec_mode = 0;    /* Monitor (external audio always audible) */
    p->sync_mode = 0;   /* Free (Rec Length in seconds) */
    p->sync_div = DEFAULT_SYNC_DIV; p->tempo_bpm = SYNC_DEFAULT_BPM;
    p->clk_frame = 0; p->clk_last_tick = 0; p->clk_last_activity = 0;
    p->clk_interval = 0.0; p->clk_bpm = 0.0;
    p->rec_tick_lock = 0; p->rec_tick_count = 0; p->rec_target_ticks = 0;
    /* seed the 20 ms smoothing companions so they don't glide from 0 on load */
    p->sm_trim = p->trim; p->sm_low = p->low; p->sm_mid = p->mid; p->sm_mid_freq = p->mid_freq;
    p->sm_high = p->high; p->sm_high_freq = p->high_freq; p->sm_input_pan = p->input_pan; p->sm_chan_vol = p->chan_vol;
    p->sm_jump = 0.0f; p->sm_scan = 0.0f; p->sm_stutter = 0.0f; p->sm_failure = 0.0f;
    /* perform engines off */
    p->jump = 0.0f; p->scan = 0.0f; p->tape_stop = 0; p->stop_speed = 600.0f;  /* 600 ms */
    p->rec_length = DEFAULT_REC_SEC;
    p->fwd = 0; p->bwd = 0; p->ts_mult = 1.0; p->jump_count = 0; p->scan_pos = 0.0;
    p->scan_xfade = 0;
    p->current_page = 0;
    p->play_pos = 0.0; p->rng = 0x1234567u;
    p->st_dir = 1; p->fail_gain = 1.0f; p->fail_state = 0;
    p->render_side = p->side; p->prev_side = p->side; p->xfade_count = 0;
    p->rate_smooth = 1.0;
    p->inst_num = g_next_instance++;    /* M1, M2, … for exported filenames (per session) */
    mkdir(RECS_DIR, 0777);              /* auto-create "Magneto Recs" on first load (idempotent) */

    if (module_dir) strncpy(p->module_dir, module_dir, sizeof(p->module_dir) - 1);

    /* Fresh id — a blank instance owns its own loop files. If this instance is part of a
     * saved Set, Schwung calls set_param("state", ...) right after this with the saved id,
     * which reloads that id's loops. */
    gen_loop_id(p->loop_id);
    load_loops(p);

    if (g_host && g_host->log) g_host->log("[magneto] instance created");
    return p;
}

static void destroy_instance(void *instance) {
    plugin_instance_t *p = (plugin_instance_t *)instance;
    if (!p) return;
    save_loops(p);
    for (int s = 0; s < NUM_SIDES; s++) free(p->loop[s]);
    free(p);
}

/* ── Transport ───────────────────────────────────────────────────────────────────── */
/* Clock is "live" when we have a measured BPM and a tick arrived within the last ~0.5 s. */
static int clock_is_live(const plugin_instance_t *p) {
    return (p->clk_bpm >= 20.0) &&
           ((p->clk_frame - p->clk_last_activity) < (uint64_t)CLK_STALE_FRAMES);
}

static void toggle_play(plugin_instance_t *p) {
    if (p->play) { p->play = 0; p->rec = 0; } else { p->play = 1; }
}
static void toggle_record(plugin_instance_t *p) {
    int s = clampi(p->side, 0, NUM_SIDES - 1);
    if (!p->rec) {
        p->rec = 1;
        p->rec_tick_lock = 0;
        if (p->loop_len[s] == 0) {
            p->write_pos = 0; p->play = 0;
            /* Sync + live clock → lock this take to an exact number of clock ticks. */
            if (p->sync_mode && clock_is_live(p)) {
                int di = clampi(p->sync_div, 0, NUM_DIV - 1);
                p->rec_target_ticks = (int)(DIV_BEATS[di] * (double)CLK_PPQN + 0.5);
                p->rec_tick_count = 0;
                p->rec_tick_lock  = 1;
            }
        }
        else { p->play = 1; p->write_pos = (int)p->play_pos % p->loop_len[s]; }
    } else {
        if (p->loop_len[s] == 0) {
            p->loop_len[s] = (p->write_pos > 0) ? p->write_pos : 0;
            p->play_pos = 0.0;
            p->play = (p->loop_len[s] > 0) ? 1 : 0;
        }
        p->rec = 0;
        p->rec_tick_lock = 0;
        save_loops(p);   /* persist the finished take immediately (not the audio thread) */
    }
}

__attribute__((visibility("default")))
void move_audio_fx_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    (void)source;
    plugin_instance_t *p = (plugin_instance_t *)instance;
    if (!p || !msg || len < 1) return;

    /* MIDI real-time clock → measure tempo for Sync mode (24 ticks per quarter note).
     * Intervals are timed in samples via the block-advanced clk_frame counter, then
     * smoothed to average out the ~128-sample block quantization of MIDI delivery. */
    uint8_t st = msg[0];
    if (st == 0xF8) {                               /* clock tick */
        uint64_t now = p->clk_frame;
        if (p->clk_last_tick != 0) {
            double iv = (double)(now - p->clk_last_tick);
            if (iv > 4.0 && iv < (double)SR) {      /* plausible tick spacing */
                if (p->clk_interval <= 0.0) p->clk_interval = iv;
                else                        p->clk_interval += 0.15 * (iv - p->clk_interval);
                double bpm = 60.0 * (double)SR / (p->clk_interval * (double)CLK_PPQN);
                if (bpm >= 20.0 && bpm <= 999.0) p->clk_bpm = bpm;
            }
        }
        if (p->rec && p->rec_tick_lock) p->rec_tick_count++;   /* tick-locked take */
        p->clk_last_tick = now;
        p->clk_last_activity = now;
        return;
    }
    if (st == 0xFA || st == 0xFB) {                 /* start / continue: re-arm measurement */
        p->clk_last_tick = 0; p->clk_interval = 0.0; p->clk_last_activity = p->clk_frame; return;
    }
    if (st == 0xFC) { return; }                     /* stop: let it go stale → manual fallback */

    if (len < 3) return;
    if ((st & 0xF0) != 0xB0) return;   /* CC only */
    uint8_t cc = msg[1], val = msg[2];
    if (cc == CC_PLAY && val >= 64)             toggle_play(p);
    else if (cc == CC_RECORD && val >= 64)      toggle_record(p);
    else if (cc == CC_JOG_CLICK && val >= 64)   p->reverse = !p->reverse;
    else if (cc == CC_JOG_ROTATE) {
        int delta = (val >= 65) ? (int)val - 128 : (int)val;   /* -63..63 */
        p->scrub_frames += (double)delta * 64.0;
    }
}

/* ── Audio ─────────────────────────────────────────────────────────────────────────── */
/* Per-block tape-coloration coefficients (shared by the loop path and the live monitor). */
typedef struct {
    float sat_drive, sat_makeup, roll_coeff, hp_coeff, hiss_amt, noise_color, ncol_coeff;
    float mb0, mb1, mb2, ma1, ma2;   /* baked per-model midrange peaking EQ */
    float tone_coeff, tone_hi, tone_lo;   /* post-tape Tone tilt */
} tape_color_t;

/* saturation → model mid EQ → HF rolloff → low cut → coloured hiss. Advances the given
 * path's filter state (tape_main for the loop, tape_mon for the input monitor). */
static inline void apply_tape_color(tape_state_t *t, const tape_color_t *c, uint32_t *rng, float *wl, float *wr) {
    float l = *wl, r = *wr;
    /* Drive into the soft clip, then makeup back to ~unity small-signal gain so the tape
     * stage colours the signal without pumping it +6 dB (was making the input run hot). */
    l = tape_soft_clip(l * c->sat_drive) * c->sat_makeup;
    r = tape_soft_clip(r * c->sat_drive) * c->sat_makeup;
    l = biquad(l, c->mb0, c->mb1, c->mb2, c->ma1, c->ma2, &t->mid_z1_l, &t->mid_z2_l);
    r = biquad(r, c->mb0, c->mb1, c->mb2, c->ma1, c->ma2, &t->mid_z1_r, &t->mid_z2_r);
    t->lp_l += c->roll_coeff * (l - t->lp_l) + DENORM;
    t->lp_r += c->roll_coeff * (r - t->lp_r) + DENORM;
    l = t->lp_l; r = t->lp_r;
    t->hp_l += c->hp_coeff * (l - t->hp_l) + DENORM;
    t->hp_r += c->hp_coeff * (r - t->hp_r) + DENORM;
    l -= t->hp_l; r -= t->hp_r;
    if (c->hiss_amt > 0.0f) {
        float nwl = frand_bipolar(rng), nwr = frand_bipolar(rng);
        t->noise_lp_l += c->ncol_coeff * (nwl - t->noise_lp_l) + DENORM;
        t->noise_lp_r += c->ncol_coeff * (nwr - t->noise_lp_r) + DENORM;
        l += (t->noise_lp_l + (nwl - t->noise_lp_l) * c->noise_color) * c->hiss_amt;
        r += (t->noise_lp_r + (nwr - t->noise_lp_r) * c->noise_color) * c->hiss_amt;
    }
    *wl = l; *wr = r;
}

/* Post-tape Tone (C-1 style): tilt around ~1 kHz — bright boosts highs & trims lows, dark the
 * reverse. Applied AFTER hiss, so the hiss brightness follows Tone. */
static inline void apply_tone(tape_state_t *t, const tape_color_t *c, float *wl, float *wr) {
    float l = *wl, r = *wr;
    t->tilt_l += c->tone_coeff * (l - t->tilt_l) + DENORM;
    t->tilt_r += c->tone_coeff * (r - t->tilt_r) + DENORM;
    *wl = t->tilt_l * c->tone_lo + (l - t->tilt_l) * c->tone_hi;
    *wr = t->tilt_r * c->tone_lo + (r - t->tilt_r) * c->tone_hi;
}

/* Channel-strip 3-band EQ (low shelf → mid peak → high shelf), routable to input or loop. */
static inline void chan_eq(plugin_instance_t *p, float *cl, float *cr,
                           const float lo[5], const float md[5], const float hi[5]) {
    *cl = biquad(*cl, lo[0],lo[1],lo[2],lo[3],lo[4], &p->eq_lo_z1l, &p->eq_lo_z2l);
    *cr = biquad(*cr, lo[0],lo[1],lo[2],lo[3],lo[4], &p->eq_lo_z1r, &p->eq_lo_z2r);
    *cl = biquad(*cl, md[0],md[1],md[2],md[3],md[4], &p->eq_md_z1l, &p->eq_md_z2l);
    *cr = biquad(*cr, md[0],md[1],md[2],md[3],md[4], &p->eq_md_z1r, &p->eq_md_z2r);
    *cl = biquad(*cl, hi[0],hi[1],hi[2],hi[3],hi[4], &p->eq_hi_z1l, &p->eq_hi_z2l);
    *cr = biquad(*cr, hi[0],hi[1],hi[2],hi[3],hi[4], &p->eq_hi_z1r, &p->eq_hi_z2r);
}

/* Full channel strip: Trim → 3-band EQ → chan_vol → Pan. Applied to the input (Channel Mode =
 * Input) or the played loop (= Tape); Off = bypass. */
static inline void channel_strip(plugin_instance_t *p, float *xl, float *xr,
                                 float trim_g, const float lo[5], const float md[5], const float hi[5],
                                 float chan_g, float pan_lg, float pan_rg, int pan_mono) {
    float l = *xl * trim_g, r = *xr * trim_g;
    chan_eq(p, &l, &r, lo, md, hi);
    l *= chan_g; r *= chan_g;
    if (pan_mono) { float mono = (l + r) * 0.5f; *xl = mono * pan_lg; *xr = mono * pan_rg; }
    else          { *xl = l * pan_lg;            *xr = r * pan_rg; }
}

static void process_block(void *instance, int16_t *buf, int frames) {
    plugin_instance_t *p = (plugin_instance_t *)instance;
    if (!p || frames <= 0) return;
    if (frames > MAX_BLOCK) frames = MAX_BLOCK;

    p->clk_frame += (uint64_t)frames;   /* sample clock for MIDI-tempo measurement */

    const int s = clampi(p->side, 0, NUM_SIDES - 1);
    /* Side change → kick off a short click-free crossfade from the previous side. */
    if (s != p->render_side) {
        p->prev_side   = p->render_side;
        p->render_side = s;
        if (p->loop_len[p->prev_side] > 1) p->xfade_count = XFADE_LEN;
    }
    int16_t  *lp   = p->loop[s];
    const int llen = p->loop_len[s];

    /* Rate target — Fwd/Bwd fast-wind overrides Var-Speed; Reverse flips direction.
     * Winding uses a slow rate-smoothing coeff so it accelerates over ~3-4 s (not instant). */
    float  rate_coeff = (p->fwd || p->bwd) ? 0.00002f : 0.0025f;
    double base_ratio = (double)SPEED_RATIO[clampi(p->speed_mode, 0, NUM_SPEED - 1)];
    double rate;
    if (p->fwd)      rate =  FAST_RATE * base_ratio;
    else if (p->bwd) rate = -FAST_RATE * base_ratio;
    else {
        double r = (double)powf(2.0f, (p->varspeed - 0.5f) * 4.0f) * base_ratio;  /* ±2 oct */
        rate = p->reverse ? -r : r;
    }

    /* Tape-stop ramp: linear over 60..10000 ms to reach a full halt. */
    float  stop_ms = clampf(p->stop_speed, 60.0f, 10000.0f);
    double ts_inc  = 1.0 / ((double)stop_ms * 0.001 * (double)SR);
    double ts_targ = p->tape_stop ? 0.0 : 1.0;
    /* Fresh-take length: Free = seconds; Sync = musical division at the effective tempo
     * (live MIDI clock if ticks are flowing, else the manual Tempo knob), capped to buffer. */
    int rec_frames;
    if (p->sync_mode) {
        int di = clampi(p->sync_div, 0, NUM_DIV - 1);
        double bpm  = clock_is_live(p) ? p->clk_bpm : (double)clampi(p->tempo_bpm, 20, 999);
        double secs = DIV_BEATS[di] * 60.0 / bpm;
        long   f    = (long)(secs * (double)SR + 0.5);
        if (f > MAX_FRAMES) f = MAX_FRAMES;   /* longer than the buffer → clamp (loop not whole-bar) */
        if (f < 1)          f = 1;
        rec_frames = (int)f;
    } else {
        rec_frames = (int)(clampf(p->rec_length, 1.0f, (float)MAX_LOOP_SEC) * (float)SR);
    }
    if (rec_frames > MAX_FRAMES) rec_frames = MAX_FRAMES;

    /* jog scrub (bonus CC path) */
    if (llen > 0 && p->scrub_frames != 0.0) {
        p->play_pos += p->scrub_frames; p->scrub_frames = 0.0;
        while (p->play_pos >= llen) p->play_pos -= llen;
        while (p->play_pos < 0)     p->play_pos += llen;
    }

    /* 20 ms smoothing on the performable Perform + Channel knobs (per-block one-pole). */
    const float SM = 0.135f;   /* ≈ 20 ms per 128-sample block */
    p->sm_trim      += SM * (clampf(p->trim, -12.0f, 12.0f)          - p->sm_trim);
    p->sm_low       += SM * (clampf(p->low, -12.0f, 12.0f)           - p->sm_low);
    p->sm_mid       += SM * (clampf(p->mid, -12.0f, 12.0f)           - p->sm_mid);
    p->sm_mid_freq  += SM * (clampf(p->mid_freq, 250.0f, 5000.0f)    - p->sm_mid_freq);
    p->sm_high      += SM * (clampf(p->high, -12.0f, 12.0f)          - p->sm_high);
    p->sm_high_freq += SM * (clampf(p->high_freq, 5000.0f, 12000.0f) - p->sm_high_freq);
    p->sm_input_pan += SM * (clampf(p->input_pan, 0.0f, 1.0f)        - p->sm_input_pan);
    p->sm_chan_vol  += SM * (clampf(p->chan_vol, 0.0f, 1.0f)         - p->sm_chan_vol);
    p->sm_jump      += SM * (clampf(p->jump, 0.0f, 1.0f)             - p->sm_jump);
    p->sm_scan      += SM * (clampf(p->scan, 0.0f, 1.0f)             - p->sm_scan);
    p->sm_stutter   += SM * (clampf(p->stutter, 0.0f, 1.0f)         - p->sm_stutter);
    p->sm_failure   += SM * (clampf(p->failure, 0.0f, 1.0f)         - p->sm_failure);

    /* Tape-coloration coeffs (Generations wear folded in) → shared shape for loop + monitor. */
    float g = clampf(p->generations, 0.0f, 1.0f);
    tape_color_t tc;
    float roll_hz = (1000.0f + p->rolloff * 17000.0f) * (1.0f - g * 0.7f);
    if (roll_hz < 400.0f) roll_hz = 400.0f;
    tc.roll_coeff = 1.0f - expf(-6.2831853f * roll_hz / (float)SR);
    float lc_amt  = clampf(p->lowcut + g * 0.40f, 0.0f, 1.0f);
    tc.hp_coeff   = 1.0f - expf(-6.2831853f * (20.0f + lc_amt * 780.0f) / (float)SR);
    tc.sat_drive  = 1.0f + (p->saturation + g * 0.25f) * 3.0f;
    tc.sat_makeup = 1.0f / tc.sat_drive;      /* unity small-signal gain (colour without level boost) */
    tc.hiss_amt   = clampf(p->hiss + g * 0.05f, 0.0f, 1.0f) * 0.01f;
    tc.ncol_coeff = 1.0f - expf(-6.2831853f * 3000.0f / (float)SR);
    const tape_model_t *tm = &MODELS[clampi(p->model, 0, NUM_MODEL - 1)];
    peaking_coeffs(tm->mid_hz, tm->mid_gain_db, tm->mid_q, &tc.mb0, &tc.mb1, &tc.mb2, &tc.ma1, &tc.ma2);
    tc.noise_color = tm->noise_color;
    /* Post-tape Tone (C-1 style, + model tilt bias) — tilts around 1 kHz, affects hiss too. */
    tc.tone_coeff = 1.0f - expf(-6.2831853f * 1000.0f / (float)SR);
    float ttone = clampf((p->tone - 0.5f) * 2.0f + tm->tilt_bias, -1.0f, 1.0f);
    tc.tone_hi = 1.0f + ttone * 1.6f;   if (tc.tone_hi < 0.0f) tc.tone_hi = 0.0f;
    tc.tone_lo = 1.0f - ttone * 0.4f;
    float wear_mod = 1.0f + g * 0.6f;
    float wphi = 6.2831853f * (0.6f + p->wow * 4.0f) / (float)SR;
    float fphi = 6.2831853f * (7.0f + p->flutter * 20.0f) / (float)SR;
    float wow_depth_fr  = p->wow * 220.0f * wear_mod;
    float flut_depth_fr = p->flutter * 55.0f * wear_mod;

    float vol = p->volume;
    float fb  = clampf(p->feedback, 0.0f, 1.0f);   /* feedback = level of processed wet re-recorded */
    int   monitor_mode = (p->rec_mode == 0);       /* Monitor (input always audible) vs Loop Only */
    float mix_lvl = clampf(p->mix, 0.0f, 1.0f);
    float ma_ = mix_lvl * 1.5707963f;
    float dry_g = cosf(ma_), wet_g = sinf(ma_);    /* Loop-Only equal-power dry/wet */

    /* Channel strip coeffs (smoothed real units). Channel Mode routes the WHOLE strip. */
    int   chan_input = (p->eq_in == 0);            /* strip on the input (recorded) */
    int   chan_tape  = (p->eq_in == 1);            /* strip on the played loop instead */
    float trim_g  = powf(10.0f, p->sm_trim / 20.0f);
    float lo[5], md[5], hi[5];
    lowshelf_coeffs(100.0f, p->sm_low, &lo[0],&lo[1],&lo[2],&lo[3],&lo[4]);
    peaking_coeffs(clampf(p->sm_mid_freq,250.0f,5000.0f), p->sm_mid, 0.8f, &md[0],&md[1],&md[2],&md[3],&md[4]);
    highshelf_coeffs(clampf(p->sm_high_freq,5000.0f,12000.0f), p->sm_high, &hi[0],&hi[1],&hi[2],&hi[3],&hi[4]);
    float chan_g = p->sm_chan_vol;

    /* pan (smoothed input_pan + pan_mode) */
    float pan = (p->sm_input_pan - 0.5f) * 2.0f;
    int   pan_mono = (p->pan_mode == 0);
    float pan_lg, pan_rg;
    if (pan_mono) { float th = (pan + 1.0f) * 0.25f * 3.14159265f; pan_lg = cosf(th) * 1.41421356f; pan_rg = sinf(th) * 1.41421356f; }
    else          { pan_lg = (pan > 0.0f) ? (1.0f - pan) : 1.0f;   pan_rg = (pan < 0.0f) ? (1.0f + pan) : 1.0f; }

    float st_amt   = p->sm_stutter;
    float fail_amt = p->sm_failure;
    float jump_amt = p->sm_jump;
    float scan_amt = p->sm_scan;
    double scan_win  = (double)llen * pow(2.0, -8.0 * (double)scan_amt);
    if (scan_win < (double)SCAN_MIN_WIN) scan_win = (double)SCAN_MIN_WIN;  /* keep micro-loop musical */
    if (scan_win > (double)llen)         scan_win = (double)llen;
    double scan_base = (double)llen * (0.5 + 0.49 * (double)scan_amt);
    /* Keep the whole scan window INSIDE the buffer. Without this, at high scan the
     * window straddles the loop end and the 2nd playhead reads across the raw seam
     * (end→start) with no crossfade → the buzz/distortion that worsens past ~80%. */
    if (scan_base + scan_win > (double)llen) scan_base = (double)llen - scan_win;
    if (scan_base < 0.0) scan_base = 0.0;

    /* Scrub = turntable jog: moving the knob sets a target + a ~0.3 s active window; the
     * playhead then *chases* the target continuously (smooth, pitch follows turn speed). */
    if (p->scrub != p->scrub_prev && llen > 1) {
        p->jog_target = (double)clampf(p->scrub, 0.0f, 1.0f) * (double)(llen - 1);
        p->jog_hold   = (int)(0.15 * SR);   /* bridges knob updates; short tail slows into the fade */
        p->scrub_prev = p->scrub;
    }
    int jog_active = (p->jog_hold > 0 && llen > 1);
    if (jog_active) { p->jog_hold -= frames; if (p->jog_hold < 0) p->jog_hold = 0; }

    for (int i = 0; i < frames; i++) {
        float raw_l = buf[i * 2] / 32768.0f, raw_r = buf[i * 2 + 1] / 32768.0f;

        /* ── Conditioned input: full channel strip if Channel Mode = Input, else raw ── */
        float cond_l = raw_l, cond_r = raw_r;
        if (chan_input) channel_strip(p, &cond_l, &cond_r, trim_g, lo, md, hi, chan_g, pan_lg, pan_rg, pan_mono);
        /* DC blocker (~14 Hz) so sound-on-sound overdub can't accumulate DC / sub-rumble */
        { float y;
          y = cond_l - p->dcb_x1l + 0.998f * p->dcb_y1l; p->dcb_x1l = cond_l; p->dcb_y1l = y; cond_l = y;
          y = cond_r - p->dcb_x1r + 0.998f * p->dcb_y1r; p->dcb_x1r = cond_r; p->dcb_y1r = y; cond_r = y;
          if (p->dcb_y1l < 1e-18f && p->dcb_y1l > -1e-18f) p->dcb_y1l = 0.0f;
          if (p->dcb_y1r < 1e-18f && p->dcb_y1r > -1e-18f) p->dcb_y1r = 0.0f;
        }

        /* tape-stop ramp toward target (per sample) */
        if (p->ts_mult < ts_targ)      { p->ts_mult += ts_inc; if (p->ts_mult > ts_targ) p->ts_mult = ts_targ; }
        else if (p->ts_mult > ts_targ) { p->ts_mult -= ts_inc; if (p->ts_mult < ts_targ) p->ts_mult = ts_targ; }

        int active_len = p->loop_len[s];
        int want_play  = (p->play || jog_active);                        /* jog plays even when stopped */
        int playing = (want_play || p->play_gain > 0.0003f) && active_len > 1 && lp;  /* render while fading */
        float wet_l = 0.0f, wet_r = 0.0f;   /* processed loop (post-Perform) — feedback source */

        if (playing) {
            if (!jog_active) {
                /* Stutter (jog overrides positioning) */
                if (st_amt > 0.001f && active_len > 4000) {
                    if (p->st_count <= 0) {
                        int slice = active_len / 12;
                        if (slice < 2000) slice = 2000;
                        if (slice > SR / 3) slice = SR / 3;
                        p->st_count = slice;
                        if (frand01(&p->rng) < st_amt * 0.85f) {
                            float r2 = frand01(&p->rng);
                            if (r2 < 0.45f)      { p->xj_pos = p->play_pos; p->xj_count = JUMP_XFADE; p->play_pos = p->st_anchor; }
                            else if (r2 < 0.75f) { p->xj_pos = p->play_pos; p->xj_count = JUMP_XFADE; p->play_pos = p->st_anchor - slice; while (p->play_pos < 0) p->play_pos += active_len; }
                            else                 p->st_dir = -p->st_dir;
                        } else { p->st_anchor = p->play_pos; p->st_dir = 1; }
                    }
                    p->st_count--;
                } else p->st_dir = 1;

                /* Jump: at each segment boundary, leap to a random spot (segment 1/2..1/1024) */
                if (jump_amt > 0.001f && active_len > 64) {
                    if (p->jump_count <= 0) {
                        int seg = (int)((float)active_len * powf(2.0f, -(1.0f + 9.0f * jump_amt)));
                        if (seg < 1) seg = 1;
                        p->jump_count = seg;
                        p->xj_pos = p->play_pos; p->xj_count = JUMP_XFADE;   /* declick the jump */
                        p->play_pos = (double)(frand01(&p->rng) * (float)(active_len - 1));
                    }
                    p->jump_count--;
                }
            } else p->st_dir = 1;

            p->wow_phase += wphi;     if (p->wow_phase > 6.2831853f) p->wow_phase -= 6.2831853f;
            p->flutter_phase += fphi; if (p->flutter_phase > 6.2831853f) p->flutter_phase -= 6.2831853f;
            double mod = (double)(sinf(p->wow_phase) * wow_depth_fr + sinf(p->flutter_phase) * flut_depth_fr);

            read_loop(lp, active_len, p->play_pos + mod, &wet_l, &wet_r);

            /* Loop-seam crossfade (~20 ms): in the last window before the wrap, fade the end
             * of the loop into its start so the loop point doesn't click. */
            if (active_len > 2 * POS_XFADE_LEN && p->play_pos > (double)(active_len - POS_XFADE_LEN)) {
                double t = (p->play_pos - (double)(active_len - POS_XFADE_LEN)) / (double)POS_XFADE_LEN;
                float sl, sr;
                read_loop(lp, active_len, p->play_pos - (double)(active_len - POS_XFADE_LEN) + mod, &sl, &sr);
                float fo = cosf((float)t * 1.5707963f), fi = sinf((float)t * 1.5707963f);
                wet_l = wet_l * fo + sl * fi;
                wet_r = wet_r * fo + sr * fi;
            }

            /* Declick Jump/Stutter position jumps: crossfade the old trajectory (~7 ms) */
            if (p->xj_count > 0) {
                float xl, xr; read_loop(lp, active_len, p->xj_pos + mod, &xl, &xr);
                float t = (float)p->xj_count / (float)JUMP_XFADE;
                float go = sinf(t * 1.5707963f), gn = cosf(t * 1.5707963f);
                wet_l = xl * go + wet_l * gn; wet_r = xr * go + wet_r * gn;
                p->xj_pos += p->rate_smooth * (double)p->st_dir * p->ts_mult;
                while (p->xj_pos >= active_len) p->xj_pos -= active_len;
                while (p->xj_pos < 0)           p->xj_pos += active_len;
                p->xj_count--;
            }

            /* Side crossfade */
            if (p->xfade_count > 0) {
                int plen = p->loop_len[p->prev_side];
                if (plen > 1) {
                    float pl, pr; read_loop(p->loop[p->prev_side], plen, p->play_pos + mod, &pl, &pr);
                    float t = (float)p->xfade_count / (float)XFADE_LEN;
                    float go = sinf(t * 1.5707963f), gn = cosf(t * 1.5707963f);
                    wet_l = pl * go + wet_l * gn; wet_r = pr * go + wet_r * gn;
                }
                p->xfade_count--;
            }

            /* Scan: second playhead (full-loop offset → micro-loop). 20 ms crossfade at BOTH
             * wrap edges + a fade-in envelope on engage so it doesn't click. */
            {
                float sc_tgt = (scan_amt > 0.001f && !jog_active) ? 1.0f : 0.0f;
                p->scan_gain += 0.0022f * (sc_tgt - p->scan_gain);   /* ~10 ms fade */
                if (p->scan_gain < 1e-6f) p->scan_gain = 0.0f;
            }
            if (p->scan_gain > 0.0f) {
                float s2l, s2r; read_loop(lp, active_len, p->scan_pos, &s2l, &s2r);
                if (p->scan_xfade > 0) {
                    float xl, xr; read_loop(lp, active_len, p->scan_xpos, &xl, &xr);
                    float t = (float)p->scan_xfade / (float)POS_XFADE_LEN;
                    float go = sinf(t * 1.5707963f), gn = cosf(t * 1.5707963f);
                    s2l = xl * go + s2l * gn; s2r = xr * go + s2r * gn;
                    p->scan_xpos += p->rate_smooth * p->ts_mult;
                    while (p->scan_xpos >= active_len) p->scan_xpos -= active_len;
                    while (p->scan_xpos < 0)           p->scan_xpos += active_len;
                    p->scan_xfade--;
                }
                wet_l += s2l * 0.9f * p->scan_gain; wet_r += s2r * 0.9f * p->scan_gain;
                p->scan_pos += p->rate_smooth * p->ts_mult;
                if (p->scan_pos >= scan_base + scan_win) {   /* wrapped past window end */
                    p->scan_xpos = p->scan_pos; p->scan_xfade = POS_XFADE_LEN; p->scan_pos -= scan_win;
                }
                if (p->scan_pos < scan_base) {               /* window moved up under us */
                    p->scan_xpos = p->scan_pos; p->scan_xfade = POS_XFADE_LEN; p->scan_pos += scan_win;
                }
                while (p->scan_pos >= active_len) p->scan_pos -= active_len;
                while (p->scan_pos < 0)           p->scan_pos += active_len;
            }

            /* Channel Mode = Tape → run the full channel strip (trim/EQ/vol/pan) on the loop */
            if (chan_tape) channel_strip(p, &wet_l, &wet_r, trim_g, lo, md, hi, chan_g, pan_lg, pan_rg, pan_mono);

            /* advance playhead: Scrub jog chases its target (pitch follows turn speed), else rate */
            if (jog_active) {
                double vel = (p->jog_target - p->play_pos) * JOG_CHASE;
                if (vel >  JOG_VEL_CAP) vel =  JOG_VEL_CAP;
                if (vel < -JOG_VEL_CAP) vel = -JOG_VEL_CAP;
                p->play_pos += vel;
            } else {
                p->rate_smooth += (double)rate_coeff * (rate - p->rate_smooth);
                p->play_pos += p->rate_smooth * (double)p->st_dir * p->ts_mult;
            }
            while (p->play_pos >= active_len) p->play_pos -= active_len;
            while (p->play_pos < 0)           p->play_pos += active_len;

            apply_tape_color(&p->tape_main, &tc, &p->rng, &wet_l, &wet_r);
            apply_tone(&p->tape_main, &tc, &wet_l, &wet_r);

            /* Failure dropouts (skipped while jogging) */
            if (!jog_active && fail_amt > 0.001f) {
                if (p->fail_count <= 0) {
                    if (p->fail_state == 1) { p->fail_state = 0; p->fail_count = (int)(SR * (0.03f + frand01(&p->rng) * 0.20f)); }
                    else if (frand01(&p->rng) < fail_amt * 0.5f) { p->fail_state = 1; p->fail_count = (int)(SR * (0.005f + frand01(&p->rng) * 0.05f)); }
                    else { p->fail_state = 0; p->fail_count = (int)(SR * (0.02f + frand01(&p->rng) * 0.20f)); }
                }
                p->fail_count--;
                float tgt = (p->fail_state == 1) ? 0.0f : 1.0f;
                p->fail_gain += 0.02f * (tgt - p->fail_gain);
            } else p->fail_gain += 0.05f * (1.0f - p->fail_gain);
            if (p->fail_gain < 1e-15f) p->fail_gain = 0.0f;
            wet_l *= p->fail_gain; wet_r *= p->fail_gain;

            /* tape-stop volume fade as it slows to a halt */
            float tsf = (float)p->ts_mult;
            wet_l *= tsf; wet_r *= tsf;
        }

        /* Loop fade envelope (declick Play/Stop): ramp toward want_play. */
        p->play_gain += PLAY_GAIN_COEFF * ((want_play ? 1.0f : 0.0f) - p->play_gain);
        if (p->play_gain < 1e-7f) p->play_gain = 0.0f;
        float pg = p->play_gain;

        /* Input monitor through tape+EQ — needed in Monitor mode, and while the loop is faded out. */
        float mon_l = 0.0f, mon_r = 0.0f;
        int need_mon = monitor_mode || pg < 0.999f;
        if (need_mon) {
            mon_l = cond_l; mon_r = cond_r;
            apply_tape_color(&p->tape_mon, &tc, &p->rng, &mon_l, &mon_r);
            apply_tone(&p->tape_mon, &tc, &mon_l, &mon_r);
        }

        /* ── Record / overdub ── */
        if (p->rec && lp) {
            if (llen == 0) {                       /* fresh take: clean conditioned input */
                /* Stop on the tick count when clock-locked, else on the sample length.
                 * MAX_FRAMES is the hard buffer cap (also catches a locked take whose
                 * clock stalls mid-record — it finalizes at 30 s instead of hanging). */
                int stop = p->rec_tick_lock ? (p->rec_tick_count >= p->rec_target_ticks)
                                            : (p->write_pos >= rec_frames);
                if (!stop && p->write_pos < MAX_FRAMES) {
                    lp[p->write_pos * 2]     = f2i16(cond_l);
                    lp[p->write_pos * 2 + 1] = f2i16(cond_r);
                    p->write_pos++;
                } else {
                    p->loop_len[s] = (p->write_pos > 0) ? p->write_pos : 0;
                    p->rec = 0; p->play = 1; p->play_pos = 0.0; p->rec_tick_lock = 0;
                }
            } else {
                /* Overdub = classic sound-on-sound: retain the RAW old loop (at the write head,
                 * aligned, no wow/flutter, no tape colour) × Feedback, then add the new input.
                 * Feeding the *processed* wet back re-warped + re-coloured the loop every pass →
                 * the wobbly, compounding mess that buried the new take. */
                int wp = p->write_pos % llen;
                float ol = lp[wp*2] / 32768.0f, orr = lp[wp*2+1] / 32768.0f;
                lp[wp*2]   = f2i16(tape_soft_clip(ol  * fb + cond_l));
                lp[wp*2+1] = f2i16(tape_soft_clip(orr * fb + cond_r));
                p->write_pos = (wp + 1) % llen;
            }
        }

        /* ── Output (loop faded in/out by play_gain to declick Play/Stop) ── */
        float out_l, out_r;
        if (monitor_mode) {
            /* input always monitored; loop fades in/out at Mix level */
            out_l = mon_l + wet_l * mix_lvl * pg;
            out_r = mon_r + wet_r * mix_lvl * pg;
        } else {
            /* Loop Only: crossfade loop (pg) ↔ monitor (1−pg), then equal-power Mix vs raw */
            float wl = wet_l * pg + mon_l * (1.0f - pg);
            float wr = wet_r * pg + mon_r * (1.0f - pg);
            out_l = dry_g * raw_l + wet_g * wl;
            out_r = dry_g * raw_r + wet_g * wr;
        }
        out_l *= vol; out_r *= vol;

        /* Airwindows ClipOnly2 — tape-style master soft ceiling */
        out_l = (float)clip_only2((double)out_l, &p->clip_l);
        out_r = (float)clip_only2((double)out_r, &p->clip_r);

        int32_t il = (int32_t)(out_l * 32767.0f);
        int32_t ir = (int32_t)(out_r * 32767.0f);
        il = il >  32767 ?  32767 : il; il = il < -32768 ? -32768 : il;
        ir = ir >  32767 ?  32767 : ir; ir = ir < -32768 ? -32768 : ir;
        buf[i * 2]     = (int16_t)il;
        buf[i * 2 + 1] = (int16_t)ir;
    }
}

/* ── Parameters ──────────────────────────────────────────────────────────────────── */
static void set_param(void *instance, const char *key, const char *val) {
    plugin_instance_t *p = (plugin_instance_t *)instance;
    if (!p || !key || !val) return;

    /* page navigation for knob-overlay routing */
    if (!strcmp(key, "_level")) {
        if      (!strcmp(val, "Perform")) p->current_page = 1;
        else if (!strcmp(val, "Channel")) p->current_page = 2;
        else if (!strcmp(val, "Tape"))    p->current_page = 3;
        else if (!strcmp(val, "Deck"))    p->current_page = 4;
        else if (!strcmp(val, "Recs"))    p->current_page = 5;
        else                              p->current_page = 0;
        return;
    }

    /* knob_N_adjust — page-aware overlay (fallback path) */
    if (strncmp(key, "knob_", 5) == 0 && strstr(key, "_adjust")) {
        int idx = atoi(key + 5) - 1;
        const knob_def_t *map = active_map(p);
        if (idx >= 0 && idx < 8 && map[idx].key) {
            int delta = atoi(val);
            const knob_def_t *k = &map[idx];
            if (k->is_enum) {
                if (!strcmp(k->key, "speed")) p->speed_mode = clampi(p->speed_mode + delta, 0, NUM_SPEED - 1);
                else if (!strcmp(k->key, "side")) p->side = clampi(p->side + delta, 0, NUM_SIDES - 1);
                else if (!strcmp(k->key, "model")) apply_model(p, clampi(p->model + delta, 0, NUM_MODEL - 1));
                else if (!strcmp(k->key, "pan_mode")) p->pan_mode = clampi(p->pan_mode + delta, 0, NUM_PANMODE - 1);
                else if (!strcmp(k->key, "play")) { if (delta > 0) p->play = 1; else if (delta < 0) { p->play = 0; p->rec = 0; } }
                else if (!strcmp(k->key, "reverse")) { if (delta > 0) p->reverse = 1; else if (delta < 0) p->reverse = 0; }
                else if (!strcmp(k->key, "fwd")) { if (delta > 0) p->fwd = 1; else if (delta < 0) p->fwd = 0; }
                else if (!strcmp(k->key, "bwd")) { if (delta > 0) p->bwd = 1; else if (delta < 0) p->bwd = 0; }
                else if (!strcmp(k->key, "tape_stop")) { if (delta > 0) p->tape_stop = 1; else if (delta < 0) p->tape_stop = 0; }
                else if (!strcmp(k->key, "rec")) {
                    int w = (delta > 0) ? 1 : 0;
                    if (w && !p->rec) toggle_record(p); else if (!w && p->rec) toggle_record(p);
                }
                else if (!strcmp(k->key, "clear")) {
                    if (delta > 0) { int sd = clampi(p->side,0,NUM_SIDES-1); p->loop_len[sd]=0; p->write_pos=0; p->play_pos=0.0; p->rec=0; save_loops(p); }
                }
            } else {
                float *f = float_param(p, k->key);
                if (f) *f = clampf(*f + delta * k->step, k->min, k->max);
            }
        }
        return;
    }

    /* transport menu triggers (enum option strings; numeric tolerated as fallback) */
    if (!strcmp(key, "play")) {
        int want = (!strcmp(val, "Playing")) ? 1 : (!strcmp(val, "Stopped")) ? 0 : (atoi(val) ? 1 : 0);
        if (want) p->play = 1; else { p->play = 0; p->rec = 0; }
        return;
    }
    if (!strcmp(key, "rec") || !strcmp(key, "record")) {
        int want = (!strcmp(val, "Rec") || !strcmp(val, "Recording")) ? 1
                 : (!strcmp(val, "Stopped")) ? 0 : (atoi(val) ? 1 : 0);
        if (want && !p->rec)      toggle_record(p);   /* arm record/overdub */
        else if (!want && p->rec) toggle_record(p);   /* finalize take */
        return;
    }
    if (!strcmp(key, "reverse")) {
        p->reverse = (!strcmp(val, "Rev")) ? 1 : (!strcmp(val, "Fwd")) ? 0 : (atoi(val) ? 1 : 0);
        return;
    }
    if (!strcmp(key, "fwd"))  { p->fwd  = (!strcmp(val, "On")) ? 1 : (!strcmp(val, "Off")) ? 0 : (atoi(val) ? 1 : 0); return; }
    if (!strcmp(key, "bwd"))  { p->bwd  = (!strcmp(val, "On")) ? 1 : (!strcmp(val, "Off")) ? 0 : (atoi(val) ? 1 : 0); return; }
    if (!strcmp(key, "tape_stop")) { p->tape_stop = (!strcmp(val, "On")) ? 1 : (!strcmp(val, "Off")) ? 0 : (atoi(val) ? 1 : 0); return; }
    if (!strcmp(key, "clear")) {
        /* momentary: idle option "Clear" is a no-op; action option "Cleared" wipes the side */
        if (!strcmp(val, "Clear")) return;
        int s = clampi(p->side, 0, NUM_SIDES - 1);
        p->loop_len[s] = 0; p->write_pos = 0; p->play_pos = 0.0; p->rec = 0;
        save_loops(p);
        return;
    }
    if (!strcmp(key, "recover")) {
        /* momentary: idle "Recover" no-op; "Recovered" scans disk for the newest loop */
        if (!strcmp(val, "Recover")) return;
        recover_loop(p);
        return;
    }
    if (!strcmp(key, "save"))    { save_loops(p); return; }

    /* Recordings page — load WAV into a side, blank a side, export to Magneto Recs */
    if (!strcmp(key, "load_a")) { if (val[0]) load_wav_into_side(p, 0, val); return; }
    if (!strcmp(key, "load_b")) { if (val[0]) load_wav_into_side(p, 1, val); return; }
    if (!strcmp(key, "blank_a")) {
        if (strcmp(val, "Blank") == 0) return;       /* idle option; "Blanked" fires */
        p->loop_len[0] = 0; p->load_path[0][0] = '\0';
        if (clampi(p->side,0,1) == 0) { p->play_pos = 0.0; p->write_pos = 0; p->rec = 0; }
        save_loops(p); return;
    }
    if (!strcmp(key, "blank_b")) {
        if (strcmp(val, "Blank") == 0) return;
        p->loop_len[1] = 0; p->load_path[1][0] = '\0';
        if (clampi(p->side,0,1) == 1) { p->play_pos = 0.0; p->write_pos = 0; p->rec = 0; }
        save_loops(p); return;
    }
    if (!strcmp(key, "save_recs")) {
        if (strcmp(val, "Save") == 0) return;        /* idle option; "Saved" fires */
        export_wav(p, clampi(p->side, 0, NUM_SIDES - 1));   /* export the active side */
        return;
    }

    /* enums accept name string or index */
    if (!strcmp(key, "speed")) {
        for (int i = 0; i < NUM_SPEED; i++) if (!strcmp(val, SPEED_NAMES[i])) { p->speed_mode = i; return; }
        p->speed_mode = clampi(atoi(val), 0, NUM_SPEED - 1); return;
    }
    if (!strcmp(key, "side")) {
        for (int i = 0; i < NUM_SIDE_OPT; i++) if (!strcmp(val, SIDE_NAMES[i])) { p->side = i; return; }
        p->side = clampi(atoi(val), 0, NUM_SIDES - 1); return;
    }
    if (!strcmp(key, "model")) {
        for (int i = 0; i < NUM_MODEL; i++) if (!strcmp(val, MODELS[i].name)) { apply_model(p, i); return; }
        apply_model(p, clampi(atoi(val), 0, NUM_MODEL - 1)); return;
    }
    if (!strcmp(key, "pan_mode")) {
        for (int i = 0; i < NUM_PANMODE; i++) if (!strcmp(val, PANMODE_NAMES[i])) { p->pan_mode = i; return; }
        p->pan_mode = clampi(atoi(val), 0, NUM_PANMODE - 1); return;
    }
    if (!strcmp(key, "eq_in")) {
        for (int i = 0; i < NUM_EQIN; i++) if (!strcmp(val, EQIN_NAMES[i])) { p->eq_in = i; return; }
        p->eq_in = clampi(atoi(val), 0, NUM_EQIN - 1); return;
    }
    if (!strcmp(key, "rec_mode")) {
        for (int i = 0; i < NUM_RECMODE; i++) if (!strcmp(val, RECMODE_NAMES[i])) { p->rec_mode = i; return; }
        p->rec_mode = clampi(atoi(val), 0, NUM_RECMODE - 1); return;
    }
    if (!strcmp(key, "sync_mode")) {
        if (!strcmp(val, "Free")) { p->sync_mode = 0; return; }
        if (!strcmp(val, "Sync")) { p->sync_mode = 1; return; }
        p->sync_mode = clampi(atoi(val), 0, 1); return;
    }
    if (!strcmp(key, "sync_div")) {
        for (int i = 0; i < NUM_DIV; i++) if (!strcmp(val, DIV_NAMES[i])) { p->sync_div = i; return; }
        p->sync_div = clampi(atoi(val), 0, NUM_DIV - 1); return;
    }
    if (!strcmp(key, "tempo")) { p->tempo_bpm = clampi(atoi(val), 20, 999); return; }

    /* real-unit params (dB / Hz / ms / sec) — clamp to their own ranges before the 0..1 catch-all */
    if (!strcmp(key, "trim"))       { p->trim       = clampf((float)atof(val), -12.0f, 12.0f);   return; }
    if (!strcmp(key, "low"))        { p->low        = clampf((float)atof(val), -12.0f, 12.0f);   return; }
    if (!strcmp(key, "mid"))        { p->mid        = clampf((float)atof(val), -12.0f, 12.0f);   return; }
    if (!strcmp(key, "high"))       { p->high       = clampf((float)atof(val), -12.0f, 12.0f);   return; }
    if (!strcmp(key, "mid_freq"))   { p->mid_freq   = clampf((float)atof(val), 250.0f, 5000.0f); return; }
    if (!strcmp(key, "high_freq"))  { p->high_freq  = clampf((float)atof(val), 5000.0f, 12000.0f); return; }
    if (!strcmp(key, "stop_speed")) { p->stop_speed = clampf((float)atof(val), 60.0f, 10000.0f); return; }
    if (!strcmp(key, "rec_length")) { p->rec_length = clampf((float)atof(val), 1.0f, (float)MAX_LOOP_SEC); return; }

    { float *f = float_param(p, key); if (f) { *f = clampf(atof(val), 0.0f, 1.0f); return; } }

    if (!strcmp(key, "state")) {
        int spd = p->speed_mode, sd = p->side, md = p->model, pm = p->pan_mode;
        sscanf(val,
            "varspeed=%f;speed=%d;side=%d;tone=%f;volume=%f;model=%d;mix=%f;feedback=%f;"
            "wow=%f;flutter=%f;saturation=%f;rolloff=%f;hiss=%f",
            &p->varspeed, &spd, &sd, &p->tone, &p->volume, &md, &p->mix, &p->feedback,
            &p->wow, &p->flutter, &p->saturation, &p->rolloff, &p->hiss);
        p->speed_mode = clampi(spd, 0, NUM_SPEED - 1);
        p->side = clampi(sd, 0, NUM_SIDES - 1);
        p->model = clampi(md, 0, NUM_MODEL - 1);
        /* newer fields parsed individually so older state strings still load */
        { const char *q;
          if ((q = strstr(val, "lowcut="))      ) p->lowcut      = clampf((float)atof(q + 7), 0.0f, 1.0f);
          if ((q = strstr(val, "generations=")) ) p->generations = clampf((float)atof(q + 12), 0.0f, 1.0f);
          if ((q = strstr(val, "input_pan="))   ) p->input_pan   = clampf((float)atof(q + 10), 0.0f, 1.0f);
          if ((q = strstr(val, "stutter="))     ) p->stutter     = clampf((float)atof(q + 8), 0.0f, 1.0f);
          if ((q = strstr(val, "failure="))     ) p->failure     = clampf((float)atof(q + 8), 0.0f, 1.0f);
          if ((q = strstr(val, "pan_mode="))    ) pm             = atoi(q + 9);
          if ((q = strstr(val, "trim="))        ) p->trim        = clampf((float)atof(q + 5), -12.0f, 12.0f);
          if ((q = strstr(val, "low="))         ) p->low         = clampf((float)atof(q + 4), -12.0f, 12.0f);
          if ((q = strstr(val, "mid="))         ) p->mid         = clampf((float)atof(q + 4), -12.0f, 12.0f);
          if ((q = strstr(val, "mid_freq="))    ) p->mid_freq    = clampf((float)atof(q + 9), 250.0f, 5000.0f);
          if ((q = strstr(val, "high="))        ) p->high        = clampf((float)atof(q + 5), -12.0f, 12.0f);
          if ((q = strstr(val, "high_freq="))   ) p->high_freq   = clampf((float)atof(q + 10), 5000.0f, 12000.0f);
          if ((q = strstr(val, "chan_vol="))    ) p->chan_vol    = clampf((float)atof(q + 9), 0.0f, 1.0f);
          if ((q = strstr(val, "jump="))        ) p->jump        = clampf((float)atof(q + 5), 0.0f, 1.0f);
          if ((q = strstr(val, "scan="))        ) p->scan        = clampf((float)atof(q + 5), 0.0f, 1.0f);
          if ((q = strstr(val, "stop_speed="))  ) p->stop_speed  = clampf((float)atof(q + 11), 60.0f, 10000.0f);
          if ((q = strstr(val, "rec_length="))  ) p->rec_length  = clampf((float)atof(q + 11), 1.0f, (float)MAX_LOOP_SEC);
          if ((q = strstr(val, "eq_in="))       ) p->eq_in       = clampi(atoi(q + 6), 0, NUM_EQIN - 1);
          if ((q = strstr(val, "rec_mode="))    ) p->rec_mode    = clampi(atoi(q + 9), 0, NUM_RECMODE - 1);
          if ((q = strstr(val, "sync_mode="))   ) p->sync_mode   = clampi(atoi(q + 10), 0, 1);
          if ((q = strstr(val, "sync_div="))    ) p->sync_div    = clampi(atoi(q + 9), 0, NUM_DIV - 1);
          if ((q = strstr(val, "tempo="))       ) p->tempo_bpm   = clampi(atoi(q + 6), 20, 999);
        }
        p->pan_mode = clampi(pm, 0, NUM_PANMODE - 1);

        /* Restore the saved loop id and reload its loops (this is how a saved Set recalls
         * what was recorded). Parsed separately so a missing id leaves this blank. */
        const char *idp = strstr(val, "id=");
        if (idp) {
            idp += 3;
            int n = 0;
            while (n < LOOP_ID_LEN - 1 && idp[n] &&
                   ((idp[n] >= '0' && idp[n] <= '9') || (idp[n] >= 'a' && idp[n] <= 'f'))) {
                p->loop_id[n] = idp[n]; n++;
            }
            if (n > 0) { p->loop_id[n] = '\0'; load_loops(p); }
        }
        return;
    }
}

static int get_param(void *instance, const char *key, char *buf, int buf_len) {
    plugin_instance_t *p = (plugin_instance_t *)instance;
    if (!p || !key || !buf || buf_len < 1) return 0;

    if (!strcmp(key, "name")) return snprintf(buf, buf_len, "Magneto");

    if (!strcmp(key, "chain_params")) return -1;  /* JSON exceeds ~4KB shim buffer; host reads the full chain_params from module.json */

    if (!strcmp(key, "speed")) return snprintf(buf, buf_len, "%s", SPEED_NAMES[clampi(p->speed_mode,0,NUM_SPEED-1)]);
    if (!strcmp(key, "side"))  return snprintf(buf, buf_len, "%s", SIDE_NAMES[clampi(p->side,0,NUM_SIDE_OPT-1)]);
    if (!strcmp(key, "model")) return snprintf(buf, buf_len, "%s", MODELS[clampi(p->model,0,NUM_MODEL-1)].name);
    if (!strcmp(key, "pan_mode")) return snprintf(buf, buf_len, "%s", PANMODE_NAMES[clampi(p->pan_mode,0,NUM_PANMODE-1)]);
    if (!strcmp(key, "eq_in"))    return snprintf(buf, buf_len, "%s", EQIN_NAMES[clampi(p->eq_in,0,NUM_EQIN-1)]);
    if (!strcmp(key, "rec_mode")) return snprintf(buf, buf_len, "%s", RECMODE_NAMES[clampi(p->rec_mode,0,NUM_RECMODE-1)]);
    if (!strcmp(key, "sync_mode")) return snprintf(buf, buf_len, "%s", p->sync_mode ? "Sync" : "Free");
    if (!strcmp(key, "sync_div"))  return snprintf(buf, buf_len, "%s", DIV_NAMES[clampi(p->sync_div,0,NUM_DIV-1)]);
    if (!strcmp(key, "tempo"))     return snprintf(buf, buf_len, "%d", clampi(p->tempo_bpm,20,999));

    /* transport menu triggers — reflect live state so the menu stays in sync */
    if (!strcmp(key, "play"))    return snprintf(buf, buf_len, "%s", p->play ? "Playing" : "Stopped");
    if (!strcmp(key, "rec") || !strcmp(key, "record"))
                                 return snprintf(buf, buf_len, "%s", p->rec ? "Rec" : "Stopped");
    if (!strcmp(key, "reverse")) return snprintf(buf, buf_len, "%s", p->reverse ? "Rev" : "Fwd");
    if (!strcmp(key, "fwd"))     return snprintf(buf, buf_len, "%s", p->fwd ? "On" : "Off");
    if (!strcmp(key, "bwd"))     return snprintf(buf, buf_len, "%s", p->bwd ? "On" : "Off");
    if (!strcmp(key, "tape_stop")) return snprintf(buf, buf_len, "%s", p->tape_stop ? "On" : "Off");
    if (!strcmp(key, "clear"))   return snprintf(buf, buf_len, "Clear");  /* idle; "Cleared" fires */
    if (!strcmp(key, "recover")) return snprintf(buf, buf_len, "Recover"); /* idle; "Recovered" fires */

    /* Recordings page */
    if (!strcmp(key, "load_a"))   return snprintf(buf, buf_len, "%s", p->load_path[0]);
    if (!strcmp(key, "load_b"))   return snprintf(buf, buf_len, "%s", p->load_path[1]);
    if (!strcmp(key, "blank_a"))  return snprintf(buf, buf_len, "Blank");
    if (!strcmp(key, "blank_b"))  return snprintf(buf, buf_len, "Blank");
    if (!strcmp(key, "save_recs"))return snprintf(buf, buf_len, "Save");

    { float *f = float_param(p, key); if (f) return snprintf(buf, buf_len, "%.2f", *f); }

    if (strncmp(key, "knob_", 5) == 0 && strstr(key, "_name")) {
        int idx = atoi(key + 5) - 1;
        const knob_def_t *map = active_map(p);
        if (idx >= 0 && idx < 8 && map[idx].label) return snprintf(buf, buf_len, "%s", map[idx].label);
        return 0;
    }
    if (strncmp(key, "knob_", 5) == 0 && strstr(key, "_value")) {
        int idx = atoi(key + 5) - 1;
        const knob_def_t *map = active_map(p);
        if (idx < 0 || idx >= 8 || !map[idx].key) return 0;
        const char *k = map[idx].key;
        if (map[idx].is_enum) return get_param(instance, k, buf, buf_len);
        if (!strcmp(k, "varspeed")) {
            float r = powf(2.0f, (p->varspeed - 0.5f) * 4.0f);
            return snprintf(buf, buf_len, "%.2fx", r);
        }
        if (!strcmp(k, "rolloff")) {
            return snprintf(buf, buf_len, "%dHz", (int)(1000.0f + p->rolloff * 17000.0f));
        }
        if (!strcmp(k, "lowcut")) {
            return snprintf(buf, buf_len, "%dHz", (int)(20.0f + p->lowcut * 780.0f));
        }
        if (!strcmp(k, "input_pan")) {
            int v = (int)((p->input_pan - 0.5f) * 200.0f + (p->input_pan >= 0.5f ? 0.5f : -0.5f));
            if (v == 0) return snprintf(buf, buf_len, "C");
            return snprintf(buf, buf_len, "%c%d", v < 0 ? 'L' : 'R', v < 0 ? -v : v);
        }
        /* channel-strip EQ: gains already in dB, freqs in Hz, stop in ms */
        if (!strcmp(k, "trim") || !strcmp(k, "low") || !strcmp(k, "mid") || !strcmp(k, "high")) {
            float *f = float_param(p, k);
            return snprintf(buf, buf_len, "%+ddB", (int)(*f + (*f >= 0.0f ? 0.5f : -0.5f)));
        }
        if (!strcmp(k, "mid_freq"))   return snprintf(buf, buf_len, "%dHz", (int)p->mid_freq);
        if (!strcmp(k, "high_freq"))  return snprintf(buf, buf_len, "%dHz", (int)p->high_freq);
        if (!strcmp(k, "stop_speed")) return snprintf(buf, buf_len, "%dms", (int)p->stop_speed);
        float *f = float_param(p, k);
        if (f) return snprintf(buf, buf_len, "%d%%", (int)(*f * 100.0f + 0.5f));
        return 0;
    }

    if (!strcmp(key, "state")) {
        return snprintf(buf, buf_len,
            "varspeed=%.6f;speed=%d;side=%d;tone=%.6f;volume=%.6f;model=%d;mix=%.6f;feedback=%.6f;"
            "wow=%.6f;flutter=%.6f;saturation=%.6f;rolloff=%.6f;hiss=%.6f;"
            "lowcut=%.6f;generations=%.6f;input_pan=%.6f;pan_mode=%d;stutter=%.6f;failure=%.6f;"
            "trim=%.6f;low=%.6f;mid=%.6f;mid_freq=%.6f;high=%.6f;high_freq=%.6f;chan_vol=%.6f;"
            "jump=%.6f;scan=%.6f;stop_speed=%.6f;rec_length=%.6f;eq_in=%d;rec_mode=%d;"
            "sync_mode=%d;sync_div=%d;tempo=%d;id=%s",
            p->varspeed, p->speed_mode, p->side, p->tone, p->volume, p->model, p->mix, p->feedback,
            p->wow, p->flutter, p->saturation, p->rolloff, p->hiss,
            p->lowcut, p->generations, p->input_pan, p->pan_mode, p->stutter, p->failure,
            p->trim, p->low, p->mid, p->mid_freq, p->high, p->high_freq, p->chan_vol,
            p->jump, p->scan, p->stop_speed, p->rec_length, p->eq_in, p->rec_mode,
            p->sync_mode, p->sync_div, p->tempo_bpm, p->loop_id);
    }

    return -1;   /* unknown key — MUST be -1, not 0 */
}

/* ── API export ──────────────────────────────────────────────────────────────────── */
static audio_fx_api_v2_t g_api = {
    .api_version      = 2,
    .create_instance  = create_instance,
    .destroy_instance = destroy_instance,
    .process_block    = process_block,
    .set_param        = set_param,
    .get_param        = get_param,
    .on_midi          = move_audio_fx_on_midi,
};

__attribute__((visibility("default")))
audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t *host) {
    g_host = host;
    if (host && host->log) host->log("[magneto] loaded");
    return &g_api;
}
