set(PICO_BOARD pico2 CACHE STRING "Board type")

cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)

project(minesweeper C CXX ASM)
pico_sdk_init()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(CMAKE_C_STANDARD 11)

set(COMPILED_DIR "${CMAKE_SOURCE_DIR}/../../compiled")

add_executable(${PROJECT_NAME}
    main.c
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
    -fno-builtin-printf
    -fno-jump-tables
    -Werror=implicit-function-declaration
    -include _ansi.h
)
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
)

pico_enable_stdio_uart(${PROJECT_NAME} 0)
pico_enable_stdio_usb(${PROJECT_NAME} 0)

set(ICON_ICO "${CMAKE_CURRENT_SOURCE_DIR}/../../../assets/icons/w95_minesweeper.ico")
set(CONVERT_ICO_16 "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/convert_ico_16.py")

add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/minesweeper.inf"
    COMMAND ${Python3_EXECUTABLE} "${CONVERT_ICO_16}"
        --inf "Minesweeper" "${ICON_ICO}"
        "${CMAKE_BINARY_DIR}/minesweeper.inf"
    DEPENDS "${CONVERT_ICO_16}" "${ICON_ICO}"
    COMMENT "Generating minesweeper.inf with embedded icon"
)
add_custom_target(minesweeper_inf ALL
    DEPENDS "${CMAKE_BINARY_DIR}/minesweeper.inf"
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:${PROJECT_NAME}>
        ${COMPILED_DIR}/minesweeper
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_BINARY_DIR}/minesweeper.inf"
        ${COMPILED_DIR}/minesweeper.inf
)
