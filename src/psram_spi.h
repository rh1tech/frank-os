/*
 * psram_spi.h - Stub for FRANK OS (no PSRAM hardware)
 *
 * Provides function declarations needed by sys_table.c.
 * All functions are implemented as no-ops in psram_stubs.c.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t init_psram(void);
uint32_t psram_size(void);
void psram_cleanup(void);
void write8psram(uint32_t addr32, uint8_t v);
void write16psram(uint32_t addr32, uint16_t v);
void write32psram(uint32_t addr32, uint32_t v);
uint8_t read8psram(uint32_t addr32);
uint16_t read16psram(uint32_t addr32);
uint32_t read32psram(uint32_t addr32);
void psram_id(uint8_t rx[8]);
void writepsram(uint32_t addr32, uint8_t* b, size_t sz);
void readpsram(uint8_t* b, uint32_t addr32, size_t sz);

#ifdef __cplusplus
}
#endif
