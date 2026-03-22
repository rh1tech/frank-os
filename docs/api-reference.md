# FRANK OS API Reference

API reference for `apps/api/frankos-app.h`. All functions are called through the MOS2 sys_table.

## sys_table Indices

| Index | Function | Category |
|-------|----------|----------|
| 17 | `xTaskGetTickCount` | FreeRTOS |
| 404 | `wm_create_window` | Window management |
| 405 | `wm_destroy_window` | Window management |
| 406 | `wm_show_window` | Window management |
| 407 | `wm_set_focus` | Window management |
| 408 | `wm_get_window` | Window management |
| 409 | `wm_set_window_rect` | Window management |
| 410 | `wm_invalidate` | Window management |
| 411 | `wm_post_event` | Window management |
| 412 | `wm_get_client_rect` | Window management |
| 413 | `wd_begin` | Drawing |
| 414 | `wd_end` | Drawing |
| 415 | `wd_pixel` | Drawing |
| 416 | `wd_hline` | Drawing |
| 417 | `wd_vline` | Drawing |
| 418 | `wd_fill_rect` | Drawing |
| 419 | `wd_clear` | Drawing |
| 420 | `wd_rect` | Drawing |
| 421 | `wd_bevel_rect` | Drawing |
| 422 | `wd_char_ui` | Drawing |
| 423 | `wd_text_ui` | Drawing |
| 424 | `menu_set` | Menus |
| 425 | `dialog_show` | Dialogs |
| 426 | `taskbar_invalidate` | Taskbar |
| 427 | `xTimerCreate` | Timers |
| 428 | `xTimerGenericCommandFromTask` | Timers |
| 429 | `pvTimerGetTimerID` | Timers |
| 430 | `xTaskGenericNotify` | Tasks |
| 431 | `ulTaskGenericNotifyTake` | Tasks |
| 432 | `wm_set_pending_icon` | Icons |
| 433 | `wd_icon_32` | Drawing |
| 434 | `wd_icon_16` | Drawing |
| 435 | `menu_popup_show` | Menus |
| 436 | `dialog_input_show` | Dialogs |
| 437 | `dialog_input_get_text` | Dialogs |
| 438 | `printf` | Serial debug (USB) |
| 439 | `file_dialog_open` | File dialogs |
| 440 | `file_dialog_get_path` | File dialogs |
| 441 | `wd_button` | Controls |
| 442 | `wd_fb_ptr` | Direct framebuffer |
| 443 | `MP3InitDecoder` | MP3 decoding |
| 444 | `MP3FreeDecoder` | MP3 decoding |
| 445 | `MP3FindSyncWord` | MP3 decoding |
| 446 | `MP3Decode` | MP3 decoding |
| 447 | `MP3GetLastFrameInfo` | MP3 decoding |
| 448 | `MP3GetNextFrameInfo` | MP3 decoding |
| 449 | `pcm_init` | PCM audio |
| 450 | `pcm_write` | PCM audio |
| 451 | `clipboard_set_text` | Clipboard |
| 452 | `clipboard_get_text` | Clipboard |
| 453 | `clipboard_get_length` | Clipboard |
| 454 | `clipboard_clear` | Clipboard |
| 455 | `scrollbar_init` | Scrollbar control |
| 456 | `scrollbar_set_range` | Scrollbar control |
| 457 | `scrollbar_set_pos` | Scrollbar control |
| 458 | `scrollbar_paint` | Scrollbar control |
| 459 | `scrollbar_event` | Scrollbar control |
| 460 | `textarea_init` | Textarea control |
| 461 | `textarea_set_text` | Textarea control |
| 462 | `textarea_get_text` | Textarea control |
| 463 | `textarea_get_length` | Textarea control |
| 464 | `textarea_set_rect` | Textarea control |
| 465 | `textarea_paint` | Textarea control |
| 466 | `textarea_event` | Textarea control |
| 467 | `textarea_cut` | Textarea control |
| 468 | `textarea_copy` | Textarea control |
| 469 | `textarea_paste` | Textarea control |
| 470 | `textarea_select_all` | Textarea control |
| 471 | `textarea_find` | Textarea control |
| 472 | `textarea_replace` | Textarea control |
| 473 | `textarea_replace_all` | Textarea control |
| 474 | `textarea_blink` | Textarea control |
| 475 | `file_dialog_save` | File dialogs |
| 476 | `find_dialog_show` | Find/Replace |
| 477 | `replace_dialog_show` | Find/Replace |
| 478 | `find_dialog_get_text` | Find/Replace |
| 479 | `find_dialog_get_replace_text` | Find/Replace |
| 480 | `find_dialog_case_sensitive` | Find/Replace |
| 481 | `find_dialog_close` | Find/Replace |
| 482 | `wm_mark_dirty` | Window management |
| 483 | `snd_open` | Sound mixer |
| 484 | `snd_write` | Sound mixer |
| 485 | `snd_close` | Sound mixer |
| 486 | `hxcmod_init` | MOD playback |
| 487 | `hxcmod_setcfg` | MOD playback |
| 488 | `hxcmod_load` | MOD playback |
| 489 | `hxcmod_fillbuffer` | MOD playback |
| 490 | `hxcmod_unload` | MOD playback |
| 491 | `psram_alloc` | PSRAM allocator |
| 492 | `psram_free` | PSRAM allocator |
| 493 | `psram_is_available` | PSRAM allocator |
| 494 | `file_assoc_scan` | File associations |
| 495 | `file_assoc_find` | File associations |
| 496 | `file_assoc_find_all` | File associations |
| 497 | `file_assoc_open` | File associations |
| 498 | `file_assoc_open_with` | File associations |
| 499 | `file_assoc_get_apps` | File associations |
| 500 | `desktop_add_shortcut` | Desktop |
| 501 | `wd_get_clip_size` | Drawing |
| 502 | `wm_toggle_fullscreen` | Window management |
| 503 | `wm_is_fullscreen` | Window management |
| 504 | `wm_find_window_by_title` | Window management |
| 505 | `app_launch_deferred` | App launcher |
| 506 | `midi_opl_init` | MIDI/OPL audio |
| 507 | `midi_opl_load` | MIDI/OPL audio |
| 508 | `midi_opl_render` | MIDI/OPL audio |
| 509 | `midi_opl_playing` | MIDI/OPL audio |
| 510 | `midi_opl_set_loop` | MIDI/OPL audio |
| 511 | `midi_opl_free` | MIDI/OPL audio |
| 512 | `wm_set_pending_icon32` | Icons |
| 513 | `ico_parse_16` | Icons |
| 514 | `ico_parse_32` | Icons |
| 515 | `display_set_video_mode` | Video mode |
| 516 | `display_get_video_mode` | Video mode |
| 517 | `display_set_palette_entry` | Video mode |
| 518 | `&display_width` | Video mode (variable) |
| 519 | `&display_height` | Video mode (variable) |
| 520 | `&display_fb_stride` | Video mode (variable) |
| 521 | `&display_bpp` | Video mode (variable) |
| 522 | `&display_draw_buffer_ptr` | Direct framebuffer |
| 523 | `display_set_pixel` | Direct framebuffer |
| 524 | `display_clear` | Direct framebuffer |
| 525 | `display_wait_vsync` | Direct framebuffer |
| 526 | `display_swap_buffers` | Direct framebuffer |
| 527 | `keyboard_poll` | Direct input |
| 528 | `keyboard_get_event` | Direct input |
| 529 | `pvPortMalloc` | SRAM allocator |
| 530 | `vPortFree` | SRAM allocator |
| 531 | `display_request_mode` | Video mode |
| 532 | `&display_compositor_idle` | Video mode (variable) |
| 533 | `wm_force_full_repaint` | Window management |
| 534 | `snd_set_volume` | Sound mixer |
| 535 | `snd_get_volume` | Sound mixer |

## Data Types

### `rect_t`
```c
typedef struct { int16_t x, y, w, h; } rect_t;
```

### `point_t`
```c
typedef struct { int16_t x, y; } point_t;
```

### `hwnd_t`
```c
typedef uint8_t hwnd_t;   // 0 = HWND_NULL (invalid), 1-16 = valid
```

### `window_event_t`
```c
typedef struct window_event {
    uint8_t type;      // WM_* constant
    uint8_t _pad;
    union {
        struct { uint8_t scancode; uint8_t modifiers; } key;
        struct { char ch; uint8_t modifiers; } charev;
        struct { int16_t x, y; uint8_t buttons, modifiers; } mouse;
        struct { int16_t x, y; } move;
        struct { int16_t w, h; } size;
        struct { uint16_t id; } command;
        struct { uint16_t timer_id; } timer;
        struct { const char *file_path; } dropfiles;
    };
} window_event_t;
```

### `window_t`
```c
struct window {
    uint16_t         flags;           // WF_* bitfield
    uint8_t          state;           // WS_NORMAL / MINIMIZED / MAXIMIZED
    rect_t           frame;           // outer frame in screen coordinates
    rect_t           restore_rect;    // saved rect before maximize
    uint8_t          bg_color;        // client area background color
    uint8_t          z_order;         // position in z-stack (0=bottom)
    char             title[24];       // null-terminated title string
    event_handler_t  event_handler;
    paint_handler_t  paint_handler;
    void            *user_data;       // opaque per-window data
};
```

### `scrollbar_t`
```c
typedef struct {
    int16_t  x, y, w, h;
    bool     horizontal;
    bool     visible;
    int32_t  range;
    int32_t  page;
    int32_t  pos;
    bool     dragging;
    int16_t  drag_offset;
} scrollbar_t;
```

### `textarea_t`
```c
typedef struct {
    char    *buf;
    int32_t  buf_size, len, cursor, sel_anchor;
    bool     cursor_visible;
    int16_t  rect_x, rect_y, rect_w, rect_h;
    int32_t  scroll_x, scroll_y;
    scrollbar_t  vscroll, hscroll;
    hwnd_t   hwnd;
    int32_t  total_lines, max_line_width;
} textarea_t;
```

### `menu_bar_t`
```c
typedef struct {
    uint8_t    menu_count;           // up to MENU_MAX_MENUS (5)
    menu_def_t menus[MENU_MAX_MENUS];
} menu_bar_t;

typedef struct {
    char        title[12];
    uint8_t     accel_key;
    uint8_t     item_count;          // up to MENU_MAX_ITEMS (8)
    menu_item_t items[MENU_MAX_ITEMS];
} menu_def_t;

typedef struct {
    char     text[20];
    uint16_t command_id;
    uint8_t  flags;                  // MIF_SEPARATOR, MIF_DISABLED
    uint8_t  accel_key;
} menu_item_t;
```

### `fa_app_t` (file association)
```c
typedef struct {
    char     name[20];
    char     path[32];
    uint8_t  icon[256];              // 16x16 icon data
    bool     has_icon;
    char     exts[8][8];             // registered extensions
    uint8_t  ext_count;
} fa_app_t;
```

## Constants

### Window Flags (`WF_*`)

| Flag | Bit | Description |
|------|-----|-------------|
| `WF_ALIVE` | 0 | Window slot is in use |
| `WF_VISIBLE` | 1 | Window is visible |
| `WF_FOCUSED` | 2 | Window has input focus |
| `WF_CLOSABLE` | 3 | Has close button |
| `WF_RESIZABLE` | 4 | Can be resized |
| `WF_MOVABLE` | 5 | Can be dragged |
| `WF_BORDER` | 6 | Has window border |
| `WF_DIRTY` | 7 | Needs repaint |
| `WF_MENUBAR` | 8 | Has menu bar |
| `WF_FRAME_DIRTY` | 9 | Decorations need repaint |
| `WF_FULLSCREENABLE` | 11 | Supports fullscreen toggle |

### Window States (`WS_*`)

| State | Value |
|-------|-------|
| `WS_NORMAL` | 0 |
| `WS_MINIMIZED` | 1 |
| `WS_MAXIMIZED` | 2 |

### Keyboard Modifiers (`KMOD_*`)

| Modifier | Bit |
|----------|-----|
| `KMOD_SHIFT` | 0 |
| `KMOD_CTRL` | 1 |
| `KMOD_ALT` | 2 |

### Dialog Icons

| Constant | Description |
|----------|-------------|
| `DLG_ICON_NONE` | No icon |
| `DLG_ICON_INFO` | Information (i) |
| `DLG_ICON_WARNING` | Warning (!) |
| `DLG_ICON_ERROR` | Error (X) |

### Dialog Buttons

| Constant | Description |
|----------|-------------|
| `DLG_BTN_OK` | OK button |
| `DLG_BTN_CANCEL` | Cancel button |
| `DLG_BTN_YES` | Yes button |
| `DLG_BTN_NO` | No button |

### Dialog Results (via `WM_COMMAND`)

| Constant | Value |
|----------|-------|
| `DLG_RESULT_OK` | `0xFF01` |
| `DLG_RESULT_CANCEL` | `0xFF02` |
| `DLG_RESULT_YES` | `0xFF03` |
| `DLG_RESULT_NO` | `0xFF04` |
| `DLG_RESULT_INPUT` | `0xFF10` |
| `DLG_RESULT_FILE` | `0xFF20` |
| `DLG_RESULT_FILE_SAVE` | `0xFF21` |
| `DLG_RESULT_FIND_NEXT` | `0xFF31` |
| `DLG_RESULT_REPLACE` | `0xFF32` |
| `DLG_RESULT_REPLACE_ALL` | `0xFF33` |

### Display Constants

| Constant | Value | Notes |
|----------|-------|-------|
| `DISPLAY_WIDTH` | 640 | Desktop mode (read `display_width` for current) |
| `DISPLAY_HEIGHT` | 480 | Desktop mode (read `display_height` for current) |
| `TASKBAR_HEIGHT` | 28 | |
| `FONT_UI_WIDTH` | 6 | |
| `FONT_UI_HEIGHT` | 12 | |
| `SCROLLBAR_WIDTH` | 16 | |

### Theme Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `THEME_TITLE_HEIGHT` | 20 | Title bar height in pixels |
| `THEME_MENU_HEIGHT` | 20 | Menu bar height |
| `THEME_BORDER_WIDTH` | 4 | Window border thickness |
| `THEME_BUTTON_FACE` | `COLOR_LIGHT_GRAY` | Button face color |

## Sound Mixer

The sound mixer supports up to 4 concurrent channels. Each app gets its own channel.

```c
// Open a mixer channel (sample_rate in Hz, e.g. 44100)
int ch = snd_open(44100);

// Write interleaved stereo 16-bit PCM samples
// Blocks until DMA has room
snd_write(ch, samples, num_samples);

// Close channel when done
snd_close(ch);

// Volume control (0-4)
snd_set_volume(3);
int vol = snd_get_volume();
```

Legacy `pcm_init`/`pcm_write` still work but route through the mixer internally.

## PSRAM Allocator

```c
if (psram_is_available()) {
    void *buf = psram_alloc(65536);  // 64 KB from PSRAM
    // use buf...
    psram_free(buf);
}
```

For SRAM allocation (faster, limited), use `pvPortMalloc`/`vPortFree` (indices 529-530).

## Video Mode Switching (Fullscreen 8bpp Apps)

Apps like Dendy use a dedicated fullscreen 320x240 8bpp mode, bypassing the window manager entirely.

```c
// Request 320x240 8bpp mode (compositor suspends, app gets exclusive access)
display_request_mode(320, 240, 8);

// Set 256-color palette
for (int i = 0; i < 64; i++)
    display_set_palette_entry(i, rgb565_color);

// Get direct framebuffer pointer
uint8_t *fb = (uint8_t *)display_draw_buffer_ptr;
int stride = display_fb_stride;

// Render frame directly into fb
// ...

// Swap front/back buffers (waits for vsync internally)
display_swap_buffers();

// Poll keyboard directly (no WM events in fullscreen mode)
keyboard_poll();
keyboard_event_t ev;
while (keyboard_get_event(&ev)) {
    // handle key press/release
}

// Restore desktop mode before exiting
display_request_mode(640, 480, 4);
wm_force_full_repaint();
```

Display state variables (read via sys_table pointers):
- `display_width`, `display_height` — current resolution
- `display_fb_stride` — bytes per framebuffer row
- `display_bpp` — bits per pixel (4 or 8)
- `display_compositor_idle` — true when compositor is bypassed
