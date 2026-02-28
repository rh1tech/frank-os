/*
 * frankos_timer.c - FRANK OS Timer/Frame Sync Backend for Digger
 *
 * Replaces rp2350_timer.c. Uses pcm_write() blocking as frame pacing
 * (same proven pattern as ZX Spectrum), plus vTaskDelay for olddelay().
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "m-os-api.h"
#include "frankos-app.h"

#undef switch
#undef inline
#undef __force_inline
#undef abs

#include <stdint.h>
#include <stdbool.h>

#include "def.h"
#include "sound.h"
#include "game.h"
#include "frankos_app.h"

/* Global app state */
extern digger_app_t *g_app;

/* Audio fill from frankos_snd.c */
extern void audio_fill_and_submit(void);

/*
 * inittimer - No-op (pacing comes from pcm_write blocking).
 */
void inittimer(void) {
}

/*
 * gethrt - Frame synchronization.
 *
 * Calls audio_fill_and_submit() which blocks via pcm_write() until the
 * DMA buffer is free, naturally throttling the game to real-time.
 * Also triggers a screen update and checks the closing flag.
 */
void gethrt(bool minsleep) {
    if (!g_app)
        return;

    /* Check if window is being closed or restart requested */
    if (g_app->closing || g_app->restart) {
        extern bool escape;
        escape = true;
        return;
    }

    /* Update menu checkmarks if sound/music state changed */
    {
        static bool prev_sound = true, prev_music = true;
        if (soundflag != prev_sound || musicflag != prev_music) {
            prev_sound = soundflag;
            prev_music = musicflag;
            digger_update_menu();
        }
    }

    /* Trigger screen repaint first, before audio blocks for pacing */
    doscreenupdate();

    /* Generate and submit audio samples - blocks for pacing */
    audio_fill_and_submit();
}

/*
 * getkips - Return processor speed estimate.
 * Returns 1 (stub, same as SDL/RP2350 versions).
 */
int32_t getkips(void) {
    return 1;
}

/*
 * olddelay - Delay in milliseconds using FreeRTOS.
 */
void olddelay(int16_t t) {
    if (t > 0)
        vTaskDelay(pdMS_TO_TICKS(t));
}

/* Sound hardware stubs (timer-based sound control not used) */
void s0soundoff(void) {}
void s0setspkrt2(void) {}
void s0settimer0(uint16_t t0v) { (void)t0v; }
void s0settimer2(uint16_t t0v, bool mode) { (void)t0v; (void)mode; }
void s0timer0(uint16_t t0v) { (void)t0v; }
void s0timer2(uint16_t t0v, bool mode) { (void)t0v; (void)mode; }
void s0soundinitglob(void) {}
void s0soundkillglob(void) {}
