/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "instrument.h"
#include <string.h>

/* Easel-1 factory projects: one per factory card. A small recipe (tempo, key,
 * pattern) expands into the normal editable project format, so no 4 KiB images
 * sit in program Flash. Nothing here touches the live instrument.
 * Notes are MIDI pitches; masks read from bit 0 (the first sixteenth note). */
extern unsigned easel_preset_count(void);
extern const char *easel_preset_name(unsigned index);
extern void easel_card_serialize(unsigned index, uint8_t out[156]);

enum { PATTERN_NONE, PATTERN_HITS, PATTERN_SPARSE, PATTERN_BELLS, PATTERN_LEAD, PATTERN_HELD,
       PATTERN_STABS, PATTERN_ARP, PATTERN_CHAOS, PATTERN_DRONE };
struct scene { uint8_t bpm, root, minor, pattern, gate; };
/* Index matches the card list in web/engine/Easel.c. */
static const struct scene scenes[GROOVE_FACTORY_PROJECTS] = {
    {100, 45, 1, PATTERN_NONE,   0},  /* INIT */
    {108, 45, 1, PATTERN_HITS,   5},  /* BONGO BLOOP */
    { 84, 57, 0, PATTERN_SPARSE, 7},  /* WATER DROPS */
    { 76, 57, 1, PATTERN_BELLS,  9},  /* FM BELLS */
    {112, 45, 1, PATTERN_LEAD,   9},  /* BUCHLA LEAD */
    { 70, 60, 0, PATTERN_SPARSE, 7},  /* WIND CHIMES */
    {120, 36, 1, PATTERN_HITS,   5},  /* SUB THUMP */
    { 90, 45, 1, PATTERN_HELD,  15},  /* SLOW WOBBLE */
    {120, 48, 1, PATTERN_HELD,  15},  /* EUCLID BLIPS */
    {100, 48, 1, PATTERN_STABS,  7},  /* RING METAL */
    { 96, 52, 0, PATTERN_LEAD,   9},  /* VEL GLASS */
    { 60, 36, 1, PATTERN_DRONE, 15},  /* DRONE CAVE */
    {118, 48, 1, PATTERN_ARP,    7},  /* PHASE PLUCK */
    {128, 48, 1, PATTERN_CHAOS,  5},  /* CHAOS SPARKS */
};
static const uint16_t masks[] = {
    0x0000, 0x4949 /* hits */, 0x0121 /* sparse */, 0x4141 /* bells */, 0xdddd /* lead */,
    0x0001 /* held */, 0x0909 /* stabs */, 0x5555 /* arp */, 0xb6d5 /* chaos */, 0x0001 /* drone */};
static const uint8_t degrees[2][7] = {{0, 2, 4, 7, 9, 12, 14}, {0, 3, 5, 7, 10, 12, 15}};

const char *groove_factory_name(unsigned index)
{
    return index < GROOVE_FACTORY_PROJECTS && index < easel_preset_count() ? easel_preset_name(index) : NULL;
}
int groove_factory_build(unsigned index, struct groove_project *p)
{
    if (!p || index >= GROOVE_FACTORY_PROJECTS || index >= easel_preset_count()) return GROOVE_PROJECT_INVALID;
    const struct scene *s = &scenes[index];
    memset(p, 0, sizeof *p);
    const char *name = easel_preset_name(index);
    memset(p->name, ' ', sizeof p->name);
    memcpy(p->name, name, strlen(name) < sizeof p->name ? strlen(name) : sizeof p->name);
    p->tempo = s->bpm;
    p->mute = 0xf0U;                       /* tracks 5..8 stay empty and muted */
    p->effects[0] = 64; p->effects[1] = GROOVE_DELAY_DEFAULT; p->effects[2] = 48; p->effects[3] = 100;
    groove_sequence_defaults(&p->sequence);
    groove_project_arp_defaults(p);
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        p->preset[t].type = 0;
        p->preset[t].volume = 100;
        p->preset[t].program = (uint8_t)index;
    }
    easel_card_serialize(index, p->preset[0].data);   /* the card travels with the project */
    unsigned mask = masks[s->pattern];
    if (s->pattern != PATTERN_NONE)
        for (unsigned bar = 0; bar < 4; ++bar)
            for (unsigned i = 0; i < 16; ++i) {
                if (!(mask & (1U << i))) continue;
                if (s->pattern == PATTERN_DRONE && (bar || i)) continue;
                struct groove_step *n = &p->step[0][bar * 16 + i];
                unsigned degree = 0, tie = 0, gate = s->gate, root = s->root;
                switch (s->pattern) {
                case PATTERN_SPARSE: degree = (bar * 3 + i / 4 + 1) % 6; root += 12; break;
                case PATTERN_BELLS:  degree = (bar + i / 6 + 2) % 7; break;
                case PATTERN_LEAD:   degree = (i * 3 + bar * 2) % 7; break;
                case PATTERN_STABS:  degree = i == 3 || i == 11 ? 3 : 0; break;
                case PATTERN_ARP:    degree = (i / 2 + bar) % 4 == 3 ? 5 : (i / 2 + bar) % 4 * 2 % 7; break;
                case PATTERN_CHAOS:  degree = (i * 5 + bar * 3) % 7; break;
                case PATTERN_HELD:   degree = bar & 1U ? 3 : 0; tie = 1; break;
                case PATTERN_DRONE:  tie = 1; break;
                default: break;                                  /* hits stay on the root */
                }
                n->note = (uint8_t)(root + degrees[s->minor][degree]);
                n->velocity = (uint8_t)(i % 4 == 0 ? 104 : i % 2 == 0 ? 88 : 72);
                n->drums = (uint16_t)((gate << 5) | (tie << 12));
            }
    return groove_project_valid(p) ? 0 : GROOVE_PROJECT_INVALID;
}
