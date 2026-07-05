#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SDCARD = ROOT / "lib" / "drivers" / "src" / "storage" / "sdcard.cpp"

READ_DATA = r'''    void sd_read_data(uint8_t *data_buff, size_t length)
    {
        uint8_t tx[64];
        memset(tx, 0xFF, sizeof(tx));
        while (length)
        {
            size_t chunk = length < sizeof(tx) ? length : sizeof(tx);
            spi8_dev_->transfer_full_duplex({ tx, std::ptrdiff_t(chunk) }, { data_buff, std::ptrdiff_t(chunk) });
            data_buff += chunk;
            length -= chunk;
        }
    }'''

READ_DATA_DMA = r'''    void sd_read_data_dma(uint8_t *data_buff)
    {
        sd_read_data(data_buff, 512);
    }'''


def find_function_span(text: str, signature: str) -> tuple[int, int]:
    start = text.find(signature)
    if start < 0:
        raise SystemExit(f"patch failed: function not found: {signature}")
    brace = text.find("{", start)
    if brace < 0:
        raise SystemExit(f"patch failed: function brace not found: {signature}")
    depth = 0
    for pos in range(brace, len(text)):
        c = text[pos]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return start, pos + 1
    raise SystemExit(f"patch failed: function end not found: {signature}")


def replace_function(text: str, signature: str, replacement: str) -> str:
    start, end = find_function_span(text, signature)
    return text[:start] + replacement + text[end:]


def main() -> int:
    old = SDCARD.read_text(encoding="utf-8")
    new = old
    new = replace_function(new, "    void sd_read_data(uint8_t *data_buff, size_t length)", READ_DATA)
    new = replace_function(new, "    void sd_read_data_dma(uint8_t *data_buff)", READ_DATA_DMA)
    if new == old:
        print("unchanged: lib/drivers/src/storage/sdcard.cpp")
    else:
        SDCARD.write_text(new, encoding="utf-8", newline="\n")
        print("patched:   lib/drivers/src/storage/sdcard.cpp")
    print("SDCARD_FD_READ_PATCH_OK dummy_tx=0xFF full_duplex=1")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
