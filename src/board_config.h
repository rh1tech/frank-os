#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include "hardware/vreg.h"

/*
 * Board Configuration for Rhea OS
 *
 * M2-only build (M1 is no longer supported).
 *
 * M2 GPIO Layout:
 *   HDMI (HSTX): CLK-=12, CLK+=13, D0-=14, D0+=15, D1-=16, D1+=17, D2-=18, D2+=19
 *   PS/2 Kbd:    CLK=2, DATA=3
 *   PS/2 Mouse:  CLK=0, DATA=1
 */

#define BOARD_M2

//=============================================================================
// CPU Speed Defaults
//=============================================================================
#ifndef CPU_CLOCK_MHZ
#define CPU_CLOCK_MHZ 252
#endif

#ifndef CPU_VOLTAGE
#define CPU_VOLTAGE VREG_VOLTAGE_1_50
#endif

//=============================================================================
// M2 Layout Configuration
//=============================================================================
#define PS2_PIN_CLK  2
#define PS2_PIN_DATA 3

#define PS2_MOUSE_CLK  0
#define PS2_MOUSE_DATA 1

#endif // BOARD_CONFIG_H
