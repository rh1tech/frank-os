set(PICO_BOARD pico2 CACHE STRING "Board type")

cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)

project(zxspectrum C CXX ASM)
pico_sdk_init()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(CMAKE_C_STANDARD 11)

set(COMPILED_DIR "${CMAKE_SOURCE_DIR}/../../compiled")

add_executable(${PROJECT_NAME}
    main.c
    Z80.c
    z80_arm.S
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../api/m-os-api-sdtfn.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../api/m-os-api-math.c
)

target_include_directories(${PROJECT_NAME} BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../api
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../api/FreeRTOS
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../api
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_options(${PROJECT_NAME} PRIVATE
    -Xlinker
    --print-memory-usage
    -r
)

if (CMAKE_C_COMPILER_ID STREQUAL "GNU")
target_compile_options(${PROJECT_NAME} PUBLIC
    -O2
    -ffixed-r9
    -fno-builtin-printf
    -Werror=implicit-function-declaration
    -include _ansi.h
)
# Z80 C interpreter is huge (~56KB at -O2). Compile for size — the hot path
# runs through the ARM ASM dispatcher, so -Os here only affects rare opcodes.
set_source_files_properties(Z80.c PROPERTIES COMPILE_FLAGS "-Os")
endif()

# Read version from version.txt ("major minor" format)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../../../version.txt" _VERSION_LINE)
list(GET _VERSION_LINE 0 _VERSION_LINE)
string(REPLACE " " ";" _VERSION_PARTS "${_VERSION_LINE}")
list(GET _VERSION_PARTS 0 FRANK_VER_MAJOR)
list(GET _VERSION_PARTS 1 FRANK_VER_MINOR)
if(FRANK_VER_MINOR LESS 10)
    set(FRANK_VERSION_STR "${FRANK_VER_MAJOR}.0${FRANK_VER_MINOR}")
else()
    set(FRANK_VERSION_STR "${FRANK_VER_MAJOR}.${FRANK_VER_MINOR}")
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE
    configSTACK_DEPTH_TYPE=uint32_t
    FRANK_VERSION_STR="${FRANK_VERSION_STR}"
    ZXSPECTRUM=1
    EXECZ80=1
    LSB_FIRST=1
)

pico_enable_stdio_uart(${PROJECT_NAME} 0)
pico_enable_stdio_usb(${PROJECT_NAME} 0)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:${PROJECT_NAME}>
        ${COMPILED_DIR}/zxspectrum
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_CURRENT_SOURCE_DIR}/zxspectrum.inf"
        ${COMPILED_DIR}/zxspectrum.inf
)
