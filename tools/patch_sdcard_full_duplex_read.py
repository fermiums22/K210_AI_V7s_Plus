#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SDCARD = ROOT / "lib" / "drivers" / "src" / "storage" / "sdcard.cpp"

ON_FIRST_OPEN = r'''    virtual void on_first_open() override
    {
        auto spi = make_accessor(spi_driver_);
        spi8_dev_ = make_accessor(spi->get_device(SPI_MODE_0, SPI_FF_STANDARD, 1, 8));

        cs_gpio_ = make_accessor(cs_gpio_driver_);
        cs_gpio_->set_drive_mode(cs_gpio_pin_, GPIO_DM_OUTPUT);
        cs_gpio_->set_pin_value(cs_gpio_pin_, GPIO_PV_HIGH);

        memset(&card_info_, 0, sizeof(card_info_));
        spi8_dev_->set_clock_rate(SD_SPI_LOW_CLOCK_RATE);
        int init_rc = sd_init();
        init_ok_ = init_rc == 0;
        printf("[sdcard] init rc=%d capacity=%llu block=%lu\n",
               init_rc,
               (unsigned long long)card_info_.CardCapacity,
               (unsigned long)card_info_.CardBlockSize);
        if (!init_ok_)
            throw init_rc;
    }'''

READ_DATA = r'''    void sd_read_data(uint8_t *data_buff, size_t length)
    {
        spi8_dev_->read({ data_buff, std::ptrdiff_t(length) });
    }'''

READ_DATA_DMA = r'''    void sd_read_data_dma(uint8_t *data_buff)
    {
        spi8_dev_->read({ data_buff, 512L });
    }'''

GET_RESPONSE = r'''    uint8_t sd_get_response()
    {
        uint8_t result;
        uint16_t timeout = 0x1FFF;
        while (timeout--)
        {
            sd_read_data(&result, 1);
            if (result != 0xFF)
                return result;
        }
        return 0xFF;
    }'''

SD_INIT = r'''    uint8_t sd_init()
    {
        uint8_t frame[10], index, result;

        printf("[sdcard] PROBE begin\n");
        set_tf_cs_high();
        for (index = 0; index < 10; index++)
            frame[index] = 0xFF;
        sd_write_data(frame, 10);

        printf("[sdcard] CMD0 send\n");
        sd_send_cmd(SD_CMD0, 0, 0x95);
        result = sd_get_response();
        sd_end_cmd();
        printf("[sdcard] CMD0 r=%02x\n", result);
        if (result != 0x01)
            return 0xFF;

        printf("[sdcard] CMD8 send\n");
        sd_send_cmd(SD_CMD8, 0x01AA, 0x87);
        result = sd_get_response();
        sd_read_data(frame, 4);
        sd_end_cmd();
        printf("[sdcard] CMD8 r=%02x ocr=%02x %02x %02x %02x\n", result, frame[0], frame[1], frame[2], frame[3]);
        if (result != 0x01)
            return 0xFF;

        index = 0xFF;
        while (index--)
        {
            sd_send_cmd(SD_CMD55, 0, 1);
            result = sd_get_response();
            sd_end_cmd();
            if (result != 0x01)
            {
                printf("[sdcard] CMD55 fail r=%02x left=%u\n", result, (unsigned)index);
                return 0xFF;
            }
            sd_send_cmd(SD_ACMD41, 0x40000000, 1);
            result = sd_get_response();
            sd_end_cmd();
            if (result == 0x00)
                break;
        }
        printf("[sdcard] ACMD41 last r=%02x left=%u\n", result, (unsigned)index);
        if (index == 0)
            return 0xFF;

        index = 0xFF;
        while (index--)
        {
            sd_send_cmd(SD_CMD58, 0, 1);
            result = sd_get_response();
            sd_read_data(frame, 4);
            sd_end_cmd();
            if (result == 0)
                break;
        }
        printf("[sdcard] CMD58 r=%02x ocr=%02x %02x %02x %02x left=%u\n", result, frame[0], frame[1], frame[2], frame[3], (unsigned)index);
        if (index == 0)
            return 0xFF;

        if ((frame[0] & 0x40) == 0)
        {
            sd_send_cmd(SD_CMD16, 512, 1);
            result = sd_get_response();
            sd_end_cmd();
            printf("[sdcard] CMD16 r=%02x\n", result);
            if (result != 0x00)
                return 0xFF;
        }

        spi8_dev_->set_clock_rate(SD_SPI_HIGH_CLOCK_RATE);
        result = sd_get_cardinfo(&card_info_);
        printf("[sdcard] CARDINFO r=%02x capacity=%llu block=%lu\n",
               result,
               (unsigned long long)card_info_.CardCapacity,
               (unsigned long)card_info_.CardBlockSize);
        return result;
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
    new = replace_function(new, "    virtual void on_first_open() override", ON_FIRST_OPEN)
    new = replace_function(new, "    void sd_read_data(uint8_t *data_buff, size_t length)", READ_DATA)
    new = replace_function(new, "    void sd_read_data_dma(uint8_t *data_buff)", READ_DATA_DMA)
    new = replace_function(new, "    uint8_t sd_get_response()", GET_RESPONSE)
    new = replace_function(new, "    uint8_t sd_init()", SD_INIT)
    if new == old:
        print("unchanged: lib/drivers/src/storage/sdcard.cpp")
    else:
        SDCARD.write_text(new, encoding="utf-8", newline="\n")
        print("patched:   lib/drivers/src/storage/sdcard.cpp")
    print("SDCARD_CMD_PROBE_PATCH_OK sdk_read=1 timeout=0x1FFF failfast=1 cs_arg=1")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
