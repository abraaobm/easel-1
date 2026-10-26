/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Web-only West Coast layer. Drives the DX7 engine exclusively through
 * the public groove_* APIs. Compiled only into the WASM preview.
 *
 * Remove this file for a hardware build; nothing in app/ or sdk/ depends
 * on it. */
#include "instrument.h"
#include <string.h>

#ifdef __wasm__
#define EXPORT(name) __attribute__((export_name(name)))
#else
#define EXPORT(name)
#endif

/* WC INIT — algorithm 5 (OP6 -> OP5, feedback em OP6).
 * Fold        = OP6 OUT LVL  (offset 16)
 * Ratio/Fine  = OP6 COARSE / FINE (offset 18 / 19)
 * Fdbk        = global feedback (135)
 * Rise/Fall   = ADSR overlay A / D
 * Loop        = arp 1/32 + hold + gate 100%
 * Depth/Offset/Soft/Resp = LPG no estilo VCA
 * Pitch/Rate/Wander/Wave = LFO do patch DX7 */
static const uint8_t wc_init_patch[156] = {
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
    'W','C',' ','I','N','I','T',' ',' ',' ',
    63,
};

enum { WC_PAGE_OSC = 0, WC_PAGE_FUNC, WC_PAGE_LPG, WC_PAGE_RAND, WC_PAGES };
typedef struct { uint8_t enabled, page, init_done; } wc_state_t;
static wc_state_t wc_state[GROOVE_TRACKS];
static uint8_t wc_read_buf[16] __attribute__((used));
static uint8_t wc_initialized;

static void wc_ensure_init(void) {
    if (wc_initialized) return;
    memset(wc_state, 0, sizeof wc_state);
    wc_initialized = 1;
}

/* Move an arp setting towards a target one detent at a time so that
 * whatever clamping groove_arp_edit applies internally still converges. */
static void wc_arp_target(unsigned track, unsigned param, unsigned target) {
    for (int guard = 0; guard < 16; ++guard) {
        unsigned cur = groove_arp_setting(track, param);
        if (cur == target) break;
        groove_arp_edit(track, param, cur < target ? 1 : -1);
        if (groove_arp_setting(track, param) == cur) break;
    }
}

static void wc_arp_configure(unsigned track, unsigned enable) {
    if (!enable) { groove_arp_set(track, 0); return; }
    wc_arp_target(track, 0, 3); /* rate 1/32 */
    wc_arp_target(track, 1, 0); /* direction up */
    wc_arp_target(track, 2, 0); /* range 1 octave */
    wc_arp_target(track, 3, 3); /* gate 100% */
    wc_arp_target(track, 4, 0); /* swing 0 */
    wc_arp_target(track, 5, 1); /* hold on */
    groove_arp_set(track, 1);
}

static int wc_install(unsigned track) {
    int r = groove_fm_import(track, wc_init_patch);
    if (r) return r;

    /* Blindagem contra silêncio: garante que o ADSR está ativo e audível */
    uint8_t patch[156];
    struct groove_fm_controls ctl;
    if (groove_fm_copy(track, patch, &ctl) == 0) {
        groove_adsr_enable(track, 1);
        if (ctl.envelope[0] < 10) groove_adsr_edit(track, 0, 20);  /* Attack */
        if (ctl.envelope[1] < 10) groove_adsr_edit(track, 1, 50);  /* Decay */
        if (ctl.envelope[2] < 50) groove_adsr_edit(track, 2, 99);  /* Sustain */
        if (ctl.envelope[3] < 10) groove_adsr_edit(track, 3, 30);  /* Release */
    } else {
        groove_adsr_enable(track, 1);
    }

    /* Garante que a track não está com volume 0 */
    struct groove_view v;
    groove_get_view(track, 0, &v);
    if (v.volume < 50) {
        groove_control_edit(track, 0, 100, 0);
    }

    return 0;
}

EXPORT("sim_wc_enter") int sim_wc_enter(unsigned track) {
    wc_ensure_init();
    if (track >= GROOVE_TRACKS) return -1;
    if (groove_source_type(track) != GROOVE_DX7) return -2;
    if (!wc_state[track].enabled) {
        if (!wc_state[track].init_done) {
            int r = wc_install(track);
            if (r) return r;
            wc_state[track].init_done = 1;
        }
        wc_state[track].enabled = 1;
        wc_state[track].page = 0;
    }
    return 0;
}

EXPORT("sim_wc_leave") void sim_wc_leave(unsigned track) {
    wc_ensure_init();
    if (track < GROOVE_TRACKS) wc_state[track].enabled = 0;
}

EXPORT("sim_wc_active") unsigned sim_wc_active(unsigned track) {
    wc_ensure_init();
    return track < GROOVE_TRACKS ? wc_state[track].enabled : 0;
}

EXPORT("sim_wc_page") unsigned sim_wc_page(unsigned track) {
    wc_ensure_init();
    return track < GROOVE_TRACKS ? wc_state[track].page : 0;
}

EXPORT("sim_wc_set_page") void sim_wc_set_page(unsigned track, unsigned page) {
    wc_ensure_init();
    if (track < GROOVE_TRACKS) wc_state[track].page = (uint8_t)(page % WC_PAGES);
}

EXPORT("sim_wc_reset_patch") int sim_wc_reset_patch(unsigned track) {
    wc_ensure_init();
    if (track >= GROOVE_TRACKS) return -1;
    wc_state[track].init_done = 0;
    return wc_install(track);
}

EXPORT("sim_wc_adjust") int sim_wc_adjust(unsigned track, unsigned knob, int delta) {
    wc_ensure_init();
    if (track >= GROOVE_TRACKS || knob > 3 || !delta) return -1;
    if (!wc_state[track].enabled) return -2;
    switch (wc_state[track].page) {
    case WC_PAGE_OSC:
        switch (knob) {
        case 0: groove_fm_edit(track, 16,  delta); break; /* Fold = OP6 OUT LVL */
        case 1: groove_fm_edit(track, 18,  delta); break; /* Ratio = OP6 COARSE */
        case 2: groove_fm_edit(track, 19,  delta); break; /* Fine  = OP6 FINE */
        case 3: groove_fm_edit(track, 135, delta); break; /* Fdbk  = global feedback */
        }
        break;
    case WC_PAGE_FUNC:
        switch (knob) {
        case 0: groove_adsr_enable(track, 1); groove_adsr_edit(track, 0, delta); break;
        case 1: groove_adsr_enable(track, 1); groove_adsr_edit(track, 1, delta); break;
        case 2: wc_arp_configure(track, delta > 0); break;
        case 3: groove_adsr_enable(track, 1); groove_adsr_edit(track, 2, delta); break;
        }
        break;
    case WC_PAGE_LPG:
        switch (knob) {
        case 0: groove_adsr_enable(track, 1); groove_adsr_edit(track, 2, delta); break;
        case 1: groove_control_edit(track, 0, delta, 0); break;
        case 2: groove_adsr_enable(track, 1); groove_adsr_edit(track, 0, delta); break;
        case 3: groove_adsr_enable(track, 1); groove_adsr_edit(track, 3, delta); break;
        }
        break;
    case WC_PAGE_RAND:
        switch (knob) {
        case 0: groove_fm_edit(track, 139, delta); break; /* PMD */
        case 1: groove_fm_edit(track, 137, delta); break; /* LFO speed */
        case 2: groove_fm_edit(track, 140, delta); break; /* AMD */
        case 3: groove_fm_edit(track, 142, delta); break; /* LFO waveform */
        }
        break;
    default: return -1;
    }
    return 0;
}

/* 16 bytes: [OSC knobs 0..3][FUNC knobs][LPG knobs][RAND knobs] */
EXPORT("sim_wc_read") void sim_wc_read(unsigned track) {
    wc_ensure_init();
    memset(wc_read_buf, 0, 16);
    if (track >= GROOVE_TRACKS) return;
    uint8_t patch[156];
    struct groove_fm_controls ctl = {0};
    struct groove_view v = {0};
    if (groove_fm_copy(track, patch, &ctl)) return;
    groove_get_view(track, 0, &v);
    wc_read_buf[0]  = patch[16];   /* OP6 OUT LVL   */
    wc_read_buf[1]  = patch[18];   /* OP6 COARSE    */
    wc_read_buf[2]  = patch[19];   /* OP6 FINE      */
    wc_read_buf[3]  = patch[135];  /* global FDBK   */
    wc_read_buf[4]  = ctl.envelope[0];
    wc_read_buf[5]  = ctl.envelope[1];
    wc_read_buf[6]  = (uint8_t)groove_arp_enabled(track);
    wc_read_buf[7]  = ctl.envelope[2];
    wc_read_buf[8]  = ctl.envelope[2];
    wc_read_buf[9]  = v.volume;
    wc_read_buf[10] = ctl.envelope[0];
    wc_read_buf[11] = ctl.envelope[3];
    wc_read_buf[12] = patch[139];
    wc_read_buf[13] = patch[137];
    wc_read_buf[14] = patch[140];
    wc_read_buf[15] = patch[142];
}

EXPORT("sim_wc_read_buf") const uint8_t *sim_wc_read_buf(void) { return wc_read_buf; }
