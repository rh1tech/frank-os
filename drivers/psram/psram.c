/*
 * FRANK OS
 * Copyright (c) 2025 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * PSRAM detection and free-list allocator for FRANK OS.
 *
 * Both detection and the allocator use the UNCACHED XIP window
 * (0x15000000) for all PSRAM access.  Using the cached window
 * (0x11000000) would pollute the XIP cache with CS1 entries, causing
 * QMI bus contention between CS0 (flash instruction fetches) and CS1
 * (PSRAM cache-line eviction / writeback) — this corrupts flash reads
 * and leads to hard faults in downstream code (e.g. ELF relocation).
 *
 * Thread safety: psram_alloc/psram_free suspend the FreeRTOS scheduler
 * during free-list manipulation (same approach as FreeRTOS heap_4.c).
 */

#include "psram.h"
#include "FreeRTOS.h"
#include "task.h"

#include <string.h>
#include <stdio.h>

/*==========================================================================
 * PSRAM size detection — ported from murmapple psram_allocator.c
 *
 * Uses the UNCACHED window (PSRAM_BASE = 0x15000000) for all probe
 * access so that detection does not fill the XIP cache with CS1 data.
 *=========================================================================*/

#define MB16 (16ul << 20)
#define MB8  (8ul  << 20)
#define MB4  (4ul  << 20)
#define MB1  (1ul  << 20)

/* Uncached byte-addressable pointer for probe writes/reads */
#define PSRAM_PROBE  ((volatile uint8_t *)(intptr_t)PSRAM_BASE)

static int cached_psram_size = -1;

unsigned int __not_in_flash_func(butter_psram_size)(void) {
    if (cached_psram_size != -1)
        return (unsigned int)cached_psram_size;

    /* Write marker values at progressively lower boundaries */
    for (register int i = MB8; i < (int)MB16; i += 4096)
        PSRAM_PROBE[i] = 16;
    for (register int i = MB4; i < (int)MB8; i += 4096)
        PSRAM_PROBE[i] = 8;
    for (register int i = MB1; i < (int)MB4; i += 4096)
        PSRAM_PROBE[i] = 4;
    for (register int i = 0; i < (int)MB1; i += 4096)
        PSRAM_PROBE[i] = 1;

    /* The value at the top page tells us the size — if reads are
     * consistent across the last 1 MB, PSRAM is present. */
    register uint32_t res = PSRAM_PROBE[MB16 - 4096];
    for (register int i = MB16 - MB1; i < (int)MB16; i += 4096) {
        if (res != PSRAM_PROBE[i]) {
            cached_psram_size = 0;
            return 0;
        }
    }

    cached_psram_size = (int)(res << 20);
    return (unsigned int)cached_psram_size;
}

/*==========================================================================
 * Free-list allocator  (uses uncached PSRAM_BASE = 0x15000000)
 *=========================================================================*/

typedef struct block_hdr {
    size_t             size;  /* usable bytes (excludes header) */
    struct block_hdr  *next;
} block_hdr_t;

#define HDR_SIZE     (sizeof(block_hdr_t))  /* 8 bytes on 32-bit */
#define ALIGN        4
#define ALIGN_UP(x)  (((x) + (ALIGN - 1)) & ~(ALIGN - 1))
#define MIN_BLOCK    16  /* minimum usable size to avoid fragmentation */

static block_hdr_t *free_list = NULL;
static size_t        detected_size = 0;

void psram_heap_init(void) {
    unsigned int sz = butter_psram_size();
    detected_size = sz;

    if (sz == 0) {
        free_list = NULL;
        return;
    }

    /* Initialise a single free block spanning all of PSRAM
     * using the uncached address window */
    block_hdr_t *blk = (block_hdr_t *)(intptr_t)PSRAM_BASE;
    blk->size = sz - HDR_SIZE;
    blk->next = NULL;
    free_list = blk;
}

void *psram_alloc(size_t size) {
    if (!free_list || size == 0)
        return NULL;

    size = ALIGN_UP(size);
    if (size < MIN_BLOCK)
        size = MIN_BLOCK;

    vTaskSuspendAll();

    /* First-fit search (with corruption guard) */
    block_hdr_t **prev = &free_list;
    block_hdr_t  *cur  = free_list;
    void *result = NULL;
    int steps = 0;
    const int max_steps = (int)(detected_size / MIN_BLOCK);

    while (cur) {
        if (++steps > max_steps) {
            printf("[psram_alloc] CORRUPT free list at %p, %d steps\n", cur, steps);
            break;
        }
        if (cur->size >= size) {
            /* Can we split? Need room for header + MIN_BLOCK. */
            if (cur->size >= size + HDR_SIZE + MIN_BLOCK) {
                block_hdr_t *rest = (block_hdr_t *)((uint8_t *)cur + HDR_SIZE + size);
                rest->size = cur->size - size - HDR_SIZE;
                rest->next = cur->next;
                cur->size  = size;
                *prev = rest;
            } else {
                *prev = cur->next;
            }
            cur->next = NULL;
            result = (void *)((uint8_t *)cur + HDR_SIZE);
            break;
        }
        prev = &cur->next;
        cur  = cur->next;
    }

    (void)xTaskResumeAll();
    return result;
}

void psram_free(void *ptr) {
    if (!ptr) return;

    uintptr_t addr = (uintptr_t)ptr;

    /* If not in PSRAM range, delegate to FreeRTOS heap */
    if (addr < PSRAM_BASE || addr >= (PSRAM_BASE + detected_size)) {
        vPortFree(ptr);
        return;
    }

    block_hdr_t *blk = (block_hdr_t *)((uint8_t *)ptr - HDR_SIZE);

    /* Sanity: block header must be inside PSRAM */
    uintptr_t blk_addr = (uintptr_t)blk;
    if (blk_addr < PSRAM_BASE || blk_addr >= (PSRAM_BASE + detected_size)) {
        printf("[psram_free] BAD ptr %p (hdr %p outside PSRAM)\n", ptr, blk);
        return;
    }

    vTaskSuspendAll();

    /* Insert into free list in address order and coalesce.
     * Guard against corrupted free list (cycle detection). */
    block_hdr_t **prev = &free_list;
    block_hdr_t  *cur  = free_list;
    int steps = 0;
    const int max_steps = (int)(detected_size / MIN_BLOCK);

    while (cur && cur < blk) {
        if (++steps > max_steps) {
            /* Free list is corrupted — break to avoid infinite loop */
            (void)xTaskResumeAll();
            printf("[psram_free] CORRUPT free list at %p (freeing %p), %d steps\n",
                   cur, ptr, steps);
            return;
        }
        /* Validate that next pointer stays within PSRAM */
        if (cur->next && ((uintptr_t)cur->next < PSRAM_BASE ||
                          (uintptr_t)cur->next >= (PSRAM_BASE + detected_size))) {
            (void)xTaskResumeAll();
            printf("[psram_free] CORRUPT node %p->next=%p (freeing %p)\n",
                   cur, cur->next, ptr);
            return;
        }
        prev = &cur->next;
        cur  = cur->next;
    }

    /* Detect double-free: blk should not already be in the free list */
    if (cur == blk) {
        (void)xTaskResumeAll();
        printf("[psram_free] DOUBLE FREE %p\n", ptr);
        return;
    }

    blk->next = cur;
    *prev = blk;

    /* Coalesce with next block */
    if (cur && (uint8_t *)blk + HDR_SIZE + blk->size == (uint8_t *)cur) {
        blk->size += HDR_SIZE + cur->size;
        blk->next  = cur->next;
    }

    /* Coalesce with previous block */
    if (prev != &free_list) {
        block_hdr_t *prv = (block_hdr_t *)((char *)prev - offsetof(block_hdr_t, next));
        if ((uint8_t *)prv + HDR_SIZE + prv->size == (uint8_t *)blk) {
            prv->size += HDR_SIZE + blk->size;
            prv->next  = blk->next;
        }
    }

    (void)xTaskResumeAll();
}

bool psram_is_available(void) {
    return detected_size > 0;
}

unsigned int psram_detected_bytes(void) {
    return (unsigned int)detected_size;
}
