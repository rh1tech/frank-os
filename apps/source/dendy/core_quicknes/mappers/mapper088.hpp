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

struct namco_34x3_state_t
{
	uint8_t bank [ 8 ];
	uint8_t mirr;
	uint8_t mode;
};

BOOST_STATIC_ASSERT( sizeof (namco_34x3_state_t) == 10 );

template < bool _is154 >
class Mapper_Namco_34x3 : public Nes_Mapper, namco_34x3_state_t {
public:
	Mapper_Namco_34x3()
	{
		namco_34x3_state_t *state = this;
		register_state( state, sizeof *state );
	}

	virtual void reset_state()
	{ }

	virtual void apply_mapping()
	{
		set_chr_bank( 0x0000, bank_2k, bank [ 0 ] );
		set_chr_bank( 0x0800, bank_2k, bank [ 1 ] );
		for ( int i = 0; i < 4; i++ )
			set_chr_bank( 0x1000 + ( i << 10 ), bank_1k, bank [ i + 2 ] );

		set_prg_bank( 0x8000, bank_8k, bank [ 6 ] );
		set_prg_bank( 0xA000, bank_8k, bank [ 7 ] );
		set_prg_bank( 0xC000, bank_8k, ~1 );
		set_prg_bank( 0xE000, bank_8k, ~0 );

		if ( _is154 )
			mirror_single( mirr );
	}

	virtual void write( nes_time_t, nes_addr_t addr, int data )
	{
		switch ( addr & 0xE001 )
		{
		case 0x8000:
			mode = data;
			mirr = ( data >> 6 ) & 0x01;
			if ( _is154 )
				mirror_single( mirr );
			break;
		case 0x8001:
			mode &= 0x07;
			switch ( mode )
			{
			case 0: case 1:
				bank [ mode ] = data >> 1;
				set_chr_bank( 0x0000 + ( mode << 11 ), bank_2k, bank [ mode ] );
				break;
			case 2: case 3: case 4: case 5:
				bank [ mode ] = data | 0x40;
				set_chr_bank( 0x1000 + ( ( mode - 2 ) << 10 ), bank_1k, bank [ mode ] );
				break;
			case 6: case 7:
				bank [ mode ] = data;
				set_prg_bank( 0x8000 + ( ( mode - 6 ) << 13 ), bank_8k, bank [ mode ] );
				break;
			}
			break;
		case 0xC000:
			mirr = ( data >> 6 ) & 0x01;
			if ( _is154 )
				mirror_single( mirr );
		}
	}
};

typedef Mapper_Namco_34x3<false> Mapper088;


// void register_mapper_namco_34xx();
// void register_mapper_namco_34xx()
// {
// 	register_mapper< Mapper_Namco_34x3 <false> > ( 88 );
// 	register_mapper< Mapper_Namco_34x3 <true> > ( 154 );
// 	register_mapper< Mapper_Namco_34xx > ( 206 );
// }
