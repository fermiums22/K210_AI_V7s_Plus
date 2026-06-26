cmake_minimum_required(VERSION 3.0)
project(hello C ASM)

set(SDK_ROOT "C:/K210/sdk/kendryte-freertos-sdk-0.7.0")
include("${SDK_ROOT}/cmake/common.cmake")

add_executable(hello main.c)
target_link_libraries(hello kendryte_sdk)
include_directories(hello "${SDK_ROOT}/lib/drivers/include"
                         "${SDK_ROOT}/lib/bsp/include"
                         "${SDK_ROOT}/third_party/fatfs/source")

install(TARGETS hello DESTINATION bin)
