/*
 * QuickNES - NES emulator core
 * Original author: Shay Green (blargg)
 * https://github.com/libretro/QuickNES_Core
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Fork maintained as part of MurmNES by Mikhail Matveev.
 * https://rh1.tech | https://github.com/rh1tech/murmnes
 */

#include "nes_cart.h"

#include <stdio.h>
#include <stdlib.h>
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

char const Nes_Cart::not_ines_file [] = "Not an iNES file";

Nes_Cart::Nes_Cart()
{
	prg_ = NULL;
	chr_ = NULL;
	prg_owned_ = true;
	chr_owned_ = true;
	clear();
}

Nes_Cart::~Nes_Cart()
{
	clear();
}

void Nes_Cart::clear()
{
	if ( prg_owned_ && prg_ )
		free( prg_ );
	if ( chr_owned_ && chr_ )
		free( chr_ );
	prg_ = NULL;
	chr_ = NULL;
	prg_owned_ = true;
	chr_owned_ = true;

	prg_size_ = 0;
	chr_size_ = 0;
	mapper = 0;
}

long Nes_Cart::round_to_bank_size( long n )
{
	n += bank_size - 1;
	return n - n % bank_size;
}

const char * Nes_Cart::resize_prg( long size )
{
	if ( size != prg_size_ )
	{
		// padding allows CPU to always read operands of instruction, which
		// might go past end of data
		void* p = realloc( prg_, round_to_bank_size( size ) + 2 );
		CHECK_ALLOC( p || !size );
		prg_ = (uint8_t*) p;
		prg_size_ = size;
	}
	return 0;
}

const char * Nes_Cart::resize_chr( long size )
{
	if ( size != chr_size_ )
	{
		void* p = realloc( chr_, round_to_bank_size( size ) );
		CHECK_ALLOC( p || !size );
		chr_ = (uint8_t*) p;
		chr_size_ = size;
	}
	return 0;
}

// iNES reading

struct ines_header_t {
	uint8_t signature [4];
	uint8_t prg_count; // number of 16K PRG banks
	uint8_t chr_count; // number of 8K CHR banks
	uint8_t flags;     // MMMM FTBV Mapper low, Four-screen, Trainer, Battery, V mirror
	uint8_t flags2;    // MMMM --XX Mapper high 4 bits
	uint8_t zero [8];  // if zero [7] is non-zero, treat flags2 as zero
};
BOOST_STATIC_ASSERT( sizeof (ines_header_t) == 16 );

const char * Nes_Cart::load_ines( Auto_File_Reader in )
{
	RETURN_ERR( in.open() );
	
	ines_header_t h;
	RETURN_ERR( in->read( &h, sizeof h ) );
	
	if ( 0 != memcmp( h.signature, "NES\x1A", 4 ) )
		return not_ines_file;
	
	if ( h.zero [7] ) // handle header defaced by a fucking idiot's handle
		h.flags2 = 0;
	
	set_mapper( h.flags, h.flags2 );
	
	if ( h.flags & 0x04 ) // skip trainer
		RETURN_ERR( in->skip( 512 ) );
	
	RETURN_ERR( resize_prg( h.prg_count * 16 * 1024L ) );
	RETURN_ERR( resize_chr( h.chr_count * 8 * 1024L ) );
	
	RETURN_ERR( in->read( prg(), prg_size() ) );
	RETURN_ERR( in->read( chr(), chr_size() ) );

	return 0;
}

const char * Nes_Cart::load_ines_data( const void* data, long size )
{
	clear();

	const uint8_t* p = (const uint8_t*) data;
	if ( size < 16 || 0 != memcmp( p, "NES\x1A", 4 ) )
		return not_ines_file;

	ines_header_t h;
	memcpy( &h, p, sizeof h );
	p += 16;

	if ( h.zero [7] )
		h.flags2 = 0;

	set_mapper( h.flags, h.flags2 );
	printf("  mapper=%d prg_banks=%d chr_banks=%d\n", mapper, h.prg_count, h.chr_count);

	if ( h.flags & 0x04 )
		p += 512; // skip trainer

	long prg_bytes = h.prg_count * 16 * 1024L;
	long chr_bytes = h.chr_count * 8 * 1024L;

	if ( p + prg_bytes + chr_bytes > (const uint8_t*) data + size )
		return "ROM file too small";

	/* Keep PRG in-place (PSRAM/flash) — the XIP cache provides adequate
	   hit rates for 6502 hot loops, and copying to SRAM would exhaust
	   the limited ~77KB heap that the PPU and tile cache also need. */
	prg_size_ = prg_bytes;
	prg_ = (uint8_t*) p;
	prg_owned_ = false;
	printf("  PRG %ldKB -> in-place\n", prg_bytes/1024);
	p += prg_bytes;

	/* CHR stays zero-copy — it is only accessed indirectly through the
	   decoded tile cache, so QMI/PSRAM latency is tolerable. */
	chr_ = (uint8_t*) p;
	chr_size_ = chr_bytes;
	chr_owned_ = false;
	return 0;
}
