/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Easel-1 engine: the sequencer, arpeggiator and MIDI routing drive one
 * West Coast voice bank (web/engine/Easel.c). There are no other sources. */
#include "engine.h"
#include "mixer.h"
#include "internal.h"
#include "fixed.h"
#include <stdlib.h>
#include <string.h>

extern void easel_note_on(uint8_t note, uint8_t vel);
extern void easel_mix(int32_t *out, unsigned frames);
extern void easel_panic(void);
extern void easel_serialize(uint8_t *buf);
extern void easel_deserialize(const uint8_t *buf);
extern unsigned easel_preset_count(void);
extern int easel_preset_current(void);
extern void easel_preset_load(unsigned index);

/* Every track plays the same Easel-1; a track only owns its mix state. */
static struct {
    uint8_t volume, program, modified, audible, type, expression, pan, padding[9];
    uint8_t meter;
    int32_t gain;
    struct groove_fm_controls controls;   /* kept for the shared track-session code */
} tracks[GROOVE_TRACKS] __attribute__((aligned(32)));
/* Control-owned masks; per-track audible is published through the render gate. */
static unsigned mute_mask, solo_mask;
static uint8_t track_roles[GROOVE_TRACKS], input_channels[GROOVE_TRACKS];
static struct dormant_track {
    struct groove_preset preset;
    struct groove_tuning tuning;
    uint8_t arp[2], mute, solo;
} *dormant[GROOVE_TRACKS];
/* A track stays "ready" while it has a role; the Easel itself is shared. */
struct source { void *ptr, *allocation; };
static struct source sources[GROOVE_TRACKS];
static int source_create(struct source *s, unsigned type, unsigned program)
{
    (void)type; (void)program;
    s->ptr = s->allocation = (void *)1;
    return 0;
}
static void source_reset(struct source *s, unsigned type) { (void)s; (void)type; }
static void source_destroy(unsigned track) { (void)track; }
static void stereo_refresh(void) {}
static void audio_reset(void) { for (unsigned t = 0; t < GROOVE_TRACKS; ++t) tracks[t].meter = 0; }
static struct { unsigned track; } edit_snapshot = {~0U};
static unsigned track_group(unsigned role), track_audible(unsigned track);
static void session_init(void);
static void session_mix_refresh(void);
static void session_project_row(unsigned track, const struct groove_project *p);
static void session_arp_apply(unsigned track, const uint8_t arp[2]);
static volatile unsigned output_level;
static unsigned master_adc = 0xffffU;
int groove_master_update(unsigned adc)
{
    if (adc > 1023U) return 0;
    unsigned first = master_adc > 1023U;
    unsigned distance = adc > master_adc ? adc - master_adc : master_adc - adc;
    if (!first && adc > 4U && adc < 1019U && distance < 4U) return 0;
    unsigned level = adc <= 4U ? 0U : adc >= 1019U ? 127U : (adc * 127U + 511U) / 1023U;
    unsigned changed = output_level != level;
    master_adc = adc;
    output_level = level;
    return !first && changed;
}
uint32_t groove_master_state(void) { return master_adc | (output_level << 16); }
unsigned fm1_engine_output_level(void) { return output_level; }
static uint8_t live[GROOVE_TRACKS][128] __attribute__((aligned(32)));
static struct {
    uint8_t bits[16], padding[16];
} seq_note[GROOVE_TRACKS] __attribute__((aligned(32)));
#include <stdatomic.h>
#define STATUS_WORD _Atomic(uint32_t)
static struct {
    STATUS_WORD word, edit_dirty;
    uint32_t padding[6];
} status[GROOVE_TRACKS] __attribute__((aligned(32)));
void groove_audio_stats(struct fm1_render_stats *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->voice_capacity = 8;
    out->master_gain = fm1_master_gain();
    out->deadline_ticks = 24000000U * FM1_BLOCK_FRAMES / FM1_SAMPLE_RATE;
}
static FM1_DSP_CODE void publish(unsigned track, unsigned level)
{
    unsigned ready = track_roles[track] != GROOVE_TRACK_OFF;
    status[track].word = (tracks[track].program & 15U) | ((unsigned)(tracks[track].program >> 4) << 23) | ((unsigned)tracks[track].volume << 4) |
                         ((level > 127 ? 127 : level) << 11) | (ready ? 262144U : 0U) |
                         ((unsigned)tracks[track].modified << 19) |
                         ((unsigned)tracks[track].type << 20);
}
FM1_DSP_CODE unsigned groove_source_type(unsigned track)
{
    return track < GROOVE_TRACKS ? tracks[track].type : GROOVE_SOURCES;
}
static struct {
    uint8_t enabled, note, next, held, velocity[128];
    uint8_t rate, mode, octaves, gate, started, descending;
    uint8_t swing, hold, odd, latched;
    uint16_t tick;
    uint32_t phase, random;
    uint8_t padding[8];
} arpeggio[GROOVE_TRACKS] __attribute__((aligned(32)));
_Static_assert(sizeof arpeggio[0] == 160, "independent arpeggiator cache lines");
unsigned groove_arp_enabled(unsigned track)
{
    return track < GROOVE_TRACKS ? arpeggio[track].enabled : 0;
}
static FM1_DSP_CODE void arp_restart(unsigned t);
static struct groove_tuning tuning[GROOVE_TRACKS];
static uint8_t alt_hold[GROOVE_TRACKS]; /* live state, never saved */
int groove_tuning_set(unsigned track, const struct groove_tuning *t)
{
    if (track >= GROOVE_TRACKS || !t) return -1;
    for (unsigned i = 0; i < GROOVE_TUNING_KEYS; ++i)
        if (t->main[i] == -128 || t->alt[i] == -128) return -2;
    tuning[track] = *t;
    return 0;
}
void groove_tuning_get(unsigned track, struct groove_tuning *t)
{
    if (!t) return;
    if (track < GROOVE_TRACKS) *t = tuning[track];
    else memset(t, 0, sizeof *t);
}
void groove_tuning_alt_hold(unsigned track, unsigned on)
{
    if (track < GROOVE_TRACKS) alt_hold[track] = on != 0;
}
/* The owner can validate once and copy the complete table atomically at a
 * command boundary; no per-track revalidation or unreachable partial failure. */
int groove_tuning_capture(struct groove_tuning t[GROOVE_TRACKS])
{
    if (!t) return -1;
    memcpy(t, tuning, sizeof tuning);
    return 0;
}
int groove_tuning_apply(const struct groove_tuning t[GROOVE_TRACKS])
{
    if (!t) return -1;
    if (memchr(t, 0x80, sizeof tuning)) return -2;
    memcpy(tuning, t, sizeof tuning);
    return 0;
}

static FM1_DSP_CODE void engine_note(unsigned track, unsigned note, unsigned velocity, int cents)
{
    (void)cents;
    /* The Easel voice is struck: a release needs no action. */
    if (!velocity || track_roles[track] == GROOVE_TRACK_OFF) return;
    unsigned scaled = velocity * tracks[track].volume / 100U;
    if (scaled > 127U) scaled = 127U;
    if (scaled < 1U) scaled = 1U;
    if (scaled > tracks[track].meter) tracks[track].meter = (uint8_t)scaled;
    easel_note_on((uint8_t)note, (uint8_t)scaled);
}
/* Live notes (sequenced == 0) take alt from the track's alt hold; sequenced and
 * arpeggiated notes pass the step's GROOVE_STEP_ALT through groove_note_ex. */
FM1_DSP_CODE void groove_note(unsigned track, unsigned note, unsigned velocity, int sequenced)
{
    groove_note_ex(track, note, velocity, sequenced, !sequenced && track < GROOVE_TRACKS && alt_hold[track]);
}
FM1_DSP_CODE unsigned groove_alt_held(unsigned track)
{
    return track < GROOVE_TRACKS && alt_hold[track];
}
FM1_DSP_CODE void groove_note_ex(unsigned track, unsigned note, unsigned velocity, int sequenced, unsigned alt)
{
    if (track >= GROOVE_TRACKS || note > 127 || velocity > 127)
        return;
    /* Live keys, pattern gates and arpeggiator gates share pitch ownership.
     * Releasing one source never cuts an identically pitched other source. */
    unsigned generated = (seq_note[track].bits[note >> 3] & (1U << (note & 7U))) || arpeggio[track].note == note;
    unsigned local_sequence = groove_midi_mode(track) != 2;
    unsigned held = (!arpeggio[track].enabled && live[track][note]) || (local_sequence && generated);
    if (sequenced == 2)
        arpeggio[track].note = velocity ? (uint8_t)note : 0xff;
    else if (sequenced) {
        if (velocity) seq_note[track].bits[note >> 3] |= (uint8_t)(1U << (note & 7U));
        else seq_note[track].bits[note >> 3] &= (uint8_t)~(1U << (note & 7U));
    }
    else if (velocity) {
        /* A fresh chord replaces the latched chord once every physical key
         * has been released. Duplicate presses still have separate owners. */
        if (arpeggio[track].enabled && arpeggio[track].hold && !arpeggio[track].held) {
            arp_restart(track);
            memset(arpeggio[track].velocity, 0, sizeof arpeggio[track].velocity);
            held = local_sequence && (seq_note[track].bits[note >> 3] & (1U << (note & 7U)));
        }
        if (!live[track][note]) ++arpeggio[track].held;
        if (live[track][note] < 255) ++live[track][note];
        arpeggio[track].velocity[note] = (uint8_t)(velocity | (alt ? 128U : 0U)); /* bit 7: pressed with alt */
        arpeggio[track].latched = 1;
    } else if (live[track][note]) {
        --live[track][note];
        if (!live[track][note]) --arpeggio[track].held;
        if (!live[track][note] && !arpeggio[track].hold) {
            arpeggio[track].velocity[note] = 0;
            if (arpeggio[track].note == note) arpeggio[track].note = 0xff;
        }
    }
    unsigned next_generated = (seq_note[track].bits[note >> 3] & (1U << (note & 7U))) || arpeggio[track].note == note;
    if ((!velocity || tracks[track].audible) &&
        generated != next_generated)
        groove_midi_note(track, note, next_generated ? velocity : 0);
    unsigned next = (!arpeggio[track].enabled && live[track][note]) || (local_sequence && next_generated);
    int cents = (alt ? tuning[track].alt : tuning[track].main)[note % 12U];
    if (!held && next) engine_note(track, note, velocity, cents);
    else if (held && !next) engine_note(track, note, 0, 0);
}

static FM1_DSP_CODE void arp_restart(unsigned t)
{
    if (arpeggio[t].note != 255) groove_note(t, arpeggio[t].note, 0, 2);
    arpeggio[t].next = arpeggio[t].started = arpeggio[t].descending = 0;
    arpeggio[t].tick = 0; arpeggio[t].phase = 0;
    arpeggio[t].odd = 0;
}
FM1_DSP_CODE void groove_arp_reset(unsigned clear_latch)
{
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        arp_restart(t);
        if (!clear_latch) continue;
        for (unsigned n = 0; n < 128; ++n)
            if (!live[t][n]) arpeggio[t].velocity[n] = 0;
        arpeggio[t].latched = arpeggio[t].held != 0;
    }
}
static __attribute__((cold, minsize)) void arp_defaults(void)
{
    memset(arpeggio, 0, sizeof arpeggio);
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        arpeggio[t].note = 255;
        arpeggio[t].rate = 2; /* 1/16, ascending, one octave, 50% gate. */
        arpeggio[t].gate = 1;
        arpeggio[t].random = 0x9e3779b9U ^ (t + 1U);
    }
}
__attribute__((cold, minsize)) void groove_project_arp_defaults(struct groove_project *p)
{
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        p->arp[t][0] = 2U | (1U << 6); /* 1/16, ascending, one octave, 50%. */
        p->arp[t][1] = 0;
    }
}
unsigned groove_arp_setting(unsigned t, unsigned parameter)
{
    if (t >= GROOVE_TRACKS) return 0;
    switch (parameter) {
    case 0: return arpeggio[t].rate;
    case 1: return arpeggio[t].mode;
    case 2: return arpeggio[t].octaves;
    case 3: return arpeggio[t].gate;
    case 4: return arpeggio[t].swing;
    case 5: return arpeggio[t].hold;
    default: return 0;
    }
}
__attribute__((cold, minsize)) void groove_arp_edit(unsigned t, unsigned parameter, int delta)
{
    if (groove_track_role(t) == GROOVE_TRACK_OFF) return;
    if (t >= GROOVE_TRACKS || parameter >= 6 || !delta) return;
    int value = (int)groove_arp_setting(t, parameter) + delta;
    int limit = parameter == 4 ? 50 : parameter == 5 ? 1 : 3;
    if (value < 0) value = 0;
    if (value > limit) value = limit;
    if (value == (int)groove_arp_setting(t, parameter)) return;
    if (track_roles[t] == GROOVE_TRACK_NORMAL) groove_edit_checkpoint(0x9d000U + t * 8U + parameter);
    fm1_web_pause();
    arp_restart(t); /* Keep held keys; the next event uses the edited values. */
    if (parameter == 0) arpeggio[t].rate = (uint8_t)value;
    if (parameter == 1) arpeggio[t].mode = (uint8_t)value;
    if (parameter == 2) arpeggio[t].octaves = (uint8_t)value;
    if (parameter == 3) arpeggio[t].gate = (uint8_t)value;
    if (parameter == 4) arpeggio[t].swing = (uint8_t)value;
    if (parameter == 5) {
        arpeggio[t].hold = (uint8_t)value;
        if (!value) {
            for (unsigned n = 0; n < 128; ++n)
                if (!live[t][n]) arpeggio[t].velocity[n] = 0;
            arpeggio[t].latched = arpeggio[t].held != 0;
        }
    }
    fm1_web_resume();
    if (track_roles[t] == GROOVE_TRACK_NORMAL) groove_project_touch();
}
static FM1_DSP_CODE void arp_next(unsigned t)
{
    if (arpeggio[t].note != 255) groove_note(t, arpeggio[t].note, 0, 2);
    uint8_t notes[128]; unsigned count = 0;
    unsigned octaves = groove_pad_count(t) ? 0 : arpeggio[t].octaves;
    /* Merge overlapping octave ranges, clip at MIDI 127, and never transpose
     * drum lanes. The list is bounded even with all input keys held. */
    for (unsigned pitch = 0; pitch < 128; ++pitch)
        for (unsigned oct = 0; oct <= octaves && oct * 12 <= pitch; ++oct)
            if (arpeggio[t].velocity[pitch - oct * 12]) { notes[count++] = (uint8_t)pitch; break; }
    if (!count) { arp_restart(t); return; }
    unsigned index = arpeggio[t].next;
    unsigned mode = arpeggio[t].mode;
    if (!arpeggio[t].started) index = mode == 1 ? count - 1 : 0;
    if (index >= count) index = mode == 1 ? count - 1 : 0;
    if (mode == 3) {
        uint32_t r = arpeggio[t].random;
        r ^= r << 13; r ^= r >> 17; r ^= r << 5;
        arpeggio[t].random = r; index = r % count;
    }
    unsigned note = notes[index], velocity = 100, alt = 0;
    for (unsigned oct = 0; oct <= octaves && oct * 12 <= note; ++oct)
        if (arpeggio[t].velocity[note - oct * 12]) {
            velocity = arpeggio[t].velocity[note - oct * 12] & 127U;
            alt = arpeggio[t].velocity[note - oct * 12] >> 7;
            break;
        }
    groove_note_ex(t, note, velocity, 2, alt);
    arpeggio[t].started = 1;
    if (mode == 1) index = index ? index - 1 : count - 1;
    else if (mode == 2 && count > 1) {
        if (!index) arpeggio[t].descending = 0;
        if (index == count - 1) arpeggio[t].descending = 1;
        index = arpeggio[t].descending ? index - 1 : index + 1;
    } else index = (index + 1) % count;
    arpeggio[t].next = (uint8_t)index;
}
static FM1_DSP_CODE unsigned arp_period(unsigned t, unsigned external)
{
    unsigned base = 24U >> arpeggio[t].rate;
    unsigned shift = (base * arpeggio[t].swing + 50U) / 100U;
    if (shift > base / 2U) shift = base / 2U;
    if (!external) {
        base *= FM1_SAMPLE_RATE * 60U;
        shift = base / 100U * arpeggio[t].swing;
    }
    return arpeggio[t].odd ? base - shift : base + shift;
}
FM1_DSP_CODE void groove_arp_render(unsigned tempo)
{
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        if (!arpeggio[t].enabled || !(arpeggio[t].held || (arpeggio[t].hold && arpeggio[t].latched))) { arp_restart(t); continue; }
        unsigned period = arp_period(t, 0);
        if (!arpeggio[t].started) { arpeggio[t].phase = 0; arp_next(t); }
        else if (arpeggio[t].phase >= period) {
            arpeggio[t].phase -= period; arpeggio[t].odd ^= 1U;
            arp_next(t); period = arp_period(t, 0);
        }
        if (arpeggio[t].phase >= period / 4U * (arpeggio[t].gate + 1U) && arpeggio[t].note != 255)
            groove_note(t, arpeggio[t].note, 0, 2);
        arpeggio[t].phase += tempo * 24U * FM1_BLOCK_FRAMES;
    }
}
FM1_DSP_CODE void groove_arp_clock(void)
{
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        if (!arpeggio[t].enabled || !(arpeggio[t].held || (arpeggio[t].hold && arpeggio[t].latched))) { arp_restart(t); continue; }
        unsigned period = arp_period(t, 1);
        if (!arpeggio[t].started) { arpeggio[t].tick = 0; arp_next(t); }
        else if (++arpeggio[t].tick >= period) {
            arpeggio[t].tick = 0; arpeggio[t].odd ^= 1U;
            arp_next(t); period = arp_period(t, 1);
        }
        if (arpeggio[t].tick >= (period * (arpeggio[t].gate + 1U) + 3U) / 4U && arpeggio[t].note != 255)
            groove_note(t, arpeggio[t].note, 0, 2);
    }
}
unsigned groove_arp_test_note(unsigned t) { return t < GROOVE_TRACKS ? arpeggio[t].note : 255; }

static FM1_DSP_CODE void panic(unsigned track)
{
    groove_midi_track_reset(track);
    alt_hold[track] = 0;
    arpeggio[track].note = 0xff;
    arpeggio[track].next = arpeggio[track].held = 0;
    arpeggio[track].started = arpeggio[track].descending = 0;
    arpeggio[track].phase = arpeggio[track].tick = 0;
    arpeggio[track].odd = arpeggio[track].latched = 0;
    memset(arpeggio[track].velocity, 0, sizeof arpeggio[track].velocity);
    memset(live[track], 0, sizeof live[track]);
    memset(seq_note[track].bits, 0, sizeof seq_note[track].bits);
    tracks[track].meter = 0;
    easel_panic();
}
/* Control task only, with rendering paused. Audio MIDI reset events use the
 * recording queue instead, preserving their order with preceding notes. */
static __attribute__((cold, minsize)) void control_panic(unsigned track)
{
    panic(track);
    groove_edit_track_reset(track);
}
__attribute__((cold, minsize)) void groove_arp_set(unsigned track, unsigned enabled)
{
    if (groove_track_role(track) == GROOVE_TRACK_OFF) return;
    if (track >= GROOVE_TRACKS || enabled > 1 || enabled == arpeggio[track].enabled) return;
    if (track_roles[track] == GROOVE_TRACK_NORMAL) groove_edit_checkpoint(0x9d000U + track * 8U + 6U);
    fm1_web_pause();
    control_panic(track);
    arpeggio[track].enabled = (uint8_t)enabled;
    fm1_web_resume();
    if (track_roles[track] == GROOVE_TRACK_NORMAL) groove_project_touch();
}

/* Program change picks a factory card for the whole instrument. */
static FM1_DSP_CODE unsigned message(unsigned track, const uint8_t *m, size_t n)
{
    unsigned type = m[0] & 0xf0U, changed = 0;
    (void)n;
    if (type == 0x90 || type == 0x80) {
        groove_note(track, m[1], type == 0x90 ? m[2] : 0, 0);
        return 0;
    }
    if (type == 0xb0 && m[1] == 121) {
        tracks[track].expression = 127;
        tracks[track].pan = 64;
    }
    if (type == 0xc0) {
        unsigned cards = easel_preset_count();
        if (cards) easel_preset_load(m[1] % cards);
        tracks[track].program = (uint8_t)(m[1] % (cards ? cards : 1U));
        changed = 1;
    } else if (type == 0xb0) {
        if (m[1] == 7) { tracks[track].volume = m[2]; changed = 1; }
        else if (m[1] == 11) tracks[track].expression = m[2];
        else if (m[1] == 10) tracks[track].pan = m[2];
        else if (m[1] == 120 || m[1] == 123) {
            /* All sound / all notes off: release every held key and generated note. */
            for (unsigned note = 0; note < 128; ++note) {
                unsigned held = live[track][note] || (seq_note[track].bits[note >> 3] & (1U << (note & 7U)));
                if (held) groove_midi_note(track, note, 0);
            }
            if (m[1] == 120) panic(track);
        }
    }
    publish(track, 0);
    return changed;
}
/* A complete short message on one channel; malformed data never reaches DSP. */
static FM1_DSP_CODE inline int demo_message(const uint8_t *m, size_t n, unsigned channel)
{
    if (!m || !n) return 0;
    if (m[0] == 0xff) return n == 1;
    if (m[0] < 0x80 || m[0] >= 0xf0 || (m[0] & 15u) != channel) return 0;
    unsigned want = (m[0] & 0xe0u) == 0xc0u ? 2u : 3u;
    if (n != want) return 0;
    for (size_t i = 1; i < n; ++i) if (m[i] & 0x80u) return 0;
    return 1;
}
static FM1_DSP_CODE void midi(const uint8_t *m, size_t n)
{
    if (!m || !n)
        return;
    if (n == 1 && m[0] == 0xf8) {
        groove_sequence_clock();
        return;
    }
    if (n == 3 && m[0] == 0xf2 && !(m[1] & 0x80) && !(m[2] & 0x80)) {
        groove_sequence_position((unsigned)m[1] | ((unsigned)m[2] << 7));
        return;
    }
    if (n == 1 && (m[0] == 0xfa || m[0] == 0xfb || m[0] == 0xfc || m[0] == 0xff)) {
        groove_sequence_transport(m[0]);
        if (m[0] == 0xff) {
            groove_record_midi(m, n);
            for (unsigned t = 0; t < GROOVE_TRACKS; ++t) panic(t);
        }
        return;
    }
    unsigned channel = m[0] & 15U;
    if (!demo_message(m, n, channel)) return;
    for (unsigned track = 0; track < GROOVE_TRACKS; ++track) {
        if (track_roles[track] == GROOVE_TRACK_OFF || input_channels[track] != channel) continue;
        uint8_t routed[3] = {(uint8_t)((m[0] & 0xf0U) | groove_channel(track)), n > 1 ? m[1] : 0, n > 2 ? m[2] : 0};
        groove_record_midi(routed, n);
        if (message(track, m, n) && track_roles[track] == GROOVE_TRACK_NORMAL)
            groove_edit_midi_changed();
    }
}

__attribute__((cold, minsize)) void fm1_web_init(void)
{
    groove_midi_init();
    output_level = 0;
    master_adc = 0xffffU;
    memset(tuning, 0, sizeof tuning);
    memset(alt_hold, 0, sizeof alt_hold);
    session_init();
    arp_defaults();
    memset(live, 0, sizeof live);
    memset(seq_note, 0, sizeof seq_note);
    /* Four tracks play; the last four project rows stay muted and empty. */
    mute_mask = 0xf0U;
    solo_mask = 0;
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        (void)source_create(&sources[t], 0, 0);
        tracks[t].type = 0;
        tracks[t].program = 0;
        tracks[t].volume = 100;
        tracks[t].expression = 127;
        tracks[t].pan = 64;
        tracks[t].modified = 0;
        tracks[t].meter = 0;
        tracks[t].audible = !(mute_mask & (1U << t));
        status[t].edit_dirty = 1;
        publish(t, 0);
    }
    fm1_master_reset();
    groove_sequence_init();
    groove_clock_set(GROOVE_CLOCK_INTERNAL);
    groove_edit_init();
    groove_project_init();
    groove_transfer_init();
    groove_screen_init();
}
void fm1_web_tick(void) { fm1_control_request(); }
void fm1_web_control(void)
{
    groove_midi_poll();
    groove_project_poll();
    groove_transfer_poll();
    groove_edit_poll();
    groove_screen_control();
}
void fm1_web_midi(const uint8_t *message, size_t bytes)
{
    midi(message, bytes);
}
/* One block: sequencer and arpeggiator first (they trigger the Easel), then
 * the voice bank, then the metronome click and the master limiter. */
int fm1_engine_render(int32_t *output, unsigned frames)
{
    if (!output || frames != FM1_BLOCK_FRAMES) return -1;
    groove_sequence_render();
    easel_mix(output, frames);
    groove_sequence_click(output, frames);
    fm1_master_process128(output);
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        tracks[t].meter = (uint8_t)(tracks[t].meter * 235U / 256U);
        publish(t, tracks[t].audible ? tracks[t].meter : 0);
    }
    return 0;
}
void fm1_engine_pause(void) { fm1_web_pause(); }
void fm1_engine_resume(void) { fm1_web_resume(); }
void fm1_engine_stop(void)
{
    groove_sequence_transport(0xfc);
}
static __attribute__((noinline)) unsigned adjust_value(unsigned value, int delta, unsigned limit)
{
    /* Compare before adding so even INT_MIN/INT_MAX increments are bounded. */
    return delta<-(int)value ? 0 : delta>(int) limit - (int)value ? limit
                                                                  : (unsigned)((int)value + delta);
}
/* PRESETS-style edits: program steps move through the factory cards, volume steps the track level. */
__attribute__((cold, minsize)) void groove_control_edit(unsigned track, int program_steps, int volume_steps, int cutoff_steps)
{
    (void)cutoff_steps;
    if (track >= GROOVE_TRACKS || track_roles[track] == GROOVE_TRACK_OFF)
        return;
    unsigned cards = easel_preset_count();
    if (!program_steps && !volume_steps) return;
    unsigned volume = tracks[track].volume, next_volume = adjust_value(volume, volume_steps, 127);
    if (!program_steps && next_volume == volume) return;
    fm1_web_pause();
    if (track_roles[track] == GROOVE_TRACK_NORMAL) groove_edit_checkpoint(0x91000U + track);
    if (track_roles[track] == GROOVE_TRACK_NORMAL) groove_project_touch();
    if (program_steps && cards) {
        int current = easel_preset_current();
        int next = (current < 0 ? 0 : current) + program_steps;
        next = ((next % (int)cards) + (int)cards) % (int)cards;
        easel_preset_load((unsigned)next);
    }
    if (next_volume != volume) {
        uint8_t m[3] = {(uint8_t)(0xb0 + groove_channel(track)), 7, (uint8_t)next_volume};
        message(track, m, 3);
    }
    fm1_web_resume();
}
void groove_get_view(unsigned track, unsigned step, struct groove_view *view)
{
    if (!view || track >= 8 || step >= 64)
        return;
    uint32_t s = status[track].word;
    view->program = (uint8_t)((s & 15U) | ((s >> 23) & 7U) << 4);
    view->volume = (s >> 4) & 127U;
    view->level = (s >> 11) & 127U;
    view->ready = (s >> 18) & 1U;
    view->modified = (s >> 19) & 1U;
    view->type = 0;
    view->muted = (mute_mask >> track) & 1U;
    view->solo = (solo_mask >> track) & 1U;
    view->audible = tracks[track].audible;
    view->mono = 0;
    groove_sequence_view(track, step, view);
}
__attribute__((cold, minsize)) void groove_mix_toggle(unsigned track, int solo)
{
    if (track >= GROOVE_TRACKS || track_roles[track] == GROOVE_TRACK_OFF)
        return;
    if (track_roles[track] == GROOVE_TRACK_NORMAL) groove_edit_checkpoint(0x92000U + track);
    if (track_roles[track] == GROOVE_TRACK_NORMAL) groove_project_touch();
    fm1_web_pause();
    if (solo) {
        solo_mask ^= 1U << track;
        if (solo_mask & (1U << track)) mute_mask &= ~(1U << track);
    } else {
        mute_mask ^= 1U << track;
        if (mute_mask & (1U << track)) solo_mask &= ~(1U << track);
    }
    session_mix_refresh();
    fm1_web_resume();
}
/* Notes are chromatic: the Easel has no drum pads. */
unsigned groove_key_note(unsigned track, unsigned key, int octave)
{
    (void)track;
    int note = 53 + (int)key + octave * 12;
    return (unsigned)(note < 0 ? 0 : note > 127 ? 127 : note);
}
FM1_DSP_CODE unsigned groove_pad_count(unsigned track) { (void)track; return 0; }
unsigned groove_pad_note(unsigned track, unsigned pad) { (void)track; (void)pad; return 60; }
int groove_pad_index(unsigned track, unsigned note) { (void)track; (void)note; return -1; }
/* Step parameter locks are reserved for Easel parameters; none are exposed yet. */
FM1_DSP_CODE void groove_lock_apply(unsigned track, unsigned parameter, unsigned value) { (void)track; (void)parameter; (void)value; }
FM1_DSP_CODE void groove_lock_restore(unsigned track) { (void)track; }
unsigned groove_lock_parameter_count(unsigned track) { (void)track; return 0; }
const char *groove_lock_parameter_name(unsigned track, unsigned index) { (void)track; (void)index; return ""; }
/* The Easel patch travels through the same calls the workbench used for a voice. */
int groove_fm_copy(unsigned track, uint8_t patch[156], struct groove_fm_controls *controls)
{
    if (track >= GROOVE_TRACKS || !patch) return -1;
    memset(patch, 0, 156);
    easel_serialize(patch);
    if (controls) memset(controls, 0, sizeof *controls);
    return 0;
}
int groove_fm_import(unsigned track, const uint8_t patch[156])
{
    if (track >= GROOVE_TRACKS || !patch) return -1;
    easel_deserialize(patch);
    return 0;
}
__attribute__((cold, minsize)) int groove_project_valid(const struct groove_project *p)
{
    if (!p || p->tempo < 30 || p->tempo > 300 || !groove_sequence_valid(p))
        return 0;
    for (unsigned i = 0; i < sizeof p->name; ++i)
        if (p->name[i] < 32 || p->name[i] > 126)
            return 0;
    for (unsigned i = 0; i < sizeof p->effects; ++i)
        if (p->effects[i] > (i == 1 ? GROOVE_DELAY_FREE_LAST : 127))
            return 0;
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        const struct groove_preset *v = &p->preset[t];
        /* Projects made for other sources still load: their voice data is ignored. */
        if ((p->arp[t][1] & 63U) > 50 || v->type >= GROOVE_SOURCES || v->volume > 127 || v->modified > 1 ||
            v->sequence_pages > 3)
            return 0;
    }
    return 1;
}
__attribute__((cold, minsize)) int groove_project_capture(struct groove_project *p)
{
    if (!p)
        return GROOVE_PROJECT_INVALID;
    fm1_web_pause();
    memset(p, 0, sizeof *p);
    groove_sequence_copy(p);
    p->mute = (uint8_t)mute_mask;
    p->solo = (uint8_t)solo_mask;
    char name[13];
    groove_project_name(name);
    memcpy(p->name, name, sizeof p->name);
    p->effects[0] = 64; p->effects[1] = GROOVE_DELAY_DEFAULT; p->effects[2] = 48; p->effects[3] = 100;
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        struct groove_preset *v = &p->preset[t];
        if (track_roles[t] == GROOVE_TRACK_OFF) {
            uint8_t pages = v->sequence_pages; *v = dormant[t]->preset; v->sequence_pages = pages;
            memcpy(p->arp[t], dormant[t]->arp, 2); continue;
        }
        v->type = 0;
        v->program = tracks[t].program;
        v->volume = tracks[t].volume;
        v->modified = tracks[t].modified;
        if (t == 0) easel_serialize(v->data);      /* one Easel patch per project */
        if (track_roles[t] == GROOVE_TRACK_PLAY) {
            memset(p->step[t], 0, sizeof p->step[t]);
            for (unsigned step=0;step<64;++step) p->step[t][step].note=60;
            for (unsigned i=0;i<GROOVE_LOCKS;++i) if (p->sequence.lock[i].track==t) p->sequence.lock[i].track=255;
        }
        p->arp[t][0] = arpeggio[t].rate | (arpeggio[t].mode << 2) |
            (arpeggio[t].octaves << 4) | (arpeggio[t].gate << 6);
        p->arp[t][1] = arpeggio[t].swing | (arpeggio[t].hold << 6) |
            (arpeggio[t].enabled << 7);
    }
    fm1_web_resume();
    return 0;
}
static __attribute__((cold, minsize)) void track_preset_apply(unsigned t, const struct groove_preset *v)
{
    tracks[t].type = 0;
    tracks[t].program = v->program;
    tracks[t].volume = v->volume;
    tracks[t].modified = v->modified;
    tracks[t].audible = track_audible(t);
    status[t].edit_dirty = 1;
    publish(t, 0);
}
#define SESSION_PAUSE fm1_web_pause
#define SESSION_RESUME fm1_web_resume
#include "track_session.inc"

/* Restore runs only on the control path. */
__attribute__((cold, minsize)) static int project_apply(const struct groove_project *p, unsigned operation)
{
    if (!groove_project_valid(p))
        return GROOVE_PROJECT_INVALID;
    unsigned normal = track_group(GROOVE_TRACK_NORMAL);
    fm1_web_pause();
    if (operation)
        groove_edit_checkpoint(operation);
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) if (normal & (1U << t)) {
        groove_midi_track_reset(t);
        alt_hold[t] = 0;
        memset(live[t], 0, sizeof live[t]);
        memset(&seq_note[t], 0, sizeof seq_note[t]);
        session_arp_apply(t, p->arp[t]);
    }
    /* Older projects could save both flags. Solo wins on import. */
    mute_mask = (mute_mask & ~normal) | (p->mute & ~p->solo & normal);
    solo_mask = (solo_mask & ~normal) | (p->solo & normal);
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        const struct groove_preset *v = &p->preset[t];
        if (!(normal & (1U << t))) {
            session_project_row(t, p);
            if (track_roles[t] == GROOVE_TRACK_OFF) {
                tracks[t].program = v->program; tracks[t].volume = v->volume;
                tracks[t].modified = v->modified; tracks[t].audible = 0; publish(t, 0);
            }
            continue;
        }
        track_preset_apply(t, v);
        unsigned notes = p->arp[t][0], behavior = p->arp[t][1];
        arpeggio[t].rate = notes & 3U;
        arpeggio[t].mode = (notes >> 2) & 3U;
        arpeggio[t].octaves = (notes >> 4) & 3U;
        arpeggio[t].gate = notes >> 6;
        arpeggio[t].swing = behavior & 63U;
        arpeggio[t].hold = (behavior >> 6) & 1U;
        arpeggio[t].enabled = behavior >> 7;
    }
    /* The Easel patch lives in row one. Projects from other instruments carry no
     * Easel magic, so the current sound stays as it is. */
    easel_panic();
    easel_deserialize(p->preset[0].data);
    groove_sequence_restore(p);
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t)
        if (normal & (1U << t)) groove_edit_track_reset(t);
    groove_project_metadata(p);
    fm1_web_resume();
    return 0;
}
int groove_project_apply(const struct groove_project *p)
{
    return project_apply(p, 0);
}
int groove_project_apply_edit(const struct groove_project *p, unsigned operation)
{
    return project_apply(p, operation);
}
