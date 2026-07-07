    .section .app_header, "a", @progbits
    .align 2
    .globl boot_app_header
boot_app_header:
    .word 0x4B323130          /* APP_HDR_MAGIC: 'K210' */
    .word 0xB4CDCEDF          /* APP_HDR_MAGIC_INV */
    .word 0x80100000          /* load_addr */
    .word slot0_entry         /* entry_addr: tiny chainload probe before SDK _start */
    .word _app_image_size     /* image_size */
    .word 0x00000000          /* image_crc32: reserved for later */
    .word 0x00000000          /* flags */
    .word 0x00000000          /* reserved */

    .section .text.start, "ax", @progbits
    .align 2
    .globl slot0_entry
    .type slot0_entry, @function
slot0_entry:
    /* Minimal pre-SDK UART marker.  Boot already configured FPIOA/UARTHS pins.
     * This confirms that boot_jump_to_app() really reached app RAM before the
     * SDK startup code touches clocks, gp, stack, bss, or FreeRTOS state. */
.option push
.option norelax
    la t0, slot0_entry_msg
.option pop
    li t1, 0x38000000        /* UARTHS txdata */
1:
    lbu t2, 0(t0)
    beqz t2, 3f
2:
    lw t3, 0(t1)
    bltz t3, 2b             /* txdata.full is bit31 */
    sw t2, 0(t1)
    addi t0, t0, 1
    j 1b
3:
    j _start

    .section .rodata.slot0_entry, "a", @progbits
slot0_entry_msg:
    .asciz "APP_SLOT0_ENTRY\r\n"
