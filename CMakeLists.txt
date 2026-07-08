cmake_minimum_required(VERSION 3.0)
cmake_policy(VERSION 3.0)

# ── Toolchain — the ONLY external dependency ──────────────────────────────────
# Everything else (lib/, third_party/, lds/, cmake/) lives in this repo.
set(TOOLCHAIN "C:/K210/toolchain/kendryte-toolchain/bin" CACHE PATH "RISC-V gcc bin dir")

# SDK_ROOT = project root; all cmake helpers reference it for lds/, lib/, etc.
set(SDK_ROOT ${CMAKE_SOURCE_DIR})

# Load cmake helpers (toolchain, compile flags, linker flags) — copied from SDK
include("${CMAKE_SOURCE_DIR}/cmake/common.cmake")

project(robot_show C CXX ASM)

# ── Global includes — must come BEFORE any add_subdirectory ───────────────────
# SDK libs reference <platform.h>, <encoding.h> etc. from lib/arch/include
include_directories(
    "${CMAKE_SOURCE_DIR}/lib/arch/include"
    "${CMAKE_SOURCE_DIR}/lib/utils/include"
    "${CMAKE_SOURCE_DIR}/lib"
    "${CMAKE_SOURCE_DIR}/third_party"
)

# ── Third-party ────────────────────────────────────────────────────────────────
add_subdirectory("${CMAKE_SOURCE_DIR}/third_party/fatfs" "${CMAKE_BINARY_DIR}/fatfs")
add_subdirectory("${CMAKE_SOURCE_DIR}/third_party/esp_serial_flasher" "${CMAKE_BINARY_DIR}/esp_serial_flasher")

# ── SDK libraries ──────────────────────────────────────────────────────────────
add_subdirectory("${CMAKE_SOURCE_DIR}/lib/hal"      "${CMAKE_BINARY_DIR}/hal")
add_subdirectory("${CMAKE_SOURCE_DIR}/lib/freertos" "${CMAKE_BINARY_DIR}/freertos")
add_subdirectory("${CMAKE_SOURCE_DIR}/lib/bsp"      "${CMAKE_BINARY_DIR}/bsp")
add_subdirectory("${CMAKE_SOURCE_DIR}/lib/drivers"  "${CMAKE_BINARY_DIR}/drivers")

# ── Our application sources ────────────────────────────────────────────────────
file(GLOB APP_SOURCES
    "${CMAKE_SOURCE_DIR}/src/*.c"
    "${CMAKE_SOURCE_DIR}/src/*.S"
)

# The boot-app header must be present only in the slot0/chainloaded build.
# A normal kflash image must still start with crt.S .text.start at 0x80000000.
list(REMOVE_ITEM APP_SOURCES "${CMAKE_SOURCE_DIR}/src/boot_app_header.S")
if (K210_APP_SLOT0)
    list(APPEND APP_SOURCES "${CMAKE_SOURCE_DIR}/src/boot_app_header.S")
    add_definitions(-DK210_APP_SLOT0=1)
endif ()

add_executable(robot_show ${APP_SOURCES})

target_include_directories(robot_show PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/lib/arch/include"
    "${CMAKE_SOURCE_DIR}/lib/bsp/include"
    "${CMAKE_SOURCE_DIR}/lib/hal/include"
    "${CMAKE_SOURCE_DIR}/lib/drivers/include"
    "${CMAKE_SOURCE_DIR}/lib/utils/include"
    "${CMAKE_SOURCE_DIR}/lib/freertos/include"
    "${CMAKE_SOURCE_DIR}/lib/freertos/conf"
    "${CMAKE_SOURCE_DIR}/lib/freertos/portable"
    "${CMAKE_SOURCE_DIR}/third_party/fatfs/source"
)

target_link_libraries(robot_show
    -Wl,--start-group
    m freertos atomic bsp c stdc++ drivers hal fatfs esp_serial_flasher
    -Wl,--end-group
)

# ── Binary for kflash ──────────────────────────────────────────────────────────
add_custom_command(TARGET robot_show POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary robot_show robot_show.bin
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "-> robot_show.bin ready for kflash"
)
