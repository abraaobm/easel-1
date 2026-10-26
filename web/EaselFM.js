/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Easel FM - Hybrid West Coast engine for web preview.
 * Uses Track Pairing: Even track = DX7 (Complex Osc), Odd track = Pluck (LPG). */
#include "instrument.h"
#include <string.h>

#ifdef __wasm__
#define EXPORT(name) __attribute__((export_name(name)))
#else
#define EXPORT(name)
#endif

/* Easel INIT patch for the FM track (Algorithm 5: OP6 -> OP5) */
static const uint8_t easel_fm_patch[156] = {
    /* OP6 (modulador) */
    99, 60,  0, 60,  99, 60, 60,  0,  50, 0, 0,  0, 0, 1, 0, 0,   0, 0,  2, 0, 7,
    /* OP5 (portadora) */
    20, 60,  0, 60,  99, 80, 60,  0,  50, 0, 0,  0, 0, 1, 0, 2,  80, 0,  1, 0, 7,
    /* OP4 mudo */
    99, 99, 99, 99,  99,  0,  0,  0,  50, 0, 0,  0, 0, 1, 0, 0,   0, 0,  1, 0, 7,
    /* OP3 mudo */
    99, 99, 99, 99,  99,  0,  0,  0,  50, 0, 0,  0, 0, 1, 0, 0,   0, 0,  1, 0, 7,
    /* OP2 mudo */
    99, 99, 99, 99,  99,  0,  0,  0,  50, 0, 0,  0, 0, 1, 0, 0,   0, 0,  1, 0, 7,
    /* OP1 mudo */
    99, 99, 99, 99,  99,  0,  0,  0,  50, 0, 0,  0, 0, 1, 0, 0,   0, 0,  1, 0, 7,
    /* Pitch EG rates + levels */
    99, 99, 99, 99,  50, 50, 50, 50,
    /* ALG FDBK SYNC LFO-SPD LFO-DLY PMD AMD LFO-SYNC LFO-WAVE PMSENS TRANS */
     4,  1,  1, 35,  0,  0,  0,  1,  4,  0, 24,
    /* Nome */
    'E','A','S','E','L',' ','I','N','I','T',
    63,
};

typedef struct { uint8_t enabled, page, init_done; } easel_state_t;
static easel_state_t state[GROOVE_TRACKS / 2];
static uint8_t read_buf[4] __attribute__((used));

static void ensure_init(void) {
    static uint8_t initialized;
    if (initialized) return;
    memset(state, 0, sizeof state);
    initialized = 1;
}

/* Helper to move a parameter to a target value one step at a time */
static void tune_track(unsigned track, unsigned page, unsigned param, int target) {
    if (page == 0) { // FM track
        uint8_t patch[156];
        if (groove_fm_copy(track, patch, NULL) == 0) {
            int current = patch[param];
            groove_fm_edit(track, param, target - current);
        }
    }
}

EXPORT("sim_easel_enter") int sim_easel_enter(unsigned track) {
    ensure_init();
    if (track >= GROOVE_TRACKS) return -1;
    
    // Force to even track (0, 2, 4, 6)
    unsigned base = track & ~1U;
    unsigned fm = base;
    unsigned plk = base + 1;
    unsigned idx = base / 2;

    if (groove_source_type(fm) != GROOVE_DX7) groove_source_set(fm, GROOVE_DX7);
    if (groove_source_type(plk) != GROOVE_PLUCK) groove_source_set(plk, GROOVE_PLUCK);

    // Synchronize MIDI channels so one keypress triggers both
    unsigned ch = groove_midi_input_channel(fm);
    groove_midi_input_set(plk, ch);

    if (!state[idx].init_done) {
        // Setup FM (Complex Oscillator)
        groove_fm_import(fm, easel_fm_patch);
        groove_adsr_enable(fm, 1);
        tune_track(fm, 0, 0, 20); // Attack
        tune_track(fm, 0, 1, 50); // Decay
        tune_track(fm, 0, 2, 80); // Sustain
        tune_track(fm, 0, 3, 40); // Release

        // Setup Pluck (LPG)
        groove_tone_edit(plk, 0, 0, 90 - 30); // Decay
        groove_tone_edit(plk, 0, 1, 90 - 50); // Brightness (Filter)
        
        state[idx].init_done = 1;
    }

    state[idx].enabled = 1;
    state[idx].page = 0;
    return 0;
}

EXPORT("sim_easel_leave") void sim_easel_leave(unsigned track) {
    ensure_init();
    unsigned idx = (track & ~1U) / 2;
    if (idx < 4) state[idx].enabled = 0;
}

EXPORT("sim_easel_page") unsigned sim_easel_page(unsigned track) {
    ensure_init();
    unsigned idx = (track & ~1U) / 2;
    return idx < 4 ? state[idx].page : 0;
}

EXPORT("sim_easel_set_page") void sim_easel_set_page(unsigned track, unsigned page) {
    ensure_init();
    unsigned idx = (track & ~1U) / 2;
    if (idx < 4) state[idx].page = (uint8_t)(page % 4);
}

EXPORT("sim_easel_adjust") int sim_easel_adjust(unsigned track, unsigned knob, int delta) {
    ensure_init();
    unsigned base = track & ~1U;
    unsigned fm = base;
    unsigned plk = base + 1;
    unsigned idx = base / 2;
    if (idx >= 4 || !state[idx].enabled || !delta) return -1;

    unsigned page = state[idx].page;

    if (page == 0) { // VOICE
        switch (knob) {
            case 0: // FOLD (Index + Feedback)
                groove_fm_edit(fm, 16, delta); // OP6 OUT
                if (delta > 0) groove_fm_edit(fm, 135, 1); // Feedback up
                break;
            case 1: groove_fm_edit(fm, 18, delta); break; // RATIO
            case 2: groove_fm_edit(fm, 19, delta); break; // FINE
            case 3: groove_fm_edit(fm, 134, delta); break; // WAVE (Algorithm)
        }
    } else if (page == 1) { // MOTION
        switch (knob) {
            case 0: // RISE
                groove_adsr_edit(fm, 0, delta);
                groove_tone_edit(plk, 0, 2, delta); // Pluck attack (if any)
                break;
            case 1: // FALL
                groove_adsr_edit(fm, 1, delta); 
                groove_adsr_edit(fm, 3, delta);
                break;
            case 2: // LOOP
                if (delta > 0) {
                    groove_arp_set(fm, 1); groove_arp_set(plk, 1);
                    groove_arp_edit(fm, 0, 3); groove_arp_edit(plk, 0, 3); // 1/32
                } else {
                    groove_arp_set(fm, 0); groove_arp_set(plk, 0);
                }
                break;
            case 3: // DEPTH (Pitch EG)
                groove_fm_edit(fm, 130, delta); 
                break;
        }
    } else if (page == 2) { // GATE
        switch (knob) {
            case 0: // LEVEL (Pluck Decay)
                groove_tone_edit(plk, 0, 0, delta); 
                break;
            case 1: // SOFT (Pluck Brightness)
                groove_tone_edit(plk, 0, 1, delta); 
                break;
            case 2: // RAND PITCH
                groove_fm_edit(fm, 139, delta); 
                break;
            case 3: // RAND TIMBRE
                groove_fm_edit(fm, 140, delta); 
                break;
        }
    } else if (page == 3) { // SEQUENCE
        switch (knob) {
            case 0: groove_tempo_edit(delta); break;
            case 1: groove_sequence_setting_edit(0, delta); break; // Swing
            case 2: {
                unsigned len = groove_sequence_length(fm);
                int next = (int)(len / 16) + delta;
                if (next < 1) next = 1;
                if (next > 4) next = 4;
                groove_sequence_length_set(fm, next * 16);
                groove_sequence_length_set(plk, next * 16);
                break;
            }
            case 3: groove_fm_edit(fm, 144, delta); break; // Transpose
        }
    }
    return 0;
}

EXPORT("sim_easel_read") void sim_easel_read(unsigned track) {
    ensure_init();
    memset(read_buf, 0, 4);
    unsigned base = track & ~1U;
    unsigned fm = base;
    unsigned plk = base + 1;
    unsigned idx = base / 2;
    if (idx >= 4) return;

    unsigned page = state[idx].page;
    uint8_t patch[156];
    struct groove_fm_controls ctl = {0};
    uint8_t plk_vals[GROOVE_TONE_PARAMS] = {0};

    groove_fm_copy(fm, patch, &ctl);
    groove_tone_copy(plk, 0, plk_vals);

    if (page == 0) {
        read_buf[0] = patch[16];  // Fold
        read_buf[1] = patch[18];  // Ratio
        read_buf[2] = patch[19];  // Fine
        read_buf[3] = patch[134]; // Wave
    } else if (page == 1) {
        read_buf[0] = ctl.envelope[0]; // Rise
        read_buf[1] = ctl.envelope[1]; // Fall
        read_buf[2] = (uint8_t)groove_arp_enabled(fm);
        read_buf[3] = patch[130]; // Depth
    } else if (page == 2) {
        read_buf[0] = plk_vals[0]; // Level
        read_buf[1] = plk_vals[1]; // Soft
        read_buf[2] = patch[139];  // Rand Pitch
        read_buf[3] = patch[140];  // Rand Timbre
    } else if (page == 3) {
        struct groove_view v = {0};
        groove_get_view(fm, 0, &v);
        read_buf[0] = (uint8_t)v.tempo;
        read_buf[1] = (uint8_t)groove_sequence_setting(0);
        read_buf[2] = (uint8_t)v.next_length;
        read_buf[3] = patch[144] + 24; // Transpose offset
    }
}

EXPORT("sim_easel_read_buf") const uint8_t *sim_easel_read_buf(void) { return read_buf; }
