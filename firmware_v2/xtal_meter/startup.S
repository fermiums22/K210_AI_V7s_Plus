    .section .text.start, "ax", @progbits
    .globl _start
_start:
    j 1f
    .half 0
    .word 0xdeadbeef
1:
    csrw mie, zero
    csrw mip, zero
    csrr t0, mhartid
    bnez t0, 5f
.option push
.option norelax
    la gp, __global_pointer$
.option pop
    li sp, 0x805ff000
    la t0, __bss_start
    la t1, __bss_end
2:
    bgeu t0, t1, 3f
    sd zero, 0(t0)
    addi t0, t0, 8
    j 2b
3:
    call fpioa_init
    call main
4:
    wfi
    j 4b
5:
    wfi
    j 5b
    .globl _init
    .globl _fini
_init:
_fini:
    ret
