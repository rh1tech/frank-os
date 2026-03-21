/*
 * QuickNES - NES emulator core
 * Original author: Shay Green (blargg)
 * https://github.com/libretro/QuickNES_Core
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Fork maintained as part of MurmNES by Mikhail Matveev.
 * https://rh1.tech | https://github.com/rh1tech/murmnes
 */


#include "abstract_file.h"

#include "blargg_config.h"

#include <string.h>
#include <stdlib.h>

/* Copyright (C) 2005-2006 Shay Green. Permission is hereby granted, free of
charge, to any person obtaining a copy of this software module and associated
documentation files (the "Software"), to deal in the Software without
restriction, including without limitation the rights to use, copy, modify,
merge, publish, distribute, sublicense, and/or sell copies of the Software, and
to permit persons to whom the Software is furnished to do so, subject to the
following conditions: The above copyright notice and this permission notice
shall be included in all copies or substantial portions of the Software. THE
SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE. */

const char *Data_Writer::write( const void*, long ) { return 0; }

// Mem_Writer

Mem_Writer::Mem_Writer( void* p, long s, int b )
{
	data_ = (char*) p;
	size_ = 0;
	allocated = s;
	mode = b ? ignore_excess : fixed;
}

Mem_Writer::Mem_Writer()
{
	data_ = 0;
	size_ = 0;
	allocated = 0;
	mode = expanding;
}

Mem_Writer::~Mem_Writer()
{
	if ( ( mode == expanding ) && data_ )
		free( data_ );
}

const char *Mem_Writer::write( const void* p, long s )
{
	long remain = allocated - size_;
	if ( s > remain )
	{
		if ( mode == fixed )
			return "Tried to write more data than expected";
		
		if ( mode == ignore_excess )
		{
			s = remain;
		}
		else // expanding
		{
			long new_allocated = size_ + s;
			new_allocated += (new_allocated >> 1) + 2048;
			void* p = realloc( data_, new_allocated );
			if ( !p )
				return "Out of memory";
			data_ = (char*) p;
			allocated = new_allocated;
		}
	}
	
	memcpy( data_ + size_, p, s );
	size_ += s;
	
	return 0;
}

// Auto_File_Reader

const char* Auto_File_Reader::open()
{
	return 0;
}

Auto_File_Reader::~Auto_File_Reader()
{
	if ( path )
		delete data;
}

// Auto_File_Writer

const char* Auto_File_Writer::open()
{
	return 0;
}

const char* Auto_File_Writer::open_comp( int level )
{
	return 0;
}

Auto_File_Writer::~Auto_File_Writer()
{
}
