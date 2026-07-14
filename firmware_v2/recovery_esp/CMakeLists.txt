cmake_minimum_required(VERSION 3.10)
cmake_policy(VERSION 3.10)

get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(TOOLCHAIN "C:/K210/toolchain/kendryte-toolchain/bin" CACHE PATH "RISC-V toolchain bin")
set(SDK_ROOT "${ROOT}")
include("${ROOT}/cmake/common.cmake")

project(k210_esp_recovery_v2 C CXX ASM)

option(RECOVERY_AT_TEST "Flash merged ESP8285 AT image and run STA diagnostics" OFF)
option(RECOVERY_FORCE_FLASH "Skip application marker probe and always flash ESP" OFF)
set(RECOVERY_BOOT_MARKER "BOOT kesp-slave-klink-v2"
    CACHE STRING "UART marker proving that the flashed ESP application booted")

set(ESP_V2_BUILD_DIR "${ROOT}/../K210_ESP_SPI_WIFI/esp8266_rtos_v2/bridge/build"
    CACHE PATH "Built ESP bridge v2 directory")
set(ESP_BOOT_BIN "${ESP_V2_BUILD_DIR}/bootloader/bootloader.bin"
    CACHE FILEPATH "ESP bootloader image")
set(ESP_PART_BIN "${ESP_V2_BUILD_DIR}/partitions_1mb_singleapp.bin"
    CACHE FILEPATH "ESP partition table image")
set(ESP_APP_BIN "${ESP_V2_BUILD_DIR}/esp8285-sta-klink.app1.bin"
    CACHE FILEPATH "ESP application image")
set(ESP_OTA_DATA_BIN "${ESP_V2_BUILD_DIR}/ota_data_initial.bin"
    CACHE FILEPATH "ESP OTA selection data")
set(ESP_PHY_INIT_BIN
    "${ROOT}/../esp8266_sdk/ESP8266_RTOS_SDK/components/esp8266/firmware/esp_init_data_default.bin"
    CACHE FILEPATH "ESP8266 SDK default PHY init data")
set(ESP_SYS_PARAM_BLANK_BIN
    "${ROOT}/../esp8266_sdk/ESP8266_RTOS_SDK/components/esp8266/firmware/blank.bin"
    CACHE FILEPATH "Blank ESP8266 SDK system parameter sector")

if(RECOVERY_AT_TEST)
    set(ESP_AT_FULL_BIN "${ROOT}/sdcard/esp_flash/esp8285_at.bin" CACHE FILEPATH "Merged 1 MiB AT firmware")
    if(NOT EXISTS "${ESP_AT_FULL_BIN}")
        message(FATAL_ERROR "Required ESP AT image is missing: ${ESP_AT_FULL_BIN}")
    endif()
    file(TO_CMAKE_PATH "${ESP_AT_FULL_BIN}" ESP_AT_FULL_BIN_ASM)
    configure_file("${CMAKE_CURRENT_LIST_DIR}/esp_at_image.S.in" "${CMAKE_BINARY_DIR}/esp_images.S" @ONLY)
    set_source_files_properties("${CMAKE_BINARY_DIR}/esp_images.S" PROPERTIES OBJECT_DEPENDS "${ESP_AT_FULL_BIN}")
else()
    foreach(image IN ITEMS ESP_BOOT_BIN ESP_PART_BIN ESP_APP_BIN ESP_OTA_DATA_BIN ESP_PHY_INIT_BIN ESP_SYS_PARAM_BLANK_BIN)
        if(NOT EXISTS "${${image}}")
            message(FATAL_ERROR "Required ESP v2 image is missing: ${${image}}")
        endif()
        file(TO_CMAKE_PATH "${${image}}" "${image}_ASM")
    endforeach()
    configure_file("${CMAKE_CURRENT_LIST_DIR}/esp_images.S.in" "${CMAKE_BINARY_DIR}/esp_images.S" @ONLY)
    set_source_files_properties("${CMAKE_BINARY_DIR}/esp_images.S" PROPERTIES
        OBJECT_DEPENDS "${ESP_BOOT_BIN};${ESP_PART_BIN};${ESP_APP_BIN};${ESP_OTA_DATA_BIN};${ESP_PHY_INIT_BIN};${ESP_SYS_PARAM_BLANK_BIN}")
endif()

include_directories(
    "${ROOT}/lib/arch/include"
    "${ROOT}/lib/utils/include"
    "${ROOT}/lib"
    "${ROOT}/third_party"
    "${ROOT}/third_party/fatfs/source"
)

add_subdirectory("${ROOT}/third_party/fatfs" "${CMAKE_BINARY_DIR}/fatfs")
add_subdirectory("${ROOT}/lib/hal" "${CMAKE_BINARY_DIR}/hal")
add_subdirectory("${ROOT}/lib/freertos" "${CMAKE_BINARY_DIR}/freertos")
target_compile_definitions(freertos PRIVATE K210_RECOVERY_SINGLE_CORE=1)
add_subdirectory("${ROOT}/lib/bsp" "${CMAKE_BINARY_DIR}/bsp")
add_subdirectory("${ROOT}/lib/drivers" "${CMAKE_BINARY_DIR}/drivers")
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/vendor/esp_serial_flasher"
                 "${CMAKE_BINARY_DIR}/esp_serial_flasher")

add_executable(k210_esp_recovery_v2
    "${CMAKE_CURRENT_LIST_DIR}/main.c"
    "${CMAKE_CURRENT_LIST_DIR}/newlib_locks.c"
    "${CMAKE_BINARY_DIR}/esp_images.S"
)
target_compile_definitions(k210_esp_recovery_v2 PRIVATE
    ESP_BOOT_MARKER="${RECOVERY_BOOT_MARKER}")
if(RECOVERY_AT_TEST)
    target_compile_definitions(k210_esp_recovery_v2 PRIVATE RECOVERY_AT_TEST=1)
endif()
if(RECOVERY_FORCE_FLASH)
    target_compile_definitions(k210_esp_recovery_v2 PRIVATE RECOVERY_FORCE_FLASH=1)
endif()

target_include_directories(k210_esp_recovery_v2 PRIVATE
    "${ROOT}/lib/arch/include"
    "${ROOT}/lib/bsp/include"
    "${ROOT}/lib/hal/include"
    "${ROOT}/lib/drivers/include"
    "${ROOT}/lib/utils/include"
    "${ROOT}/lib/freertos/include"
    "${ROOT}/lib/freertos/conf"
    "${ROOT}/lib/freertos/portable"
    "${ROOT}/third_party/fatfs/source"
)

target_link_libraries(k210_esp_recovery_v2
    -Wl,--start-group
    m freertos atomic bsp c stdc++ drivers hal fatfs esp_serial_flasher
    -Wl,--end-group
)

add_custom_command(TARGET k210_esp_recovery_v2 POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary k210_esp_recovery_v2 k210_esp_recovery_v2.bin
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "-> k210_esp_recovery_v2.bin ready for kflash"
)
