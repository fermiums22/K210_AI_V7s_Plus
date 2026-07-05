#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
P = ROOT / "lib" / "drivers" / "src" / "storage" / "sdcard.cpp"

RAW_SPI_BLOCK = r'''    volatile spi_t &raw_spi1()
    {
        return *reinterpret_cast<volatile spi_t *>(SPI1_BASE_ADDR);
    }

    void raw_spi1_setup(uint32_t hz)
    {
        volatile spi_t &spi = raw_spi1();
        uint32_t clk = sysctl_clock_get_freq(SYSCTL_CLOCK_SPI1);
        uint32_t div = (clk + hz - 1) / hz;
        if (div < 2)
            div = 2;
        if (div & 1)
            div++;
        if (div > 65534)
            div = 65534;

        spi.ssienr = 0;
        spi.ser = 0;
        spi.baudr = div;
        spi.imr = 0;
        spi.dmacr = 0;
        spi.dmatdlr = 0x10;
        spi.dmardlr = 0;
        spi.ctrlr0 = (7u << 16) | (0u << 8); /* 8-bit, mode0, standard, tx/rx */
        spi.spi_ctrlr0 = 0;
        spi.ctrlr1 = 0;
    }

    uint8_t raw_spi1_xfer(uint8_t out)
    {
        volatile spi_t &spi = raw_spi1();
        uint32_t guard;

        spi.ssienr = 1;
        spi.ser = 2; /* MaixPy SD_SS = SPI_CHIP_SELECT_1 */

        guard = 100000;
        while (!(spi.sr & 0x02) && --guard)
            ;
        spi.dr[0] = out;

        guard = 100000;
        while (!(spi.sr & 0x08) && --guard)
            ;
        uint8_t in = (uint8_t)spi.dr[0];

        guard = 100000;
        while (((spi.sr & 0x05) != 0x04) && --guard)
            ;

        spi.ser = 0;
        spi.ssienr = 0;
        return in;
    }

    void sd_write_data(const uint8_t *data_buff, size_t length)
    {
        raw_spi1_setup(SD_SPI_LOW_CLOCK_RATE);
        while (length--)
            raw_spi1_xfer(*data_buff++);
    }'''

READ_DATA = r'''    void sd_read_data(uint8_t *data_buff, size_t length)
    {
        raw_spi1_setup(SD_SPI_LOW_CLOCK_RATE);
        while (length--)
            *data_buff++ = raw_spi1_xfer(0xFF);
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
        if text[pos] == "{":
            depth += 1
        elif text[pos] == "}":
            depth -= 1
            if depth == 0:
                return start, pos + 1
    raise SystemExit(f"patch failed: function end not found: {signature}")


def replace_function(text: str, signature: str, replacement: str) -> str:
    start, end = find_function_span(text, signature)
    return text[:start] + replacement + text[end:]


s = P.read_text(encoding="utf-8")
s = replace_function(s, "    void sd_write_data(const uint8_t *data_buff, size_t length)", RAW_SPI_BLOCK)
s = replace_function(s, "    void sd_read_data(uint8_t *data_buff, size_t length)", READ_DATA)
s = replace_function(s, "    void sd_read_data_dma(uint8_t *data_buff)", READ_DATA_DMA)
P.write_text(s, encoding="utf-8", newline="\n")
print("patched:   lib/drivers/src/storage/sdcard.cpp")
print("SDCARD_RAW_SPI_PATCH_OK spi1_registers=1 byte_xfer=1 cs_mask=2")
