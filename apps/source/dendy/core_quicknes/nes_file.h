/*
 * QuickNES - NES emulator core
 * Original author: Shay Green (blargg)
 * https://github.com/libretro/QuickNES_Core
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Fork maintained as part of MurmNES by Mikhail Matveev.
 * https://rh1.tech | https://github.com/rh1tech/murmnes
 */

#ifndef NES_FILE_H
#define NES_FILE_H

#include "blargg_common.h"
#include "abstract_file.h"
#include "nes_data.h"

// Writes a structured file
class Nes_File_Writer : public Data_Writer {
public:
	Nes_File_Writer();
	~Nes_File_Writer();
	
	// Begin writing file with specified signature tag
	const char * begin( Auto_File_Writer, nes_tag_t );
	
	// Begin tagged group
	const char * begin_group( nes_tag_t );
	
	// Write tagged block
	const char * write_block( nes_tag_t, void const*, long size );
	
	// Write tagged block header. 'Size' bytes must be written before next block.
	const char * write_block_header( nes_tag_t, long size );
	
	// Write data to current block
	const char *write( void const*, long );
	
	// End tagged group
	const char * end_group();
	
	// End file
	const char * end();
	
private:
	Auto_File_Writer out;
	long write_remain;
	int depth_;
	const char * write_header( nes_tag_t tag, long size );
};

// Reads a structured file
class Nes_File_Reader : public Data_Reader {
public:
	Nes_File_Reader();
	~Nes_File_Reader();
	
	// If true, verify checksums of any blocks that have them
	void enable_checksums( bool = true );
	
	// Begin reading file. Until next_block() is called, block_tag() yields tag for file.
	const char * begin( Auto_File_Reader );
	
	// Read header of next block in current group
	const char * next_block();
	
	// Type of current block
	enum block_type_t {
		data_block,
		group_begin,
		group_end,
		invalid
	};
	block_type_t block_type() const { return block_type_; }
	
	// Tag of current block
	nes_tag_t block_tag() const { return h.tag; }
	
	// Read at most s bytes from block and skip any remaining bytes
	const char * read_block_data( void*, long s );
	
	// Read at most 's' bytes from current block and return number of bytes actually read
	virtual const char * read_v( void*, int n );
	
	// Skip 's' bytes in current block
	virtual const char * skip_v( int s );
	
	// Read first sub-block of current group block
	const char * enter_group();
	
	// Skip past current group
	const char * exit_group();
	
	// Current depth, where 0 is top-level in file and higher is deeper
	int depth() const { return depth_; }
	
	// True if all data has been read
	bool done() const { return depth() == 0 && block_type() == group_end; }
private:
	Auto_File_Reader in;
	nes_block_t h;
	block_type_t block_type_;
	int depth_;
	const char * read_header();
};

template<class T>
inline const char * read_nes_state( Nes_File_Reader& in, T* out )
{
	const char * err = in.read_block_data( out, sizeof *out );
	out->swap();
	return err;
}

template<class T>
inline const char * write_nes_state( Nes_File_Writer& out, T& in )
{
	in.swap();
	const char * err = out.write_block( in.tag, &in, sizeof in );
	in.swap();
	return err;
}

template<class T>
inline const char * write_nes_state( Nes_File_Writer& out, const T& in )
{
	T copy = in;
	copy.swap();
	return out.write_block( copy.tag, &copy, sizeof copy );
}

#endif
