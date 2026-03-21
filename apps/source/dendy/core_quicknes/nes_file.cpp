/*
 * QuickNES - NES emulator core
 * Original author: Shay Green (blargg)
 * https://github.com/libretro/QuickNES_Core
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Fork maintained as part of MurmNES by Mikhail Matveev.
 * https://rh1.tech | https://github.com/rh1tech/murmnes
 */

#include "nes_file.h"

#include "blargg_endian.h"

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

// Nes_File_Writer

Nes_File_Writer::Nes_File_Writer()
{
	write_remain = 0;
	depth_ = 0;
}

Nes_File_Writer::~Nes_File_Writer()
{
}
	
const char * Nes_File_Writer::begin( Auto_File_Writer dw, nes_tag_t tag )
{
	out = dw;
	RETURN_ERR( out.open_comp() );
	return begin_group( tag );
}

const char * Nes_File_Writer::begin_group( nes_tag_t tag )
{
	depth_++;
	return write_header( tag, group_begin_size );
}

const char * Nes_File_Writer::write_header( nes_tag_t tag, long size )
{
	nes_block_t h;
	h.tag = tag;
	h.size = size;
	h.swap();
	return out->write( &h, sizeof h );
}

const char * Nes_File_Writer::write_block( nes_tag_t tag, void const* data, long size )
{
	RETURN_ERR( write_block_header( tag, size ) );
	return write( data, size );
}

const char * Nes_File_Writer::write_block_header( nes_tag_t tag, long size )
{
	write_remain = size;
	return write_header( tag, size );
}

const char *Nes_File_Writer::write( void const* p, long s )
{
	write_remain -= s;
	return out->write( p, s );
}

const char * Nes_File_Writer::end()
{
	return end_group();
}

const char * Nes_File_Writer::end_group()
{
	depth_--;
	return write_header( group_end_tag, 0 );
}

// Nes_File_Reader

Nes_File_Reader::Nes_File_Reader()
{
	h.tag = 0;
	h.size = 0;
	block_type_ = invalid;
	depth_ = -1;
}

Nes_File_Reader::~Nes_File_Reader()
{
}
	
const char * Nes_File_Reader::read_block_data( void* p, long s )
{
	long extra = remain();
	if ( s > extra )
		s = extra;
	extra -= s;
	RETURN_ERR( read( p, s ) );
	if ( extra )
		RETURN_ERR( skip( extra ) );
	return 0;
}

const char * Nes_File_Reader::begin( Auto_File_Reader dr )
{
	RETURN_ERR( dr.open() );
	in = dr;
	RETURN_ERR( read_header() );
	if ( block_type() != group_begin )
		return "File is wrong type";
	return enter_group();
}

const char * Nes_File_Reader::read_header()
{
	RETURN_ERR( in->read( &h, sizeof h ) );
	h.swap();
	block_type_ = data_block;
	if ( h.size == group_begin_size )
	{
		block_type_ = group_begin;
		h.size = 0;
	}
	if ( (long) h.tag == group_end_tag )
	{
		block_type_ = group_end;
		h.tag = 0;
	}
	set_remain( h.size );
	return 0;
}

const char * Nes_File_Reader::next_block()
{
	switch ( block_type() )
	{
		case group_end:
			return "Tried to go past end of blocks";
		
		case group_begin: {
			int d = 1;
			do
			{
				RETURN_ERR( skip( h.size ) );
				RETURN_ERR( read_header() );
				if ( block_type() == group_begin )
					d++;
				if ( block_type() == group_end )
					d--;
			}
			while ( d > 0);
			break;
		}
		
		case data_block:
			RETURN_ERR( skip( h.size ) );
			break;
		
		case invalid:
			break;
	}
	return read_header();
}

const char * Nes_File_Reader::enter_group()
{
	block_type_ = invalid; // cause next_block() not to skip group
	depth_++;
	return 0;
}

const char * Nes_File_Reader::exit_group()
{
	int d = 1;
	while ( true )
	{
		if ( block_type() == group_end )
			d--;
		if ( block_type() == group_begin )
			d++;
		if ( d == 0 )
			break;
		RETURN_ERR( skip( h.size ) );
		RETURN_ERR( read_header() );
	}
	
	block_type_ = invalid; // cause next_block() to read past end block
	depth_--;
	return 0;
}

const char * Nes_File_Reader::skip_v( int s )
{
	if ( (unsigned long) s > h.size )
		return "Tried to skip past end of data";
	h.size -= s;
	set_remain( h.size );
	return in->skip( s );
}

const char * Nes_File_Reader::read_v( void* p, int n )
{
	if ( (unsigned long) n > h.size )
		n = h.size;
	h.size -= n;
	set_remain( h.size );
	return in->read( p, n );
}
