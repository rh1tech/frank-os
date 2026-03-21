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
 
// Irem_Tam_S1 

class Mapper097 : public Nes_Mapper {
public:
	Mapper097()
	{
		register_state( &bank, 1 );
	}

	virtual void reset_state()
	{
		bank = ~0;
	}

	virtual void apply_mapping()
	{
		write( 0, 0, bank );
	}

	virtual void write( nes_time_t, nes_addr_t, int data )
	{
		bank = data;
		set_prg_bank( 0x8000, bank_16k, ~0 );
		set_prg_bank( 0xC000, bank_16k, bank & 0x0F );

		switch ( ( bank >> 6 ) & 0x03 )
		{
		case 1: mirror_horiz(); break;
		case 2: mirror_vert(); break;
		case 0:
		case 3: mirror_single( bank & 0x01 ); break;
		}
	}

	uint8_t bank;
};
