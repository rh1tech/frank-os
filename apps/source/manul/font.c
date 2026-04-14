/*
 * Manul — Leggie font rendering (6x12 regular/bold + 9x18 heading)
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 *
 * Uses the Leggie bitmap font by W. Kerr (CC BY 4.0).
 * Pre-generated C arrays with Win1251 character mapping.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "font.h"

#include "font_leggie_regular.h"
#include "font_leggie_bold.h"
#include "font_leggie_heading.h"

void cfont_init(void) {
    /* Font data is const — nothing to initialize */
}

uint8_t cfont_row(uint8_t ch, uint8_t row, bool bold) {
    if (row >= 12) return 0;
    if (bold)
        return font_leggie_bold[(uint16_t)ch * 12 + row];
    return font_leggie_regular[(uint16_t)ch * 12 + row];
}

uint16_t cfont_heading_row(uint8_t ch, uint8_t row) {
    if (row >= 18) return 0;
    /* 2 bytes per row: high byte first, low byte second */
    uint16_t off = (uint16_t)ch * 18 * 2 + row * 2;
    return ((uint16_t)font_leggie_heading[off] << 8) |
            (uint16_t)font_leggie_heading[off + 1];
}
