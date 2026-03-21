/*
 * blargg utility header
 * Original author: Shay Green (blargg)
 * https://github.com/libretro/QuickNES_Core
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Fork maintained as part of MurmNES by Mikhail Matveev.
 * https://rh1.tech | https://github.com/rh1tech/murmnes
 */

#ifndef BLARGG_ENDIAN
#define BLARGG_ENDIAN

#include "blargg_common.h"

// BLARGG_CPU_CISC: Defined if CPU has very few general-purpose registers (< 16)
#if defined (_M_IX86) || defined (_M_IA64) || defined (__i486__) || \
		defined (__x86_64__) || defined (__ia64__)
	#define BLARGG_CPU_X86 1
	#define BLARGG_CPU_CISC 1
#endif

#if defined (__powerpc__) || defined (__ppc__) || defined (__POWERPC__) || defined (__powerc)
	#define BLARGG_CPU_POWERPC 1
#endif

inline unsigned get_le16( void const* p ) {
	return  ((unsigned char*) p) [1] * 0x100u +
			((unsigned char*) p) [0];
}
inline unsigned get_be16( void const* p ) {
	return  ((unsigned char*) p) [0] * 0x100u +
			((unsigned char*) p) [1];
}
inline unsigned long get_le32( void const* p ) {
	return  ((unsigned char*) p) [3] * 0x01000000ul +
			((unsigned char*) p) [2] * 0x00010000ul +
			((unsigned char*) p) [1] * 0x00000100ul +
			((unsigned char*) p) [0];
}
inline unsigned long get_be32( void const* p ) {
	return  ((unsigned char*) p) [0] * 0x01000000ul +
			((unsigned char*) p) [1] * 0x00010000ul +
			((unsigned char*) p) [2] * 0x00000100ul +
			((unsigned char*) p) [3];
}
inline void set_le16( void* p, unsigned n ) {
	((unsigned char*) p) [1] = (unsigned char) (n >> 8);
	((unsigned char*) p) [0] = (unsigned char) n;
}
inline void set_be16( void* p, unsigned n ) {
	((unsigned char*) p) [0] = (unsigned char) (n >> 8);
	((unsigned char*) p) [1] = (unsigned char) n;
}
inline void set_le32( void* p, unsigned long n ) {
	((unsigned char*) p) [3] = (unsigned char) (n >> 24);
	((unsigned char*) p) [2] = (unsigned char) (n >> 16);
	((unsigned char*) p) [1] = (unsigned char) (n >> 8);
	((unsigned char*) p) [0] = (unsigned char) n;
}
inline void set_be32( void* p, unsigned long n ) {
	((unsigned char*) p) [0] = (unsigned char) (n >> 24);
	((unsigned char*) p) [1] = (unsigned char) (n >> 16);
	((unsigned char*) p) [2] = (unsigned char) (n >> 8);
	((unsigned char*) p) [3] = (unsigned char) n;
}

#if BLARGG_NONPORTABLE
	// Optimized implementation if byte order is known
#ifdef MSB_FIRST
		#define GET_BE16( addr )        (*(uint16_t*) (addr))
		#define GET_BE32( addr )        (*(uint32_t*) (addr))
		#define SET_BE16( addr, data )  (void) (*(uint16_t*) (addr) = (data))
		#define SET_BE32( addr, data )  (void) (*(uint32_t*) (addr) = (data))
#else
		#define GET_LE16( addr )        (*(uint16_t*) (addr))
		#define SET_LE16( addr, data )  (void) (*(uint16_t*) (addr) = (data))
		#define SET_LE32( addr, data )  (void) (*(uint32_t*) (addr) = (data))
#endif
	
	#if BLARGG_CPU_POWERPC && defined (__MWERKS__)
		// PowerPC has special byte-reversed instructions
		// to do: assumes that PowerPC is running in big-endian mode
		// to do: implement for other compilers which don't support these macros
		#define GET_LE16( addr )        (__lhbrx( (addr), 0 ))
		#define SET_LE16( addr, data )  (__sthbrx( (data), (addr), 0 ))
		#define SET_LE32( addr, data )  (__stwbrx( (data), (addr), 0 ))
	#endif
#endif

#ifndef GET_LE16
	#define GET_LE16( addr )        get_le16( addr )
	#define SET_LE16( addr, data )  set_le16( addr, data )
	#define SET_LE32( addr, data )  set_le32( addr, data )
#endif

#ifndef GET_BE16
	#define GET_BE16( addr )        get_be16( addr )
	#define GET_BE32( addr )        get_be32( addr )
	#define SET_BE16( addr, data )  set_be16( addr, data )
	#define SET_BE32( addr, data )  set_be32( addr, data )
#endif

// auto-selecting versions

inline void set_le( uint16_t* p, unsigned      n ) { SET_LE16( p, n ); }
inline void set_le( uint32_t* p, unsigned long n ) { SET_LE32( p, n ); }
inline void set_be( uint16_t* p, unsigned      n ) { SET_BE16( p, n ); }
inline void set_be( uint32_t* p, unsigned long n ) { SET_BE32( p, n ); }
inline unsigned      get_be( uint16_t* p ) { return GET_BE16( p ); }
inline unsigned long get_be( uint32_t* p ) { return GET_BE32( p ); }

#endif
