    .section .app_header, "a", @progbits
    .align 2
    .globl boot_app_header
boot_app_header:
    .word 0x4B323130          /* APP_HDR_MAGIC: 'K210' */
    .word 0xB4CDCEDF          /* APP_HDR_MAGIC_INV */
    .word 0x80100000          /* load_addr */
    .word _start              /* entry_addr */
    .word _app_image_size     /* image_size */
    .word 0x00000000          /* image_crc32: reserved for later */
    .word 0x00000000          /* flags */
    .word 0x00000000          /* reserved */
