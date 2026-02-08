add_library(ps2kbd STATIC)

target_sources(ps2kbd PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/ps2kbd_mrmltr.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ps2kbd_mrmltr.h
    ${CMAKE_CURRENT_LIST_DIR}/ps2kbd_wrapper.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ps2kbd_wrapper.h
)

target_link_libraries(ps2kbd PRIVATE hardware_pio hardware_clocks)

# M2-only build
target_compile_definitions(ps2kbd PRIVATE BOARD_M2 KBD_CLOCK_PIN=2)

target_include_directories(ps2kbd PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
)

target_include_directories(ps2kbd PRIVATE
    ../../src
)

pico_generate_pio_header(ps2kbd
    ${CMAKE_CURRENT_LIST_DIR}/ps2kbd_mrmltr.pio
)

pico_generate_pio_header(ps2kbd
    ${CMAKE_CURRENT_LIST_DIR}/ps2kbd_mrmltr2.pio
)
