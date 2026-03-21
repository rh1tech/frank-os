cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2 CACHE STRING "Board type")

include(${CMAKE_CURRENT_LIST_DIR}/../../../pico_sdk_import.cmake)

project(murmnes C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

set(PROJECT_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../..)
set(SRCDIR ${PROJECT_ROOT}/src)
set(DRIVERSDIR ${SRCDIR}/drivers)
set(COREDIR ${SRCDIR}/core_quicknes)
set(EMU2413DIR ${SRCDIR}/emu2413)
set(PICODIR ${SRCDIR}/platform/pico)

# HSTX clocked from PLL_USB at 126MHz (reconfigured in main)
add_compile_definitions(MODE_HSTX_CLK_DIV=1)
add_subdirectory(${DRIVERSDIR}/pico_hdmi pico_hdmi)

# Peripheral drivers (always compiled)
set(PSRAM_MAX_FREQ_MHZ 133 CACHE STRING "PSRAM clock frequency in MHz")
add_compile_definitions(PSRAM_MAX_FREQ_MHZ=${PSRAM_MAX_FREQ_MHZ})

add_subdirectory(${SRCDIR}/lib/fatfs fatfs)
add_subdirectory(${DRIVERSDIR}/sdcard sdcard)
add_subdirectory(${DRIVERSDIR}/nespad nespad)
add_subdirectory(${DRIVERSDIR}/ps2kbd ps2kbd)

option(USB_HID_ENABLED "Enable USB HID host (disables USB serial)" OFF)
if(USB_HID_ENABLED)
    add_subdirectory(${DRIVERSDIR}/usbhid usbhid)
endif()

# QuickNES emulation core sources
set(QNES_SOURCES
    ${COREDIR}/nes_emu.cpp
    ${COREDIR}/nes_core.cpp
    ${COREDIR}/nes_cpu.cpp
    ${COREDIR}/nes_ppu.cpp
    ${COREDIR}/nes_ppu_impl.cpp
    ${COREDIR}/nes_ppu_rendering.cpp
    ${COREDIR}/nes_cart.cpp
    ${COREDIR}/nes_mapper.cpp
    ${COREDIR}/nes_apu.cpp
    ${COREDIR}/nes_oscs.cpp
    ${COREDIR}/nes_file.cpp
    ${COREDIR}/nes_state.cpp
    ${COREDIR}/nes_buffer.cpp
    ${COREDIR}/nes_effects_buffer.cpp
    ${COREDIR}/blip_buffer.cpp
    ${COREDIR}/multi_buffer.cpp
    ${COREDIR}/effects_buffer.cpp
    ${COREDIR}/data_reader.cpp
    ${COREDIR}/abstract_file.cpp
    ${COREDIR}/nes_data.cpp
    ${COREDIR}/nes_util.cpp
    ${COREDIR}/apu_state.cpp
    ${COREDIR}/nes_fme7_apu.cpp
    ${COREDIR}/nes_namco_apu.cpp
    ${COREDIR}/nes_vrc6_apu.cpp
    ${COREDIR}/nes_vrc7.cpp
    ${COREDIR}/quicknes.cpp
)

# emu2413 (YM2413/VRC7 emulator)
set(EMU2413_SOURCES
    ${EMU2413DIR}/emu2413.cpp
    ${EMU2413DIR}/emu2413_state.cpp
)

add_executable(murmnes
    ${PICODIR}/main_pico.c
    ${PICODIR}/settings.c
    ${PICODIR}/rom_selector.c
    ${PICODIR}/uart_logging.c
    ${DRIVERSDIR}/psram/psram_init.c
    ${DRIVERSDIR}/psram/psram_allocator.c
    ${DRIVERSDIR}/i2s/i2s_audio.c
    ${QNES_SOURCES}
    ${EMU2413_SOURCES}
)

# Generate PIO header for I2S audio
pico_generate_pio_header(murmnes ${DRIVERSDIR}/i2s/audio_i2s.pio)

target_include_directories(murmnes PRIVATE
    ${PICODIR}
    ${COREDIR}
    ${EMU2413DIR}
    ${DRIVERSDIR}
    ${DRIVERSDIR}/psram
    ${DRIVERSDIR}/i2s
    ${SRCDIR}/lib/fatfs
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_compile_definitions(murmnes PRIVATE
    PICO_CORE1_STACK_SIZE=0xC00
    PICO_STACK_SIZE=0x1000
    NDEBUG
    NO_UNALIGNED_ACCESS
    QNES_EXT_TILE_CACHE
    SDCARD_PIN_SPI0_SCK=6
    SDCARD_PIN_SPI0_MOSI=7
    SDCARD_PIN_SPI0_MISO=4
    SDCARD_PIN_SPI0_CS=5
)

# C++ flags: no exceptions, no RTTI (saves flash/RAM)
# -mno-unaligned-access: QuickNES casts byte* to uint32_t* for tile cache,
# which can produce misaligned pointers. LDRD faults on unaligned on Cortex-M33.
target_compile_options(murmnes PRIVATE
    -O2
    -mno-unaligned-access
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    -Wno-sign-compare
    -Wno-unused-parameter
)

target_link_libraries(murmnes
    pico_stdlib
    pico_multicore
    hardware_clocks
    hardware_dma
    hardware_irq
    hardware_gpio
    hardware_pio
    hardware_pll
    pico_hdmi
    sdcard
    nespad
    ps2kbd
)

if(USB_HID_ENABLED)
    target_link_libraries(murmnes usbhid)
endif()
pico_enable_stdio_usb(murmnes 0)
pico_enable_stdio_uart(murmnes 0)
target_link_libraries(murmnes hardware_uart)

# Optional: embed NES ROM into flash
if(DEFINED NES_ROM_PATH)
    set(ROM_COPY ${CMAKE_CURRENT_BINARY_DIR}/nes_rom.bin)
    set(ROM_OBJ ${CMAKE_CURRENT_BINARY_DIR}/nes_rom.o)

    add_custom_command(
        OUTPUT ${ROM_COPY}
        COMMAND ${CMAKE_COMMAND} -E copy ${NES_ROM_PATH} ${ROM_COPY}
        DEPENDS ${NES_ROM_PATH}
        COMMENT "Copying ROM to build dir"
    )

    add_custom_command(
        OUTPUT ${ROM_OBJ}
        COMMAND ${CMAKE_OBJCOPY}
            -I binary -O elf32-littlearm
            -B arm
            --rename-section .data=.rodata,alloc,load,readonly,data,contents
            nes_rom.bin nes_rom.o
        DEPENDS ${ROM_COPY}
        COMMENT "Embedding NES ROM into flash"
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    )

    target_sources(murmnes PRIVATE ${ROM_OBJ})
    target_compile_definitions(murmnes PRIVATE HAS_NES_ROM)

    target_link_options(murmnes PRIVATE
        -Wl,--defsym=nes_rom_data=_binary_nes_rom_bin_start
        -Wl,--defsym=nes_rom_end=_binary_nes_rom_bin_end
    )
endif()

# Read version from version.txt (format: "MAJOR MINOR", e.g. "1 00" -> "1.00")
file(READ "${PROJECT_ROOT}/version.txt" MURMNES_VERSION_RAW)
string(STRIP "${MURMNES_VERSION_RAW}" MURMNES_VERSION_RAW)
string(REPLACE " " "." MURMNES_VERSION_STR "${MURMNES_VERSION_RAW}")
target_compile_definitions(murmnes PRIVATE MURMNES_VERSION="${MURMNES_VERSION_STR}")

pico_add_extra_outputs(murmnes)
