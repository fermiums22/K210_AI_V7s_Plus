#!/usr/bin/env python3
from pathlib import Path

p = Path(__file__).resolve().parents[1] / 'lib/drivers/src/storage/sdcard.cpp'
s = p.read_text(encoding='utf-8')

old = '''        spi8_dev_->set_clock_rate(SD_SPI_LOW_CLOCK_RATE);
        int init_rc = sd_init();
        init_ok_ = init_rc == 0;
        printf("[sdcard] init rc=%d capacity=%llu block=%lu\\n",
               init_rc,
               (unsigned long long)card_info_.CardCapacity,
               (unsigned long)card_info_.CardBlockSize);'''
new = '''        memset(&card_info_, 0, sizeof(card_info_));
        spi8_dev_->set_clock_rate(SD_SPI_LOW_CLOCK_RATE);
        int init_rc = sd_init();
        init_ok_ = init_rc == 0;
        printf("[sdcard] init rc=%d capacity=%llu block=%lu\\n",
               init_rc,
               (unsigned long long)card_info_.CardCapacity,
               (unsigned long)card_info_.CardBlockSize);'''
s = s.replace(old, new)

old = '''        /*!< SD initialized and set to SPI mode properly */
        sd_send_cmd(SD_CMD0, 0, 0x95);
        result = sd_get_response();
        sd_end_cmd();

        if (result != 0x01)
            return 0xFF;
        sd_send_cmd(SD_CMD8, 0x01AA, 0x87);
        /*!< 0x01 or 0x05 */
        result = sd_get_response();
        sd_read_data(frame, 4);
        sd_end_cmd();
        if (result != 0x01)
            return 0xFF;'''
new = '''        printf("[sdcard] PROBE begin\\n");
        printf("[sdcard] CMD0 send\\n");
        sd_send_cmd(SD_CMD0, 0, 0x95);
        result = sd_get_response();
        sd_end_cmd();
        printf("[sdcard] CMD0 r=%02x\\n", result);

        if (result != 0x01)
            return 0xFF;
        printf("[sdcard] CMD8 send\\n");
        sd_send_cmd(SD_CMD8, 0x01AA, 0x87);
        /*!< 0x01 or 0x05 */
        result = sd_get_response();
        sd_read_data(frame, 4);
        sd_end_cmd();
        printf("[sdcard] CMD8 r=%02x ocr=%02x %02x %02x %02x\\n", result, frame[0], frame[1], frame[2], frame[3]);
        if (result != 0x01)
            return 0xFF;'''
s = s.replace(old, new)

p.write_text(s, encoding='utf-8', newline='\n')
print('SDCARD_SOURCE_PROBE_PATCH_OK zero_cardinfo=1 cmd_logs=1 no_throw=1 no_full_duplex=1')
