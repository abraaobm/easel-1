/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "instrument.h"
#include "fm1_engine_control.h"
#include <string.h>
#ifdef FM1_HOST_TEST
#include <stdatomic.h>
#define WORD _Atomic(unsigned)
#else
#define WORD volatile unsigned
#endif

/* ... (o resto do arquivo midi_router.c permanece igual até a função groove_midi_note) ... */

/* Modificação para o Easel-1 Web */
FM1_AUDIO_CODE void groove_midi_note(unsigned track, unsigned note, unsigned velocity)
{
#ifdef FM1_OPEN_WEB
    // Desvia todas as notas (Sequenciador, Arp, Teclas) para o nosso motor West Coast
    extern void easel_note_on(uint8_t note, uint8_t vel);
    easel_note_on((uint8_t)note, (uint8_t)velocity);
#endif
    if (!groove_midi_mode(track)) return;
    unsigned ch = (config.channel[track >> 1] >> (4U * (track & 1U))) & 15U;
    enqueue(track & 1U, track, config.flags & 7U, (velocity ? 0x90U : 0x80U) | ch, note, velocity);
}

/* ... (o resto do arquivo midi_router.c permanece igual) ... */
