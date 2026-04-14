#ifndef MANUL_FONT_H
#define MANUL_FONT_H

#include <stdint.h>
#include <stdbool.h>

/* Leggie 6x12 for body text, 9x18 for headings */
#define LFONT_W   6
#define LFONT_H  12
#define HFONT_W   9
#define HFONT_H  18

void cfont_init(void);

/* 6x12 glyph row — works for ALL chars 0-255 (ASCII + Win1251 Cyrillic) */
uint8_t cfont_row(uint8_t ch, uint8_t row, bool bold);

/* 9x18 heading glyph — returns 2 bytes: high byte has bits 15..8,
 * low byte has bit 7 (9th pixel in bit 7). */
uint16_t cfont_heading_row(uint8_t ch, uint8_t row);

#endif
