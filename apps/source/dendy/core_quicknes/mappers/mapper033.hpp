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

struct tc0190_state_t
{
	uint8_t preg [ 2 ];
	uint8_t creg [ 6 ];
	uint8_t mirr;
};

BOOST_STATIC_ASSERT( sizeof ( tc0190_state_t ) == 9 );

// TaitoTC0190

class Mapper033 : public Nes_Mapper, tc0190_state_t {
public:
	Mapper033()
	{
		tc0190_state_t *state = this;
		register_state( state, sizeof *state );
	}

	virtual void reset_state()
	{ }

	virtual void apply_mapping()
	{
		for ( int i = 0; i < 2; i++ )
		{
			set_prg_bank ( 0x8000 + ( i << 13 ), bank_8k, preg [ i ] );
			set_chr_bank ( 0x0000 + ( i << 11 ), bank_2k, creg [ i ] );
		}

		for ( int i = 0; i < 4; i++ )
			set_chr_bank ( 0x1000 + ( i << 10 ), bank_1k, creg [ 2 + i ] );

		if ( mirr ) mirror_horiz();
		else mirror_vert();
	}

	virtual void write( nes_time_t, nes_addr_t addr, int data )
	{
		switch ( addr & 0xA003 )
		{
		case 0x8000:
			preg [ 0 ] = data & 0x3F;
			mirr = data >> 6;
			set_prg_bank ( 0x8000, bank_8k, preg [ 0 ] );
			if ( mirr ) mirror_horiz();
			else mirror_vert();
			break;
		case 0x8001:
			preg [ 1 ] = data & 0x3F;
			set_prg_bank ( 0xA000, bank_8k, preg [ 1 ] );
			break;
		case 0x8002: case 0x8003:
			addr &= 0x01;
			creg [ addr ] = data;
			set_chr_bank ( addr << 11, bank_2k, creg [ addr ] );
			break;
		case 0xA000: case 0xA001:
		case 0xA002: case 0xA003:
			addr &= 0x03;
			creg [ 2 + addr ] = data;
			set_chr_bank ( 0x1000 | ( addr << 10 ), bank_1k, creg [ 2 + addr ] );
			break;
		}
	}
};
