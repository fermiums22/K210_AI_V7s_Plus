#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Keep the build script immutable/repeatable: remove old SD patch hooks if they exist.
P = ROOT / "build_k210.bat"
text = P.read_text(encoding="utf-8")
blocks = [
    '''
if exist "tools\\patch_sdcard_raw_spi.py" (
  py -3 tools\\patch_sdcard_raw_spi.py
  if errorlevel 1 (
    echo ERROR: SD raw SPI patch failed
    exit /b 1
  )
)
''',
    '''
if exist "tools\\patch_sdcard_full_duplex_read.py" (
  py -3 tools\\patch_sdcard_full_duplex_read.py
  if errorlevel 1 (
    echo ERROR: SD command probe patch failed
    exit /b 1
  )
)
''',
    '''
if exist "tools\\patch_sdcard_cs_mask.py" (
  py -3 tools\\patch_sdcard_cs_mask.py
  if errorlevel 1 (
    echo ERROR: SD CS mask patch failed
    exit /b 1
  )
)
''',
]
changed = False
for block in blocks:
    if block in text:
        text = text.replace(block, "", 1)
        changed = True
if changed:
    P.write_text(text, encoding="utf-8", newline="\r\n")
    print("patched:   build_k210.bat")
else:
    print("unchanged: build_k210.bat")
print("BUILD_RAW_SPI_PATCH_DISABLED_OK")
print("BUILD_SDCARD_BUILD_PATCHES_DISABLED_OK")

# Fast ESP8285 flashing baseline.  ESP8266 ROM sync stays at 115200, then the
# uploaded stub is switched to 921600.  The upstream esp-serial-flasher blocks
# ESP8266 baud changes unconditionally; for our path this is too strict because
# we only change baud after esp_loader_connect_with_stub() succeeds.
esp = ROOT / "src" / "esp_flasher.c"
s = esp.read_text(encoding="utf-8")
s = s.replace("#define ESP_FLASH_BAUD       115200u\n#define ESP_FLASH_BLOCK      1024u",
              "#define ESP_ROM_BAUD         115200u\n#define ESP_FLASH_BAUD       921600u\n#define ESP_FLASH_BLOCK      4096u")
s = s.replace("    p->baud = ESP_FLASH_BAUD;\n    uart_config(p->uart, p->baud, 8, UART_STOP_1, UART_PARITY_NONE);",
              "    p->baud = ESP_ROM_BAUD;\n    uart_config(p->uart, p->baud, 8, UART_STOP_1, UART_PARITY_NONE);")
needle = "    target_chip_t chip = esp_loader_get_target(loader);\n    LOGF(\"[esp-flash] connected target=%s\", target_name(chip));\n    LOGF(\"[esp-flash] session baud=%lu block=%lu\", (unsigned long)ESP_FLASH_BAUD,\n         (unsigned long)sizeof(s_flash_buf));"
repl = "    target_chip_t chip = esp_loader_get_target(loader);\n    LOGF(\"[esp-flash] connected target=%s\", target_name(chip));\n\n    err = esp_loader_change_transmission_rate(loader, ESP_FLASH_BAUD);\n    if (err != ESP_LOADER_SUCCESS) {\n        LOGF(\"[esp-flash] change baud to %lu failed: %d\",\n             (unsigned long)ESP_FLASH_BAUD, err);\n        diag_printf(8, \"ESP baud fail %d\", err);\n        esp_reset_normal_boot();\n        esp_loader_deinit(loader);\n        return false;\n    }\n\n    LOGF(\"[esp-flash] session baud=%lu block=%lu\", (unsigned long)ESP_FLASH_BAUD,\n         (unsigned long)sizeof(s_flash_buf));"
s = s.replace(needle, repl)
esp.write_text(s, encoding="utf-8", newline="\n")
print("ESP_FLASH_FAST_PATCH_OK rom_baud=115200 flash_baud=921600 block=4096")

loader = ROOT / "third_party" / "esp_serial_flasher" / "src" / "esp_loader.c"
t = loader.read_text(encoding="utf-8")
t = t.replace("    if (loader->_target == ESP8266_CHIP || loader->_protocol_type == ESP_LOADER_PROTOCOL_SDIO) {\n        return ESP_LOADER_ERROR_UNSUPPORTED_FUNC;\n    }",
              "    if ((loader->_target == ESP8266_CHIP && !loader->_stub_running) ||\n        loader->_protocol_type == ESP_LOADER_PROTOCOL_SDIO) {\n        return ESP_LOADER_ERROR_UNSUPPORTED_FUNC;\n    }")
loader.write_text(t, encoding="utf-8", newline="\n")
print("ESP8266_STUB_BAUD_CHANGE_PATCH_OK")
