# CMakeLists.txt — Frank OS port of MMBasic (PicoMite)
#
# Build variant: PICOMITE + rp2350  (matches Frank OS / Frank M2 hardware)
#
# Substitutions vs. original PicoMite build:
#   Draw.c              → replaced by frankos_platform.c
#   ff.c/ffsystem.c/
#   ffunicode.c/
#   mmc_stm32.c         → replaced by frankos_ff.c (routes through OS sys_table)
#   USBKeyboard.c       → excluded (Frank OS handles HID)
#   network files       → excluded (no network on Frank OS)

# PICO_BOARD/PICO_PLATFORM must be set BEFORE pico_sdk_import.cmake
set(PICO_BOARD pico2 CACHE STRING "Board type")
# Disable --gc-sections: incompatible with -r (relocatable ELF for Frank OS)
set(PICO_NO_GC_SECTIONS 1 CACHE BOOL "Disable pico-sdk --gc-sections for Frank OS app build")

cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)

project(basic C CXX ASM)
pico_sdk_init()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(CMAKE_C_STANDARD 11)
set(COMPILED_DIR "${CMAKE_SOURCE_DIR}/../../compiled")

# ── MMBasic sources — core interpreter only, no direct hardware access ───────
# Excluded (hardware-specific): Custom.c Audio.c I2C.c SPI.c SPI-LCD.c Serial.c
#   Onewire.c GPS.c VS1053.c psram.c stepper.c Keyboard.c mouse.c CFunction.c
#   VGA222.c RGB121.c SSD1963.c GUI.c Touch.c Turtle.c Raycaster.c Draw.c
#   ff.c ffsystem.c ffunicode.c mmc_stm32.c USBKeyboard.c
# Their cmd_*/fun_* entry points are stubbed in frankos_stubs.c.
set(MMBASIC_SOURCES
    PicoMite.c
    MMBasic.c
    Operators.c
    Functions.c
    Commands.c
    FileIO.c
    Editor.c
    XModem.c
    MM_Misc.c
    Memory.c
    External.c
    MATHS.c
    re.c
    aes.c
    BmpDecoder.c
    picojpeg.c
    upng.c
    hxcmod.c
)

# ── Frank OS platform layer ──────────────────────────────────────────────────
set(FRANKOS_SOURCES
    frankos_main.c
    frankos_platform.c
    frankos_alloc.c
    frankos_libc.c
    frankos_stubs.c
    frankos_ff.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/font8x16.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../api/m-os-api-sdtfn.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../api/m-os-api-math.c
)

add_executable(basic
    ${MMBASIC_SOURCES}
    ${FRANKOS_SOURCES}
)

# PIO header required by Custom.c and Include.h
pico_generate_pio_header(basic ${CMAKE_CURRENT_LIST_DIR}/PicoMiteI2S.pio)

# ── Include directories ──────────────────────────────────────────────────────
# frankos/ MUST be first so ff.h / diskio.h / class/cdc/cdc_device.h
# override the originals that live in the basic source tree.
target_include_directories(basic BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/frankos
    ${CMAKE_CURRENT_SOURCE_DIR}/../../api
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../api/FreeRTOS
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../api
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# ── Compile definitions ──────────────────────────────────────────────────────
target_compile_definitions(basic PRIVATE
    _FRANKOS
    PICOMITE
    rp2350
    STRUCTENABLED
    GCODE
    GUICONTROLS
    NDEBUG
    PICO_FLASH_SIZE_BYTES=16777216
    PICO_HEAP_SIZE=0x800
    PICO_CORE0_STACK_SIZE=0x2000
    PICO_MALLOC_PANIC
    CFG_TUD_CDC_RX_BUFSIZE=256
    CFG_TUD_CDC_TX_BUFSIZE=256
    CFG_TUD_CDC_EP_BUFSIZE=256
    # Defined by pico/stdio_usb.h when USB stdio is enabled; we disable USB
    # stdio but MM_Misc.c still references this constant.
    PICO_STDIO_USB_RESET_BOOTSEL_INTERFACE_DISABLE_MASK=0
    # FreeRTOS type needed by m-os-api-tasks.h when compiled outside FreeRTOS
    configSTACK_DEPTH_TYPE=uint16_t
)

# ── Compile options ──────────────────────────────────────────────────────────
target_compile_options(basic PRIVATE
    -O2
    -fno-builtin-printf
    -fno-jump-tables
    -Werror=implicit-function-declaration
    "-include${CMAKE_CURRENT_SOURCE_DIR}/frankos/frankos_pico_overrides.h"
    -include _ansi.h
)

set_source_files_properties(MATHS.c      PROPERTIES COMPILE_FLAGS -Os)
set_source_files_properties(Editor.c     PROPERTIES COMPILE_FLAGS -Os)
set_source_files_properties(aes.c        PROPERTIES COMPILE_FLAGS -Os)
set_source_files_properties(re.c         PROPERTIES COMPILE_FLAGS -Os)
set_source_files_properties(hxcmod.c     PROPERTIES COMPILE_FLAGS -Os)
set_source_files_properties(GPS.c        PROPERTIES COMPILE_FLAGS -Os)
set_source_files_properties(I2C.c        PROPERTIES COMPILE_FLAGS -Os)
set_source_files_properties(picojpeg.c   PROPERTIES COMPILE_FLAGS -Os)
set_source_files_properties(Turtle.c     PROPERTIES COMPILE_FLAGS -Os)
set_source_files_properties(Raycaster.c  PROPERTIES COMPILE_FLAGS -Os)
set_source_files_properties(BmpDecoder.c PROPERTIES COMPILE_FLAGS -Os)
set_source_files_properties(GUI.c        PROPERTIES COMPILE_FLAGS -Os)
set_source_files_properties(SSD1963.c    PROPERTIES COMPILE_FLAGS -Os)
set_source_files_properties(Audio.c      PROPERTIES COMPILE_FLAGS -Os)
set_source_files_properties(Serial.c     PROPERTIES COMPILE_FLAGS -Os)
set_source_files_properties(Custom.c     PROPERTIES COMPILE_FLAGS -Os)

# ── Link options ─────────────────────────────────────────────────────────────
target_link_options(basic PRIVATE
    -Xlinker --print-memory-usage
    -r
    -Wl,--wrap,getConsole
    -Wl,--wrap,kbhitConsole
    -Wl,--wrap,SoftReset
    -Wl,--wrap,safe_flash_range_erase
    -Wl,--wrap,safe_flash_range_program
    -Wl,--wrap,sleep_ms
    -Wl,--wrap,sleep_us
    -Wl,--wrap,disable_interrupts_pico
    -Wl,--wrap,enable_interrupts_pico
    -Wl,--wrap,flash_do_cmd
    -Wl,--no-gc-sections                          # -r + gc-sections is invalid
)

# Override the pico-sdk memory-map linker script: Frank OS apps are relocatable
# ELFs; memory layout is deferred to the OS loader.
pico_set_linker_script(basic ${CMAKE_CURRENT_SOURCE_DIR}/frankos_app.ld)

# pico-sdk appends --gc-sections after our target_link_options, so we force
# --no-gc-sections to appear last using LINK_FLAGS (appended at the very end).
# --gc-sections is incompatible with -r (relocatable) output.
set_property(TARGET basic APPEND_STRING PROPERTY LINK_FLAGS " -Wl,--no-gc-sections")

pico_enable_stdio_uart(basic 0)
pico_enable_stdio_usb(basic 0)

# Link pico-sdk libraries so CMake adds their header search paths.
# The actual library code is irrelevant (we use -r / relocatable output and
# all hardware functions are stubbed in frankos_platform.c).
target_link_libraries(basic
    pico_stdlib
    hardware_flash
    hardware_irq
    hardware_adc
    hardware_pwm
    hardware_i2c
    hardware_spi
    hardware_dma
    hardware_exception
    hardware_pio
    hardware_sync
    hardware_uart
    hardware_clocks
    hardware_vreg
    hardware_watchdog
    hardware_timer
    hardware_gpio
    pico_multicore
    pico_rand
    pico_unique_id
    pico_bootrom
)

# ── Icon + .inf generation ───────────────────────────────────────────────────
set(ICON_ICO "${CMAKE_CURRENT_SOURCE_DIR}/../../../assets/icons/w2k_text_document.ico")
set(CONVERT_ICO_16 "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/convert_ico_16.py")

add_custom_command(
    OUTPUT  "${CMAKE_BINARY_DIR}/basic.inf"
    COMMAND ${Python3_EXECUTABLE} "${CONVERT_ICO_16}"
            --inf --ext "ext:bas"
            "MMBasic" "${ICON_ICO}" "${CMAKE_BINARY_DIR}/basic.inf"
    DEPENDS "${CONVERT_ICO_16}" "${ICON_ICO}"
    COMMENT "Generating basic.inf"
)
add_custom_target(basic_inf ALL DEPENDS "${CMAKE_BINARY_DIR}/basic.inf")
add_dependencies(basic basic_inf)

# ── Install outputs ───────────────────────────────────────────────────────────
# Strip debug sections BEFORE install: removes .debug_*, .rel.debug_* and
# excess local symbols, cutting the ELF from ~3.6 MB to ~0.8 MB and
# relocations from ~98 K to ~19 K — dramatically faster OS load time.
add_custom_command(TARGET basic POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} --strip-debug $<TARGET_FILE:basic>
    COMMAND ${CMAKE_COMMAND} -E make_directory ${COMPILED_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:basic>         ${COMPILED_DIR}/basic
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_BINARY_DIR}/basic.inf" ${COMPILED_DIR}/basic.inf
    COMMENT "Stripping debug info and installing basic to ${COMPILED_DIR}"
)
