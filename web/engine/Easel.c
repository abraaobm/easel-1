/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Easel-1 Core DSP Engine - Com Random e Smooth */
#include <math.h>
#include <string.h>
#include <stdint.h>

#ifdef __wasm__
#define EXPORT(name) __attribute__((export_name(name)))
#else
#define EXPORT(name)
#endif

typedef struct {
    float phase;
    float env;
    float lpg_low;
    float freq;
    float velocity;
    uint8_t note;
    uint8_t active;
} easel_voice_t;

static easel_voice_t voices[8];

// Parâmetros Globais (VOICE)
static float p_shape = 0.0f;
static float p_fold = 0.2f;
static float p_decay = 0.5f;
static float p_lpg = 0.5f;

// Function Generator (MOTION)
static float p_rise = 0.1f;
static float p_fall = 0.3f;
static float p_loop = 0.0f;
static float p_route = 0.0f;

// Source of Uncertainty (RANDOM)
static float p_rand_rate = 0.5f;
static float p_rand_amt = 0.0f;
static float p_rand_target = 0.0f;
static float p_rand_smooth = 0.0f;

// Estados Internos
static float fg_phase = 0.0f;
static float fg_out = 0.0f;
static uint8_t fg_running = 0;

static float rand_phase = 0.0f;
static float rand_out = 0.0f;
static float rand_current = 0.0f;
static float rand_prev = 0.0f;
static uint32_t rand_seed = 12345;

static float get_random() {
    rand_seed = (rand_seed * 1103515245 + 12345) & 0x7fffffff;
    return ((float)rand_seed / 1073741824.0f) - 1.0f; // -1.0 a 1.0
}

static float osc_morph(float phase, float shape) {
    float sine = sinf(phase * 6.2831853f);
    float tri = 2.0f * fabsf(2.0f * (phase - floorf(phase + 0.5f))) - 1.0f;
    float saw = 2.0f * phase - 1.0f;
    float square = (phase < 0.5f) ? 1.0f : -1.0f;
    
    if (shape < 0.33f) {
        float t = shape * 3.0f;
        return sine * (1.0f - t) + tri * t;
    } else if (shape < 0.66f) {
        float t = (shape - 0.33f) * 3.0f;
        return tri * (1.0f - t) + saw * t;
    } else {
        float t = (shape - 0.66f) * 3.0f;
        return saw * (1.0f - t) + square * t;
    }
}

void easel_note_on(uint8_t note, uint8_t vel) {
    if (!vel) return;
    for (int i = 0; i < 8; i++) {
        if (voices[i].active && voices[i].note == note) {
            voices[i].env = 1.0f;
            voices[i].phase = 0.0f;
            voices[i].velocity = vel / 127.0f;
            fg_phase = 0.0f;
            fg_running = 1;
            return;
        }
    }
    for (int i = 0; i < 8; i++) {
        if (!voices[i].active) {
            voices[i].active = 1;
            voices[i].note = note;
            voices[i].freq = 440.0f * powf(2.0f, (note - 69) / 12.0f);
            voices[i].phase = 0.0f;
            voices[i].env = 1.0f;
            voices[i].velocity = vel / 127.0f;
            voices[i].lpg_low = 0.0f;
            fg_phase = 0.0f;
            fg_out = 0.0f;
            fg_running = 1;
            return;
        }
    }
    int steal = 0;
    float min_env = voices[0].env;
    for (int i = 1; i < 8; i++) {
        if (voices[i].env < min_env) {
            min_env = voices[i].env;
            steal = i;
        }
    }
    voices[steal].active = 1;
    voices[steal].note = note;
    voices[steal].freq = 440.0f * powf(2.0f, (note - 69) / 12.0f);
    voices[steal].phase = 0.0f;
    voices[steal].env = 1.0f;
    voices[steal].velocity = vel / 127.0f;
    voices[steal].lpg_low = 0.0f;
    fg_phase = 0.0f;
    fg_out = 0.0f;
    fg_running = 1;
}

void easel_set_param(int param, float val) {
    if (param == 0) p_shape = val;
    if (param == 1) p_fold = val;
    if (param == 2) p_decay = val;
    if (param == 3) p_lpg = val;
    if (param == 4) p_rise = val;
    if (param == 5) p_fall = val;
    if (param == 6) p_loop = val > 0.5f ? 1.0f : 0.0f;
    if (param == 7) p_route = val;
    if (param == 8) p_rand_rate = val;
    if (param == 9) p_rand_amt = val;
    if (param == 10) p_rand_target = val;
    if (param == 11) p_rand_smooth = val;
}

void easel_mix(int32_t *out, unsigned frames) {
    static float sample_rate = 44100.0f;
    
    memset(out, 0, frames * 2 * sizeof(int32_t));

    for (unsigned i = 0; i < frames; i++) {
        // --- Function Generator ---
        if (fg_running) {
            float rise_time = 0.01f + p_rise * 2.0f;
            float fall_time = 0.01f + p_fall * 2.0f;
            float rate = (fg_phase < 1.0f) ? (1.0f / (rise_time * sample_rate)) : (1.0f / (fall_time * sample_rate));
            fg_phase += rate;
            
            if (fg_phase < 1.0f) fg_out = fg_phase;
            else if (fg_phase < 2.0f) fg_out = 2.0f - fg_phase;
            else {
                if (p_loop > 0.5f) fg_phase -= 2.0f;
                else { fg_phase = 2.0f; fg_out = 0.0f; fg_running = 0; }
            }
        }

        // --- Source of Uncertainty (Random) ---
        float rand_rate_hz = 0.1f + p_rand_rate * 19.9f; // 0.1 Hz a 20 Hz
        rand_phase += rand_rate_hz / sample_rate;
        
        if (rand_phase >= 1.0f) {
            rand_phase -= 1.0f;
            rand_prev = rand_current;
            rand_current = get_random();
        }
        
        // Smooth (0 = S&H abrupto, 1 = Suave)
        float smooth_interp = rand_prev + (rand_current - rand_prev) * rand_phase;
        float target_out = smooth_interp * (1.0f - p_rand_smooth) + rand_current * p_rand_smooth;
        
        // Mistura com a intensidade (Amount)
        rand_out = target_out * p_rand_amt;

        float mixed = 0.0f;
        for (int v = 0; v < 8; v++) {
            if (!voices[v].active) continue;
            
            // Aplica a modulação do Function Generator
            float fold_mod = 1.0f + (fg_out * 3.0f * (1.0f - p_route)); 
            float pitch_mod = powf(2.0f, (fg_out * 1.5f * p_route));
            
            // Aplica a modulação do Random
            fold_mod += (rand_out * 2.0f * (1.0f - p_rand_target));
            pitch_mod *= powf(2.0f, (rand_out * 0.5f * p_rand_target));

            voices[v].phase += (voices[v].freq * pitch_mod) / sample_rate;
            if (voices[v].phase >= 1.0f) voices[v].phase -= 1.0f;
            float raw = osc_morph(voices[v].phase, p_shape);
            
            float fold_depth = (1.0f + p_fold * 6.0f) * fold_mod;
            float u = raw * fold_depth;
            while (u > 1.0f) u = 2.0f - u;
            while (u < -1.0f) u = -2.0f - u;
            float folded = u;
            
            float base_cutoff = 100.0f + p_lpg * 10000.0f;
            float cutoff = base_cutoff * (0.05f + 0.95f * voices[v].env);
            if (cutoff > 18000.0f) cutoff = 18000.0f;
            
            float alpha = 1.0f - expf(-2.0f * 3.14159f * cutoff / sample_rate);
            voices[v].lpg_low += alpha * (folded - voices[v].lpg_low);
            
            float lpg_out = voices[v].lpg_low * voices[v].env;
            
            float decay_time = 0.1f + p_decay * 2.0f;
            float decay_rate = expf(-1.0f / (decay_time * 44100.0f));
            voices[v].env *= decay_rate; 
            if (voices[v].env < 0.0001f) voices[v].active = 0;
            
            mixed += lpg_out * voices[v].velocity * 0.4f;
        }
        mixed = tanhf(mixed);
        out[i * 2] += (int32_t)(mixed * 8388608.0f);
        out[i * 2 + 1] += (int32_t)(mixed * 8388608.0f);
    }
}
