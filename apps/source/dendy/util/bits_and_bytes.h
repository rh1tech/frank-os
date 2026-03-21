/*
 * MurmNES - NES Emulator for RP2350
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech | https://github.com/rh1tech/murmnes
 * SPDX-License-Identifier: MIT
 */

#ifndef __BITS_AND_BYTES__
#define __BITS_AND_BYTES__

#include <stdint.h>

unsigned get_nth_bit(unsigned input, unsigned bit_pos); // Only returns a 0 or 1
uint16_t append_hi_byte_to_lo_byte(uint8_t hi_byte, uint8_t lo_byte);

#endif /* __BITS_AND_BYTES__ */
