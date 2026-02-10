#ifdef __cplusplus
extern "C" {
#endif

#include "stdbool.h"
#include "stdio.h"
#include "stdint.h"
#include "ff.h"
#include "app.h"

#ifdef TFT
#define TFT_DRV 5
#endif
#ifdef HDMI
#include "hdmi.h"
#define HDMI_DRV 1
#endif
#ifdef VGA
#include "vga.h"
#define VGA_DRV 2
#endif
#ifdef TV
#include "tv.h"
#define RGB_DRV 3
#endif
#ifdef SOFTTV
#include "tv-software.h"
#define SOFTTV_DRV 4
#endif

int graphics_get_default_mode(void);
void graphics_init(int drv_type);
void graphics_set_buffer(uint8_t* buffer);
void graphics_set_offset(int x, int y);
void graphics_set_bgcolor(uint32_t color888);

void draw_text(const char* string, int x, int y, uint8_t color, uint8_t bgcolor);
void draw_window(const char* title, uint32_t x, uint32_t y, uint32_t width, uint32_t height);
void goutf(const char *__restrict str, ...) _ATTRIBUTE ((__format__ (__printf__, 1, 2)));
void fgoutf(FIL *f, const char *__restrict str, ...) _ATTRIBUTE ((__format__ (__printf__, 2, 3)));
void __putc(char);
void gouta(char* buf);
void gbackspace();
void graphics_set_con_pos(int x, int y);
void graphics_set_con_color(uint8_t color, uint8_t bgcolor);
uint32_t get_console_width();
uint32_t get_console_height();
uint8_t get_console_bitness();
uint32_t get_screen_width();
uint32_t get_screen_height();
uint8_t* get_buffer();
size_t get_buffer_size();
uint8_t get_screen_bitness();
void cleanup_graphics();
bool is_buffer_text();
int graphics_con_x(void);
int graphics_con_y(void);
void graphics_lock_buffer(bool);
void set_graphics_clkdiv(uint32_t pixel_clock, uint32_t line_size);

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
typedef void (*set_dma_handler_impl_fn)(dma_handler_impl_fn impl);

// VGA driver stubs (FRANK OS uses HSTX, not VGA — stubs for sys_table compat)
void set_vga_dma_handler_impl(dma_handler_impl_fn impl);
void set_vga_clkdiv(uint32_t pixel_clock, uint32_t line_size);
void vga_dma_channel_set_read_addr(const volatile void* addr);

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
void install_graphics_driver(graphics_driver_t*);
graphics_driver_t* get_graphics_driver();
void clrScr(uint8_t color);
bool graphics_set_mode(int mode);
int graphics_get_mode(void);
bool graphics_is_mode_text(int mode);
void set_cursor_color(uint8_t color);

uint8_t* graphics_get_font_table(void);
uint8_t graphics_get_font_width(void);
uint8_t graphics_get_font_height(void);
bool graphics_set_font(uint8_t, uint8_t);
bool graphics_set_ext_font(uint8_t*, uint8_t, uint8_t);

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

typedef struct line {
   int8_t off;
   char* txt;
} line_t;

typedef struct lines {
   uint8_t sz;
   uint8_t toff;
   line_t* plns;
} lines_t;


void draw_label(color_schema_t* pcs, int left, int top, int width, char* txt, bool selected, bool highlighted);
void draw_box(color_schema_t* pcs, int left, int top, int width, int height, const char* title, const lines_t* plines);
void draw_panel(color_schema_t* pcs, int left, int top, int width, int height, char* title, char* bottom);
void draw_button(color_schema_t* pcs, int left, int top, int width, const char* txt, bool selected);

#ifdef __cplusplus
}
#endif
