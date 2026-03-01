/*
 * pshell_libc.c — C library functions not provided by m-os-api.h
 *
 * This file does NOT include m-os-api.h to avoid conflicts between
 * m-os-api.h's inline static definitions and standard C library headers.
 * Functions here are compiled as extern symbols and linked into the ELF.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

/* Access the OS system call table directly — we can't include m-os-api.h
 * in this translation unit, so we use the fixed address manually.
 * 0x10FFF000 = 0x10000000 + (16 << 20) - (4 << 10) */
#define _sys_table_ptrs ((unsigned long *)(0x10000000ul + (16 << 20) - (4 << 10)))

/* ── Extern wrappers for m-os-api.h inline static functions ────────── */
/* These are needed because assembly code (cc_printf.S) and libc call
 * malloc/free/strlen by external symbol name. m-os-api.h only provides
 * inline static versions, which don't create linkable symbols. */

void *malloc(size_t size) {
    typedef void *(*fn)(size_t);
    return ((fn)_sys_table_ptrs[32])(size);
}

void free(void *ptr) {
    typedef void (*fn)(void *);
    ((fn)_sys_table_ptrs[33])(ptr);
}

size_t strlen(const char *s) {
    typedef size_t (*fn)(const char *);
    return ((fn)_sys_table_ptrs[62])(s);
}

/* memcpy is already provided by m-os-api-sdtfn.c */
extern void *memcpy(void *dst, const void *src, size_t n);

int sprintf(char *str, const char *fmt, ...) {
    typedef int (*fn)(char *, size_t, const char *, va_list);
    va_list ap;
    va_start(ap, fmt);
    int n = ((fn)_sys_table_ptrs[67])(str, 0x7FFF, fmt, ap); /* vsnprintf */
    va_end(ap);
    return n;
}

/* ── String search functions ──────────────────────────────────────────── */

char *strchr(const char *s, int c) {
    for (; *s; s++)
        if ((unsigned char)*s == (unsigned char)c) return (char *)s;
    return (c == '\0') ? (char *)s : NULL;
}

char *strrchr(const char *s, int c) {
    const char *found = NULL;
    for (; *s; s++)
        if ((unsigned char)*s == (unsigned char)c) found = s;
    return (c == '\0') ? (char *)s : (char *)found;
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)haystack;
    }
    return NULL;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    while (n--) {
        if (*p == (unsigned char)c) return (void *)p;
        p++;
    }
    return NULL;
}

char *index(const char *s, int c) {
    return strchr(s, c);
}

char *stpcpy(char *dst, const char *src) {
    while ((*dst = *src)) { dst++; src++; }
    return dst;
}

char *strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *d = malloc(len);
    if (d) memcpy(d, s, len);
    return d;
}

char *strndup(const char *s, size_t n) {
    size_t len = 0;
    while (len < n && s[len]) len++;
    char *d = malloc(len + 1);
    if (d) { memcpy(d, s, len); d[len] = '\0'; }
    return d;
}

int strncasecmp(const char *a, const char *b, size_t n) {
    while (n--) {
        unsigned char ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return (int)ca - (int)cb;
        if (!ca) return 0;
    }
    return 0;
}

char *strerror(int errnum) {
    (void)errnum;
    return (char *)"error";
}

/* ── Number conversion ────────────────────────────────────────────────── */

long strtol(const char *s, char **end, int base) {
    long r = 0;
    int neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    if (!base) {
        if (*s == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (*s == '0') { base = 8; s++; }
        else base = 10;
    } else if (base == 16 && *s == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    while (1) {
        int d = (*s >= '0' && *s <= '9') ? *s - '0' :
                (*s >= 'a' && *s <= 'f') ? *s - 'a' + 10 :
                (*s >= 'A' && *s <= 'F') ? *s - 'A' + 10 : -1;
        if (d < 0 || d >= base) break;
        r = r * base + d;
        s++;
    }
    if (end) *end = (char *)s;
    return neg ? -r : r;
}

unsigned long strtoul(const char *s, char **end, int base) {
    return (unsigned long)strtol(s, end, base);
}

long atol(const char *s) {
    return strtol(s, NULL, 10);
}

double strtod(const char *s, char **end) {
    double r = 0.0;
    int neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') r = r * 10.0 + (*s++ - '0');
    if (*s == '.') {
        s++;
        double frac = 0.1;
        while (*s >= '0' && *s <= '9') {
            r += (*s++ - '0') * frac;
            frac *= 0.1;
        }
    }
    if (*s == 'e' || *s == 'E') {
        s++;
        int eneg = 0, exp = 0;
        if (*s == '-') { eneg = 1; s++; }
        else if (*s == '+') s++;
        while (*s >= '0' && *s <= '9') exp = exp * 10 + (*s++ - '0');
        double mul = 1.0;
        for (int i = 0; i < exp; i++) mul *= 10.0;
        r = eneg ? r / mul : r * mul;
    }
    if (end) *end = (char *)s;
    return neg ? -r : r;
}

float strtof(const char *s, char **end) {
    return (float)strtod(s, end);
}

double atof(const char *s) {
    return strtod(s, NULL);
}

/* ── Memory ───────────────────────────────────────────────────────────── */

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }
    void *new_ptr = malloc(size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, size);
    free(ptr);
    return new_ptr;
}

/* ── String helpers ───────────────────────────────────────────────────── */

char *strncat(char *d, const char *s, size_t n) {
    char *p = d;
    while (*p) p++;
    while (n-- && *s) *p++ = *s++;
    *p = '\0';
    return d;
}

/* ── errno ────────────────────────────────────────────────────────────── */

static int __pshell_errno;

int *__errno(void) {
    return &__pshell_errno;
}

/* ── ctype table ──────────────────────────────────────────────────────── */
/* Minimal ASCII-only character classification table used by <ctype.h>.
 * Bit layout matches newlib's _ctype_ convention:
 *   _U=01  _L=02  _N=04  _S=010  _P=020  _C=040  _X=0100  _B=0200 */

#define _U  0x01
#define _L  0x02
#define _N  0x04
#define _S  0x08
#define _P  0x10
#define _C  0x20
#define _X  0x40
#define _B  0x80

const char _ctype_[1 + 256] = {
    0, /* EOF = -1 entry */
    _C,_C,_C,_C,_C,_C,_C,_C,                           /* 0x00-0x07 */
    _C,_C|_S,_C|_S,_C|_S,_C|_S,_C|_S,_C,_C,            /* 0x08-0x0F */
    _C,_C,_C,_C,_C,_C,_C,_C,                           /* 0x10-0x17 */
    _C,_C,_C,_C,_C,_C,_C,_C,                           /* 0x18-0x1F */
    _S|_B,_P,_P,_P,_P,_P,_P,_P,                        /* 0x20-0x27 */
    _P,_P,_P,_P,_P,_P,_P,_P,                           /* 0x28-0x2F */
    _N,_N,_N,_N,_N,_N,_N,_N,                           /* 0x30-0x37 */
    _N,_N,_P,_P,_P,_P,_P,_P,                           /* 0x38-0x3F */
    _P,_U|_X,_U|_X,_U|_X,_U|_X,_U|_X,_U|_X,_U,       /* 0x40-0x47 */
    _U,_U,_U,_U,_U,_U,_U,_U,                           /* 0x48-0x4F */
    _U,_U,_U,_U,_U,_U,_U,_U,                           /* 0x50-0x57 */
    _U,_U,_U,_P,_P,_P,_P,_P,                           /* 0x58-0x5F */
    _P,_L|_X,_L|_X,_L|_X,_L|_X,_L|_X,_L|_X,_L,       /* 0x60-0x67 */
    _L,_L,_L,_L,_L,_L,_L,_L,                           /* 0x68-0x6F */
    _L,_L,_L,_L,_L,_L,_L,_L,                           /* 0x70-0x77 */
    _L,_L,_L,_P,_P,_P,_P,_C,                           /* 0x78-0x7F */
    /* 0x80-0xFF: all zero (non-ASCII) */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};

/* ── GCC compiler runtime ─────────────────────────────────────────────── */

/* __popcountsi2 — count set bits in a 32-bit integer */
int __popcountsi2(unsigned int x) {
    x = x - ((x >> 1) & 0x55555555);
    x = (x & 0x33333333) + ((x >> 2) & 0x33333333);
    return (((x + (x >> 4)) & 0x0F0F0F0F) * 0x01010101) >> 24;
}

/* __aeabi_ul2d — convert unsigned 64-bit integer to double.
 * Split into two 32-bit conversions to avoid infinite recursion. */
double __aeabi_ul2d(uint64_t v) {
    double hi = (double)(uint32_t)(v >> 32);
    hi *= 4294967296.0; /* 2^32 */
    return hi + (double)(uint32_t)v;
}

