add_library(opl STATIC
    emu8950.c
    slot_render.cpp
    midifile.c
    midi_opl.c
)
target_compile_definitions(opl PRIVATE
    USE_EMU8950_OPL=1
    EMU8950_LINEAR=1
    EMU8950_ASM=0
    EMU8950_SLOT_RENDER=1
    EMU8950_NO_RATECONV=1
    EMU8950_NO_FLOAT=1
    EMU8950_NO_TIMER=1
    EMU8950_NO_TEST_FLAG=1
    EMU8950_NO_TLL=1
    EMU8950_NO_PERCUSSION_MODE=1
    EMU8950_NO_WAVE_TABLE_MAP=1
    EMU8950_LINEAR_SKIP=1
    EMU8950_LINEAR_END_OF_NOTE_OPTIMIZATION=1
    PICO_ON_DEVICE=1
)
target_include_directories(opl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(opl PRIVATE
    ${CMAKE_SOURCE_DIR}/drivers/fatfs
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/lib/FreeRTOS-Kernel/include
    ${CMAKE_SOURCE_DIR}/lib/FreeRTOS-Kernel/portable/ThirdParty/GCC/RP2350_ARM_NTZ/non_secure
)
target_link_libraries(opl PRIVATE hardware_interp hardware_gpio)
