/*
 * FRANK OS
 * Copyright (c) 2025 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include "hardware/vreg.h"

/*
 * Board Configuration for FRANK OS
 *
 * M2-only build (M1 is no longer supported).
 *
 * M2 GPIO Layout:
 *   PS/2 Mouse:  CLK=0, DATA=1
 *   PS/2 Kbd:    CLK=2, DATA=3
 *   SD Card:     MISO=4, CSn=5, SCK=6, MOSI=7
 *   PSRAM:       CS=8
 *   I2S Audio:   DATA=9, BCLK=10, LRCLK=11
 *   HDMI (HSTX): CLK-=12, CLK+=13, D0-=14, D0+=15, D1-=16, D1+=17, D2-=18, D2+=19
 */

#define BOARD_M2

//=============================================================================
// CPU Speed Defaults
//=============================================================================
#ifndef CPU_CLOCK_MHZ
#define CPU_CLOCK_MHZ 504
#endif

#ifndef CPU_VOLTAGE
#define CPU_VOLTAGE VREG_VOLTAGE_1_65
#endif

//=============================================================================
// M2 Layout Configuration
//=============================================================================
#define PS2_PIN_CLK  2
#define PS2_PIN_DATA 3

#define PS2_MOUSE_CLK  0
#define PS2_MOUSE_DATA 1

//=============================================================================
// SD Card (SPI0, GPIO 4-7)
//=============================================================================
#define SDCARD_PIN_CLK  6   /* SPI0 SCK */
#define SDCARD_PIN_CMD  7   /* SPI0 TX / MOSI */
#define SDCARD_PIN_D0   4   /* SPI0 RX / MISO */
#define SDCARD_PIN_D3   5   /* SPI0 CSn */

//=============================================================================
// PSRAM (QSPI CS1 — pin depends on RP2350 package variant)
//   RP2350A (QFN-60, 30 GPIO): GPIO 8  (M2 board layout)
//   RP2350B (QFN-80, 48 GPIO): GPIO 47
//=============================================================================
#define PSRAM_PIN_RP2350A 8
#define PSRAM_PIN_RP2350B 47

#if PICO_RP2350
#include "hardware/structs/sysinfo.h"
static inline uint get_psram_pin(void) {
    uint32_t package_sel = *((io_ro_32 *)(SYSINFO_BASE + SYSINFO_PACKAGE_SEL_OFFSET));
    if (package_sel & 1) {
        return PSRAM_PIN_RP2350A;
    } else {
        return PSRAM_PIN_RP2350B;
    }
}
#endif

//=============================================================================
// Audio — I2S via PIO1 (GPIO 9/10/11, matching M2 board layout)
//=============================================================================
#define I2S_DATA_PIN       9    /* I2S serial data */
#define I2S_CLOCK_PIN_BASE 10   /* BCLK=10, LRCLK=11 */

#endif // BOARD_CONFIG_H
