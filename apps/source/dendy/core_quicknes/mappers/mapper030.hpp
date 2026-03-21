/*
 * QuickNES - NES emulator core
 * Original author: Shay Green (blargg)
 * https://github.com/libretro/QuickNES_Core
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Fork maintained as part of MurmNES by Mikhail Matveev.
 * https://rh1.tech | https://github.com/rh1tech/murmnes
 */

#pragma once

#include "nes_mapper.h"

// Unrom512 

class Mapper030 : public Nes_Mapper {
public:
	Mapper030() { }

	void reset_state() { }

	void apply_mapping() { }

	void write( nes_time_t, nes_addr_t addr, int data )
	{
		if ( ( addr & 0xF000 ) >= 0x8000 )
		{
			set_prg_bank(0x8000, bank_16k, data & 0x1F);
			set_chr_bank(0x0000, bank_8k, (data >> 5) & 0x3);
		}
	}
};

