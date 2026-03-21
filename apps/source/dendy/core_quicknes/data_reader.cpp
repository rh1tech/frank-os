/*
 * QuickNES - NES emulator core
 * Original author: Shay Green (blargg)
 * https://github.com/libretro/QuickNES_Core
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Fork maintained as part of MurmNES by Mikhail Matveev.
 * https://rh1.tech | https://github.com/rh1tech/murmnes
 */

#include "data_reader.h"

#include "blargg_endian.h"
#include <errno.h>

/* Copyright (C) 2005-2009 Shay Green. This module is free software; you
can redistribute it and/or modify it under the terms of the GNU Lesser
General Public License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version. This
module is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more
details. You should have received a copy of the GNU Lesser General Public
License along with this module; if not, write to the Free Software Foundation,
Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA */

#include "blargg_source.h"

// Data_Reader

const char * Data_Reader::read( void* p, int n )
{
	if ( n < 0 )
		return "Internal usage bug";
	
	if ( n <= 0 )
		return 0;
	
	if ( n > remain() )
		return "Truncated file";
	
	const char * err = read_v( p, n );
	if ( !err )
		remain_ -= n;
	
	return err;
}

const char * Data_Reader::read_avail( void* p, int* n_ )
{
	int n = min( (uint64_t)(*n_), remain() );
	*n_ = 0;
	
	if ( n < 0 )
		return "Internal usage bug";
	
	if ( n <= 0 )
		return 0;
	
	const char * err = read_v( p, n );
	if ( !err )
	{
		remain_ -= n;
		*n_ = n;
	}
	
	return err;
}

const char * Data_Reader::read_avail( void* p, long* n )
{
	int i = STATIC_CAST(int, *n);
	const char * err = read_avail( p, &i );
	*n = i;
	return err;
}

const char * Data_Reader::skip_v( int count )
{
	char buf [512];
	while ( count )
	{
		int n = min( count, (int) sizeof buf );
		count -= n;
		RETURN_ERR( read_v( buf, n ) );
	}
	return 0;
}

const char * Data_Reader::skip( int n )
{
	if ( n < 0 )
		return "Internal usage bug";
	
	if ( n <= 0 )
		return 0;
	
	if ( n > remain() )
		return "Truncated file";
	
	const char * err = skip_v( n );
	if ( !err )
		remain_ -= n;
	
	return err;
}


// File_Reader

// Mem_File_Reader

Mem_File_Reader::Mem_File_Reader( const void* p, long s ) :
	begin( STATIC_CAST(const char*, p) )
{
	set_size( s );
}

const char * Mem_File_Reader::read_v( void* p, int s )
{
	memcpy( p, begin + tell(), s );
	return 0;
}
