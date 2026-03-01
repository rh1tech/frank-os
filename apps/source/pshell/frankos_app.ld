/*
 * frankos_app.ld -- Linker script for PShell on Frank OS (relocatable ELF)
 *
 * Merges .ccudata.begin / .ccudata / .ccudata.end (and viudata equivalents)
 * into single output sections so the sentinel variables define boundaries
 * for bulk-zeroing.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
SECTIONS {
    .ccudata : {
        *(.ccudata.begin)
        *(.ccudata)
        *(.ccudata.end)
    }
    .viudata : {
        *(.viudata.begin)
        *(.viudata)
        *(.viudata.end)
    }
}
