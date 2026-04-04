/*
 * pshell_compat.h — LittleFS → FatFs shim and Pico SDK stubs for FRANK OS
 *
 * This header replaces all Pico SDK and LittleFS includes in pshell source
 * files when building as a FRANK OS windowed app.  It provides:
 *   1. LittleFS-compatible type definitions and API wrappers over FatFs
 *   2. Pico SDK function stubs (sleep_ms, getchar_timeout_us, etc.)
 *   3. stdio routing through the VT100 terminal emulator
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PSHELL_COMPAT_H
#define PSHELL_COMPAT_H

#include "m-os-api.h"

/*
 * m-os-api.h defines several macros that conflict with standard C headers
 * and pshell's own implementations.  Undo them here:
 *   switch    → prevents switch statements (we use -fno-jump-tables instead)
 *   abs       → conflicts with <stdlib.h> declaration
 *   printf    → routes to OS console; we route to VT100 instead
 *   getchar   → routes to OS getch(); we route to VT100 input
 *   putchar   → routes to OS putc(); we route to VT100 output
 *   sleep_ms  → simple alias; we redefine with pdMS_TO_TICKS conversion
 */
#undef switch
#undef inline
#undef __force_inline
#undef abs
#undef printf
#undef getchar
#undef putchar
#undef puts
#undef sleep_ms

#include "frankos-app.h"
#include "m-os-api-ff.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
/* Do NOT include <string.h> or <stdlib.h> here — they conflict with
 * m-os-api.h's inline static redefinitions of strlen, atoi, malloc, etc.
 * m-os-api.h and m-os-api-sdtfn.c provide most of what we need.
 * Missing functions are declared below and implemented in pshell_libc.c. */
#include <ctype.h>
#include <stdarg.h>

/* ── Declarations for C library functions not in m-os-api.h ───────────── */
/* These are implemented in pshell_libc.c (separate TU, no m-os-api.h) */
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
long  strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
double strtod(const char *s, char **end);
float  strtof(const char *s, char **end);
long  atol(const char *s);
double atof(const char *s);
void *realloc(void *ptr, size_t size);
char *strncat(char *d, const char *s, size_t n);

/* ── Forward declarations for VT100 terminal ──────────────────────────── */
void    vt100_putc(char c);
void    vt100_puts_nl(const char *s);
int     vt100_printf(const char *fmt, ...) __attribute__((__format__(__printf__, 1, 2)));
int     vt100_getch(void);
int     vt100_getch_timeout(int us);
void    vt100_ungetc(int c);
void    vt100_get_size(int *cols, int *rows);
void    vt100_input_flush(void);
void    vt100_flush(void);

/* ── LittleFS type compatibility ──────────────────────────────────────── */

/* LittleFS file handle → FatFs FIL */
typedef FIL lfs_file_t;

/* LittleFS directory handle → FatFs DIR */
/* DIR is already defined by m-os-api-ff.h */
typedef DIR lfs_dir_t;

/* LittleFS uses signed types for sizes/offsets */
typedef int32_t  lfs_ssize_t;
typedef int32_t  lfs_soff_t;
typedef uint32_t lfs_size_t;
typedef uint32_t lfs_off_t;

/* LittleFS file info structure */
struct lfs_info {
    uint8_t  type;       /* LFS_TYPE_DIR or LFS_TYPE_REG */
    uint32_t size;
    char     name[256];
};

/* LittleFS type constants */
#define LFS_TYPE_DIR   1
#define LFS_TYPE_REG   2

/* LittleFS error codes */
#define LFS_ERR_OK      0
#define LFS_ERR_NOENT  (-2)

/* LittleFS open flags */
#define LFS_O_RDONLY    0x0001
#define LFS_O_WRONLY    0x0002
#define LFS_O_CREAT     0x0100
#define LFS_O_EXCL      0x0200
#define LFS_O_TRUNC     0x0400
#define LFS_O_APPEND    0x0800

/* LittleFS seek whence */
#define LFS_SEEK_SET    0
#define LFS_SEEK_CUR    1
#define LFS_SEEK_END    2

/* LittleFS version (for version_cmd display) */
#define LFS_VERSION     ((2 << 16) | 5)

/* Dummy lfs_t and lfs_config — never actually used */
typedef struct { int dummy; } lfs_t;
struct lfs_config { int dummy; };

/* ── LittleFS → FatFs wrapper functions ───────────────────────────────── */

int  fs_file_open(lfs_file_t *file, const char *path, int flags);
int  fs_file_close(lfs_file_t *file);
int  fs_file_read(lfs_file_t *file, void *buf, int size);
int  fs_file_write(lfs_file_t *file, const void *buf, int size);
int  fs_file_seek(lfs_file_t *file, int off, int whence);
int  fs_file_tell(lfs_file_t *file);
int  fs_file_size(lfs_file_t *file);
int  fs_file_rewind(lfs_file_t *file);
int  fs_file_truncate(lfs_file_t *file, int size);

int  fs_dir_open(lfs_dir_t *dir, const char *path);
int  fs_dir_close(lfs_dir_t *dir);
int  fs_dir_read(lfs_dir_t *dir, struct lfs_info *info);
int  fs_dir_rewind(lfs_dir_t *dir);

int  fs_stat(const char *path, struct lfs_info *info);
int  fs_remove(const char *path);
int  fs_rename(const char *oldpath, const char *newpath);
int  fs_mkdir(const char *path);

int  fs_mount(void);
int  fs_unmount(void);
int  fs_load(void);
int  fs_unload(void);
int  fs_format(void);
int  fs_gc(void);

int  fs_getattr(const char *path, uint8_t type, void *buffer, uint32_t size);
int  fs_setattr(const char *path, uint8_t type, const void *buffer, uint32_t size);
int  fs_removeattr(const char *path, uint8_t type);

/* fs_fsstat for status_cmd */
struct fs_fsstat_t {
    uint32_t block_size;
    uint32_t block_count;
    uint32_t blocks_used;
};
int  fs_fsstat(struct fs_fsstat_t *stat);
int  fs_fs_size(void);

/* Globals that io.h normally exports (never used, but referenced) */
extern lfs_t fs_lfs;
extern struct lfs_config fs_cfg;

/* ── Pico SDK stubs ───────────────────────────────────────────────────── */

#define PICO_ERROR_TIMEOUT  (-1)
#define PICO_RP2350          1
#define PICO_RP2040          0

/* Board name */
#ifndef PICO_BOARD
#define PICO_BOARD "pico2"
#endif

/* SDK version (for version_cmd display) */
#define PICO_SDK_VERSION_MAJOR   2
#define PICO_SDK_VERSION_MINOR   0
#define PICO_SDK_VERSION_REVISION 0

/* sleep_ms / busy_wait_ms → FreeRTOS vTaskDelay */
#define sleep_ms(ms)       vTaskDelay(pdMS_TO_TICKS(ms))
#define sleep_us(us)       vTaskDelay(pdMS_TO_TICKS((us) / 1000 + 1))
#define busy_wait_ms(ms)   vTaskDelay(pdMS_TO_TICKS(ms))

/* time_us_32 — only define if pico/time.h wasn't already included
 * (cc.c includes pico/time.h directly for cc_extrns.h) */
#ifndef _PICO_TIME_H
static inline uint32_t time_us_32(void) {
    /* Approximate with FreeRTOS tick count */
    return (uint32_t)(xTaskGetTickCount() * 1000);
}
#endif

/* getchar_timeout_us → VT100 input with timeout */
#define getchar_timeout_us(us)  vt100_getch_timeout(us)

/* stdio_set_translate_crlf → no-op */
#define set_translate_crlf(e)   ((void)0)

/* Pico SDK stdio types (referenced but not used) */
typedef struct { int dummy; } stdio_driver_t;

/* stdio_init_all → no-op */
#define stdio_init_all()        ((void)0)

/* SCB hardware — referenced by Fault_Handler and reboot_cmd */
struct scb_hw_t { volatile uint32_t aircr; volatile uint32_t *vtor; };

/* reset_usb_boot → no-op */
#define reset_usb_boot(a, b)    ((void)0)

/* stdio driver refs — never used in FRANK OS build */
#define LIB_PICO_STDIO_UART    0
#define LIB_PICO_STDIO_USB     0

/* random number — only define if pico/rand.h wasn't already included */
#ifndef _PICO_RAND_H
static inline uint32_t get_rand_32(void) {
    static uint32_t seed = 12345;
    seed = seed * 1103515245 + 12345;
    return seed;
}
#endif

/* ── Heap boundaries (for status_cmd) ─────────────────────────────────── */
extern char __heap_start;
extern char __heap_end;

/* ── stdio routing through VT100 ──────────────────────────────────────── */

/* These functions are implemented in pshell_compat.c, which routes all
 * stdio through the VT100 terminal emulator.  The -fno-builtin-printf
 * etc. flags ensure our versions win over compiler builtins. */
int printf(const char *fmt, ...) __attribute__((__format__(__printf__, 1, 2)));
int vprintf(const char *fmt, va_list ap);
int putchar(int c);
int puts(const char *s);
int getchar(void);

/* sprintf — not provided by m-os-api.h (snprintf IS provided as a macro) */
static inline int pshell_sprintf(char *str, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(str, 0x7FFF, fmt, ap);
    va_end(ap);
    return n;
}
#define sprintf pshell_sprintf

/* sscanf — declare from toolchain's libc (linked, just need prototype) */
int sscanf(const char *str, const char *fmt, ...);

/* fflush → flush VT100 display output */
#define fflush(x)               vt100_flush()

/* ungetc → VT100 ungetc */
#define ungetc(c, f)            vt100_ungetc(c)

/* ── Hardware includes that are safe to stub ──────────────────────────── */

/* hardware/timer.h — for vi.c using time_us_32() */
/* Already stubbed above */

/* pico/time.h — already handled */

/* pico/sync.h — FreeRTOS provides synchronization */

/* pico/rand.h — get_rand_32 stubbed above */

/* ── Compiler cc.c needs these defines for RP2350 ─────────────────────── */
/* keep hardware access defines available since HW registers are still accessible */

#endif /* PSHELL_COMPAT_H */
