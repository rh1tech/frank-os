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

// https://www.nesdev.org/wiki/INES_Mapper244

struct mapper244_state_t
{
	uint8_t preg;
	uint8_t creg;
};

BOOST_STATIC_ASSERT( sizeof (mapper244_state_t) == 2 );

class Mapper244 : public Nes_Mapper, mapper244_state_t {
public:
	Mapper244()
	{
		mapper244_state_t *state = this;
		register_state( state, sizeof *state );
	}

	virtual void reset_state()
	{ }

	virtual void apply_mapping()
	{
		set_prg_bank( 0x8000, bank_32k, preg );
		set_chr_bank( 0x0000, bank_8k, creg );
	}

	virtual void write( nes_time_t, nes_addr_t addr, int data )
	{
		if ( addr >= 0x8065 && addr <= 0x80A4 )
		{
			preg = ( addr - 0x8065 ) & 0x03;
			set_prg_bank( 0x8000, bank_32k, preg );
		}

		if ( addr >= 0x80A5 && addr <= 0x80E4 )
		{
			creg = (addr - 0x80A5 ) & 0x07;
			set_chr_bank( 0x0000, bank_8k, creg );
		}
	}
};
