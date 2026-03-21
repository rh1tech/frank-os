/*
 * FRANK OS - C library stubs for Dendy NES emulator
 *
 * QuickNES C++ code uses standard libc functions (printf, strlen, etc.)
 * via #include <stdio.h> etc., NOT m-os-api.h.  This file provides
 * implementations forwarding to the MOS2 sys_table.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

/* sys_table base */
#ifndef M_OS_API_SYS_TABLE_BASE
#define M_OS_API_SYS_TABLE_BASE ((void*)(0x10000000ul + (16 << 20) - (4 << 10)))
static const unsigned long * const _sys_table_ptrs =
    (const unsigned long * const)M_OS_API_SYS_TABLE_BASE;
#endif

/* --- String functions --- */

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const unsigned char *a = (const unsigned char *)s1;
    const unsigned char *b = (const unsigned char *)s2;
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)a[i] - (int)b[i];
    }
    return 0;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == 0) ? (char *)s : (char *)0;
}

int toupper(int c) {
    if (c >= 'a' && c <= 'z') return c - ('a' - 'A');
    return c;
}

/* --- I/O --- */

/* vsnprintf: sys_table[67] */
static int _vsnprintf(char *buf, size_t lim, const char *fmt, va_list ap) {
    typedef int (*fn_t)(char *, size_t, const char *, va_list);
    return ((fn_t)_sys_table_ptrs[67])(buf, lim, fmt, ap);
}

/* gouta: sys_table[41] (goutf with no args = just print string) */
static void _gouta(const char *s) {
    typedef void (*fn_t)(const char *, ...);
    ((fn_t)_sys_table_ptrs[41])("%s", s);
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[256];
    int n = _vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    _gouta(buf);
    return n;
}

int fprintf(void *stream, const char *fmt, ...) {
    (void)stream;
    va_list ap;
    va_start(ap, fmt);
    char buf[256];
    int n = _vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    _gouta(buf);
    return n;
}

/* --- _impure_ptr (newlib reentrancy, unused) --- */
void *_impure_ptr = 0;

/* --- Memory allocation: hybrid SRAM/PSRAM ---
 * Small allocs (< 4KB) -> SRAM (fast, for NES CPU hot state)
 * Large allocs (>= 4KB) -> PSRAM via __malloc (for ROM data, tables) */

#define SRAM_THRESHOLD 4096

static void *_hybrid_malloc(size_t sz) {
    typedef void *(*fn_t)(size_t);
    if (sz < SRAM_THRESHOLD) {
        void *p = ((fn_t)_sys_table_ptrs[529])(sz);  /* pvPortMalloc (SRAM) */
        if (p) return p;
    }
    return ((fn_t)_sys_table_ptrs[32])(sz);  /* __malloc (PSRAM first) */
}

static void _hybrid_free(void *p) {
    typedef void (*fn_t)(void *);
    ((fn_t)_sys_table_ptrs[33])(p);  /* __free handles both SRAM and PSRAM */
}

void *malloc(size_t sz) {
    return _hybrid_malloc(sz);
}

void free(void *p) {
    _hybrid_free(p);
}

void *calloc(size_t n, size_t sz) {
    size_t total = n * sz;
    void *p = _hybrid_malloc(total);
    if (p) {
        unsigned char *d = (unsigned char *)p;
        for (size_t i = 0; i < total; i++) d[i] = 0;
    }
    return p;
}

void *realloc(void *p, size_t sz) {
    if (!p) return _hybrid_malloc(sz);
    if (sz == 0) { _hybrid_free(p); return (void *)0; }
    void *np = _hybrid_malloc(sz);
    if (np) {
        const unsigned char *s = (const unsigned char *)p;
        unsigned char *d = (unsigned char *)np;
        for (size_t i = 0; i < sz; i++) d[i] = s[i];
        _hybrid_free(p);
    }
    return np;
}

/* --- Math: sys_table indices from m-os-api.h --- */

typedef double (*math_d_d_t)(double);
typedef double (*math_dd_d_t)(double, double);

double sin(double x)   { return ((math_d_d_t)_sys_table_ptrs[204])(x); }
double cos(double x)   { return ((math_d_d_t)_sys_table_ptrs[205])(x); }
double log(double x)   { return ((math_d_d_t)_sys_table_ptrs[208])(x); }
double pow(double x, double y) { return ((math_dd_d_t)_sys_table_ptrs[202])(x, y); }
double floor(double x) { return ((math_d_d_t)_sys_table_ptrs[201])(x); }
float powf(float x, float y) { return (float)pow((double)x, (double)y); }
double log10(double x) { return log(x) / 2.302585092994046; }
