/*
 * frankos_platform.c — Frank OS platform layer for MMBasic
 *
 * Provides:
 *  • Draw.c global variables and stub functions (DisplayPutC, ClearScreen, …)
 *  • Flash-to-RAM redirection buffers and pointer assignments
 *  • Linker-wrapped functions: getConsole, kbhitConsole, SoftReset,
 *    safe_flash_range_erase, safe_flash_range_program
 *  • Hardware stubs: sleep_ms/sleep_us, watchdog_enable, flash_range_erase/program
 *  • 1 ms FreeRTOS tick timer for mSecTimer, InkeyTimer, PauseTimer …
 *  • basic_platform_init() and basic_run_interpreter()
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Pull in pico/stdlib.h FIRST so that <stdio.h> (and its FILE, fclose, …
 * declarations) are established before any FatFS or MMBasic headers.
 * DO NOT include m-os-api.h or m-os-api-ff.h here: those headers define
 *   #define fclose(f) f_close(f)
 *   typedef FIL FILE
 * which conflict with the stdlib FILE and function prototypes.
 *
 * FreeRTOS and Frank OS runtime functions are forward-declared below; they
 * are resolved at ELF load time by the OS dynamic linker.
 */
#include "pico/stdlib.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <setjmp.h>
/* Memory allocation wrappers — see frankos_alloc.c for rationale.
 *
 * pvPortMalloc and pvPortCalloc are NOT bare OS symbols resolvable by the
 * Frank OS ELF loader.  They are inline functions in m-os-api.h that call
 * _sys_table_ptrs[32/166] (a fixed Flash address 0x10FFF000).  Declaring
 * them as 'extern' here would produce SHN_UNDEF relocation entries that the
 * loader marks as "Unsupported link… UNDEF" and aborts loading EVERY section
 * that references them (including this file and transitively main()).
 *
 * frankos_alloc.c includes ONLY m-os-api.h and provides in-ELF wrapper
 * functions.  Calling those wrappers here generates only in-ELF cross-section
 * references (always resolvable) with no SHN_UNDEF entries. */
extern void *frankos_malloc (size_t sz);
extern void *frankos_calloc (size_t n, size_t sz);
extern void  frankos_free   (void *p);
extern void *frankos_realloc(void *p, size_t sz);

/* Provide standard C allocation symbols so MMBasic internals and embedded
 * libraries (dr_wav, dr_mp3, cJSON, …) that call bare malloc/calloc/free/
 * realloc all land here and route to the OS PSRAM-first allocator. */
void *calloc (size_t n,   size_t sz) { return frankos_calloc(n, sz);  }
void *malloc (size_t sz)             { return frankos_malloc(sz);     }
void  free   (void *p)               { frankos_free(p);               }
void *realloc(void *p,   size_t sz)  { return frankos_realloc(p, sz); }

/* turtle_free: defined in Turtle.c which is excluded from this build.
 * Provide an empty stub so MMBasic.c can call it on program reset without
 * crashing (there is no turtle state to free anyway). */
void turtle_free(void) { }

/* MMBasic and peripheral headers
 * (frankos/ override dir is first in include path, so ff.h → frankos/ff.h →
 * #include_next → basic/ff.h with FatFS types only, no macros) */
#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "Version.h"

/* COPYRIGHT is defined in PicoMite.c but we need it here too */
#ifndef COPYRIGHT
#define COPYRIGHT "Copyright " YEAR " Geoff Graham\r\n" \
                  "Copyright " YEAR2 " Peter Mather\r\n"
#endif

/* ── FreeRTOS / Frank OS forward declarations ───────────────────────────────
 *
 * These types and functions are exported by the Frank OS kernel.  We forward-
 * declare them instead of including m-os-api.h so we avoid the stdio conflict.
 * configTICK_RATE_HZ = 1000 in Frank OS → pdMS_TO_TICKS(ms) = (ms).
 */
typedef void                       *TimerHandle_t;
typedef uint32_t                    TickType_t;
typedef int32_t                     BaseType_t;
#define pdTRUE                      ((BaseType_t)1)
#define pdFALSE                     ((BaseType_t)0)
#define pdMS_TO_TICKS(ms)           ((TickType_t)(ms))   /* 1000 Hz tick */
typedef void (*TimerCallbackFunction_t)(TimerHandle_t xTimer);

extern TimerHandle_t xTimerCreate(const char *pcTimerName,
                                  TickType_t xTimerPeriodInTicks,
                                  uint32_t uxAutoReload,
                                  void *pvTimerID,
                                  TimerCallbackFunction_t pxCallbackFunction);
extern BaseType_t    xTimerStart(TimerHandle_t xTimer, TickType_t xTicksToWait);
extern BaseType_t    xTimerStop(TimerHandle_t xTimer, TickType_t xTicksToWait);
extern BaseType_t    xTimerDelete(TimerHandle_t xTimer, TickType_t xTicksToWait);
extern void          vTaskDelay(TickType_t xTicksToDelay);
extern uint32_t      ulTaskNotifyTake(uint32_t xClearCountOnExit,
                                      TickType_t xTicksToWait);

/* ── Frank OS terminal API (exported from frankos_main.c) ───────────────── */
extern void basic_textbuf_putc(int c);
extern void basic_kbuf_push(int c);
extern int  basic_kbuf_pop(void);
extern int  basic_kbuf_avail(void);
extern volatile bool g_closing;
extern char          g_autorun_path[256];

/* ── Convenience macro (like DECREMENT_IF_ACTIVE in PicoMite.c) ─────────── */
#define DECR_IF_ACTIVE(x)  do { if ((x) > 0) (x)--; } while (0)

/* ═════════════════════════════════════════════════════════════════════════
 * Flash-to-RAM redirection
 *
 * In PicoMite mode the BASIC interpreter reads/writes program code and
 * options from/to physical flash.  On Frank OS we redirect everything
 * to plain RAM buffers.  The pointer variables are DEFINED here because
 * PicoMite.c's originals are guarded with #ifndef _FRANKOS.
 * ═════════════════════════════════════════════════════════════════════════ */

/* Options area (FLASH_ERASE_SIZE = 4 KiB) — heap-allocated in basic_platform_init. */
static uint8_t *g_basic_opt_buf = NULL;

/* Saved-variables area (SAVEDVARS_FLASH_SIZE = 16 KiB) — ditto. */
static uint8_t *g_basic_savedvars_buf = NULL;

/* Main program slot (MAX_PROG_SIZE ≈ 328 KiB) — allocated at runtime to keep
 * BSS small.  Frank OS's malloc routes to PSRAM via pvPortMalloc. */
uint8_t *g_basic_prog_buf = NULL;

/* Library slot (also MAX_PROG_SIZE) — ditto. */
static uint8_t *g_basic_lib_buf = NULL;

/* The pointer variables read by LoadOptions() / m_alloc(M_PROG) / …
 * Initialised to the static buffers for the options/savedvars areas;
 * prog/lib pointers are filled in by basic_platform_init() after malloc. */
const uint8_t *flash_option_contents  = NULL;  /* set in basic_platform_init() */
const uint8_t *SavedVarsFlash         = NULL;  /* set in basic_platform_init() */
const uint8_t *flash_target_contents  = NULL;   /* set in basic_platform_init() */
const uint8_t *flash_progmemory       = NULL;   /* set in basic_platform_init() */
const uint8_t *flash_libmemory        = NULL;   /* set in basic_platform_init() */

/* Map a flash byte-offset (as used by safe_flash_range_*) to a RAM pointer. */
static uint8_t *basic_flash_to_ram(uint32_t flash_offs)
{
    /* Options block */
    if (flash_offs >= FLASH_TARGET_OFFSET &&
        flash_offs <  FLASH_TARGET_OFFSET + FLASH_ERASE_SIZE)
        return g_basic_opt_buf + (flash_offs - FLASH_TARGET_OFFSET);

    /* Saved-variables block */
    if (flash_offs >= FLASH_TARGET_OFFSET + FLASH_ERASE_SIZE &&
        flash_offs <  FLASH_TARGET_OFFSET + FLASH_ERASE_SIZE + SAVEDVARS_FLASH_SIZE)
        return g_basic_savedvars_buf + (flash_offs - (FLASH_TARGET_OFFSET + FLASH_ERASE_SIZE));

    /* Library program slot (PROGSTART - MAX_PROG_SIZE) */
    if (flash_offs >= (uint32_t)(PROGSTART - MAX_PROG_SIZE) &&
        flash_offs <  (uint32_t)(PROGSTART))
        return g_basic_lib_buf + (flash_offs - (PROGSTART - MAX_PROG_SIZE));

    /* Main program slot (PROGSTART) */
    if (flash_offs >= (uint32_t)PROGSTART &&
        flash_offs <  (uint32_t)(PROGSTART + MAX_PROG_SIZE))
        return g_basic_prog_buf + (flash_offs - PROGSTART);

    /* Flash-slot area -> all map to main prog buffer */
    return g_basic_prog_buf;
}

/* ═════════════════════════════════════════════════════════════════════════
 * Draw.c globals (declared extern in Draw.h; defined here instead of Draw.c)
 * ═════════════════════════════════════════════════════════════════════════ */

short           gui_font            = 0x11;  /* font 1, scale 1 */
short           gui_font_width      = 8;
short           gui_font_height     = 16;
int             gui_fcolour         = 0xFFFFFF;
int             gui_bcolour         = 0x000000;
unsigned char  *FontTable[FONT_TABLE_SIZE]; /* font 0 set in basic_platform_init */

short           DisplayHRes         = 632;
short           DisplayVRes         = 384;
short           HRes                = 632;
short           VRes                = 384;
volatile short  low_x               = 0;
volatile short  high_x              = 631;
volatile short  low_y               = 0;
volatile short  high_y              = 383;
short           CurrentX            = 0;
short           CurrentY            = 0;
int             PrintPixelMode      = 0;
char            CMM1                = 0;
int             ScreenSize          = 0;

struct D3D     *struct3d[MAX3D + 1];
s_camera        camera[MAXCAM + 1];
struct spritebuffer *spritebuff[MAXBLITBUF + 1];
struct blitbuffer    blitbuff[MAXBLITBUF + 1];
struct stobject      stobjects[MAXSTOBJECTS + 1];

char  *COLLISIONInterrupt   = NULL;
bool   CollisionFound       = false;
char  *STCollisionInterrupt = NULL;
bool   STCollisionFound     = false;
int    sprite_hit_st        = 0;
int    st_which_collided    = 0;
uint8_t sprite_transparent  = 0;
int    RGB121map[16];
bool   mergerunning         = false;
uint32_t mergetimer         = 0;
bool   mergedread           = false;

/* Draw function pointers — point to safe no-op stubs so indirect calls
 * through these pointers (e.g. in MX470Display / Editor.c) don't crash. */
static void nop_DrawPixel(int x, int y, int c) { (void)x;(void)y;(void)c; }
static void nop_DrawRectangle(int x1,int y1,int x2,int y2,int c)
{ (void)x1;(void)y1;(void)x2;(void)y2;(void)c; }
static void nop_DrawBitmap(int x1,int y1,int w,int h,int s,int fc,int bc,unsigned char *bm)
{ (void)x1;(void)y1;(void)w;(void)h;(void)s;(void)fc;(void)bc;(void)bm; }
static void nop_ScrollLCD(int n) { (void)n; }
static void nop_DrawBuffer(int x1,int y1,int x2,int y2,unsigned char *c)
{ (void)x1;(void)y1;(void)x2;(void)y2;(void)c; }
static void nop_ReadBuffer(int x1,int y1,int x2,int y2,unsigned char *c)
{ (void)x1;(void)y1;(void)x2;(void)y2;(void)c; }

void (*DrawPixel)(int, int, int)                      = nop_DrawPixel;
void (*DrawRectangle)(int, int, int, int, int)        = nop_DrawRectangle;
void (*DrawBitmap)(int,int,int,int,int,int,int,unsigned char*) = nop_DrawBitmap;
void (*ScrollLCD)(int)                                = nop_ScrollLCD;
void (*DrawBuffer)(int,int,int,int,unsigned char*)    = nop_DrawBuffer;
void (*ReadBuffer)(int,int,int,int,unsigned char*)    = nop_ReadBuffer;
void (*DrawBLITBuffer)(int,int,int,int,unsigned char*)= nop_DrawBuffer;
void (*ReadBLITBuffer)(int,int,int,int,unsigned char*)= nop_ReadBuffer;
void (*ReadBufferFast)(int,int,int,int,unsigned char*)= nop_ReadBuffer;

/* ═════════════════════════════════════════════════════════════════════════
 * Draw.c stub functions
 * ═════════════════════════════════════════════════════════════════════════ */

/*
 * DisplayPutC — the only Draw.c output function called by putConsole().
 * Routes text to our Frank OS terminal buffer.
 */
void DisplayPutC(char c)
{
    basic_textbuf_putc((unsigned char)c);
}

void ClearScreen(int c)
{
    (void)c;
    /* Delegate to frankos_main.c which has the correct g_textbuf type. */
    extern void basic_tbuf_clear(void);
    basic_tbuf_clear();
}

void ShowCursor(int show)
{
    (void)show;
    /* Cursor visible state is controlled by the blink timer in frankos_main.c. */
}

void CheckDisplay(void)     { }
void ResetDisplay(void)     { }
void setmode(int m, bool c) { (void)m; (void)c; }

void SetFont(int fnt)
{
    gui_font        = (short)fnt;
    gui_font_width  = GetFontWidth(fnt);
    gui_font_height = GetFontHeight(fnt);
}

int GetFontWidth(int fnt)
{
    (void)fnt;
    return 8;   /* Only font 0 (8×16) is available in this port. */
}

int GetFontHeight(int fnt)
{
    (void)fnt;
    return 16;
}

void initFonts(void)
{
    memset(FontTable, 0, sizeof(FontTable));
    /* Font 0 is the built-in 8×16 terminal font; no external font data needed. */
}

void GUIPrintString(int x, int y, int fnt, int jh, int jv, int jo,
                    int fc, int bc, char *str)
{
    (void)x; (void)y; (void)fnt; (void)jh; (void)jv; (void)jo;
    (void)fc; (void)bc;
    /* Print to console terminal as a fallback. */
    while (str && *str)
        basic_textbuf_putc((unsigned char)*str++);
}

int GetJustification(char *p, int *jh, int *jv, int *jo)
{
    (void)p; if (jh) *jh = 0; if (jv) *jv = 0; if (jo) *jo = 0;
    return 0;
}

void DrawLine(int x1,int y1,int x2,int y2,int w,int c)
{ (void)x1;(void)y1;(void)x2;(void)y2;(void)w;(void)c; }
void DrawBox(int x1,int y1,int x2,int y2,int w,int c,int fill)
{ (void)x1;(void)y1;(void)x2;(void)y2;(void)w;(void)c;(void)fill; }
void DrawRBox(int x1,int y1,int x2,int y2,int r,int c,int fill)
{ (void)x1;(void)y1;(void)x2;(void)y2;(void)r;(void)c;(void)fill; }
void DrawCircle(int x,int y,int r,int w,int c,int fill,MMFLOAT aspect)
{ (void)x;(void)y;(void)r;(void)w;(void)c;(void)fill;(void)aspect; }
void DrawTriangle(int x0,int y0,int x1,int y1,int x2,int y2,int c,int fill)
{ (void)x0;(void)y0;(void)x1;(void)y1;(void)x2;(void)y2;(void)c;(void)fill; }
void DrawPixelNormal(int x,int y,int c) { (void)x;(void)y;(void)c; }
void ReadBuffer2(int x1,int y1,int x2,int y2,unsigned char *c)
{ (void)x1;(void)y1;(void)x2;(void)y2;(void)c; }
void copyframetoscreen(uint8_t *s,int x1,int x2,int y1,int y2,int odd)
{ (void)s;(void)x1;(void)x2;(void)y1;(void)y2;(void)odd; }
void copybuffertoscreen(int a,int b,int c,int d,int e)
{ (void)a;(void)b;(void)c;(void)d;(void)e; }
void restorepanel(void)     { }
void InitDisplayVirtual(void) { }
void ConfigDisplayVirtual(unsigned char *p) { (void)p; }
void merge(uint8_t colour)  { (void)colour; }
void blitmerge(int x0,int y0,int w,int h,uint8_t colour)
{ (void)x0;(void)y0;(void)w;(void)h;(void)colour; }
void DrawRectangleUser(int x1,int y1,int x2,int y2,int c)
{ (void)x1;(void)y1;(void)x2;(void)y2;(void)c; }
void DrawBitmapUser(int x1,int y1,int w,int h,int s,int fc,int bc,unsigned char *bm)
{ (void)x1;(void)y1;(void)w;(void)h;(void)s;(void)fc;(void)bc;(void)bm; }
void closeall3d(void)            { }
void closeallsprites(void)       { }
void closeallstobjects(void)     { }
void closeframebuffer(char layer){ (void)layer; }
int  rgb(int r,int g,int b)      { return (r<<16)|(g<<8)|b; }
int  getColour(char *c, int minus){ (void)c;(void)minus; return 0; }

/* ═════════════════════════════════════════════════════════════════════════
 * Keyboard / console stubs (not part of Draw.c, but needed here)
 * ═════════════════════════════════════════════════════════════════════════ */

/* initKeyboard — no PS/2 keyboard on Frank OS; key input comes from WM events */
void initKeyboard(void)     { }
void CheckKeyboard(void)    { }

/* ═════════════════════════════════════════════════════════════════════════
 * Sleep with FreeRTOS yield
 *
 * pico/time.h declares sleep_ms / sleep_us.  We provide Frank OS equivalents
 * that yield to the scheduler instead of busy-waiting.  The linker --wrap
 * flag redirects calls to pico-sdk's sleep_ms/sleep_us through these.
 * ═════════════════════════════════════════════════════════════════════════ */

void __wrap_sleep_ms(uint32_t ms)
{
    if (ms == 0) return;
    vTaskDelay(pdMS_TO_TICKS(ms < 1 ? 1 : ms));
}

void __wrap_sleep_us(uint64_t us)
{
    if (us < 1000) {
        /* Busy-wait for sub-millisecond delays to avoid FreeRTOS overhead. */
        volatile uint32_t n = (uint32_t)(us * 10);
        while (n--) __asm volatile ("nop");
    } else {
        vTaskDelay(pdMS_TO_TICKS((uint32_t)(us / 1000)));
    }
}

/* uSec is defined in PicoMite.c and in MM_Misc.c — do not redefine here */

/* Audio stubs (Audio.c calls these; no audio hardware on Frank OS) */
void start_i2s(int pio, int sm)       { (void)pio; (void)sm; }
void pcm_init(int rate, int channels) { (void)rate; (void)channels; }
void pcm_cleanup(void)      { }
void pcm_submit(void)       { }

/* Various PicoMite init stubs */
void initMouse0(int sensitivity)  { (void)sensitivity; }
void PS2mouse(void)        { }
void initFastTimer(void)   { }
void initWii(void)         { }
/* disable_sd, disable_audio, disable_systemi2c, disable_systemspi
 * are defined in MM_Misc.c — do not redefine here */

/* psram.c stub */
void psram_init(uint8_t pin) { (void)pin; }

/* ═════════════════════════════════════════════════════════════════════════
 * Linker-wrapped functions
 * ═════════════════════════════════════════════════════════════════════════ */

/*
 * __wrap_getConsole — replaces PicoMite.c's getConsole().
 * Returns next byte from the keyboard ring buffer, or -1 if empty.
 * Does NOT block indefinitely; the calling loop (MMgetchar/MMInkey) retries.
 */
int __wrap_getConsole(void)
{
    int c = basic_kbuf_pop();
    if (c == -1) {
        /* Yield briefly so other Frank OS tasks can run.  MMgetchar() loops
         * on us anyway, so a short sleep avoids busy-spinning at 100% CPU. */
        ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(5));
        c = basic_kbuf_pop();
    }
    return c;
}

/*
 * __wrap_kbhitConsole — replaces PicoMite.c's kbhitConsole().
 * Returns number of characters in the ring buffer.
 */
int __wrap_kbhitConsole(void)
{
    return basic_kbuf_avail();
}

/*
 * SoftReset longjmp target — set before entering the interpreter loop.
 */
static jmp_buf  g_softreset_jmp;
static volatile bool g_softreset_armed = false;

/*
 * __wrap_SoftReset — intercepts PicoMite's watchdog-reset.
 * On Frank OS we long-jump back to basic_run_interpreter() to restart.
 */
void __wrap_SoftReset(int code)
{
    if (g_softreset_armed)
        longjmp(g_softreset_jmp, code + 1);
    /* If not armed (e.g. during init), just return. */
}

/*
 * __wrap_safe_flash_range_erase — fills the corresponding RAM buffer with 0xFF.
 */
void __wrap_safe_flash_range_erase(uint32_t flash_offs, size_t count)
{
    uint8_t *dst = basic_flash_to_ram(flash_offs);
    if (dst)
        memset(dst, 0xFF, count);
}

/*
 * __wrap_safe_flash_range_program — copies data into the RAM buffer.
 */
void __wrap_safe_flash_range_program(uint32_t flash_offs,
                                      const uint8_t *data, size_t count)
{
    uint8_t *dst = basic_flash_to_ram(flash_offs);
    if (dst)
        memcpy(dst, data, count);
}

/* ═════════════════════════════════════════════════════════════════════════
 * Hardware function stubs
 *
 * PicoMite's disable_interrupts_pico / enable_interrupts_pico / flash_do_cmd
 * live in .time_critical sections and access QMI PSRAM controller registers,
 * SPI flash, and interrupt masking.  An ELF app running from PSRAM cannot
 * safely call any of these.  We intercept them via --wrap linker flags.
 * ═════════════════════════════════════════════════════════════════════════ */

/*
 * __wrap_disable_interrupts_pico — replaces FileIO.c version that accesses
 * qmi_hw registers and disables all interrupts.  No-op in Frank OS: we must
 * not touch QMI settings (PSRAM is our code/data store) or disable interrupts
 * (FreeRTOS requires them).
 */
void __wrap_disable_interrupts_pico(void) { }

/*
 * __wrap_enable_interrupts_pico — replaces FileIO.c version that restores
 * QMI registers and interrupts.  No-op for the same reasons.
 */
void __wrap_enable_interrupts_pico(void) { }

/*
 * __wrap_flash_do_cmd — replaces pico-sdk flash_do_cmd() which sends raw SPI
 * commands to the flash chip.  We can't touch flash.
 * ResetOptions sends JEDEC-ID command (0x9f) and reads rxbuf[3] as the
 * log2 capacity byte.  Return 0x18 (24) → 1<<24 = 16 MB.
 */
void __wrap_flash_do_cmd(const uint8_t *txbuf, uint8_t *rxbuf, size_t count)
{
    (void)txbuf;
    if (rxbuf) {
        memset(rxbuf, 0, count);
        if (count >= 4)
            rxbuf[3] = 24;   /* 1 << 24 = 16 MB */
    }
}

/* ═════════════════════════════════════════════════════════════════════════
 * 1 ms FreeRTOS tick timer
 * Drives mSecTimer, InkeyTimer, PauseTimer and the user SETTICK timers.
 * ═════════════════════════════════════════════════════════════════════════ */

static void basic_tick_cb(TimerHandle_t t)
{
    (void)t;
    mSecTimer++;
    InkeyTimer++;
    PauseTimer++;
    IntPauseTimer++;
    DECR_IF_ACTIVE(Timer1);
    DECR_IF_ACTIVE(Timer2);
    DECR_IF_ACTIVE(Timer3);
    DECR_IF_ACTIVE(Timer4);
    DECR_IF_ACTIVE(Timer5);
    if (InterruptUsed) {
        for (int i = 0; i < NBRSETTICKS; i++)
            if (TickActive[i]) TickTimer[i]++;
    }
}

static TimerHandle_t g_tick_tmr = NULL;

static void basic_start_tick_timer(void)
{
    g_tick_tmr = xTimerCreate("BTICK",
                               pdMS_TO_TICKS(1),
                               pdTRUE, NULL, basic_tick_cb);
    if (g_tick_tmr)
        xTimerStart(g_tick_tmr, 0);
}

static void basic_stop_tick_timer(void)
{
    if (g_tick_tmr) {
        xTimerStop(g_tick_tmr, 0);
        xTimerDelete(g_tick_tmr, 0);
        g_tick_tmr = NULL;
    }
}

/* ═════════════════════════════════════════════════════════════════════════
 * Platform initialisation
 * ═════════════════════════════════════════════════════════════════════════ */

/* Direct serial output — bypasses the printf macro from m-os-api.h.
 * Uses sys_table slot 438 which is pico SDK printf (USB serial).
 * NOT static — also called from MMBasic.c debug prints. */
void serial_dbg(const char *msg)
{
    static void * const * const _st = (void * const *)0x10FFF000UL;
    typedef void(*fn)(const char*,...);
    ((fn)_st[438])("%s", msg);
}

void basic_platform_init(void)
{
    /* ── Allocate interpreter memory pools from PSRAM ─────────────────────
     * All of these were previously static BSS arrays totalling ~460 KB.
     * Frank OS apps have a very small static BSS budget (~72 KB).
     * malloc/calloc are resolved by the OS loader → pvPortMalloc → PSRAM.
     */

    /* BASIC interpreter heap (AllMemory) + page bitmap (psmap) */
    if (!AllMemory) {
        AllMemory = (unsigned char *)calloc(1, HEAP_MEMORY_SIZE + 256);
        if (!AllMemory) {
            extern void basic_textbuf_putc(int c);
            const char *msg = "\r\n[FATAL] Out of memory: cannot allocate AllMemory\r\n";
            while (*msg) basic_textbuf_putc((unsigned char)*msg++);
            return;
        }
        MMHeap = AllMemory;
    }
    if (!psmap)
        psmap = (unsigned int *)calloc(6 * 1024 * 1024 / PAGESIZE / PAGESPERWORD,
                                       sizeof(unsigned int));

    /* BASIC variable table and function/subroutine table */
    if (!g_vartbl)
        g_vartbl = (struct s_vartbl *)calloc(MAXVARS, sizeof(struct s_vartbl));
    if (!funtbl)
        funtbl   = (struct s_funtbl *)calloc(MAXSUBFUN, sizeof(struct s_funtbl));

    if (!g_vartbl || !funtbl) {
        extern void basic_textbuf_putc(int c);
        const char *msg = "\r\n[FATAL] Out of memory: cannot allocate vartbl/funtbl\r\n";
        while (*msg) basic_textbuf_putc((unsigned char)*msg++);
        return;
    }

    /* Allocate the large program buffers from PSRAM (~328 KB each). */
    if (!g_basic_prog_buf) {
        g_basic_prog_buf = (uint8_t *)calloc(1, MAX_PROG_SIZE);
        if (!g_basic_prog_buf) {
            /* If this fails, the interpreter cannot run — show a visible error. */
            extern void basic_textbuf_putc(int c);
            const char *msg = "\r\n[FATAL] Out of memory: cannot allocate BASIC program buffer\r\n";
            while (*msg) basic_textbuf_putc((unsigned char)*msg++);
            return;
        }
    }
    if (!g_basic_lib_buf) {
        g_basic_lib_buf = (uint8_t *)calloc(1, MAX_PROG_SIZE);
        if (!g_basic_lib_buf) {
            const char *msg = "\r\n[WARN] No library buffer: LIBRARY features disabled\r\n";
            while (*msg) basic_textbuf_putc((unsigned char)*msg++);
            /* Non-fatal: library support is optional. */
        }
    }

    /* Allocate and initialise option/savedvars buffers. */
    if (!g_basic_opt_buf) {
        g_basic_opt_buf = (uint8_t *)malloc(FLASH_ERASE_SIZE);
        if (!g_basic_opt_buf) {
            extern void basic_textbuf_putc(int c);
            const char *msg = "\r\n[FATAL] Out of memory: opt buf\r\n";
            while (*msg) basic_textbuf_putc((unsigned char)*msg++);
            return;
        }
    }
    if (!g_basic_savedvars_buf) {
        g_basic_savedvars_buf = (uint8_t *)malloc(SAVEDVARS_FLASH_SIZE);
        if (!g_basic_savedvars_buf) {
            extern void basic_textbuf_putc(int c);
            const char *msg = "\r\n[FATAL] Out of memory: savedvars buf\r\n";
            while (*msg) basic_textbuf_putc((unsigned char)*msg++);
            return;
        }
    }
    memset(g_basic_opt_buf,       0xFF, FLASH_ERASE_SIZE);
    memset(g_basic_savedvars_buf, 0xFF, SAVEDVARS_FLASH_SIZE);
    memset(g_basic_prog_buf,      0,    MAX_PROG_SIZE);
    if (g_basic_lib_buf)
        memset(g_basic_lib_buf, 0, MAX_PROG_SIZE);

    /* Assign the flash pointers to our RAM buffers. */
    flash_option_contents = g_basic_opt_buf;
    SavedVarsFlash        = g_basic_savedvars_buf;
    flash_target_contents = g_basic_prog_buf;
    flash_progmemory      = g_basic_prog_buf;
    flash_libmemory       = g_basic_lib_buf ? g_basic_lib_buf : g_basic_prog_buf;

    /* Initialise Draw.c font table. */
    serial_dbg("[basic] before initFonts\n");
    initFonts();
    serial_dbg("[basic] initFonts done\n");

    /* Start the 1 ms timer. */
    serial_dbg("[basic] starting tick timer\n");
    basic_start_tick_timer();
    serial_dbg("[basic] platform_init done\n");
}

/* ═════════════════════════════════════════════════════════════════════════
 * BASIC interpreter entry point
 * ═════════════════════════════════════════════════════════════════════════ */

/* These are declared in MMBasic.h / Hardware_Includes.h but not always
 * visible; verify they compile. */
extern jmp_buf jmprun;
extern jmp_buf mark;
extern int MMCharPos;
extern int MMPromptPos;
extern unsigned char EchoOption;

void basic_run_interpreter(void)
{
reset:
    serial_dbg("[basic] run_interpreter start\n");
    /* Set up sensible defaults (replaces LoadOptions + hardware init). */
    ResetOptions(true);
    serial_dbg("[basic] ResetOptions done\n");
    Option.DISPLAY_CONSOLE = 1;   /* enable console display output */
    Option.Width           = 79;  /* terminal width */
    Option.Height          = 24;  /* terminal height */
    Option.SerialConsole   = 0;   /* no UART serial console */
    Option.CPU_Speed       = 150000;

    /* Point program memory at our RAM buffer. */
    flash_progmemory = g_basic_prog_buf;
    flash_libmemory  = g_basic_lib_buf;
    serial_dbg("[basic] m_alloc...\n");
    m_alloc(M_PROG);
    LibMemory = (uint8_t *)flash_libmemory;

    /* Initialise the heap and BASIC interpreter state. */
    serial_dbg("[basic] InitHeap...\n");
    InitHeap(true);
    serial_dbg("[basic] InitBasic...\n");
    InitBasic();
    serial_dbg("[basic] InitBasic done\n");

    /* Override OptionConsole to display-only after InitBasic sets it to 3. */
    OptionConsole = 2;

    /* Print startup banner. */
    serial_dbg("[basic] printing banner...\n");
    MMPrintString("\r\nMMBasic " VERSION " " CHIP "Frank OS port\r\n");
    MMPrintString(COPYRIGHT);
    MMPrintString("\r\n");
    serial_dbg("[basic] banner done\n");

    /* Set the SoftReset recovery point then arm it. */
    int rst = setjmp(g_softreset_jmp);
    if (rst != 0) {
        if (g_closing)
            goto shutdown;
        /* Re-initialise after a soft reset. */
        OptionConsole = 2;
        flash_progmemory = g_basic_prog_buf;
        m_alloc(M_PROG);
        InitHeap(true);
        InitBasic();
        OptionConsole = 2;
        MMPrintString("\r\nSystem restarted.\r\n");
    }
    g_softreset_armed = true;

    /* ── BASIC prompt / execution loop ─────────────────────────────────── */
    /* Set the longjmp point for error recovery and end-of-command. */
    if (setjmp(mark) != 0)
        ClearTempMemory();   /* error recovery: clear temp strings */

    /* If an autorun file was specified via argv[1], inject RUN command. */
    if (g_autorun_path[0]) {
        char runcmd[280];
        snprintf(runcmd, sizeof(runcmd), "RUN \"%s\"", g_autorun_path);
        g_autorun_path[0] = '\0';  /* consume — only auto-run once */
        strcpy((char *)inpbuf, runcmd);
        tokenise(true);
        if (setjmp(jmprun) != 0) {
            PrepareProgram(false);
            CurrentLinePtr = 0;
        }
        ExecuteProgram(tknbuf);
        memset(inpbuf, 0, STRINGSIZE);
        longjmp(mark, 1);
    }

    while (!g_closing) {
        MMAbort   = false;
        BreakKey  = BREAK_KEY;
        EchoOption = true;
        ClearTempMemory();
        CurrentLinePtr = NULL;

        if (MMCharPos > 1)
            MMPrintString("\r\n");

        PrepareProgram(false);

        /* Print prompt (simplified: no MM.PROMPT sub detection). */
        MMPrintString("> ");
        MMPromptPos = 2;

        EditInputLine();

        if (!*inpbuf)
            continue;   /* empty line: re-prompt */

        tokenise(true);  /* convert to token stream */

        /* Error recovery for execution errors (e.g. RUN errors). */
        if (setjmp(jmprun) != 0) {
            PrepareProgram(false);
            CurrentLinePtr = 0;
        }

        ExecuteProgram(tknbuf);

        /* After each command, restart the prompt loop via longjmp. */
        memset(inpbuf, 0, STRINGSIZE);
        longjmp(mark, 1);
    }

shutdown:
    g_softreset_armed = false;
    basic_stop_tick_timer();
}
