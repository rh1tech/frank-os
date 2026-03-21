/*
 * QuickNES - NES emulator core
 * Original author: Shay Green (blargg)
 * https://github.com/libretro/QuickNES_Core
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Fork maintained as part of MurmNES by Mikhail Matveev.
 * https://rh1.tech | https://github.com/rh1tech/murmnes
 */

while ( true )
{
	while ( count-- )
	{
		int attrib = attr_table [addr >> 2 & 0x07];
		attrib >>= (addr >> 4 & 4) | (addr & 2);
		unsigned long offset = (attrib & 3) * attrib_factor + this->palette_offset;
		
		// draw one tile
		cache_t const* lines = this->get_bg_tile( nametable [addr] + bg_bank );
		uint8_t* p = pixels;
		addr++;
		pixels += 8; // next tile
		
		if ( !clipped )
		{
			// optimal case: no clipping
			for ( int n = 4; n--; )
			{
				unsigned long line = *lines++;
				((unaligned_uint32_t*) p) [0].val = (line >> 4 & mask) + offset;
				((unaligned_uint32_t*) p) [1].val = (line      & mask) + offset;
				p += row_bytes;
				((unaligned_uint32_t*) p) [0].val = (line >> 6 & mask) + offset;
				((unaligned_uint32_t*) p) [1].val = (line >> 2 & mask) + offset;
				p += row_bytes;
			}
		}
		else
		{
			lines += fine_y >> 1;
			
			if ( fine_y & 1 )
			{
				unsigned long line = *lines++;
				((unaligned_uint32_t*) p) [0].val = (line >> 6 & mask) + offset;
				((unaligned_uint32_t*) p) [1].val = (line >> 2 & mask) + offset;
				p += row_bytes;
			}
			
			for ( int n = height >> 1; n--; )
			{
				unsigned long line = *lines++;
				((unaligned_uint32_t*) p) [0].val = (line >> 4 & mask) + offset;
				((unaligned_uint32_t*) p) [1].val = (line      & mask) + offset;
				p += row_bytes;
				((unaligned_uint32_t*) p) [0].val = (line >> 6 & mask) + offset;
				((unaligned_uint32_t*) p) [1].val = (line >> 2 & mask) + offset;
				p += row_bytes;
			}
			
			if ( height & 1 )
			{
				unsigned long line = *lines;
				((unaligned_uint32_t*) p) [0].val = (line >> 4 & mask) + offset;
				((unaligned_uint32_t*) p) [1].val = (line      & mask) + offset;
			}
		} 
	}
	
	count = count2;
	count2 = 0;
	addr -= 32;
	attr_table = attr_table - nametable + nametable2;
	nametable = nametable2;
	if ( !count )
		break;
}
