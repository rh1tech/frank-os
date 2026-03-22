# Developing Applications for FRANK OS

## Overview

FRANK OS applications are standalone ARM ELF binaries compiled against the `frankos-app.h` header. Apps call OS services through a sys_table — a fixed-address array of function pointers at `0x10FFF000`. This means apps are binary-compatible across firmware versions as long as the sys_table indices are stable.

## Quick Start

### Minimal App

```c
#include "frankos-app.h"

static hwnd_t my_hwnd;

static void on_paint(hwnd_t hwnd) {
    wd_begin(hwnd);
    wd_clear(COLOR_WHITE);
    wd_text_ui(10, 10, "Hello, FRANK OS!", COLOR_BLACK, COLOR_WHITE);
    wd_end();
}

static bool on_event(hwnd_t hwnd, const window_event_t *ev) {
    if (ev->type == WM_CLOSE) {
        wm_destroy_window(hwnd);
        return true;
    }
    return false;
}

int main(void) {
    my_hwnd = wm_create_window(100, 50, 300, 200,
                                "My App", WSTYLE_DEFAULT,
                                on_event, on_paint);
    wm_show_window(my_hwnd);
    wm_set_focus(my_hwnd);

    // Block until window is destroyed
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return 0;
}
```

### CMakeLists.txt Template

```cmake
cmake_minimum_required(VERSION 3.13)

set(PICO_PLATFORM rp2350-arm-s)
include(../../../pico_sdk_import.cmake)

project(myapp C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

set(COMPILED_DIR "${CMAKE_SOURCE_DIR}/../../../sdcard/fos")

find_package(Python3 COMPONENTS Interpreter REQUIRED)

# Read version from firmware
file(STRINGS "${CMAKE_SOURCE_DIR}/../../../version.txt" _VL)
list(GET _VL 0 _VL)
string(REPLACE " " ";" _VP "${_VL}")
list(GET _VP 0 _MAJ)
list(GET _VP 1 _MIN)
if(_MIN LESS 10)
    set(FRANK_VERSION_STR "${_MAJ}.0${_MIN}")
else()
    set(FRANK_VERSION_STR "${_MAJ}.${_MIN}")
endif()

add_executable(${PROJECT_NAME} main.c)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/../../api
)

# MOS2 app linker settings
target_link_options(${PROJECT_NAME} PRIVATE
    -Wl,-zmax-page-size=4
    -nostartfiles
    -nodefaultlibs
)

pico_set_binary_type(${PROJECT_NAME} no_flash)

target_link_libraries(${PROJECT_NAME}
    pico_stdlib
    hardware_gpio
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    configSTACK_DEPTH_TYPE=uint32_t
    FRANK_VERSION_STR="${FRANK_VERSION_STR}"
)

pico_enable_stdio_uart(${PROJECT_NAME} 0)
pico_enable_stdio_usb(${PROJECT_NAME} 0)

# Copy binary to sdcard/fos after build
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:${PROJECT_NAME}>
        ${COMPILED_DIR}/myapp
)
```

## API Reference

### Window Management

| Function | Description |
|----------|-------------|
| `wm_create_window(x, y, w, h, title, style, event_cb, paint_cb)` | Create a window. Returns `hwnd_t`. |
| `wm_destroy_window(hwnd)` | Destroy a window and free its slot. |
| `wm_show_window(hwnd)` | Make window visible. |
| `wm_set_focus(hwnd)` | Set input focus to window. |
| `wm_get_window(hwnd)` | Get pointer to `window_t` struct. |
| `wm_set_window_rect(hwnd, x, y, w, h)` | Move/resize window. |
| `wm_invalidate(hwnd)` | Mark window for repaint. |
| `wm_get_client_rect(hwnd)` | Get client area dimensions. |
| `wm_toggle_fullscreen(hwnd)` | Toggle fullscreen mode. |
| `wm_is_fullscreen(hwnd)` | Check if window is fullscreen. |
| `wm_find_window_by_title(title)` | Find window by title string. |
| `wm_mark_dirty()` | Trigger compositor without marking a window. |

### Window Styles

| Constant | Flags |
|----------|-------|
| `WSTYLE_DEFAULT` | Closable, resizable, movable, bordered |
| `WSTYLE_DIALOG` | Closable, movable, bordered (not resizable) |
| `WSTYLE_POPUP` | No decorations |

### Drawing Context

All drawing must occur inside a `wd_begin()`/`wd_end()` pair, typically in your `paint_handler`. Coordinates are relative to the client area origin.

| Function | Description |
|----------|-------------|
| `wd_begin(hwnd)` | Start drawing in window's client area. |
| `wd_end()` | End drawing context. |
| `wd_pixel(x, y, color)` | Set a single pixel. |
| `wd_hline(x, y, w, color)` | Horizontal line. |
| `wd_vline(x, y, h, color)` | Vertical line. |
| `wd_fill_rect(x, y, w, h, color)` | Filled rectangle. |
| `wd_clear(color)` | Clear entire client area. |
| `wd_rect(x, y, w, h, color)` | Outline rectangle. |
| `wd_bevel_rect(x, y, w, h, light, dark, face)` | 3D beveled rectangle. |
| `wd_char_ui(x, y, ch, fg, bg)` | Draw single character (6x12 UI font). |
| `wd_text_ui(x, y, str, fg, bg)` | Draw text string (6x12 UI font). |
| `wd_button(x, y, w, h, label, focused, pressed)` | Draw Win95-style button. |
| `wd_fb_ptr(cx, cy, &stride)` | Get direct framebuffer pointer. |
| `wd_get_clip_size(&w, &h)` | Get visible client area size. |

### Event Types

| Event | Description | Data |
|-------|-------------|------|
| `WM_CREATE` | Window created | — |
| `WM_DESTROY` | Window being destroyed | — |
| `WM_CLOSE` | Close button clicked | — |
| `WM_PAINT` | Repaint needed | — |
| `WM_SETFOCUS` | Window gained focus | — |
| `WM_KILLFOCUS` | Window lost focus | — |
| `WM_MOVE` | Window moved | `move.x`, `move.y` |
| `WM_SIZE` | Window resized | `size.w`, `size.h` |
| `WM_KEYDOWN` | Key pressed | `key.scancode`, `key.modifiers` |
| `WM_KEYUP` | Key released | `key.scancode`, `key.modifiers` |
| `WM_CHAR` | Character input | `charev.ch`, `charev.modifiers` |
| `WM_MOUSEMOVE` | Mouse moved | `mouse.x`, `mouse.y`, `mouse.buttons` |
| `WM_LBUTTONDOWN` | Left click | `mouse.x`, `mouse.y` |
| `WM_LBUTTONUP` | Left release | `mouse.x`, `mouse.y` |
| `WM_RBUTTONDOWN` | Right click | `mouse.x`, `mouse.y` |
| `WM_RBUTTONUP` | Right release | `mouse.x`, `mouse.y` |
| `WM_TIMER` | Timer fired | `timer.timer_id` |
| `WM_COMMAND` | Menu/dialog command | `command.id` |
| `WM_DROPFILES` | File opened via association | `dropfiles.file_path` |

### Colors

16-color CGA/EGA palette:

| Constant | Index | Color |
|----------|-------|-------|
| `COLOR_BLACK` | 0 | Black |
| `COLOR_BLUE` | 1 | Navy blue |
| `COLOR_GREEN` | 2 | Green |
| `COLOR_CYAN` | 3 | Teal |
| `COLOR_RED` | 4 | Maroon |
| `COLOR_MAGENTA` | 5 | Purple |
| `COLOR_BROWN` | 6 | Olive/brown |
| `COLOR_LIGHT_GRAY` | 7 | Silver |
| `COLOR_DARK_GRAY` | 8 | Gray |
| `COLOR_LIGHT_BLUE` | 9 | Blue |
| `COLOR_LIGHT_GREEN` | 10 | Lime |
| `COLOR_LIGHT_CYAN` | 11 | Aqua |
| `COLOR_LIGHT_RED` | 12 | Red |
| `COLOR_LIGHT_MAGENTA` | 13 | Fuchsia |
| `COLOR_YELLOW` | 14 | Yellow |
| `COLOR_WHITE` | 15 | White |

### Menus

```c
static menu_bar_t my_menu = {
    .menu_count = 2,
    .menus = {
        {
            .title = "File",
            .accel_key = 'F',       // Alt+F opens this menu
            .item_count = 3,
            .items = {
                { .text = "Open...",  .command_id = 100, .accel_key = 'O' },
                { .text = "",         .flags = MIF_SEPARATOR },
                { .text = "Exit",     .command_id = 101, .accel_key = 0 },
            }
        },
        {
            .title = "Help",
            .accel_key = 'H',
            .item_count = 1,
            .items = {
                { .text = "About",    .command_id = 200 },
            }
        }
    }
};

// In window creation:
menu_set(my_hwnd, &my_menu);  // Attach menu bar
// WF_MENUBAR is auto-set by menu_set()
```

Menu item selections arrive as `WM_COMMAND` events with the `command.id` you specified.

### Dialogs

```c
// Message box
hwnd_t dlg = dialog_show(parent_hwnd, "Title", "Message text",
                          DLG_ICON_INFO, DLG_BTN_OK | DLG_BTN_CANCEL);
// Result arrives as WM_COMMAND with DLG_RESULT_OK or DLG_RESULT_CANCEL

// File open dialog
hwnd_t fdlg = file_dialog_open(parent_hwnd, "Open File", "/fos", ".txt");
// Result: WM_COMMAND with DLG_RESULT_FILE, then call file_dialog_get_path()

// File save dialog
hwnd_t sdlg = file_dialog_save(parent_hwnd, "Save As", "/fos", ".txt", "untitled.txt");
// Result: WM_COMMAND with DLG_RESULT_FILE_SAVE
```

### Timers

```c
TimerHandle_t tmr = xTimerCreate("blink", pdMS_TO_TICKS(500),
                                  pdTRUE, NULL, timer_callback);
xTimerStart(tmr, 0);

// In your event handler:
if (ev->type == WM_TIMER) {
    // timer fired
}

// Cleanup:
xTimerStop(tmr, 0);
xTimerDelete(tmr, 0);
```

### Clipboard

```c
clipboard_set_text("Hello", 5);

const char *text = clipboard_get_text();
uint16_t len = clipboard_get_length();

clipboard_clear();
```

### Textarea Control

For multi-line text editing (used by Notepad):

```c
textarea_t ta;
char buf[32768];

textarea_init(&ta, buf, sizeof(buf), my_hwnd);
textarea_set_rect(&ta, 0, 0, client_w, client_h);
textarea_set_text(&ta, "Initial text", -1);

// In paint handler:
textarea_paint(&ta);

// In event handler:
textarea_event(&ta, event);
```

### Scrollbar Control

```c
scrollbar_t sb;
scrollbar_init(&sb, false);  // vertical
scrollbar_set_range(&sb, total_items, visible_items);
scrollbar_set_pos(&sb, current_pos);

// In paint handler:
scrollbar_paint(&sb);

// In event handler:
int32_t new_pos;
if (scrollbar_event(&sb, event, &new_pos)) {
    // scroll position changed
}
```

### File Associations

Apps can register file type associations through their `.inf` file:

```
MyApp
ext:txt,log,cfg
```

When a user opens a `.txt` file, FRANK OS finds MyApp and launches it with `WM_DROPFILES`.

### Application Flags

Export `__app_flags` to control app behavior:

```c
uint32_t __app_flags(void) {
    return APPFLAG_BACKGROUND | APPFLAG_SINGLETON;
}
```

| Flag | Effect |
|------|--------|
| `APPFLAG_BACKGROUND` | App keeps running when not focused |
| `APPFLAG_SINGLETON` | Only one instance allowed |

### Sound Mixer

```c
// Open a channel at desired sample rate
int ch = snd_open(22050);

// Write stereo 16-bit PCM (blocks until DMA has room)
int16_t samples[512];
snd_write(ch, samples, 256);  // 256 stereo sample pairs

// Done
snd_close(ch);
```

The mixer resamples per-channel to 44100 Hz and mixes all active channels. See the API reference for volume control (`snd_set_volume`/`snd_get_volume`).

### PSRAM Allocator

For large buffers (ROM images, sample data) that don't need SRAM speed:

```c
if (psram_is_available()) {
    uint8_t *rom = psram_alloc(262144);  // 256 KB
    // load ROM into rom...
    psram_free(rom);
}
```

### Fullscreen 8bpp Mode

Apps can request exclusive fullscreen with a 256-color palette (used by Dendy):

```c
display_request_mode(320, 240, 8);

// Set palette, render to display_draw_buffer_ptr, call display_swap_buffers()
// Poll keyboard directly with keyboard_poll() / keyboard_get_event()

// Restore desktop before exiting
display_request_mode(640, 480, 4);
wm_force_full_repaint();
```

In this mode the compositor and input tasks are paused. The app has exclusive access to the framebuffer and keyboard. See the API reference for the full list of display/keyboard functions.

## Icon Format

App icons are `.ico` files placed in each app's source directory (`apps/source/<appname>/<appname>.ico`). The build script copies them to `sdcard/fos/`. The OS parses ICO files at runtime via `ico_parse_16` and `ico_parse_32`.

Two sizes are used:
- **16x16** — taskbar buttons, file manager small icons
- **32x32** — file manager large icons, Alt+Tab switcher

## Deploying Your App

1. Place your source in `apps/source/myapp/`
2. Create `CMakeLists.txt` following the template above
3. Optionally add `apps/source/myapp/myapp.ico` for an icon
4. Add your app to `tools/regen_inf.py` APPS list
5. Run `cd apps && ./build_apps.sh`
6. Copy `sdcard/fos/` contents to your SD card
