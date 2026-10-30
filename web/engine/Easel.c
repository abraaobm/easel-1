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

typedef struct {
    float phase;
    float env;
    float hold_timer;
    float lpg_low;
    float freq;
    float velocity;
    float pitch_env;
    uint8_t note;
    uint8_t active;
} easel_voice_t;

static easel_voice_t voices[8];

static float p_engine_mode = 0.0f; 
static float p_shape = 0.0f;       
static float p_fold = 0.2f;
static float p_decay = 0.5f;
static float p_lpg = 0.5f;

static float p_rise = 0.1f;
static float p_fall = 0.3f;
static int p_loop = 0;
static float p_pulse_width = 0.0f;

static float p_euclid_rate = 0.2f;
static float p_euclid_pulses = 0.3f;

static float p_rand_rate = 0.5f;
static float p_rand_amt = 0.0f;
static float p_rand_target = 0.0f;
static float p_rand_smooth = 0.0f;

static float p_pitch_amt = 0.5f;
static float p_pitch_decay = 0.2f;
static float p_reverb_mix = 0.3f;
static float p_reverb_time = 0.5f;

static float p_seq_transpose = 0.5f;
static float p_seq_octave = 0.5f;
static float p_seq_timbre = 0.5f;
static float p_seq_lpg_offset = 0.5f;

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
}

void easel_deserialize(const uint8_t *buf) {
    if (buf[0] != 'E' || buf[1] != 'A' || buf[2] != 'S' || buf[3] != 'L') return;
    p_shape = buf[4] / 255.0f;
    p_fold = buf[5] / 255.0f;
    p_decay = buf[6] / 255.0f;
    p_lpg = buf[7] / 255.0f;
    p_rise = buf[8] / 255.0f;
    p_fall = buf[9] / 255.0f;
    p_loop = (int)(buf[10] / 50.0f);
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
            voices[i].env = 1.0f;
            voices[i].pitch_env = 1.0f;
            voices[i].velocity = vel / 127.0f;
            voices[i].lpg_low = 0.0f;
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
    voices[steal].env = 1.0f;
    voices[steal].pitch_env = 1.0f;
    voices[steal].velocity = vel / 127.0f;
    voices[steal].lpg_low = 0.0f;
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
}

void easel_mix(int32_t *out, unsigned frames) {
    static float sample_rate = 44100.0f;
    memset(out, 0, frames * 2 * sizeof(int32_t));

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

        for (int v = 0; v < 8; v++) {
            if (!voices[v].active) continue;
            active_count++;
            
            float fg_fold = 1.0f + (fg_out * 1.5f * (1.0f - p_rand_target));
            float fg_pitch = powf(2.0f, (fg_out * 1.0f * p_rand_target));
            
            float rand_fold = rand_out * 1.0f * (1.0f - p_rand_target);
            float rand_pitch = powf(2.0f, (rand_out * 0.2f * p_rand_target));

            float fold_mod = fg_fold + rand_fold;
            float pitch_mod = fg_pitch * rand_pitch;

            float p_decay_time = 0.01f + p_pitch_decay * 0.5f;
            float p_decay_rate = expf(-1.0f / (p_decay_time * sample_rate));
            voices[v].pitch_env *= p_decay_rate;
            pitch_mod *= powf(2.0f, (voices[v].pitch_env * p_pitch_amt * 1.5f));

            voices[v].phase += (voices[v].freq * pitch_mod) / sample_rate;
            if (voices[v].phase >= 1.0f) voices[v].phase -= 1.0f;
            
            float raw = synth_core(voices[v].phase, p_shape, p_engine_mode);
            
            float fold_depth = (1.0f + p_fold * 4.0f) * fold_mod * seq_global_fold;
            float u = raw * fold_depth;
            if (u > 1.0f || u < -1.0f) {
                u = fmodf(u + 1.0f, 4.0f);
                if (u < 0) u += 4.0f;
                u = (u > 2.0f) ? 4.0f - u : u;
                u -= 1.0f;
            }
            float folded = u;

            float envelope_out;
            if (voices[v].hold_timer > 0.0f) {
                voices[v].hold_timer -= 1.0f / sample_rate;
                envelope_out = 1.0f; 
            } else {
                float decay_time = 0.1f + p_decay * 2.0f;
                float decay_rate = expf(-1.0f / (decay_time * 44100.0f));
                voices[v].env *= decay_rate;
                envelope_out = voices[v].env;
                if (voices[v].env < 0.0001f) voices[v].active = 0;
            }
            
            float base_cutoff = seq_global_lpg + p_lpg * 5000.0f;
            float cutoff = base_cutoff * (0.05f + 0.95f * envelope_out);
            if (cutoff > 18000.0f) cutoff = 18000.0f;
            
            float alpha = 1.0f - expf(-2.0f * 3.14159f * cutoff / sample_rate);
            voices[v].lpg_low += alpha * (folded - voices[v].lpg_low);
            
            float lpg_out = voices[v].lpg_low * envelope_out;
            
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
