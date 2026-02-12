cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD frank_m2)
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)
set(PICO_PLATFORM rp2350-arm-s)

set(FREERTOS_KERNEL_PATH ${CMAKE_CURRENT_LIST_DIR}/lib/FreeRTOS-Kernel)

include(pico_sdk_import.cmake)
include(FreeRTOS_Kernel_import.cmake)

project(frankos C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)

pico_sdk_init()

# Tell FreeRTOS port where to find FreeRTOSConfig.h
set(FREERTOS_CONFIG_FILE_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/src CACHE STRING "")

# DispHSTX display driver (DVI via HSTX peripheral)
add_subdirectory(lib/DispHSTX DispHSTX)

# Unified PS/2 driver (keyboard + mouse)
add_subdirectory(drivers/ps2)

# FatFS filesystem library
add_subdirectory(drivers/fatfs)

# SD card SPI driver
include(drivers/sdcard/sdcard.cmake)

# PSRAM speed option (set to "" to disable PSRAM HW init; detection still runs)
set(PSRAM_SPEED "84" CACHE STRING "PSRAM max frequency in MHz (empty to disable HW init)")

# Collect MOS2 musl source files
file(GLOB MUSL_SRC "src/musl/*.c")

# Collect MOS2 posix.1 source files
file(GLOB POSIX_SRC "src/posix.1/*.c" "src/posix.1/sys/*.c")

# Main executable
add_executable(frankos
    # FRANK OS core
    src/main.c
    src/display.c
    src/keyboard.c
    src/font8x8.c
    src/font8x16.c
    src/gfx.c
    src/cursor.c
    src/window.c
    src/window_event.c
    src/window_draw.c
    src/sdcard_init.c
    src/font_ui.c
    src/font_ui_bold.c
    src/terminal.c
    src/shell.c
    src/menu.c
    src/taskbar.c
    src/startmenu.c
    src/sysmenu.c
    src/dialog.c
    src/dialog_icons.c
    src/default_icon.c
    src/filemanager.c
    src/fn_icons.c

    # MOS2 core (copied from murmulator-os2)
    src/app.c
    src/cmd.c
    src/sys_table.c
    src/math-wrapper.c
    src/hooks.c
    src/overclock.c
    src/ram_page.c
    src/sound.c
    src/freertos_ext.c
    src/mos2_stubs.c

    # MOS2 POSIX / musl / obsd
    ${MUSL_SRC}
    ${POSIX_SRC}
    src/obsd/fts.c

    # PSRAM allocator (always compiled — returns NULL when no HW present)
    drivers/psram/psram.c
)
target_include_directories(frankos PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/drivers/ps2
    ${CMAKE_CURRENT_LIST_DIR}/drivers/fatfs
    ${CMAKE_CURRENT_LIST_DIR}/drivers/psram
    ${CMAKE_CURRENT_LIST_DIR}/src/posix.1/include
    ${CMAKE_CURRENT_LIST_DIR}/src/musl/include
    ${CMAKE_CURRENT_LIST_DIR}/src/musl/internal
    ${CMAKE_CURRENT_LIST_DIR}/src/obsd/include
)

# PSRAM HW init (only when PSRAM_SPEED is set)
if(PSRAM_SPEED)
    target_sources(frankos PRIVATE drivers/psram/psram_init.c)
    target_compile_definitions(frankos PRIVATE PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED})
endif()

# Force-include our DispHSTX config so the library sees it before its own _config.h
target_compile_options(frankos PRIVATE -include ${CMAKE_CURRENT_LIST_DIR}/src/config.h)

# MOS2 code uses int↔ptr casts extensively; relax these warnings
target_compile_options(frankos PRIVATE
    -Wno-int-conversion
    -Wno-incompatible-pointer-types
    -Wno-discarded-qualifiers
    -Wno-pointer-sign
    -Wno-implicit-function-declaration
)

# Read version from version.txt ("major minor" format)
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/version.txt" _VERSION_LINE)
list(GET _VERSION_LINE 0 _VERSION_LINE)
string(REPLACE " " ";" _VERSION_PARTS "${_VERSION_LINE}")
list(GET _VERSION_PARTS 0 FRANK_VER_MAJOR)
list(GET _VERSION_PARTS 1 FRANK_VER_MINOR)
if(FRANK_VER_MINOR LESS 10)
    set(FRANK_VERSION_STR "${FRANK_VER_MAJOR}.0${FRANK_VER_MINOR}")
else()
    set(FRANK_VERSION_STR "${FRANK_VER_MAJOR}.${FRANK_VER_MINOR}")
endif()

# MOS2 compile definitions
target_compile_definitions(frankos PRIVATE
    FRANK_VERSION_STR="${FRANK_VERSION_STR}"
    FIRMWARE_OFFSET=256
    M_API_VERSION=27
    MIN_API_VERSION=14
    RESERVED_RAM=512
    OS_FLASH_SIZE=256
    OVERCLOCKING=378
    PWM_PIN0=20
    PWM_PIN1=21
    BEEPER_PIN=22
)

target_link_libraries(frankos
    DispHSTX
    ps2_driver
    sdcard
    pico_stdlib
    pico_multicore
    hardware_clocks
    hardware_gpio
    hardware_spi
    hardware_pwm
    hardware_flash
    hardware_watchdog
    FreeRTOS-Kernel-Heap4
)

# Place MOS2 system function table at flash address 0x103FF000.
# On RP2350 with 4MB flash, reads from 0x10FFF000 (the canonical MOS2 address)
# wrap to physical flash offset 0x3FF000, which is where this table lives.
target_link_options(frankos PRIVATE
    -Wl,--section-start=.sys_table=0x103FF000
    -Wl,--undefined=sys_table_ptrs
)

# USB output for printf, no UART
pico_enable_stdio_usb(frankos 1)
pico_enable_stdio_uart(frankos 0)

pico_add_extra_outputs(frankos)
