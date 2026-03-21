/*
 * QuickNES - NES emulator core
 * Original author: Shay Green (blargg)
 * https://github.com/libretro/QuickNES_Core
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Fork maintained as part of MurmNES by Mikhail Matveev.
 * https://rh1.tech | https://github.com/rh1tech/murmnes
 */

#include "nes_util.h"

#include "nes_cart.h"
#include "nes_emu.h"
#include <ctype.h>
#include <string.h>

/* Copyright (C) 2004-2006 Shay Green. This module is free software; you
can redistribute it and/or modify it under the terms of the GNU Lesser
General Public License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version. This
module is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
more details. You should have received a copy of the GNU Lesser General
Public License along with this module; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA */

#include "blargg_source.h"

// game_genie_patch_t

const char *game_genie_patch_t::decode( const char* in )
{
	int const code_len = 8;
	unsigned char result [code_len] = { 0 };
	int in_len = strlen( in );
	if ( in_len != 6 && in_len != 8 )
		return "Game Genie code is wrong length";
	for ( int i = 0; i < code_len; i++ )
	{
		char c = 'A';
		if ( i < in_len )
			c = toupper( in [i] );
		
		static char const letters [17] = "AEPOZXLUGKISTVYN";
		char const* p = strchr( (char*) letters, c );
		if ( !p )
			return "Game Genie code had invalid character";
		int n = p - letters;
		
		result [i] |= n >> 1;
		result [(i + 1) % code_len] |= (n << 3) & 0x0f;
	}
	
	addr = result [3]<<12 | result [5]<<8 | result [2]<<4 | result [4];
	change_to = result [1]<<4 | result [0];
	compare_with = -1;
	if ( addr & 0x8000 )
		compare_with = result [7]<<4 | result [6];
	addr |= 0x8000;
	
	return 0;
}

int game_genie_patch_t::apply( Nes_Cart& cart ) const
{
	// determine bank size
	long bank_size = 32 * 1024L; // mappers 0, 2, 3, 7, 11, 34, 71, 87
	switch ( cart.mapper_code() )
	{
	case 1:   // MMC1
	case 71:  // Camerica
	case 232: // Quattro
		bank_size = 16 * 1024L;
		break;
	
	case 4:  // MMC3
	case 5:  // MMC5
	case 24: // VRC6
	case 26: // VRC6
	case 69: // FME7
		bank_size = 8 * 1024L;
		break;
	}
	
	// patch each bank (not very good, since it might patch banks that never occupy
	// that address)
	int mask = (compare_with >= 0 ? ~0 : 0);
	uint8_t* p = cart.prg() + addr % bank_size;
	int count = 0;
	for ( int n = cart.prg_size() / bank_size; n--; p += bank_size )
	{
		if ( !((*p ^ compare_with) & mask) )
		{
			*p = change_to;
			count++;
		}
	}
	return count;
}

// Cheat_Value_Finder

Cheat_Value_Finder::Cheat_Value_Finder()
{
	emu = NULL;
}

void Cheat_Value_Finder::start( Nes_Emu* new_emu )
{
	emu = new_emu;
	pos = 0;
	memcpy( original, emu->low_mem(), low_mem_size );
	memset( changed, 0, low_mem_size );
}

void Cheat_Value_Finder::rescan()
{
	uint8_t const* low_mem = emu->low_mem();
	for ( int i = 0; i < low_mem_size; i++ )
		changed [i] |= original [i] ^ low_mem [i];
	memcpy( original, emu->low_mem(), low_mem_size );
}

void Cheat_Value_Finder::search( int new_original, int new_changed )
{
	original_value = new_original;
	changed_value = new_changed;
	pos = -1;
}

int Cheat_Value_Finder::next_match( int* addr )
{
	uint8_t const* low_mem = emu->low_mem();
	while ( ++pos < low_mem_size )
	{
		if ( !changed [pos] )
		{
			int old = (original [pos] - original_value) & 0xff;
			int cur = (low_mem [pos] - changed_value) & 0xff;
			
			if ( old == cur )
			{
				if ( addr )
					*addr = pos;
				return (char) old; // sign-extend
			}
		}
	}
	
	return no_match;
}

int Cheat_Value_Finder::change_value( int new_value )
{
	int result = emu->low_mem() [pos];
	emu->low_mem() [pos] = new_value;
	return result;
}
