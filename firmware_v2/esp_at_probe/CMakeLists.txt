cmake_minimum_required(VERSION 3.10)
cmake_policy(VERSION 3.10)
get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(TOOLCHAIN "C:/K210/toolchain/kendryte-toolchain/bin" CACHE PATH "RISC-V toolchain bin")
set(SDK_ROOT "${ROOT}")
include("${ROOT}/cmake/common.cmake")
project(k210_esp_at_probe C CXX ASM)
include_directories("${ROOT}/lib/arch/include" "${ROOT}/lib/utils/include" "${ROOT}/lib" "${ROOT}/third_party" "${ROOT}/third_party/fatfs/source")
add_subdirectory("${ROOT}/third_party/fatfs" "${CMAKE_BINARY_DIR}/fatfs")
add_subdirectory("${ROOT}/lib/hal" "${CMAKE_BINARY_DIR}/hal")
add_subdirectory("${ROOT}/lib/freertos" "${CMAKE_BINARY_DIR}/freertos")
target_compile_definitions(freertos PRIVATE K210_RECOVERY_SINGLE_CORE=1)
add_subdirectory("${ROOT}/lib/bsp" "${CMAKE_BINARY_DIR}/bsp")
add_subdirectory("${ROOT}/lib/drivers" "${CMAKE_BINARY_DIR}/drivers")
add_executable(k210_esp_at_probe main.c ../klink_bringup/newlib_locks.c)
target_include_directories(k210_esp_at_probe PRIVATE
  "${ROOT}/lib/arch/include" "${ROOT}/lib/bsp/include" "${ROOT}/lib/hal/include"
  "${ROOT}/lib/drivers/include" "${ROOT}/lib/utils/include" "${ROOT}/lib/freertos/include"
  "${ROOT}/lib/freertos/conf" "${ROOT}/lib/freertos/portable" "${ROOT}/third_party/fatfs/source")
target_link_libraries(k210_esp_at_probe -Wl,--start-group m freertos atomic bsp c stdc++ drivers hal fatfs -Wl,--end-group)
add_custom_command(TARGET k210_esp_at_probe POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -O binary k210_esp_at_probe k210_esp_at_probe.bin
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
