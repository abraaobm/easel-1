/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Easel-1 Core DSP Engine */
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
static float p_shape = 0.0f;  // 0..1
static float p_fold = 0.2f;   // 0..1
static float p_decay = 0.5f;  // 0..1
static float p_lpg = 0.5f;    // 0..1

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
    if (!vel) {
        for (int i = 0; i < 8; i++) {
            if (voices[i].active && voices[i].note == note) voices[i].active = 0;
        }
        return;
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
            break;
        }
    }
}

void easel_set_param(int param, float val) {
    if (param == 0) p_shape = val;
    if (param == 1) p_fold = val;
    if (param == 2) p_decay = val;
    if (param == 3) p_lpg = val;
}

void easel_mix(int32_t *out, unsigned frames) {
    static float sample_rate = 44100.0f;
    
    memset(out, 0, frames * 2 * sizeof(int32_t));

    for (unsigned i = 0; i < frames; i++) {
        float mixed = 0.0f;
        for (int v = 0; v < 8; v++) {
            if (!voices[v].active) continue;
            
            voices[v].phase += voices[v].freq / sample_rate;
            if (voices[v].phase >= 1.0f) voices[v].phase -= 1.0f;
            float raw = osc_morph(voices[v].phase, p_shape);
            
            // Wavefolder
            float fold_depth = 1.0f + p_fold * 6.0f;
            float u = raw * fold_depth;
            while (u > 1.0f) u = 2.0f - u;
            while (u < -1.0f) u = -2.0f - u;
            float folded = u;
            
            // Low Pass Gate
            float base_cutoff = 100.0f + p_lpg * 10000.0f;
            float cutoff = base_cutoff * (0.05f + 0.95f * voices[v].env);
            if (cutoff > 18000.0f) cutoff = 18000.0f;
            
            float alpha = 1.0f - expf(-2.0f * 3.14159f * cutoff / sample_rate);
            voices[v].lpg_low += alpha * (folded - voices[v].lpg_low);
            
            float lpg_out = voices[v].lpg_low * voices[v].env;
            
            // Envelope
            float decay_time = 0.1f + p_decay * 2.0f;
            float decay_rate = expf(-1.0f / (decay_time * 44100.0f));
            voices[v].env *= decay_rate; 
            if (voices[v].env < 0.0001f) voices[v].active = 0;
            
            // Soma as vozes com uma margem de segurança (0.4)
            mixed += lpg_out * voices[v].velocity * 0.4f;
        }
        
        // Soft Clipping (Saturação Analógica) para evitar o estouro de sinal
        // Se tocar 4 notas, o som "cola" em vez de estalar digitalmente.
        mixed = tanhf(mixed);
        
        out[i * 2] += (int32_t)(mixed * 8388608.0f);
        out[i * 2 + 1] += (int32_t)(mixed * 8388608.0f);
    }
}
