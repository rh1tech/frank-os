/*
 * FRANK OS - Hybrid allocator for Dendy NES emulator
 *
 * Small allocations (< 4KB) go to SRAM (pvPortMalloc, index 529) for speed.
 * Large allocations (>= 4KB) go to PSRAM (__malloc, index 32) since SRAM
 * is too scarce for PRG ROM copies, audio buffers, etc.
 *
 * This gives NES CPU emulation fast SRAM access for its small, hot state
 * (registers, internal RAM, mapper state) while keeping large cold data
 * (PRG/CHR ROM, lookup tables) in spacious PSRAM.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stddef.h>
#include <stdint.h>

#ifndef M_OS_API_SYS_TABLE_BASE
#define M_OS_API_SYS_TABLE_BASE ((void*)(0x10000000ul + (16 << 20) - (4 << 10)))
static const unsigned long * const _sys_table_ptrs =
    (const unsigned long * const)M_OS_API_SYS_TABLE_BASE;
#endif

#define SRAM_THRESHOLD 4096

static inline void* _sram_malloc(size_t sz) {
    typedef void* (*fn_t)(size_t);
    return ((fn_t)_sys_table_ptrs[529])(sz);
}

static inline void* _any_malloc(size_t sz) {
    typedef void* (*fn_t)(size_t);
    /* __malloc (index 32): tries PSRAM first, SRAM fallback */
    return ((fn_t)_sys_table_ptrs[32])(sz);
}

/* vPortFree (index 530) handles both SRAM and PSRAM pointers —
 * psram_free checks the address range internally. */
static inline void _any_free(void* p) {
    typedef void (*fn_t)(void*);
    ((fn_t)_sys_table_ptrs[33])(p);
}

static inline void* _hybrid_alloc(size_t sz) {
    if (sz < SRAM_THRESHOLD) {
        void* p = _sram_malloc(sz);
        if (p) return p;
    }
    return _any_malloc(sz);
}

void* operator new(size_t sz) {
    return _hybrid_alloc(sz);
}

void operator delete(void* p) noexcept {
    _any_free(p);
}

void operator delete(void* p, size_t) noexcept {
    _any_free(p);
}

void* operator new[](size_t sz) {
    return _hybrid_alloc(sz);
}

void operator delete[](void* p) noexcept {
    _any_free(p);
}

void operator delete[](void* p, size_t) noexcept {
    _any_free(p);
}
