cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2)
set(PICO_PLATFORM rp2350-arm-s)

set(FREERTOS_KERNEL_PATH ${CMAKE_CURRENT_LIST_DIR}/lib/FreeRTOS-Kernel)

include(pico_sdk_import.cmake)
include(FreeRTOS_Kernel_import.cmake)

project(rhea C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)

pico_sdk_init()

# Tell FreeRTOS port where to find FreeRTOSConfig.h
set(FREERTOS_CONFIG_FILE_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/src CACHE STRING "")

# DispHSTX display driver (DVI via HSTX peripheral)
add_subdirectory(lib/DispHSTX DispHSTX)

# Unified PS/2 driver (keyboard + mouse)
add_subdirectory(drivers/ps2)

# Main executable
add_executable(rhea
    src/main.c
    src/display.c
    src/keyboard.c
    src/font.c
    src/gfx.c
    src/cursor.c
    src/window.c
    src/window_event.c
    src/window_draw.c
)
target_include_directories(rhea PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/drivers/ps2
)

# Force-include our DispHSTX config so the library sees it before its own _config.h
target_compile_options(rhea PRIVATE -include ${CMAKE_CURRENT_LIST_DIR}/src/config.h)

target_link_libraries(rhea
    DispHSTX
    ps2_driver
    pico_stdlib
    pico_multicore
    hardware_clocks
    hardware_gpio
    FreeRTOS-Kernel-Heap4
)

# USB output for printf, no UART
pico_enable_stdio_usb(rhea 1)
pico_enable_stdio_uart(rhea 0)

pico_add_extra_outputs(rhea)
