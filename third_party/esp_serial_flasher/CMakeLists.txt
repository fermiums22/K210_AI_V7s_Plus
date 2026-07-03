project(esp_serial_flasher C)

set(ESP_SERIAL_FLASHER_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set(ESP_SERIAL_FLASHER_SRCS
    ${ESP_SERIAL_FLASHER_DIR}/src/md5_hash.c
    ${ESP_SERIAL_FLASHER_DIR}/src/esp_loader.c
    ${ESP_SERIAL_FLASHER_DIR}/src/esp_targets.c
    ${ESP_SERIAL_FLASHER_DIR}/src/stubs/esp_stubs_table.c
    ${ESP_SERIAL_FLASHER_DIR}/src/stubs/esp_stub_esp8266.c
    ${ESP_SERIAL_FLASHER_DIR}/src/stubs/esp_stub_esp32.c
    ${ESP_SERIAL_FLASHER_DIR}/src/stubs/esp_stub_esp32s2.c
    ${ESP_SERIAL_FLASHER_DIR}/src/stubs/esp_stub_esp32c3.c
    ${ESP_SERIAL_FLASHER_DIR}/src/stubs/esp_stub_esp32s3.c
    ${ESP_SERIAL_FLASHER_DIR}/src/stubs/esp_stub_esp32c2.c
    ${ESP_SERIAL_FLASHER_DIR}/src/stubs/esp_stub_esp32c5.c
    ${ESP_SERIAL_FLASHER_DIR}/src/stubs/esp_stub_esp32h2.c
    ${ESP_SERIAL_FLASHER_DIR}/src/stubs/esp_stub_esp32c6.c
    ${ESP_SERIAL_FLASHER_DIR}/src/stubs/esp_stub_esp32p4.c
    ${ESP_SERIAL_FLASHER_DIR}/src/stubs/esp_stub_esp32p4rev1.c
    ${ESP_SERIAL_FLASHER_DIR}/src/stubs/esp_stub_esp32c61.c
    ${ESP_SERIAL_FLASHER_DIR}/src/protocol_serial.c
    ${ESP_SERIAL_FLASHER_DIR}/src/protocol_uart.c
    ${ESP_SERIAL_FLASHER_DIR}/src/protocol_spi.c
    ${ESP_SERIAL_FLASHER_DIR}/src/protocol_sdio.c
    ${ESP_SERIAL_FLASHER_DIR}/src/slip.c
)

add_library(esp_serial_flasher STATIC ${ESP_SERIAL_FLASHER_SRCS})

target_include_directories(esp_serial_flasher PUBLIC
    ${ESP_SERIAL_FLASHER_DIR}/include
)

target_include_directories(esp_serial_flasher PRIVATE
    ${ESP_SERIAL_FLASHER_DIR}/private_include
)

target_compile_definitions(esp_serial_flasher PUBLIC
    SERIAL_FLASHER_LOG_LEVEL=3
    SERIAL_FLASHER_RESET_HOLD_TIME_MS=100
    SERIAL_FLASHER_BOOT_HOLD_TIME_MS=50
    SERIAL_FLASHER_WRITE_BLOCK_RETRIES=3
    SERIAL_FLASHER_RESET_INVERT=false
    SERIAL_FLASHER_BOOT_INVERT=false
)
