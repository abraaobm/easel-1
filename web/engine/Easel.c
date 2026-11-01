/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Easel-1 Core DSP Engine - Com Serialização (Program Cards) */
#include <math.h>
#include <string.h>
#include <stdint.h>

#ifdef __wasm__
#define EXPORT(name) __attribute__((export_name(name)))
#else
#define EXPORT(name)
#endif

#define REV_BUF_LEN 16384

/* Vactrol (LED + photoresistor) low-pass gate model: a fast stage and a slow stage
 * that follow the drive, plus a memory of how long it has been lit. */
typedef struct { float fast, slow, mem; } vactrol_t;

typedef struct {
    float phase;
    float env;
    float hold_timer;
    float lpg_low;
    float freq;
    float velocity;
    float pitch_env;
    float dc_x, dc_y;
    vactrol_t vac;
    float strike, vactrol_gain, mod_phase, ctrl, last_mod;
    uint8_t note;
    uint8_t active;
} easel_voice_t;

static easel_voice_t voices[8];

static float p_engine_mode = 0.0f; 
static float p_shape = 0.0f;       
static float p_fold = 0.0f;
static float p_decay = 0.5f;
static float p_lpg = 1.0f;            /* open: pure sound until you close it */

static float p_rise = 0.0f;
static float p_fall = 0.0f;
static int p_loop = 0;
static float p_pulse_width = 0.0f;

static float p_euclid_rate = 0.0f;
static float p_euclid_pulses = 0.0f;

static float p_rand_rate = 0.0f;
static float p_rand_amt = 0.0f;
static float p_rand_target = 0.0f;
static float p_rand_smooth = 0.0f;

static float p_pitch_amt = 0.0f;
static float p_pitch_decay = 0.0f;
static float p_reverb_mix = 0.0f;
static float p_reverb_time = 0.0f;

static float p_seq_transpose = 0.5f;
static float p_seq_octave = 0.5f;
static float p_seq_timbre = 0.0f;
static float p_seq_lpg_offset = 1.0f;   /* open */
static float p_symmetry = 0.5f;   /* 0.5 = centred; moves the wave before the folder */
static int p_lpg_mode = 1;        /* 0 = LPG (filter + VCA), 1 = VCA only, 2 = filter only */
static float p_vactrol = 0.0f;    /* 0 = ideal envelope, 1 = full vactrol behaviour */

/* Modulation oscillator (sine). TRACK: ratio of each note's pitch; LFO: free running. */
static float p_mod_ratio = 0.25f; /* TRACK 0.5x..8x (0.25 = 1x), LFO 0.05..100 Hz */
static float p_mod_fm = 0.0f;     /* frequency modulation of the main oscillator */
static float p_mod_timbre = 0.0f; /* modulation of the wavefolder depth */
static int p_mod_lfo = 0;
static float mod_lfo_phase = 0.0f;
/* Patch matrix. Rows are sources (FUNC, RAND, ENV, MOD, VEL), columns are destinations
 * (PITCH, SHAPE, FOLD, SYMM, CUTOFF, FM); each cell is a signed amount, -100..+100 %.
 * Everything starts unpatched, so the instrument starts as a plain oscillator. */
enum { PATCH_SOURCES = 5, PATCH_DESTS = 6, PATCH_CELLS = 30 };
static int8_t patch_cell[PATCH_CELLS];
static float p_patch_depth = 1.0f;

static float clamp01(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }
static int preset_current = -1;   /* factory card last loaded, -1 = custom */
#define EASEL_PATCH_VERSION 2     /* patches saved with other defaults are ignored */

static float fg_phase = 0.0f;
static float fg_out = 0.0f;
static uint8_t fg_running = 0;

static float euclid_clock = 0.0f;
static int euclid_step = 0;
static int euclid_bucket = 0;

static float rand_phase = 0.0f;
static float rand_out = 0.0f;
static float rand_current = 0.0f;
static float rand_prev = 0.0f;
static uint32_t rand_seed = 12345;

static float rev_buf_L[REV_BUF_LEN];
static float rev_buf_R[REV_BUF_LEN];
static unsigned rev_idx = 0;

static float get_random() {
    rand_seed = (rand_seed * 1103515245 + 12345) & 0x7fffffff;
    return ((float)rand_seed / 1073741824.0f) - 1.0f;
}

// --- SERIALIZAÇÃO PARA O PROGRAM CARD ---
void easel_serialize(uint8_t *buf) {
    memset(buf, 0, 156);
    buf[0] = 'E'; buf[1] = 'A'; buf[2] = 'S'; buf[3] = 'L';
    buf[4] = (uint8_t)(p_shape * 255.0f);
    buf[5] = (uint8_t)(p_fold * 255.0f);
    buf[6] = (uint8_t)(p_decay * 255.0f);
    buf[7] = (uint8_t)(p_lpg * 255.0f);
    buf[8] = (uint8_t)(p_rise * 255.0f);
    buf[9] = (uint8_t)(p_fall * 255.0f);
    buf[10] = (uint8_t)(p_loop * 127.0f);
    buf[11] = (uint8_t)(p_pulse_width * 255.0f);
    buf[12] = (uint8_t)(p_rand_rate * 255.0f);
    buf[13] = (uint8_t)(p_rand_amt * 255.0f);
    buf[14] = (uint8_t)(p_rand_target * 255.0f);
    buf[15] = (uint8_t)(p_rand_smooth * 255.0f);
    buf[16] = (uint8_t)(p_pitch_amt * 255.0f);
    buf[17] = (uint8_t)(p_pitch_decay * 255.0f);
    buf[18] = (uint8_t)(p_reverb_mix * 255.0f);
    buf[19] = (uint8_t)(p_reverb_time * 255.0f);
    buf[20] = (uint8_t)(p_euclid_rate * 255.0f);
    buf[21] = (uint8_t)(p_euclid_pulses * 255.0f);
    buf[22] = (uint8_t)(p_seq_transpose * 255.0f);
    buf[23] = (uint8_t)(p_seq_octave * 255.0f);
    buf[24] = (uint8_t)(p_seq_timbre * 255.0f);
    buf[25] = (uint8_t)(p_seq_lpg_offset * 255.0f);
    buf[26] = (uint8_t)(p_engine_mode * 255.0f);
    {
        int sym = (int)((p_symmetry - 0.5f) * 254.0f);
        if (sym > 127) sym = 127;
        if (sym < -127) sym = -127;
        buf[27] = (uint8_t)(int8_t)sym;
    }
    buf[28] = (uint8_t)(p_lpg_mode * 127.0f);
    buf[29] = (uint8_t)(p_vactrol * 255.0f);
    buf[30] = (uint8_t)(p_mod_ratio * 255.0f);
    buf[31] = (uint8_t)(p_mod_fm * 255.0f);
    buf[32] = (uint8_t)(p_mod_timbre * 255.0f);
    buf[33] = p_mod_lfo ? 255 : 0;
    buf[34] = EASEL_PATCH_VERSION;
    for (int c = 0; c < PATCH_CELLS; c++) buf[35 + c] = (uint8_t)patch_cell[c];
    buf[65] = (uint8_t)((1.0f - p_patch_depth) * 255.0f);   /* inverted: 0 in older patches = full depth */
}

void easel_deserialize(const uint8_t *buf) {
    if (buf[0] != 'E' || buf[1] != 'A' || buf[2] != 'S' || buf[3] != 'L') return;
    if (buf[34] != EASEL_PATCH_VERSION) return;
    p_shape = buf[4] / 255.0f;
    p_fold = buf[5] / 255.0f;
    p_decay = buf[6] / 255.0f;
    p_lpg = buf[7] / 255.0f;
    p_rise = buf[8] / 255.0f;
    p_fall = buf[9] / 255.0f;
    p_loop = (int)((buf[10] + 63U) / 127U);
    if (p_loop > 2) p_loop = 2;
    p_pulse_width = buf[11] / 255.0f;
    p_rand_rate = buf[12] / 255.0f;
    p_rand_amt = buf[13] / 255.0f;
    p_rand_target = buf[14] / 255.0f;
    p_rand_smooth = buf[15] / 255.0f;
    p_pitch_amt = buf[16] / 255.0f;
    p_pitch_decay = buf[17] / 255.0f;
    p_reverb_mix = buf[18] / 255.0f;
    p_reverb_time = buf[19] / 255.0f;
    p_euclid_rate = buf[20] / 255.0f;
    p_euclid_pulses = buf[21] / 255.0f;
    p_seq_transpose = buf[22] / 255.0f;
    p_seq_octave = buf[23] / 255.0f;
    p_seq_timbre = buf[24] / 255.0f;
    p_seq_lpg_offset = buf[25] / 255.0f;
    p_engine_mode = buf[26] / 255.0f;
    p_symmetry = 0.5f + (float)(int8_t)buf[27] / 254.0f;
    p_lpg_mode = (int)((buf[28] + 63U) / 127U);
    if (p_lpg_mode > 2) p_lpg_mode = 2;
    p_vactrol = buf[29] / 255.0f;
    p_mod_ratio = buf[30] / 255.0f;
    p_mod_fm = buf[31] / 255.0f;
    p_mod_timbre = buf[32] / 255.0f;
    p_mod_lfo = buf[33] > 127;
    for (int c = 0; c < PATCH_CELLS; c++) {
        int cell = (int8_t)buf[35 + c];
        patch_cell[c] = (int8_t)(cell > 100 ? 100 : cell < -100 ? -100 : cell);
    }
    p_patch_depth = 1.0f - buf[65] / 255.0f;
    preset_current = -1;
}

EXPORT("sim_easel_get_state") const uint8_t *sim_easel_get_state(void) {
    static uint8_t buf[156];
    easel_serialize(buf);
    return buf;
}

// --- MOTORES DE SÍNTESE ---
static float synth_core(float phase, float shape, float engine) {
    float sine = sinf(phase * 6.2831853f);
    int mode = (int)(engine * 3.1f);

    if (mode == 0) {
        float tri = 2.0f * fabsf(2.0f * (phase - floorf(phase + 0.5f))) - 1.0f;
        float saw = 2.0f * phase - 1.0f;
        float square = (phase < 0.5f) ? 1.0f : -1.0f;
        if (shape < 0.33f) { float t = shape * 3.0f; return sine * (1.0f - t) + tri * t; }
        else if (shape < 0.66f) { float t = (shape - 0.33f) * 3.0f; return tri * (1.0f - t) + saw * t; }
        else { float t = (shape - 0.66f) * 3.0f; return saw * (1.0f - t) + square * t; }
    } else if (mode == 1) {
        float mod = sinf(phase * 6.2831853f * 2.0f);
        float index = shape * 15.0f;
        return sinf(phase * 6.2831853f + mod * index);
    } else if (mode == 2) {
        float modulator = sinf(phase * 6.2831853f * 1.414f);
        return sine * (1.0f - shape) + (sine * modulator) * shape;
    } else {
        float dist = shape * 0.9f;
        float ph = phase;
        if (ph < 0.5f) ph = ph * (1.0f + dist);
        else ph = 0.5f + (ph - 0.5f) * (1.0f - dist) + dist * 0.5f;
        if (ph > 1.0f) ph -= 1.0f;
        return sinf(ph * 6.2831853f);
    }
}

/* One wavefolder shared by the DSP and the LCD scope. */
static float wavefold(float u) {
    if (u > 1.0f || u < -1.0f) {
        u = fmodf(u + 1.0f, 4.0f);
        if (u < 0) u += 4.0f;
        u = (u > 2.0f) ? 4.0f - u : u;
        u -= 1.0f;
    }
    return u;
}

static float mod_track_ratio(void) { return 0.5f * exp2f(4.0f * p_mod_ratio); }
static float mod_lfo_hz(void) { return 0.05f * exp2f(11.0f * p_mod_ratio); }
/* FM depth in octaves of pitch deviation; squared so the low end is easy to set. */
static float mod_fm_octaves(void) { return p_mod_fm * p_mod_fm * 3.0f; }
/* Value shown on the LCD: the ratio in TRACK mode, hertz in LFO mode. */
float easel_ui_mod_rate(void) { return p_mod_lfo ? mod_lfo_hz() : mod_track_ratio(); }

/* One step of the vactrol. The fast stage gives the quick "ping", the slow stage the
 * tail; the tail lasts longer the longer the LED has been lit (program dependence). */
static float vactrol_step(vactrol_t *s, float drive, float dt, float decay) {
    float tau_fast = drive > s->fast ? 0.0015f : 0.025f;
    float tau_slow = drive > s->slow ? 0.005f : (0.08f + decay * 2.0f) * (0.6f + s->mem);
    float tau_mem = drive > s->mem ? 0.35f : 0.6f;
    s->fast += (1.0f - expf(-dt / tau_fast)) * (drive - s->fast);
    s->slow += (1.0f - expf(-dt / tau_slow)) * (drive - s->slow);
    s->mem += (1.0f - expf(-dt / tau_mem)) * (drive - s->mem);
    float g = 0.5f * s->fast + 0.5f * s->slow;
    return g * (0.5f + 0.5f * g);
}

/* ---- Read-only access for the LCD panel (app/screen.c) ---- */
float easel_get_param(int param) {
    switch (param) {
    case 0: return p_shape;
    case 1: return p_fold;
    case 2: return p_decay;
    case 3: return p_lpg;
    case 4: return p_rise;
    case 5: return p_fall;
    case 6: return (float)p_loop * 0.5f;
    case 7: return p_pulse_width;
    case 8: return p_rand_rate;
    case 9: return p_rand_amt;
    case 10: return p_rand_target;
    case 11: return p_rand_smooth;
    case 12: return p_pitch_amt;
    case 13: return p_pitch_decay;
    case 14: return p_reverb_mix;
    case 15: return p_reverb_time;
    case 16: return p_euclid_rate;
    case 17: return p_euclid_pulses;
    case 18: return p_seq_transpose;
    case 19: return p_seq_octave;
    case 20: return p_seq_timbre;
    case 21: return p_seq_lpg_offset;
    case 22: return p_engine_mode;
    case 23: return p_symmetry;
    case 24: return (float)p_lpg_mode * 0.5f;
    case 25: return p_vactrol;
    case 26: return p_mod_ratio;
    case 27: return p_mod_fm;
    case 28: return p_mod_timbre;
    case 29: return p_mod_lfo ? 1.0f : 0.0f;
    case 30: return p_patch_depth;
    default:
        if (param >= 32 && param < 32 + PATCH_CELLS) return 0.5f + (float)patch_cell[param - 32] / 200.0f;
        return 0.0f;
    }
}

/* viz: 0 function phase (0..2), 1 function out, 2 function running,
 * 3 random S&H value, 4 random out, 5 loudest voice envelope,
 * 6 euclid step, 7 active voices. */
void easel_ui_snapshot(float viz[8]) {
    float peak = 0.0f;
    int active = 0;
    for (int i = 0; i < 8; i++) {
        if (!voices[i].active) continue;
        active++;
        if (voices[i].env > peak) peak = voices[i].env;
    }
    viz[0] = fg_phase;
    viz[1] = fg_out;
    viz[2] = fg_running ? 1.0f : 0.0f;
    viz[3] = rand_current;
    viz[4] = rand_out;
    viz[5] = peak;
    viz[6] = (float)euclid_step;
    viz[7] = (float)active;
}

/* Static preview of the voice: oscillator + wavefolder with the current knobs. */
void easel_ui_scope(int8_t *out, unsigned count, float cycles) {
    float depth = (1.0f + p_fold * 4.0f) * (1.0f + p_seq_timbre * 1.5f);
    float symmetry = (p_symmetry - 0.5f) * 2.0f, wave[256], mean = 0.0f;
    if (count > 256) count = 256;
    for (unsigned i = 0; i < count; i++) {
        float phase = fmodf(cycles * (float)i / (float)count, 1.0f);
        wave[i] = wavefold((synth_core(phase, p_shape, p_engine_mode) + symmetry) * depth);
        mean += wave[i];
    }
    mean /= (float)(count ? count : 1);
    float peak = 1.0f;
    for (unsigned i = 0; i < count; i++) {
        float a = fabsf(wave[i] - mean);
        if (a > peak) peak = a;
    }
    for (unsigned i = 0; i < count; i++) out[i] = (int8_t)((wave[i] - mean) / peak * 127.0f);
}

/* Preview of the modulator (mod) and of the main oscillator it modulates (out). */
void easel_ui_modosc(int8_t *mod_out, int8_t *out, unsigned count) {
    float carrier_cycles = p_mod_lfo ? 16.0f : 3.0f;
    float mod_cycles = p_mod_lfo ? mod_lfo_hz() : 3.0f * mod_track_ratio();
    float base = (1.0f + p_fold * 4.0f) * (1.0f + p_seq_timbre * 1.5f);
    float symmetry = (p_symmetry - 0.5f) * 2.0f, wave[256], mean = 0.0f, phase = 0.0f;
    if (count > 256) count = 256;
    for (unsigned i = 0; i < count; i++) {
        float t = (float)i / (float)count;
        float mod = sinf(6.2831853f * mod_cycles * t);
        phase += carrier_cycles / (float)count * exp2f(mod * mod_fm_octaves());
        float depth = base * (1.0f + p_mod_timbre * 0.8f * mod);
        wave[i] = wavefold((synth_core(phase - floorf(phase), p_shape, p_engine_mode) + symmetry) * depth);
        mean += wave[i];
        mod_out[i] = (int8_t)(mod * 127.0f);
    }
    mean /= (float)(count ? count : 1);
    float peak = 1.0f;
    for (unsigned i = 0; i < count; i++) {
        float a = fabsf(wave[i] - mean);
        if (a > peak) peak = a;
    }
    for (unsigned i = 0; i < count; i++) out[i] = (int8_t)((wave[i] - mean) / peak * 127.0f);
}

/* Strike at t = 0.08 s and follow the same model the voices use, for 1.6 s. */
void easel_ui_lpg(uint8_t *amp, uint8_t *bright, uint8_t *ideal, unsigned count) {
    const float dt = 0.0005f, strike_at = 0.08f, window = 1.6f;
    float hold = p_pulse_width * 1.5f, decay_time = 0.1f + p_decay * 2.0f, env = 0.0f;
    vactrol_t v = {0.0f, 0.0f, 0.0f};
    float t = 0.0f, control = 0.0f;
    for (unsigned n = 0; n < count; n++) {
        float until = window * (float)n / (float)count;
        for (; t < until; t += dt) {
            int lit = t >= strike_at && (t < strike_at + 0.012f || t < strike_at + hold);
            if (t < strike_at) env = 0.0f;
            else if (t < strike_at + hold) env = 1.0f;
            else if (env == 0.0f && t < strike_at + 0.001f) env = 1.0f;
            else env *= expf(-dt / decay_time);
            float g = vactrol_step(&v, lit ? 1.0f : 0.0f, dt, p_decay);
            control = env + p_vactrol * (g - env);
        }
        float a = p_lpg_mode == 2 ? sqrtf(control) : control;
        float b = p_lpg_mode == 1 ? 1.0f : 0.05f + 0.95f * control;
        amp[n] = (uint8_t)(a > 1.0f ? 255.0f : a * 255.0f);
        bright[n] = (uint8_t)(b > 1.0f ? 255.0f : b * 255.0f);
        ideal[n] = (uint8_t)(env > 1.0f ? 255.0f : env * 255.0f);
    }
}

void easel_note_on(uint8_t note, uint8_t vel) {
    if (!vel) return;
    
    int transposed = note;
    transposed += (int)((p_seq_transpose - 0.5f) * 24.0f);
    transposed += (int)((p_seq_octave - 0.5f) * 4.0f) * 12;
    
    if (transposed < 0) transposed = 0;
    if (transposed > 127) transposed = 127;
    float base_freq = 440.0f * powf(2.0f, (transposed - 69) / 12.0f);

    for (int i = 0; i < 8; i++) {
        if (voices[i].active && voices[i].note == transposed) {
            voices[i].env = 1.0f;
            voices[i].strike = 0.012f;
            voices[i].mod_phase = 0.0f;
            voices[i].phase = 0.0f;
            voices[i].pitch_env = 1.0f;
            voices[i].velocity = vel / 127.0f;
            voices[i].freq = base_freq;
            voices[i].hold_timer = p_pulse_width * 1.5f;
            fg_phase = 0.0f;
            fg_running = 1;
            if (p_loop == 2) { euclid_clock = 0.0f; euclid_step = 0; euclid_bucket = 0; }
            return;
        }
    }
    for (int i = 0; i < 8; i++) {
        if (!voices[i].active) {
            voices[i].active = 1;
            voices[i].note = transposed;
            voices[i].freq = base_freq;
            voices[i].phase = 0.0f;
            voices[i].mod_phase = 0.0f;
            voices[i].env = 1.0f;
            voices[i].pitch_env = 1.0f;
            voices[i].velocity = vel / 127.0f;
            voices[i].lpg_low = 0.0f;
            voices[i].dc_x = 0.0f; voices[i].dc_y = 0.0f;
            memset(&voices[i].vac, 0, sizeof voices[i].vac);
            voices[i].strike = 0.012f; voices[i].vactrol_gain = 0.0f;
            voices[i].ctrl = 0.0f; voices[i].last_mod = 0.0f;
            voices[i].hold_timer = p_pulse_width * 1.5f;
            fg_phase = 0.0f;
            fg_out = 0.0f;
            fg_running = 1;
            if (p_loop == 2) { euclid_clock = 0.0f; euclid_step = 0; euclid_bucket = 0; }
            return;
        }
    }
    int steal = 0;
    float min_env = voices[0].env;
    for (int i = 1; i < 8; i++) {
        if (voices[i].env < min_env) { min_env = voices[i].env; steal = i; }
    }
    voices[steal].active = 1;
    voices[steal].note = transposed;
    voices[steal].freq = base_freq;
    voices[steal].phase = 0.0f;
    voices[steal].mod_phase = 0.0f;
    voices[steal].env = 1.0f;
    voices[steal].pitch_env = 1.0f;
    voices[steal].velocity = vel / 127.0f;
    voices[steal].lpg_low = 0.0f;
    voices[steal].dc_x = 0.0f; voices[steal].dc_y = 0.0f;
    memset(&voices[steal].vac, 0, sizeof voices[steal].vac);
    voices[steal].strike = 0.012f; voices[steal].vactrol_gain = 0.0f;
    voices[steal].ctrl = 0.0f; voices[steal].last_mod = 0.0f;
    voices[steal].hold_timer = p_pulse_width * 1.5f;
    fg_phase = 0.0f;
    fg_out = 0.0f;
    fg_running = 1;
    if (p_loop == 2) { euclid_clock = 0.0f; euclid_step = 0; euclid_bucket = 0; }
}

void easel_set_param(int param, float val) {
    if (param == 0) p_shape = val;
    if (param == 1) p_fold = val;
    if (param == 2) p_decay = val;
    if (param == 3) p_lpg = val;
    if (param == 4) p_rise = val;
    if (param == 5) p_fall = val;
    if (param == 6) p_loop = (int)(val * 2.9f);
    if (param == 7) p_pulse_width = val; 
    if (param == 8) p_rand_rate = val;
    if (param == 9) p_rand_amt = val;
    if (param == 10) p_rand_target = val;
    if (param == 11) p_rand_smooth = val;
    if (param == 12) p_pitch_amt = val;
    if (param == 13) p_pitch_decay = val;
    if (param == 14) p_reverb_mix = val;
    if (param == 15) p_reverb_time = val;
    if (param == 16) p_euclid_rate = val;
    if (param == 17) p_euclid_pulses = val;
    if (param == 18) p_seq_transpose = val;
    if (param == 19) p_seq_octave = val;
    if (param == 20) p_seq_timbre = val;
    if (param == 21) p_seq_lpg_offset = val;
    if (param == 22) p_engine_mode = val; 
    if (param == 23) p_symmetry = val;
    if (param == 24) p_lpg_mode = (int)(val * 2.9f);
    if (param == 25) p_vactrol = val;
    if (param == 26) p_mod_ratio = val;
    if (param == 27) p_mod_fm = val;
    if (param == 28) p_mod_timbre = val;
    if (param == 29) p_mod_lfo = val > 0.5f;
    if (param == 30) p_patch_depth = val;
    if (param >= 32 && param < 32 + PATCH_CELLS) {
        int cell = (int)lroundf((val - 0.5f) * 200.0f);
        patch_cell[param - 32] = (int8_t)(cell > 100 ? 100 : cell < -100 ? -100 : cell);
    }
}

/* ---- Factory cards -------------------------------------------------------
 * A card is a short list of settings applied on top of the pure defaults, so
 * anything it does not mention stays neutral. Cables use the PATCH page order. */
enum { P_SHAPE, P_FOLD, P_DECAY, P_CUTOFF, P_RISE, P_FALL, P_LOOP, P_PULSE, P_RRATE, P_RAMT, P_RTARGET,
       P_RSMOOTH, P_PAMT, P_PDEC, P_REVMIX, P_REVTIME, P_EURATE, P_EUPULSE, P_TRANSP, P_OCT, P_GFOLD,
       P_GLPG, P_ENGINE, P_SYMM, P_LPGMODE, P_VACTROL, P_MRATIO, P_MFM, P_MTIMBRE, P_MMODE, P_DEPTH };
enum { S_FUNC, S_RAND, S_ENV, S_MOD, S_VEL };
enum { D_PITCH, D_SHAPE, D_FOLD, D_SYMM, D_CUT, D_FM };
#define CABLE(src, dst, amount) {(uint8_t)(32 + (src) * 6 + (dst)), 0.5f + (float)(amount) / 200.0f}
#define CARD_END {255, 0.0f}
typedef struct { uint8_t param; float value; } card_set_t;

static const card_set_t card_init[] = {CARD_END};
/* Falling pitch plus a vactrol ping: a hand drum. */
static const card_set_t card_bongo[] = {
    {P_OCT, 0.25f}, {P_FOLD, 0.12f}, {P_DECAY, 0.15f}, {P_PAMT, 0.8f}, {P_PDEC, 0.1f},
    {P_LPGMODE, 0.0f}, {P_VACTROL, 1.0f}, {P_REVMIX, 0.1f}, {P_REVTIME, 0.3f}, CARD_END};
/* The envelope pulls the pitch down, so each drop rises as it fades. */
static const card_set_t card_water[] = {
    {P_OCT, 0.75f}, {P_DECAY, 0.2f}, {P_LPGMODE, 0.0f}, {P_VACTROL, 0.9f}, {P_REVMIX, 0.35f},
    {P_REVTIME, 0.6f}, {P_RRATE, 0.15f}, {P_RSMOOTH, 1.0f},
    CABLE(S_ENV, D_PITCH, -35), CABLE(S_RAND, D_PITCH, 15), CARD_END};
/* Inharmonic FM whose index dies away with the gate. */
static const card_set_t card_bells[] = {
    {P_DECAY, 0.45f}, {P_LPGMODE, 0.0f}, {P_VACTROL, 0.85f}, {P_MRATIO, 0.702f}, {P_MFM, 0.18f},
    {P_REVMIX, 0.3f}, {P_REVTIME, 0.7f}, CABLE(S_ENV, D_FM, 85), CARD_END};
/* Folded, off-centre wave with a short timbre sweep on every note. */
static const card_set_t card_lead[] = {
    {P_SHAPE, 0.5f}, {P_FOLD, 0.3f}, {P_SYMM, 0.65f}, {P_DECAY, 0.5f}, {P_PULSE, 0.25f},
    {P_RISE, 0.04f}, {P_FALL, 0.45f}, {P_VACTROL, 0.4f}, {P_LPGMODE, 0.0f}, {P_MRATIO, 0.5f},
    {P_MFM, 0.1f}, {P_REVMIX, 0.2f}, {P_REVTIME, 0.5f}, CABLE(S_FUNC, D_FOLD, 50), CARD_END};
/* Stepped random pitch: every note lands somewhere new. */
static const card_set_t card_chimes[] = {
    {P_SHAPE, 0.1f}, {P_OCT, 0.75f}, {P_DECAY, 0.6f}, {P_LPGMODE, 0.0f}, {P_VACTROL, 1.0f},
    {P_RRATE, 0.3f}, {P_RSMOOTH, 1.0f}, {P_MRATIO, 0.845f}, {P_MFM, 0.12f}, {P_REVMIX, 0.45f},
    {P_REVTIME, 0.85f}, CABLE(S_RAND, D_PITCH, 35), CABLE(S_ENV, D_FM, 40), CARD_END};
static const card_set_t card_thump[] = {
    {P_OCT, 0.0f}, {P_FOLD, 0.08f}, {P_DECAY, 0.3f}, {P_PAMT, 0.9f}, {P_PDEC, 0.25f},
    {P_LPGMODE, 0.5f}, {P_VACTROL, 0.5f}, CARD_END};
/* A held note whose filter and fold breathe with a slow LFO. */
static const card_set_t card_wobble[] = {
    {P_SHAPE, 0.3f}, {P_FOLD, 0.15f}, {P_DECAY, 0.8f}, {P_PULSE, 1.0f}, {P_CUTOFF, 0.2f}, {P_GLPG, 0.2f},
    {P_LPGMODE, 0.5f}, {P_MMODE, 1.0f}, {P_MRATIO, 0.273f}, {P_REVMIX, 0.4f}, {P_REVTIME, 0.8f},
    CABLE(S_MOD, D_CUT, 55), CABLE(S_MOD, D_FOLD, 35), CARD_END};
/* Euclidean pulses ripple through a held note. */
static const card_set_t card_euclid[] = {
    {P_SHAPE, 0.2f}, {P_FOLD, 0.1f}, {P_LOOP, 1.0f}, {P_EURATE, 0.5f}, {P_EUPULSE, 0.3f}, {P_PULSE, 1.0f},
    {P_DECAY, 0.4f}, {P_REVMIX, 0.3f}, {P_REVTIME, 0.5f},
    CABLE(S_FUNC, D_PITCH, 25), CABLE(S_FUNC, D_FOLD, 60), CARD_END};
static const card_set_t card_ring[] = {
    {P_ENGINE, 0.6667f}, {P_SHAPE, 0.55f}, {P_FOLD, 0.15f}, {P_SYMM, 0.55f}, {P_DECAY, 0.35f},
    {P_LPGMODE, 0.0f}, {P_VACTROL, 0.8f}, {P_MRATIO, 0.3745f}, {P_MFM, 0.25f}, {P_MTIMBRE, 0.2f},
    {P_REVMIX, 0.25f}, {P_REVTIME, 0.6f}, CARD_END};
/* Velocity opens both the fold and the filter. */
static const card_set_t card_velocity[] = {
    {P_SHAPE, 0.4f}, {P_FOLD, 0.05f}, {P_DECAY, 0.45f}, {P_CUTOFF, 0.3f}, {P_GLPG, 0.3f},
    {P_LPGMODE, 0.0f}, {P_VACTROL, 0.5f}, {P_REVMIX, 0.25f}, {P_REVTIME, 0.5f},
    CABLE(S_VEL, D_FOLD, 80), CABLE(S_VEL, D_CUT, 60), CARD_END};
/* Low FM drone that wanders on slow random. */
static const card_set_t card_drone[] = {
    {P_ENGINE, 0.3333f}, {P_SHAPE, 0.12f}, {P_OCT, 0.25f}, {P_PULSE, 1.0f}, {P_DECAY, 1.0f},
    {P_CUTOFF, 0.5f}, {P_GLPG, 0.5f}, {P_LPGMODE, 0.5f}, {P_RRATE, 0.03f}, {P_RSMOOTH, 0.0f},
    {P_MMODE, 1.0f}, {P_MRATIO, 0.18f}, {P_MFM, 0.1f}, {P_REVMIX, 0.5f}, {P_REVTIME, 0.95f},
    CABLE(S_RAND, D_SHAPE, 45), CABLE(S_RAND, D_SYMM, 35), CARD_END};
static const card_set_t card_phase[] = {
    {P_ENGINE, 1.0f}, {P_SHAPE, 0.25f}, {P_FOLD, 0.05f}, {P_DECAY, 0.3f}, {P_PAMT, 0.2f}, {P_PDEC, 0.05f},
    {P_LPGMODE, 0.0f}, {P_VACTROL, 0.9f}, {P_REVMIX, 0.2f}, {P_REVTIME, 0.4f}, CABLE(S_ENV, D_SHAPE, 70), CARD_END};
/* Fast stepped random drives FM index, pitch and fold. */
static const card_set_t card_chaos[] = {
    {P_DECAY, 0.35f}, {P_PULSE, 0.3f}, {P_LPGMODE, 0.0f}, {P_VACTROL, 0.6f}, {P_RRATE, 0.4f},
    {P_RSMOOTH, 1.0f}, {P_MRATIO, 0.5f}, {P_MFM, 0.1f}, {P_REVMIX, 0.3f}, {P_REVTIME, 0.6f},
    CABLE(S_RAND, D_FM, 70), CABLE(S_RAND, D_PITCH, 12), CABLE(S_RAND, D_FOLD, 40), CARD_END};

static const struct { const char *name; const card_set_t *set; } cards[] = {
    {"INIT", card_init}, {"BONGO BLOOP", card_bongo}, {"WATER DROPS", card_water}, {"FM BELLS", card_bells},
    {"BUCHLA LEAD", card_lead}, {"WIND CHIMES", card_chimes}, {"SUB THUMP", card_thump},
    {"SLOW WOBBLE", card_wobble}, {"EUCLID BLIPS", card_euclid}, {"RING METAL", card_ring},
    {"VEL GLASS", card_velocity}, {"DRONE CAVE", card_drone}, {"PHASE PLUCK", card_phase},
    {"CHAOS SPARKS", card_chaos}};
#define CARD_COUNT ((unsigned)(sizeof cards / sizeof cards[0]))

/* Every parameter back to its power-on value; keep in step with the initialisers. */
static void reset_defaults(void) {
    p_engine_mode = 0.0f; p_shape = 0.0f; p_fold = 0.0f; p_decay = 0.5f; p_lpg = 1.0f;
    p_rise = 0.0f; p_fall = 0.0f; p_loop = 0; p_pulse_width = 0.0f;
    p_euclid_rate = 0.0f; p_euclid_pulses = 0.0f;
    p_rand_rate = 0.0f; p_rand_amt = 0.0f; p_rand_target = 0.0f; p_rand_smooth = 0.0f;
    p_pitch_amt = 0.0f; p_pitch_decay = 0.0f; p_reverb_mix = 0.0f; p_reverb_time = 0.0f;
    p_seq_transpose = 0.5f; p_seq_octave = 0.5f; p_seq_timbre = 0.0f; p_seq_lpg_offset = 1.0f;
    p_symmetry = 0.5f; p_lpg_mode = 1; p_vactrol = 0.0f;
    p_mod_ratio = 0.25f; p_mod_fm = 0.0f; p_mod_timbre = 0.0f; p_mod_lfo = 0;
    memset(patch_cell, 0, sizeof patch_cell);
    p_patch_depth = 1.0f;
}

static uint8_t card_snapshot[66];
unsigned easel_preset_count(void) { return CARD_COUNT; }
const char *easel_preset_name(unsigned index) { return index < CARD_COUNT ? cards[index].name : 0; }
int easel_preset_current(void) { return preset_current; }
void easel_preset_load(unsigned index) {
    if (index >= CARD_COUNT) return;
    reset_defaults();
    for (const card_set_t *set = cards[index].set; set->param != 255; set++) easel_set_param(set->param, set->value);
    uint8_t buf[156];
    easel_serialize(buf);
    memcpy(card_snapshot, buf, sizeof card_snapshot);
    preset_current = (int)index;
}
/* 1 once the loaded card has been edited. */
int easel_preset_dirty(void) {
    uint8_t buf[156];
    if (preset_current < 0) return 0;
    easel_serialize(buf);
    return memcmp(buf, card_snapshot, sizeof card_snapshot) != 0;
}

void easel_mix(int32_t *out, unsigned frames) {
    static float sample_rate = 44100.0f;
    memset(out, 0, frames * 2 * sizeof(int32_t));

    /* Only the patched cells are visited per sample. */
    struct { float gain; uint8_t src, dst; } links[PATCH_CELLS];
    unsigned n_links = 0;
    int patch_mod = 0;
    for (unsigned c = 0; c < PATCH_CELLS; c++) {
        if (!patch_cell[c]) continue;
        links[n_links].src = (uint8_t)(c / PATCH_DESTS);
        links[n_links].dst = (uint8_t)(c % PATCH_DESTS);
        links[n_links].gain = (float)patch_cell[c] * 0.01f * p_patch_depth;
        if (links[n_links].src == 3 || links[n_links].dst == 5) patch_mod = 1;
        n_links++;
    }

    for (unsigned i = 0; i < frames; i++) {
        if (p_loop == 2) {
            float rate_hz = 0.5f + p_euclid_rate * 9.5f;
            euclid_clock += rate_hz / sample_rate;
            if (euclid_clock >= 1.0f) {
                euclid_clock -= 1.0f;
                euclid_step = (euclid_step + 1) % 16;
                int pulses = 1 + (int)(p_euclid_pulses * 15.0f);
                euclid_bucket += pulses;
                if (euclid_bucket >= 16) {
                    euclid_bucket -= 16;
                    fg_phase = 0.0f;
                    fg_running = 1;
                }
            }
            if (fg_running) {
                float rate = (fg_phase < 1.0f) ? (1.0f / (0.005f * sample_rate)) : (1.0f / (0.05f * sample_rate));
                fg_phase += rate;
                if (fg_phase < 1.0f) fg_out = fg_phase;
                else if (fg_phase < 2.0f) fg_out = 2.0f - fg_phase;
                else { fg_phase = 2.0f; fg_out = 0.0f; fg_running = 0; }
            }
        } else if (p_loop == 1) {
            if (fg_running) {
                float rise_time = 0.01f + p_rise * 2.0f;
                float fall_time = 0.01f + p_fall * 2.0f;
                float rate = (fg_phase < 1.0f) ? (1.0f / (rise_time * sample_rate)) : (1.0f / (fall_time * sample_rate));
                fg_phase += rate;
                if (fg_phase < 1.0f) fg_out = fg_phase;
                else if (fg_phase < 2.0f) fg_out = 2.0f - fg_phase;
                else { fg_phase -= 2.0f; }
            }
        } else {
            if (fg_running) {
                float rise_time = 0.01f + p_rise * 2.0f;
                float fall_time = 0.01f + p_fall * 2.0f;
                float rate = (fg_phase < 1.0f) ? (1.0f / (rise_time * sample_rate)) : (1.0f / (fall_time * sample_rate));
                fg_phase += rate;
                if (fg_phase < 1.0f) fg_out = fg_phase;
                else if (fg_phase < 2.0f) fg_out = 2.0f - fg_phase;
                else { fg_phase = 2.0f; fg_out = 0.0f; fg_running = 0; }
            }
        }

        float rand_rate_hz = 0.1f + p_rand_rate * 19.9f;
        rand_phase += rand_rate_hz / sample_rate;
        if (rand_phase >= 1.0f) {
            rand_phase -= 1.0f;
            rand_prev = rand_current;
            rand_current = get_random();
        }
        float smooth_interp = rand_prev + (rand_current - rand_prev) * rand_phase;
        float target_out = smooth_interp * (1.0f - p_rand_smooth) + rand_current * p_rand_smooth;
        rand_out = target_out * p_rand_amt;

        float mixed = 0.0f;
        int active_count = 0;

        float seq_global_fold = 1.0f + (p_seq_timbre * 1.5f);
        float seq_global_lpg = 100.0f + (p_seq_lpg_offset * 5000.0f);

        int mod_on = p_mod_fm > 0.0f || p_mod_timbre > 0.0f || patch_mod;
        if (mod_on && p_mod_lfo) {
            mod_lfo_phase += mod_lfo_hz() / sample_rate;
            if (mod_lfo_phase >= 1.0f) mod_lfo_phase -= floorf(mod_lfo_phase);
        }

        for (int v = 0; v < 8; v++) {
            if (!voices[v].active) continue;
            active_count++;
            
            /* Patch matrix: sum every patched source into its destination. */
            float pd[PATCH_DESTS] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
            if (n_links) {
                float src[PATCH_SOURCES] = {fg_out, target_out, voices[v].ctrl, voices[v].last_mod, voices[v].velocity};
                for (unsigned l = 0; l < n_links; l++) pd[links[l].dst] += src[links[l].src] * links[l].gain;
            }

            float rand_fold = rand_out * 1.0f * (1.0f - p_rand_target);
            float rand_pitch = powf(2.0f, (rand_out * 0.2f * p_rand_target));

            float fold_mod = 1.0f + rand_fold;
            float pitch_mod = rand_pitch * exp2f(pd[0] * 2.0f);

            float p_decay_time = 0.01f + p_pitch_decay * 0.5f;
            float p_decay_rate = expf(-1.0f / (p_decay_time * sample_rate));
            voices[v].pitch_env *= p_decay_rate;
            pitch_mod *= powf(2.0f, (voices[v].pitch_env * p_pitch_amt * 1.5f));

            float mod = 0.0f;
            if (mod_on) {
                if (p_mod_lfo) {
                    mod = sinf(6.2831853f * mod_lfo_phase);
                } else {
                    mod = sinf(6.2831853f * voices[v].mod_phase);
                    voices[v].mod_phase += voices[v].freq * pitch_mod * mod_track_ratio() / sample_rate;
                    if (voices[v].mod_phase >= 1.0f) voices[v].mod_phase -= floorf(voices[v].mod_phase);
                }
            }
            voices[v].last_mod = mod;
            float fm_amount = clamp01(p_mod_fm + pd[5] * 0.5f);
            voices[v].phase += (voices[v].freq * pitch_mod * exp2f(mod * fm_amount * fm_amount * 3.0f)) / sample_rate;
            if (voices[v].phase >= 1.0f) voices[v].phase -= 1.0f;
            
            float raw = synth_core(voices[v].phase, clamp01(p_shape + pd[1] * 0.5f), p_engine_mode);
            
            float fold_depth = (1.0f + clamp01(p_fold + pd[2] * 0.5f) * 4.0f) * fold_mod * seq_global_fold * (1.0f + p_mod_timbre * 0.8f * mod);
            /* Symmetry moves the wave before the folder, so the folds land on one side
             * first (even harmonics). The high-pass keeps that offset from reaching the LPG. */
            float symmetry = (p_symmetry - 0.5f) * 2.0f + pd[3];
            float folded_raw = wavefold((raw + symmetry) * fold_depth);
            float folded = folded_raw - voices[v].dc_x + 0.9995f * voices[v].dc_y;
            voices[v].dc_x = folded_raw;
            voices[v].dc_y = folded;

            float envelope_out;
            int holding = voices[v].hold_timer > 0.0f;
            if (holding) {
                voices[v].hold_timer -= 1.0f / sample_rate;
                envelope_out = 1.0f; 
            } else {
                float decay_time = 0.1f + p_decay * 2.0f;
                float decay_rate = expf(-1.0f / (decay_time * 44100.0f));
                voices[v].env *= decay_rate;
                envelope_out = voices[v].env;
            }

            /* Gate control: the plain envelope, or the vactrol driven by a short strike pulse
             * (plus the PULSE WIDTH hold), blended by VACTROL. */
            float control = envelope_out;
            if (p_vactrol >= 0.001f) {
                int lit = holding || voices[v].strike > 0.0f;
                if (voices[v].strike > 0.0f) voices[v].strike -= 1.0f / sample_rate;
                float g = vactrol_step(&voices[v].vac, lit ? 1.0f : 0.0f, 1.0f / sample_rate, p_decay);
                voices[v].vactrol_gain = g;
                control = envelope_out + p_vactrol * (g - envelope_out);
            }

            voices[v].ctrl = control;
            if (!holding && voices[v].strike <= 0.0f && control < (p_lpg_mode == 2 ? 0.000001f : 0.0001f))
                voices[v].active = 0;

            float base_cutoff = seq_global_lpg + p_lpg * 5000.0f;
            float cutoff = base_cutoff * (0.05f + 0.95f * (p_lpg_mode == 1 ? 1.0f : control));
            cutoff *= exp2f(pd[4] * 3.0f);
            if (cutoff > 18000.0f) cutoff = 18000.0f;
            
            float alpha = 1.0f - expf(-2.0f * 3.14159f * cutoff / sample_rate);
            voices[v].lpg_low += alpha * (folded - voices[v].lpg_low);
            
            float lpg_out = voices[v].lpg_low * (p_lpg_mode == 2 ? sqrtf(control) : control);
            
            mixed += lpg_out * voices[v].velocity;
        }
        
        if (active_count > 1) mixed /= sqrtf((float)active_count);
        mixed *= 0.35f;
        mixed = tanhf(mixed);

        int delay_samples = (int)(4410.0f + p_reverb_time * 2205.0f); 
        if (delay_samples >= REV_BUF_LEN) delay_samples = REV_BUF_LEN - 1;
        
        int read_idx = (rev_idx - delay_samples + REV_BUF_LEN) % REV_BUF_LEN;
        float delayed_L = rev_buf_L[read_idx];
        float delayed_R = rev_buf_R[read_idx];
        
        float feedback = 0.7f + p_reverb_time * 0.29f;
        rev_buf_L[rev_idx] = mixed + delayed_R * feedback;
        rev_buf_R[rev_idx] = mixed + delayed_L * feedback;
        rev_idx = (rev_idx + 1) % REV_BUF_LEN;

        float out_L = mixed * (1.0f - p_reverb_mix) + delayed_L * p_reverb_mix;
        float out_R = mixed * (1.0f - p_reverb_mix) + delayed_R * p_reverb_mix;
        out_L = tanhf(out_L);
        out_R = tanhf(out_R);

        out[i * 2] += (int32_t)(out_L * 8388608.0f);
        out[i * 2 + 1] += (int32_t)(out_R * 8388608.0f);
    }
}
