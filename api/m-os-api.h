/*
 * Originally from Murmulator OS 2 by DnCraptor
 * https://github.com/DnCraptor/murmulator-os2
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef M_OS_API_H
#define M_OS_API_H

#ifndef M_API_VERSION

#define __force_inline __attribute__((always_inline))
#define inline __force_inline
// switch used in MinSizeRel optimisation style will call __gnu_thumb1_case_uni, not defined on this stage
#define switch DO_NOT_USE_SWITCH

#ifdef __cplusplus
extern "C" {
#endif

#if !M_API_VERSION
#define M_API_VERSION 27
#endif

#ifndef M_OS_API_SYS_TABLE_BASE
#define M_OS_API_SYS_TABLE_BASE ((void*)(0x10000000ul + (16 << 20) - (4 << 10)))
static const unsigned long * const _sys_table_ptrs = (const unsigned long * const)M_OS_API_SYS_TABLE_BASE;
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "m-os-api-tasks.h"
#include "m-os-api-ff.h"

#define __in_boota(group) __attribute__((section(".boota" group)))
#define	__aligned(x)	__attribute__((__aligned__(x)))

#define _snprintfPtrIdx 29

#define _draw_text_ptr_idx 30
#define _draw_window_ptr_idx 31

/*! \brief Macro to return the maximum of two comparable values
 *  \ingroup pico_platform
 */
#ifndef MAX
#define MAX(a, b) ((a)>(b)?(a):(b))
#endif

/*! \brief Macro to return the minimum of two comparable values
 *  \ingroup pico_platform
 */
#ifndef MIN
#define MIN(a, b) ((b)>(a)?(a):(b))
#endif


//#include <stdio.h>
typedef int	(*snprintf_ptr_t)(char *__restrict, size_t, const char *__restrict, ...) _ATTRIBUTE ((__format__ (__printf__, 3, 4)));
#define snprintf(...) ((snprintf_ptr_t)_sys_table_ptrs[_snprintfPtrIdx])(__VA_ARGS__)

// grapthics.h
typedef void (*draw_text_ptr_t)(const char *string, uint32_t x, uint32_t y, uint8_t color, uint8_t bgcolor);
inline static void draw_text(const char *string, uint32_t x, uint32_t y, uint8_t color, uint8_t bgcolor) {
    ((draw_text_ptr_t)_sys_table_ptrs[_draw_text_ptr_idx])(string, x, y, color, bgcolor);
}

typedef void (*draw_window_ptr_t)(const char*, uint32_t x, uint32_t y, uint32_t width, uint32_t height);
inline static void draw_window(const char* t, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    ((draw_window_ptr_t)_sys_table_ptrs[_draw_window_ptr_idx])(t, x, y, width, height);
}

typedef __builtin_va_list __gnuc_va_list;
#define __VALIST __gnuc_va_list

typedef int (*vsnprintf_ptr_t)(char *__restrict, size_t, const char *__restrict, __VALIST)
               _ATTRIBUTE ((__format__ (__printf__, 3, 0)));
inline static int vsnprintf(char *__restrict buff, size_t lim, const char *__restrict msg, __VALIST lst) {
    return ((vsnprintf_ptr_t)_sys_table_ptrs[67])(buff, lim, msg, lst);
}

typedef void (*goutf_ptr_t)(const char *__restrict str, ...) _ATTRIBUTE ((__format__ (__printf__, 1, 2)));
#define goutf(...) ((goutf_ptr_t)_sys_table_ptrs[41])(__VA_ARGS__)
#define printf(...) ((goutf_ptr_t)_sys_table_ptrs[41])(__VA_ARGS__)
#define serial_printf(...) ((goutf_ptr_t)_sys_table_ptrs[438])(__VA_ARGS__)

/* typedef va_list only when required */
#if __POSIX_VISIBLE >= 200809 || __XSI_VISIBLE
#ifdef __GNUC__
#ifndef _VA_LIST_DEFINED
typedef __gnuc_va_list va_list;
#define _VA_LIST_DEFINED
#endif
#else /* !__GNUC__ */
#include <stdarg.h>
#endif
#endif /* __POSIX_VISIBLE >= 200809 || __XSI_VISIBLE */
#define va_start(v,l)	__builtin_va_start(v,l)
#define va_end(v)	__builtin_va_end(v)

inline static void gouta(char* string) {
    typedef void (*t_ptr_t)(char*);
    ((t_ptr_t)_sys_table_ptrs[127])(string);
}
#define puts(s) gouta(s)
/*
static void goutf(const char *__restrict str, ...) {
    va_list ap;
    char* buf = (char*)pvPortMalloc(512);
    va_start(ap, str);
    vsnprintf(buf, 512, str, ap); // TODO: optimise (skip)
    va_end(ap);
    gouta(buf);
    vPortFree(buf);
}
*/
inline static bool graphics_set_mode(int color) {
    typedef bool (*ptr_t)( int color );
    return ((ptr_t)_sys_table_ptrs[34])(color);
}
inline static void clrScr(uint8_t color) {
    typedef void (*cls_ptr_t)( uint8_t color );
    ((cls_ptr_t)_sys_table_ptrs[44])(color);
}

typedef void (*graphics_set_con_pos_ptr_t)(int x, int y);
typedef void (*graphics_set_con_color_ptr_t)(uint8_t color, uint8_t bgcolor);
inline static void graphics_set_con_pos(int x, int y) {
    ((graphics_set_con_pos_ptr_t)_sys_table_ptrs[42])(x, y);
}
inline static void graphics_set_con_color(uint8_t color, uint8_t bgcolor) {
    typedef void (*graphics_set_con_color_ptr_t)(uint8_t color, uint8_t bgcolor);
    ((graphics_set_con_color_ptr_t)_sys_table_ptrs[43])(color, bgcolor);
}

typedef FIL* (*FIL_getter_ptr_t)();
inline static FIL* get_stdout() {
    return ((FIL_getter_ptr_t)_sys_table_ptrs[68])();
}
inline static FIL* get_stderr() {
    return ((FIL_getter_ptr_t)_sys_table_ptrs[69])();
}

typedef void (*fgoutf_ptr_t)(FIL*, const char *__restrict str, ...) _ATTRIBUTE ((__format__ (__printf__, 2, 3)));
#define fgoutf(...) ((fgoutf_ptr_t)_sys_table_ptrs[70])(__VA_ARGS__)
#define fprintf(...) ((fgoutf_ptr_t)_sys_table_ptrs[70])(__VA_ARGS__)

typedef uint32_t (*u32v_ptr_t)();
inline static uint32_t psram_size() {
    return ((u32v_ptr_t)_sys_table_ptrs[71])();
}
typedef void (*vv_ptr_t)();
inline static void psram_cleanup() {
    return ((vv_ptr_t)_sys_table_ptrs[72])();
}
typedef void (*vu32u8_ptr_t)(uint32_t addr32, uint8_t v);
inline static void write8psram(uint32_t addr32, uint8_t v) {
    return ((vu32u8_ptr_t)_sys_table_ptrs[73])(addr32, v);
}
typedef void (*vu32u16_ptr_t)(uint32_t addr32, uint16_t v);
inline static void write16psram(uint32_t addr32, uint16_t v) {
    return ((vu32u16_ptr_t)_sys_table_ptrs[74])(addr32, v);
}
typedef void (*vu32u32_ptr_t)(uint32_t addr32, uint32_t v);
inline static void write32psram(uint32_t addr32, uint32_t v) {
    return ((vu32u32_ptr_t)_sys_table_ptrs[75])(addr32, v);
}
typedef uint8_t (*u8u32_ptr_t)(uint32_t addr32);
inline static uint8_t read8psram(uint32_t addr32) {
    return ((u8u32_ptr_t)_sys_table_ptrs[76])(addr32);
}
typedef uint16_t (*u16u32_ptr_t)(uint32_t addr32);
inline static uint16_t read16psram(uint32_t addr32) {
    return ((u16u32_ptr_t)_sys_table_ptrs[77])(addr32);
}
typedef uint32_t (*u32u32_ptr_t)(uint32_t addr32);
inline static uint32_t read32psram(uint32_t addr32) {
    return ((u32u32_ptr_t)_sys_table_ptrs[78])(addr32);
}

typedef uint32_t (*u32u32u32_ptr_t)(uint32_t, uint32_t);
inline static uint32_t __u32u32u32_div(uint32_t x, uint32_t y) {
    return ((u32u32u32_ptr_t)_sys_table_ptrs[79])(x, y);
}
inline static uint32_t __u32u32u32_rem(uint32_t x, uint32_t y) {
    return ((u32u32u32_ptr_t)_sys_table_ptrs[80])(x, y);
}

typedef float (*fff_ptr_t)(float, float);
inline static float __fff_div(float x, float y) {
    return ((fff_ptr_t)_sys_table_ptrs[81])(x, y);
}
inline static float __fff_mul(float x, float y) {
    return ((fff_ptr_t)_sys_table_ptrs[82])(x, y);
}
typedef float (*ffu32_ptr_t)(float, uint32_t);
inline static float __ffu32_mul(float x, uint32_t y) {
    return ((ffu32_ptr_t)_sys_table_ptrs[83])(x, y);
}

typedef double (*ddd_ptr_t)(double, double);
inline static double __ddd_div(double x, double y) {
    return ((ddd_ptr_t)_sys_table_ptrs[84])(x, y);
}
inline static double __ddd_mul(double x, double y) {
    return ((ddd_ptr_t)_sys_table_ptrs[85])(x, y);
}
typedef double (*ddu32_ptr_t)(double, uint32_t);
inline static double __ddu32_mul(double x, uint32_t y) {
    return ((ddu32_ptr_t)_sys_table_ptrs[86])(x, y);
}
typedef double (*ddf_ptr_t)(double, float);
inline static double __ddf_mul(double x, float y) {
    return ((ddf_ptr_t)_sys_table_ptrs[87])(x, y);
}

inline static float __ffu32_div(float x, uint32_t y) {
    return ((ffu32_ptr_t)_sys_table_ptrs[88])(x, y);
}
inline static double __ddu32_div(double x, uint32_t y) {
    return ((ddu32_ptr_t)_sys_table_ptrs[89])(x, y);
}

inline static uint32_t swap_size() {
    return ((u32v_ptr_t)_sys_table_ptrs[90])();
}
inline static uint32_t swap_base_size() {
    return ((u32v_ptr_t)_sys_table_ptrs[91])();
}
typedef uint8_t* (*pu8v_ptr_t)();
inline static uint8_t* swap_base() {
    return ((pu8v_ptr_t)_sys_table_ptrs[92])();
}

inline static uint8_t ram_page_read(uint32_t addr32) {
    return ((u8u32_ptr_t)_sys_table_ptrs[93])(addr32);
}
inline static uint16_t ram_page_read16(uint32_t addr32) {
    return ((u16u32_ptr_t)_sys_table_ptrs[94])(addr32);
}
inline static uint32_t ram_page_read32(uint32_t addr32) {
    return ((u32u32_ptr_t)_sys_table_ptrs[95])(addr32);
}

inline static void ram_page_write(uint32_t addr32, uint8_t value) {
    ((vu32u8_ptr_t)_sys_table_ptrs[96])(addr32, value);
}
inline static void ram_page_write16(uint32_t addr32, uint16_t value) {
    ((vu32u16_ptr_t)_sys_table_ptrs[97])(addr32, value);
}
inline static void ram_page_write32(uint32_t addr32, uint32_t value) {
    ((vu32u32_ptr_t)_sys_table_ptrs[98])(addr32, value);
}

typedef struct {
    char* del_addr;
    char* prg_addr;
    uint16_t sec_num;
} sect_entry_t;

typedef int (*bootb_req_ver_ptr_t)( void );
typedef void* (*bootb_init_ptr_t)( void );
typedef int (*bootb_main_ptr_t)( int, char** );
typedef void (*bootb_fini_ptr_t)( void* );
typedef void (*bootb_sig_ptr_t)( int );

typedef struct {
    bootb_req_ver_ptr_t req_ver_fn;
    bootb_init_ptr_t _init_fn;
    bootb_main_ptr_t main_fn;
    bootb_fini_ptr_t _fini_fn;
    bootb_sig_ptr_t sig_fn;
    void* /* list_ of sect_entry_t*/ sections;
} bootb_ctx_t;

typedef struct {
    const char* key;
    char* value;
} vars_t;

typedef enum {
    INITIAL,
    PREPARED,
    FOUND,
    VALID,
    LOAD,
    EXECUTED,
    INVALIDATED,
    SIGTERM_ST
} cmd_exec_stage_t;

typedef struct cmd_ctx {
    uint32_t argc;
    char** argv;
    char* orig_cmd;

    FIL* std_in;
    FIL* std_out;
    FIL* std_err;
    int ret_code;
    bool detached;

    struct cmd_ctx* prev;
    bootb_ctx_t* pboot_ctx;

    vars_t* vars;
    size_t vars_num;

    struct cmd_ctx* next;

    volatile cmd_exec_stage_t stage;
    void* user_data;
    bool force_flash;
    TaskHandle_t parent_task;
} cmd_ctx_t;

inline static TaskHandle_t xTaskGetCurrentTaskHandle( void ) {
    typedef TaskHandle_t (*f_ptr_t)(void);
    return ((f_ptr_t)_sys_table_ptrs[136])();
}

inline static
void vTaskSetThreadLocalStoragePointer( TaskHandle_t xTaskToSet,
                                        BaseType_t xIndex,
                                        void* pvValue ) {
    typedef void (*f_ptr_t)(TaskHandle_t, BaseType_t, void*);
    ((f_ptr_t)_sys_table_ptrs[23])(xTaskToSet, xIndex, pvValue);
}

inline static cmd_ctx_t* get_cmd_startup_ctx() {
    typedef cmd_ctx_t* (*f_ptr_t)();
    return ((f_ptr_t)_sys_table_ptrs[99])();
}

typedef int (*ipc_ptr_t)(const char *);
inline static int atoi (const char * s) {
    return ((ipc_ptr_t)_sys_table_ptrs[100])(s);
}
inline static void overclocking() {
    return ((vv_ptr_t)_sys_table_ptrs[101])();
}
typedef void (*vu32u32u32_ptr_t)(uint32_t, uint32_t, uint32_t);
inline static void overclocking_ex(uint32_t vco, int32_t postdiv1, int32_t postdiv2) {
    return ((vu32u32u32_ptr_t)_sys_table_ptrs[102])(vco, postdiv1, postdiv2);
}
typedef void (*vu32_ptr_t)(uint32_t);
inline static void set_overclocking(uint32_t khz) {
    return ((vu32_ptr_t)_sys_table_ptrs[104])(khz);
}
inline static uint32_t get_overclocking_khz() {
    return ((u32v_ptr_t)_sys_table_ptrs[103])();
}

inline static void set_sys_clock_pll(uint32_t vco_freq, uint32_t post_div1, uint32_t post_div2) {
    ((vu32u32u32_ptr_t)_sys_table_ptrs[105])(vco_freq, post_div1, post_div2);
} 
typedef bool (*bu32pu32pu32pu32_ptr_t)(uint32_t, uint32_t*, uint32_t*, uint32_t*);
inline static bool check_sys_clock_khz(uint32_t freq_khz, uint32_t *vco_out, uint32_t *postdiv1_out, uint32_t *postdiv2_out) {
    return ((bu32pu32pu32pu32_ptr_t)_sys_table_ptrs[106])(freq_khz, vco_out, postdiv1_out, postdiv2_out);
}

typedef char* (*pcpc_ptr_t)(char*);
inline static char* next_token(char* t) {
    return ((pcpc_ptr_t)_sys_table_ptrs[107])(t);
}

inline static size_t strlen(const char * s) {
    typedef size_t (*fn_ptr_t)(const char * s);
    return ((fn_ptr_t)_sys_table_ptrs[62])(s);
}

inline static char* strncpy(char* t, const char * s, size_t sz) {
    typedef char* (*fn_ptr_t)(char*, const char*, size_t);
    return ((fn_ptr_t)_sys_table_ptrs[63])(t, s, sz);
}

inline static int strcmp(const char * s1, const char * s2) {
    typedef int (*fn_ptr_t)(const char*, const char*);
    return ((fn_ptr_t)_sys_table_ptrs[108])(s1, s2);
}

inline static int strncmp(const char * s1, const char * s2, size_t sz) {
    typedef int (*fn_ptr_t)(const char*, const char*, size_t);
    return ((fn_ptr_t)_sys_table_ptrs[109])(s1, s2, sz);
}

/* Used to pass information about the heap out of vPortGetHeapStats(). */
typedef struct xHeapStats
{
    size_t xAvailableHeapSpaceInBytes;      /* The total heap size currently available - this is the sum of all the free blocks, not the largest block that can be allocated. */
    size_t xSizeOfLargestFreeBlockInBytes;  /* The maximum size, in bytes, of all the free blocks within the heap at the time vPortGetHeapStats() is called. */
    size_t xSizeOfSmallestFreeBlockInBytes; /* The minimum size, in bytes, of all the free blocks within the heap at the time vPortGetHeapStats() is called. */
    size_t xNumberOfFreeBlocks;             /* The number of free memory blocks within the heap at the time vPortGetHeapStats() is called. */
    size_t xMinimumEverFreeBytesRemaining;  /* The minimum amount of total free memory (sum of all free blocks) there has been in the heap since the system booted. */
    size_t xNumberOfSuccessfulAllocations;  /* The number of calls to pvPortMalloc() that have returned a valid memory block. */
    size_t xNumberOfSuccessfulFrees;        /* The number of calls to vPortFree() that has successfully freed a block of memory. */
} HeapStats_t;
inline static void vPortGetHeapStats( HeapStats_t * pxHeapStats ) {
    typedef void (*fn_ptr_t)(HeapStats_t *);
    ((fn_ptr_t)_sys_table_ptrs[110])(pxHeapStats);
}

inline static uint32_t get_cpu_ram_size(void) {
    typedef uint32_t (*fn_ptr_t)(void);
    return ((fn_ptr_t)_sys_table_ptrs[111])();
}
inline static uint32_t get_cpu_flash_size() {
    typedef uint32_t (*fn_ptr_t)();
    return ((fn_ptr_t)_sys_table_ptrs[112])();
}

inline static FATFS* get_mount_fs() { // only one FS is supported foe now
    typedef FATFS* (*fn_ptr_t)();
    return ((fn_ptr_t)_sys_table_ptrs[113])();
}
inline static uint32_t f_getfree32(FATFS * fs) {
    typedef uint32_t (*fn_ptr_t)(FATFS*);
    return ((fn_ptr_t)_sys_table_ptrs[114])(fs);
}

typedef bool (*scancode_handler_t)(const uint32_t);
inline static scancode_handler_t get_scancode_handler() {
    typedef scancode_handler_t (*fn_ptr_t)();
    return ((fn_ptr_t)_sys_table_ptrs[115])();
}
inline static void set_scancode_handler(scancode_handler_t h) {
    typedef void (*fn_ptr_t)(scancode_handler_t);
    ((fn_ptr_t)_sys_table_ptrs[116])(h);
}
typedef bool (*cp866_handler_t)(const char, uint32_t);
inline static cp866_handler_t get_cp866_handler() {
    typedef cp866_handler_t (*fn_ptr_t)();
    return ((fn_ptr_t)_sys_table_ptrs[117])();
}
inline static void set_cp866_handler(cp866_handler_t h) {
    typedef void (*fn_ptr_t)(cp866_handler_t);
    ((fn_ptr_t)_sys_table_ptrs[118])(h);
}
inline static void gbackspace() {
    typedef void (*fn_ptr_t)();
    ((fn_ptr_t)_sys_table_ptrs[119])();
}

inline static bool load_firmware(char* path) {
    typedef bool (*fn_ptr_t)(char*);
    return ((fn_ptr_t)_sys_table_ptrs[65])(path);
}
inline static void run_app(char * name) {
    typedef void (*fn_ptr_t)(char*);
    ((fn_ptr_t)_sys_table_ptrs[66])(name);
}
inline static void system_reboot(void) {
    typedef void (*fn_ptr_t)(uint32_t, uint32_t, uint32_t);
    ((fn_ptr_t)_sys_table_ptrs[536])(0, 0, 0);
}
inline static void system_reboot_to_firmware(void) {
    typedef void (*fn_ptr_t)(void);
    ((fn_ptr_t)_sys_table_ptrs[537])();
}

inline static bool is_new_app(cmd_ctx_t* c) {
    typedef bool (*fn_ptr_t)(cmd_ctx_t* c);
    return ((fn_ptr_t)_sys_table_ptrs[120])(c);
}
inline static bool run_new_app(cmd_ctx_t* c) {
    typedef bool (*fn_ptr_t)(cmd_ctx_t* c);
    return ((fn_ptr_t)_sys_table_ptrs[121])(c);
}

#define CHAR_CODE_BS    8
#define CHAR_CODE_UP    17
#define CHAR_CODE_DOWN  18
#define CHAR_CODE_ENTER '\n'
#define CHAR_CODE_TAB   '\t'
#define CHAR_CODE_ESC   0x1B
#define CHAR_CODE_EOF   0xFF

inline static char getch(void) {
    typedef char (*fn_ptr_t)(void);
    return ((fn_ptr_t)_sys_table_ptrs[122])();
}
#define getchar() getch()
inline static void putc(char c) {
    typedef void (*fn_ptr_t)(char);
    ((fn_ptr_t)_sys_table_ptrs[123])(c);
}
#define putchar(c) putc(c)

inline static void cleanup_bootb_ctx(cmd_ctx_t* ctx) {
    typedef void (*fn_ptr_t)(cmd_ctx_t* ctx);
    ((fn_ptr_t)_sys_table_ptrs[124])(ctx);
}
inline static bool load_app(cmd_ctx_t* ctx) {
    typedef bool (*fn_ptr_t)(cmd_ctx_t* ctx);
    return ((fn_ptr_t)_sys_table_ptrs[125])(ctx);
}
inline static void exec(cmd_ctx_t* ctx) {
    typedef void (*fn_ptr_t)(cmd_ctx_t* ctx);
    ((fn_ptr_t)_sys_table_ptrs[126])(ctx);
}

inline static bool exists(cmd_ctx_t* ctx) {
    typedef bool (*fn_ptr_t)(cmd_ctx_t*);
    return ((fn_ptr_t)_sys_table_ptrs[128])(ctx);
}
inline static char* concat(const char* s1, const char* s2) {
    typedef char* (*fn_ptr_t)(const char*, const char*);
    return ((fn_ptr_t)_sys_table_ptrs[129])(s1, s2);
}

inline static void flash_block(uint8_t* buffer, size_t flash_target_offset) {
    typedef void (*fn_ptr_t)(uint8_t*, size_t);
    ((fn_ptr_t)_sys_table_ptrs[131])(buffer, flash_target_offset);
}

inline static size_t get_heap_total() {
    typedef size_t (*fn_ptr_t)();
    return ((fn_ptr_t)_sys_table_ptrs[132])();
}
inline static uint32_t swap_pages() {
    typedef uint32_t (*fn_ptr_t)();
    return ((fn_ptr_t)_sys_table_ptrs[133])();
}
inline static uint16_t* swap_pages_base() {
    typedef uint16_t* (*fn_ptr_t)();
    return ((fn_ptr_t)_sys_table_ptrs[134])();
}
inline static uint32_t swap_page_size() {
    typedef uint32_t (*fn_ptr_t)();
    return ((fn_ptr_t)_sys_table_ptrs[135])();
}

inline static char* copy_str(const char* s) {
    typedef char* (*fn_ptr_t)(const char*);
    return ((fn_ptr_t)_sys_table_ptrs[137])(s);
}
inline static cmd_ctx_t* get_cmd_ctx() {
    typedef cmd_ctx_t* (*fn_ptr_t)();
    return ((fn_ptr_t)_sys_table_ptrs[138])();
}
inline static void cleanup_ctx(cmd_ctx_t* src) {
    typedef void (*fn_ptr_t)(cmd_ctx_t*);
    ((fn_ptr_t)_sys_table_ptrs[139])(src);
}

inline static void* malloc(size_t xWantedSize) {
    typedef void* (*pvPortMalloc_ptr_t)( size_t xWantedSize );
    return ((pvPortMalloc_ptr_t)_sys_table_ptrs[32])(xWantedSize);
}

inline static void* __memset(void* p, int v, size_t sz) {
    typedef void* (*fn)(void *, int, size_t);
    return ((fn)_sys_table_ptrs[142])(p, v, sz);
}

inline static void* calloc(size_t cnt, size_t sz) {
    typedef char* (*pvPortCalloc_ptr_t)( size_t cnt, size_t sz );
    return ((pvPortCalloc_ptr_t)_sys_table_ptrs[166])(cnt, sz);
}
inline static void* pvPortCalloc(size_t cnt, size_t sz) {
    typedef char* (*pvPortCalloc_ptr_t)( size_t cnt, size_t sz );
    return ((pvPortCalloc_ptr_t)_sys_table_ptrs[166])(cnt, sz);
}
inline static size_t xPortGetFreeHeapSize( void ) {
    typedef size_t (*_ptr_t)(void);
    return ((_ptr_t)_sys_table_ptrs[145])();
}
inline static void free(void * pv) {
    typedef void (*vPortFree_ptr_t)( void * pv );
    ((vPortFree_ptr_t)_sys_table_ptrs[33])(pv);
}
inline static char* get_ctx_var(cmd_ctx_t* src, const char* k) {
    typedef char* (*fn_ptr_t)(cmd_ctx_t*, const char*);
    return ((fn_ptr_t)_sys_table_ptrs[140])(src, k);
}
inline static cmd_ctx_t* set_ctx_var(cmd_ctx_t* src, const char* k, char* v) {
    typedef void (*fn_ptr_t)(cmd_ctx_t*, const char*, char*);
    ((fn_ptr_t)_sys_table_ptrs[141])(src, k, v);
}
inline static cmd_ctx_t* clone_ctx(cmd_ctx_t* src) {
    typedef cmd_ctx_t* (*fn_ptr_t)(cmd_ctx_t*);
    return ((fn_ptr_t)_sys_table_ptrs[143])(src);
}
inline static void remove_ctx(cmd_ctx_t* src) {
    typedef void (*fn_ptr_t)(cmd_ctx_t*);
    ((fn_ptr_t)_sys_table_ptrs[144])(src);
}

inline static uint32_t get_console_width() {
    typedef uint32_t (*fn_ptr_t)();
    return ((fn_ptr_t)_sys_table_ptrs[146])();
}
inline static uint32_t get_console_height() {
    typedef uint32_t (*fn_ptr_t)();
    return ((fn_ptr_t)_sys_table_ptrs[147])();
}
inline static uint32_t get_screen_width() {
    typedef uint32_t (*fn_ptr_t)();
    return ((fn_ptr_t)_sys_table_ptrs[158])();
}
inline static uint32_t get_screen_height() {
    typedef uint32_t (*fn_ptr_t)();
    return ((fn_ptr_t)_sys_table_ptrs[159])();
}
typedef FIL FILE;
inline static int getc(FILE* in) { // return -1 for EOF
    typedef int (*fn_ptr_t)(FILE* in);
    return ((fn_ptr_t)_sys_table_ptrs[148])(in);
}

inline static uint8_t* get_buffer() {
    typedef uint8_t* (*fn_ptr_t)();
    return ((fn_ptr_t)_sys_table_ptrs[152])();
}
inline static size_t get_buffer_size() {
    typedef size_t (*fn_ptr_t)();
    return ((fn_ptr_t)_sys_table_ptrs[153])();
}
inline static uint8_t get_screen_bitness() {
    typedef uint8_t (*fn_ptr_t)();
    return ((fn_ptr_t)_sys_table_ptrs[154])();
}
inline static void cleanup_graphics() {
    typedef void (*fn_ptr_t)();
    ((fn_ptr_t)_sys_table_ptrs[155])();
}
inline static uint8_t get_console_bitness() {
    typedef uint8_t (*fn_ptr_t)();
    return ((fn_ptr_t)_sys_table_ptrs[157])();
}


typedef void (*vv_fn)(void);
typedef void (*vb_fn)(bool);
typedef bool (*bi_fn)(int);
typedef int (*iv_fn)(void);
typedef bool (*bv_fn)(void);
typedef uint32_t (*u32v_fn)(void);
typedef uint8_t* (*pu8v_fn)(void);
typedef uint8_t (*u8v_fn)(void);
typedef void (*vpu8_fn)(uint8_t*);
typedef void (*vu8_fn)(uint8_t);
typedef void (*dt_fn)(const char*, int, int, uint8_t, uint8_t);
typedef void (*vii_fn)(const int, const int);
typedef void (*vu8u8_fn)(uint8_t, uint8_t);
typedef bool (*bpu8u8u8_fn)(uint8_t*, uint8_t, uint8_t);
typedef bool (*bu8u8_fn)(uint8_t, uint8_t);
typedef void (*vcu32_fn)(const uint32_t);
typedef uint8_t* (*dma_handler_impl_fn)(void);

typedef struct graphics_driver {
    cmd_ctx_t* ctx;
    vv_fn init;
    vv_fn cleanup;
    bi_fn set_mode;
    bv_fn is_text;
    u32v_fn console_width;
    u32v_fn console_height;
    u32v_fn screen_width;
    u32v_fn screen_height;
    pu8v_fn buffer;
    vpu8_fn set_buffer;
    vu8_fn cls;
    dt_fn draw_text;
    u8v_fn console_bitness;
    u8v_fn screen_bitness;
    vii_fn set_offsets;
    vcu32_fn set_bgcolor;
    u32v_fn allocated;
    vii_fn set_con_pos;
    iv_fn pos_x;
    iv_fn pos_y;
    vu8u8_fn set_con_color;
    vpu8_fn print;
    vv_fn backspace;
    vb_fn lock_buffer;
    iv_fn get_mode;
    bi_fn is_mode_text;
    vu8_fn set_cursor_color;
    iv_fn get_default_mode;
    bu8u8_fn set_font;
    u8v_fn get_font_width;
    u8v_fn get_font_height;
    bpu8u8u8_fn set_ext_font;
    pu8v_fn get_font_table;
} graphics_driver_t;
inline static void install_graphics_driver(graphics_driver_t* gd) {
    typedef void (*fn_ptr_t)(graphics_driver_t* gd);
    ((fn_ptr_t)_sys_table_ptrs[156])(gd);
}
inline static graphics_driver_t* get_graphics_driver() {
    typedef graphics_driver_t* (*fn_ptr_t)();
    return ((fn_ptr_t)_sys_table_ptrs[160])();
}
inline static bool is_buffer_text(void) {
    typedef bool (*fn_ptr_t)();
    return ((fn_ptr_t)_sys_table_ptrs[161])();
}
inline static int graphics_get_mode(void) {
    typedef int (*fn_ptr_t)();
    return ((fn_ptr_t)_sys_table_ptrs[162])();
}
inline static bool graphics_is_mode_text(int mode) {
    typedef bool (*fn_ptr_t)(int);
    return ((fn_ptr_t)_sys_table_ptrs[163])(mode);
}

typedef uint8_t* (*dma_handler_impl_fn)(void);
inline static void set_vga_dma_handler_impl(dma_handler_impl_fn impl) {
    typedef void (*fn_ptr_t)(dma_handler_impl_fn);
    return ((fn_ptr_t)_sys_table_ptrs[164])(impl);
}

inline static void set_graphics_clkdiv(uint32_t pixel_clock, uint32_t line_size) {
    typedef void (*fn_ptr_t)(uint32_t, uint32_t);
    ((fn_ptr_t)_sys_table_ptrs[165])(pixel_clock, line_size);
}

inline static void vga_dma_channel_set_read_addr(const volatile void* addr) {
    typedef void (*fn_ptr_t)(const volatile void*);
    ((fn_ptr_t)_sys_table_ptrs[168])(addr);
}

#ifndef __compar_fn_t_defined
#define __compar_fn_t_defined
typedef int (*__compar_fn_t) (const void *, const void *);
#endif
inline static void qsort(void *__base, size_t __nmemb, size_t __size, __compar_fn_t _compar) {
    typedef void (*fn_ptr_t)(void *__base, size_t __nmemb, size_t __size, __compar_fn_t _compar);
    ((fn_ptr_t)_sys_table_ptrs[169])(__base, __nmemb, __size, _compar);
}

inline static size_t strnlen(const char *str, size_t sz) {
    typedef size_t (*fn_ptr_t)(const char *str, size_t sz);
    return ((fn_ptr_t)_sys_table_ptrs[170])(str, sz);
}

inline static void psram_id(uint8_t rx[8]) {
    typedef void (*fn_ptr_t)(uint8_t rx[8]);
    ((fn_ptr_t)_sys_table_ptrs[178])(rx);
}

#define DPAD_LEFT 0x40
#define DPAD_RIGHT 0x80
#define DPAD_DOWN 0x20
#define DPAD_UP 0x10
#define DPAD_START 0x08
#define DPAD_SELECT 0x04
#define DPAD_B 0x02
#define DPAD_A 0x01

#define PS2_LED_SCROLL_LOCK 1
#define PS2_LED_NUM_LOCK    2
#define PS2_LED_CAPS_LOCK   4

inline static uint8_t get_leds_stat() {
    typedef uint8_t (*fn_ptr_t)();
    return ((fn_ptr_t)_sys_table_ptrs[64])();
}

// USB
// (better use usb_driver)
inline static void init_pico_usb_drive() {
    typedef void (*fn_ptr_t)();
    ((fn_ptr_t)_sys_table_ptrs[179])();
}

// (better use usb_driver)
inline static void pico_usb_drive_heartbeat() {
    typedef void (*fn_ptr_t)();
    ((fn_ptr_t)_sys_table_ptrs[180])();
}

inline static bool tud_msc_ejected() {
    typedef bool (*fn_ptr_t)();
    return ((fn_ptr_t)_sys_table_ptrs[181])();
}

// (better use usb_driver)
inline static void set_tud_msc_ejected(bool v) {
    typedef void (*fn_ptr_t)(bool);
    ((fn_ptr_t)_sys_table_ptrs[182])(v);
}

inline static void show_logo(bool with_top) {
    typedef void (*fn_ptr_t)(bool);
    ((fn_ptr_t)_sys_table_ptrs[183])(with_top);
}

inline static char getch_now(void) {
    typedef char (*fn_ptr_t)(void);
    return ((fn_ptr_t)_sys_table_ptrs[184])();
}

inline static void usb_driver(bool on) {
    typedef void (*fn_ptr_t)(bool);
    ((fn_ptr_t)_sys_table_ptrs[185])(on);
}

inline static void set_cursor_color(uint8_t c) {
    typedef void (*fn_ptr_t)(uint8_t);
    ((fn_ptr_t)_sys_table_ptrs[186])(c);
}

inline static void nespad_stat(uint8_t* pad1, uint8_t* pad2) {
    typedef void (*fn_ptr_t)(uint8_t*, uint8_t*);
    ((fn_ptr_t)_sys_table_ptrs[187])(pad1, pad2);
}

inline static uint8_t* graphics_get_font_table(void) { // 189
    typedef uint8_t* (*fn_ptr_t)(void);
    return ((fn_ptr_t)_sys_table_ptrs[189])();
}
inline static uint8_t graphics_get_font_width(void) { // 190
    typedef uint8_t (*fn_ptr_t)(void);
    return ((fn_ptr_t)_sys_table_ptrs[190])();
}
inline static uint8_t graphics_get_font_height(void) { // 191
    typedef uint8_t (*fn_ptr_t)(void);
    return ((fn_ptr_t)_sys_table_ptrs[191])();
}
inline static bool graphics_set_font(uint8_t w, uint8_t h) { // 192
    typedef bool (*fn_ptr_t)(uint8_t, uint8_t);
    return ((fn_ptr_t)_sys_table_ptrs[192])(w, h);
}
inline static bool graphics_set_ext_font(uint8_t* t, uint8_t w, uint8_t h) { // 193
    typedef bool (*fn_ptr_t)(uint8_t*, uint8_t, uint8_t);
    return ((fn_ptr_t)_sys_table_ptrs[193])(t, w, h);
}

// since 0.2.0 (API v.17)
inline static void blimp(uint32_t cnt, uint32_t ticks_to_delay) { // 194
    typedef void (*fn_ptr_t)(uint32_t, uint32_t);
    ((fn_ptr_t)_sys_table_ptrs[194])(cnt, ticks_to_delay);
}

inline static int graphics_con_x(void) { // 195
    typedef int (*fn_ptr_t)(void);
    return ((fn_ptr_t)_sys_table_ptrs[195])();
}
inline static int graphics_con_y(void) { // 196
    typedef int (*fn_ptr_t)(void);
    return ((fn_ptr_t)_sys_table_ptrs[196])();
}

// since 0.2.1 (API v.18)
inline static void pcm_setup(int hz) { // 197
    typedef void (*fn_ptr_t)(int);
    ((fn_ptr_t)_sys_table_ptrs[197])(hz);
}
inline static void pcm_cleanup(void) { // 198
    typedef void (*fn_ptr_t)(void);
    ((fn_ptr_t)_sys_table_ptrs[198])();
}

// assumed callback is executing on core#1
typedef char* (*pcm_end_callback_t)(size_t* size);
// N.B. size in 16-bit values, not in bytes
inline static void pcm_set_buffer(int16_t* buff, uint8_t channels, size_t size, pcm_end_callback_t cb) { // 199
    typedef void (*fn_ptr_t)(int16_t* buff, uint8_t channels, size_t size, pcm_end_callback_t cb);
    ((fn_ptr_t)_sys_table_ptrs[199])(buff, channels, size, cb);
}

// since 0.2.3
#define WAV_IN_PIO 22
// since 0.2.4 (API v.19)
#define BUFSIZE 127
#define BUFSIZ  512
#define SYSTYPE_PASPPI 1

inline static double trunc (double t) {
    typedef double (*fn_ptr_t)(double);
    return ((fn_ptr_t)_sys_table_ptrs[200])(t);
}

inline static double floor (double t) {
    typedef double (*fn_ptr_t)(double);
    return ((fn_ptr_t)_sys_table_ptrs[201])(t);
}

inline static double pow (double x, double y) {
    typedef double (*fn_ptr_t)(double, double);
    return ((fn_ptr_t)_sys_table_ptrs[202])(x, y);
}
inline static float powf(float x, float y) {
    typedef float (*fn)(float, float);
    return ((fn)_sys_table_ptrs[257])(x, y);
}

inline static double sqrt (double x) {
    typedef double (*fn_ptr_t)(double);
    return ((fn_ptr_t)_sys_table_ptrs[203])(x);
}

inline static double sin (double x) {
    typedef double (*fn_ptr_t)(double);
    return ((fn_ptr_t)_sys_table_ptrs[204])(x);
}

inline static double cos (double x) {
    typedef double (*fn_ptr_t)(double);
    return ((fn_ptr_t)_sys_table_ptrs[205])(x);
}

inline static double tan (double x) {
    typedef double (*fn_ptr_t)(double);
    return ((fn_ptr_t)_sys_table_ptrs[206])(x);
}
inline static double atan (double x) {
    typedef double (*fn_ptr_t)(double);
    return ((fn_ptr_t)_sys_table_ptrs[207])(x);
}
inline static double log (double x) {
    typedef double (*fn_ptr_t)(double);
    return ((fn_ptr_t)_sys_table_ptrs[208])(x);
}
inline static double exp (double x) {
    typedef double (*fn_ptr_t)(double);
    return ((fn_ptr_t)_sys_table_ptrs[209])(x);
}

inline static DIR* opendir(const char* p) {
    DIR* d = (DIR*)malloc(sizeof(DIR));
    if (f_opendir(d, p) == FR_OK) return d;
    free(d);
    return NULL;
}

inline static void closedir(DIR* d) {
    if (!d) return;
    f_closedir(d);
    free(d);
}

inline static void fputc(char c, FILE* f) {
    char b[] = { c };
    UINT bw;
    f_write(f, b, 1, &bw);
}

inline static int fgetc(FILE* f) {
    if (f_eof(f)) return -1;
    else {
        char b;
        UINT br;
        if (f_read(f, &b, 1, &br) == FR_OK && br == 1) {
            return b;
        }
    }
    return -1;
}

inline static FILINFO* readdir(DIR* d, FILINFO* fi) {
    if (f_readdir(d, fi) == FR_OK && fi->fname[0] != '\0') {
        return fi;
    }
    free(fi);
    return NULL;
}

typedef void (*usb_detached_handler_t)(void);
inline static bool set_usb_detached_handler(usb_detached_handler_t h) {
    typedef bool (*fn_ptr_t)(usb_detached_handler_t);
    return ((fn_ptr_t)_sys_table_ptrs[236])(h);
}

typedef FRESULT (*FRFpvUpU_ptr_t)(FIL*, void*, UINT, UINT*);
inline static void op_console(cmd_ctx_t* ctx, FRFpvUpU_ptr_t fn, BYTE mode) {
    typedef void (*fn_ptr_t)(cmd_ctx_t* ctx, FRFpvUpU_ptr_t fn, BYTE mode);
    ((fn_ptr_t)_sys_table_ptrs[237])(ctx, fn, mode);
}

typedef struct color_schema {
    uint8_t BACKGROUND_FIELD_COLOR;
    uint8_t FOREGROUND_FIELD_COLOR;
    uint8_t HIGHLIGHTED_FIELD_COLOR;
    uint8_t BACKGROUND_F1_12_COLOR;
    uint8_t FOREGROUND_F1_12_COLOR;
    uint8_t BACKGROUND_F_BTN_COLOR;
    uint8_t FOREGROUND_F_BTN_COLOR;
    uint8_t BACKGROUND_CMD_COLOR;
    uint8_t FOREGROUND_CMD_COLOR;
    uint8_t BACKGROUND_SEL_BTN_COLOR;
    uint8_t FOREGROUND_SELECTED_COLOR;
    uint8_t BACKGROUND_SELECTED_COLOR;
} color_schema_t;

inline static 
void draw_label(color_schema_t* pcs, int left, int top, int width, char* txt, bool selected, bool highlighted) {
    typedef void (*fn_ptr_t)(color_schema_t* pcs, int left, int top, int width, char* txt, bool selected, bool highlighted);
    ((fn_ptr_t)_sys_table_ptrs[239])(pcs, left, top, width, txt, selected, highlighted);
}

typedef struct line {
   int8_t off;
   char* txt;
} line_t;

typedef struct lines {
   uint8_t sz;
   uint8_t toff;
   line_t* plns;
} lines_t;

inline static 
void draw_box(color_schema_t* pcs, int left, int top, int width, int height, const char* title, const lines_t* plines) {
    typedef void (*fn_ptr_t)(color_schema_t* pcs, int left, int top, int width, int height, const char* title, const lines_t* plines);
    ((fn_ptr_t)_sys_table_ptrs[240])(pcs, left, top, width, height, title, plines);
}

inline static 
void draw_panel(color_schema_t* pcs, int left, int top, int width, int height, char* title, char* bottom) {
    typedef void (*fn_ptr_t)(color_schema_t* pcs, int left, int top, int width, int height, char* title, char* bottom);
    ((fn_ptr_t)_sys_table_ptrs[241])(pcs, left, top, width, height, title, bottom);
}

inline static 
void draw_button(color_schema_t* pcs, int left, int top, int width, const char* txt, bool selected) {
    typedef void (*fn_ptr_t)(color_schema_t* pcs, int left, int top, int width, const char* txt, bool selected);
    ((fn_ptr_t)_sys_table_ptrs[242])(pcs, left, top, width, txt, selected);
}

inline static int kill(uint32_t task_n) {
    typedef int (*fn_ptr_t)(uint32_t);
    return ((fn_ptr_t)_sys_table_ptrs[244])(task_n);
}

// TODO: separate header
static unsigned ___srand___;
inline static void srand(unsigned x) {
    ___srand___ = x;
}
static int rand(void) {
	___srand___ = (31421 * ___srand___ + 6927) & 0xffff;
	return ___srand___ / 0x10000 + 1;
}

inline static int memcmp( const void *buffer1, const void *buffer2, size_t count ) {
    typedef int (*fn_ptr_t)(const void *buffer1, const void *buffer2, size_t count);
    return ((fn_ptr_t)_sys_table_ptrs[253])(buffer1, buffer2, count);
}

void* memmove(void* dst, const void* src, size_t sz);

inline static void reboot_me( void ) {
    typedef void (*fn_ptr_t)(void);
    ((fn_ptr_t)_sys_table_ptrs[254])();
}

// API v23
inline static size_t free_app_flash(void) {
    typedef size_t (*fn_ptr_t)(void);
    return ((fn_ptr_t)_sys_table_ptrs[255])();
}

// since API v.28 — MP3 decoding (helix)
typedef void* HMP3Decoder;
typedef struct {
    int bitrate;
    int nChans;
    int samprate;
    int bitsPerSample;
    int outputSamps;
    int layer;
    int version;
} MP3FrameInfo;

inline static HMP3Decoder MP3InitDecoder(void) { // 443
    typedef HMP3Decoder (*fn_ptr_t)(void);
    return ((fn_ptr_t)_sys_table_ptrs[443])();
}
inline static void MP3FreeDecoder(HMP3Decoder hMP3Decoder) { // 444
    typedef void (*fn_ptr_t)(HMP3Decoder);
    ((fn_ptr_t)_sys_table_ptrs[444])(hMP3Decoder);
}
inline static int MP3FindSyncWord(unsigned char *buf, int nBytes) { // 445
    typedef int (*fn_ptr_t)(unsigned char *, int);
    return ((fn_ptr_t)_sys_table_ptrs[445])(buf, nBytes);
}
inline static int MP3Decode(HMP3Decoder hMP3Decoder, unsigned char **inbuf,
                            int *bytesLeft, short *outbuf, int useSize) { // 446
    typedef int (*fn_ptr_t)(HMP3Decoder, unsigned char **, int *, short *, int);
    return ((fn_ptr_t)_sys_table_ptrs[446])(hMP3Decoder, inbuf, bytesLeft, outbuf, useSize);
}
inline static void MP3GetLastFrameInfo(HMP3Decoder hMP3Decoder, MP3FrameInfo *info) { // 447
    typedef void (*fn_ptr_t)(HMP3Decoder, MP3FrameInfo *);
    ((fn_ptr_t)_sys_table_ptrs[447])(hMP3Decoder, info);
}
inline static int MP3GetNextFrameInfo(HMP3Decoder hMP3Decoder, MP3FrameInfo *info,
                                      unsigned char *buf) { // 448
    typedef int (*fn_ptr_t)(HMP3Decoder, MP3FrameInfo *, unsigned char *);
    return ((fn_ptr_t)_sys_table_ptrs[448])(hMP3Decoder, info, buf);
}

/* Direct I2S audio — blocking writes from task context (no ISR). */
inline static void pcm_init(int sample_rate, int channels) { // 449
    typedef void (*fn_ptr_t)(int, int);
    ((fn_ptr_t)_sys_table_ptrs[449])(sample_rate, channels);
}
inline static void pcm_write(const int16_t *samples, int count) { // 450
    typedef void (*fn_ptr_t)(const int16_t *, int);
    ((fn_ptr_t)_sys_table_ptrs[450])(samples, count);
}

// since API v.30 — MOD playback (HxCModPlayer)
#include "hxcmod.h"

inline static int hxcmod_init(modcontext *modctx) { // 486
    typedef int (*fn_ptr_t)(modcontext *);
    return ((fn_ptr_t)_sys_table_ptrs[486])(modctx);
}
inline static int hxcmod_setcfg(modcontext *modctx, int samplerate,
                                int stereo_separation, int filter) { // 487
    typedef int (*fn_ptr_t)(modcontext *, int, int, int);
    return ((fn_ptr_t)_sys_table_ptrs[487])(modctx, samplerate,
                                            stereo_separation, filter);
}
inline static int hxcmod_load(modcontext *modctx, void *mod_data,
                              int mod_data_size) { // 488
    typedef int (*fn_ptr_t)(modcontext *, void *, int);
    return ((fn_ptr_t)_sys_table_ptrs[488])(modctx, mod_data, mod_data_size);
}
inline static void hxcmod_fillbuffer(modcontext *modctx, msample *outbuffer,
                                     mssize nbsample,
                                     tracker_buffer_state *trkbuf) { // 489
    typedef void (*fn_ptr_t)(modcontext *, msample *, mssize,
                             tracker_buffer_state *);
    ((fn_ptr_t)_sys_table_ptrs[489])(modctx, outbuffer, nbsample, trkbuf);
}
inline static void hxcmod_unload(modcontext *modctx) { // 490
    typedef void (*fn_ptr_t)(modcontext *);
    ((fn_ptr_t)_sys_table_ptrs[490])(modctx);
}

// since API v.30 — PSRAM allocator
inline static void *psram_alloc(size_t size) { // 491
    typedef void *(*fn_ptr_t)(size_t);
    return ((fn_ptr_t)_sys_table_ptrs[491])(size);
}
inline static void psram_free(void *ptr) { // 492
    typedef void (*fn_ptr_t)(void *);
    ((fn_ptr_t)_sys_table_ptrs[492])(ptr);
}
inline static bool psram_is_available(void) { // 493
    typedef bool (*fn_ptr_t)(void);
    return ((fn_ptr_t)_sys_table_ptrs[493])();
}

#define abs(x) (x > 0 ? x : -x)

extern volatile bool marked_to_exit;

#ifdef __cplusplus
}
#include "m-os-api-cpp-string.h"
#include "m-os-api-cpp-alloc.h"

#else

#include "m-os-api-c-array.h"
#include "m-os-api-c-string.h"
#include "m-os-api-c-list.h"

inline static void cmd_tab(cmd_ctx_t* ctx, string_t* s_cmd) {
    typedef void (*fn_ptr_t)(cmd_ctx_t* ctx, string_t* s_cmd);
    ((fn_ptr_t)_sys_table_ptrs[233])(ctx, s_cmd);
}

inline static int history_steps(cmd_ctx_t* ctx, int cmd_history_idx, string_t* s_cmd) {
    typedef int (*fn_ptr_t)(cmd_ctx_t* ctx, int cmd_history_idx, string_t* s_cmd);
    return ((fn_ptr_t)_sys_table_ptrs[234])(ctx, cmd_history_idx, s_cmd);
}

inline static bool cmd_enter_helper(cmd_ctx_t* ctx, string_t* s_cmd) {
    typedef bool (*fn_ptr_t)(cmd_ctx_t* ctx, string_t* s_cmd);
    return ((fn_ptr_t)_sys_table_ptrs[235])(ctx, s_cmd);
}

#endif

#endif

#endif // M_OS_API_H
