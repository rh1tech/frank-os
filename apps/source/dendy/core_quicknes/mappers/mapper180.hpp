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
 
// UxROM (inverted)

class Mapper180 : public Nes_Mapper {
public:
	Mapper180()
	{
		register_state( &bank, 1 );
	}

	virtual void reset_state()
	{ }

	virtual void apply_mapping()
	{
		set_prg_bank( 0x8000, bank_16k, 0 );
		write( 0, 0, bank );
	}

	virtual void write( nes_time_t, nes_addr_t, int data )
	{
		bank = data;
		set_prg_bank( 0xC000, bank_16k, data );
	}

	uint8_t bank;
};
