set(PICO_BOARD pico2 CACHE STRING "Board type")

cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)

project(pshell C CXX ASM)
pico_sdk_init()

set(CMAKE_C_STANDARD 11)

set(COMPILED_DIR "${CMAKE_SOURCE_DIR}/../../compiled")

add_executable(${PROJECT_NAME}
    main.c
    pshell_vt100.c
    pshell_compat.c
    pshell_libc.c
    pshell_setjmp.S
    shell.c
    cc/cc.c
    cc/cc_malloc.c
    cc/cc_peep.c
    cc/cc_printf.S
    vi/vi.c
    misc/readln.c
    misc/tar.c
    disassembler/armdisasm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/font8x16.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../api/m-os-api-sdtfn.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../api/m-os-api-math.c
)

target_include_directories(${PROJECT_NAME} BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../api
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../api/FreeRTOS
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../api
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/cc
    ${CMAKE_CURRENT_SOURCE_DIR}/vi
    ${CMAKE_CURRENT_SOURCE_DIR}/misc
    ${CMAKE_CURRENT_SOURCE_DIR}/disassembler
)

target_link_options(${PROJECT_NAME} PRIVATE
    -Xlinker
    --print-memory-usage
    -r
)

if (CMAKE_C_COMPILER_ID STREQUAL "GNU")
target_compile_options(${PROJECT_NAME} PUBLIC
    -fno-builtin-printf
    -fno-builtin-putchar
    -fno-builtin-puts
    -fno-builtin-getchar
    -fno-jump-tables
    -Werror=implicit-function-declaration
    -include _ansi.h
)
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE
    configSTACK_DEPTH_TYPE=uint32_t
    PSHELL_FRANKOS=1
    PICO_RP2350=1
    PICO_RP2040=0
    PSHELL_GIT_TAG="frankos"
    PICO_MALLOC_PANIC=0
    NDEBUG=1
)

pico_enable_stdio_uart(${PROJECT_NAME} 0)
pico_enable_stdio_usb(${PROJECT_NAME} 0)

# ── Icon + .inf generation ───────────────────────────────────────────────────
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(ICON_ICO "${CMAKE_CURRENT_SOURCE_DIR}/../../../assets/icons/w2k_ms-dos_application.ico")
set(CONVERT_ICO_16 "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/convert_ico_16.py")

add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/pshell.inf"
    COMMAND ${Python3_EXECUTABLE} "${CONVERT_ICO_16}"
        --inf
        "PShell" "${ICON_ICO}"
        "${CMAKE_BINARY_DIR}/pshell.inf"
    DEPENDS "${CONVERT_ICO_16}" "${ICON_ICO}"
    COMMENT "Generating pshell.inf with embedded icon"
)
add_custom_target(pshell_inf ALL
    DEPENDS "${CMAKE_BINARY_DIR}/pshell.inf"
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} --strip-debug
        $<TARGET_FILE:${PROJECT_NAME}>
        ${COMPILED_DIR}/pshell
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_BINARY_DIR}/pshell.inf"
        ${COMPILED_DIR}/pshell.inf
)
