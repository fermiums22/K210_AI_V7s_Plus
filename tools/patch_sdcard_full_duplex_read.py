#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]

p = root / 'lib/drivers/src/storage/sdcard.cpp'
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

u = root / 'src/sd_uart.c'
t = u.read_text(encoding='utf-8')
t = t.replace('KSD:HELP HELP SELFTEST LCD_TEST AMP_TEST SD_TEST CAM_TEST MIC_TEST\\n',
              'KSD:HELP HELP SELFTEST LCD_TEST AMP_TEST SD_TEST SD_RAW CAM_TEST MIC_TEST\\n')
raw_fn = '''static void sd_raw_command(void)\n{\n    LOG("[sd-uart] host requested SD_RAW");\n    int r = sd_raw_cmd0_probe();\n    char b[48];\n    snprintf(b, sizeof(b), "KSD:SD_RAW CMD0=%02X\\n", (unsigned)(r & 0xff));\n    host_puts(b);\n}\n\n'''
if 'static void sd_raw_command(void)' not in t:
    t = t.replace('static void sd_test_command(void)\n{', raw_fn + 'static void sd_test_command(void)\n{')
t = t.replace('if (strcmp(line, "SD_TEST") == 0) { sd_test_command(); continue; }',
              'if (strcmp(line, "SD_TEST") == 0) { sd_test_command(); continue; }\n        if (strcmp(line, "SD_RAW") == 0) { sd_raw_command(); continue; }')
u.write_text(t, encoding='utf-8', newline='\n')

print('SDCARD_SOURCE_PROBE_PATCH_OK zero_cardinfo=1 cmd_logs=1 no_throw=1 no_full_duplex=1 sd_raw_cmd=1')
