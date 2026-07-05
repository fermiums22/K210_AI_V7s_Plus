# K210 SD/SPI register map and bring-up notes

This document is mandatory reading before touching SD, SPI1, FPIOA, GPIOHS, LCD/SD bus sharing, or KSD `SD_TEST` code.

The purpose is to stop blind patching. The current failure is not a FatFs failure and not an ESP/Wi-Fi failure. The current reproducible symptom is:

```text
[sd-uart] host requested SD_TEST
[sdcard] PROBE begin
[sdcard] CMD0 send
[sdcard] CMD0 r=ff
[sdcard] init rc=255 capacity=0 block=0
[sd] card init failed (inserted? wiring?)
KSD:SD_FAIL rw
```

`CMD0 r=ff` means the card did not answer the first SD SPI-mode command. Anything after CMD0, including CMD8, ACMD41, CMD58, CSD/CID, FatFs mount, file IO, ESP flashing, and Wi-Fi/SPI tests, is downstream and must not be debugged first.

## Source-of-truth references

Use these as reference sources before changing low-level code:

```text
Official K210 standalone SDK:
https://github.com/kendryte/kendryte-standalone-sdk

K210 address map:
kendryte-standalone-sdk/lib/bsp/include/platform.h
commit/reference observed: 02576ba67e8797444f3ee3f34c625b5ed048e707

K210 standalone SPI driver:
kendryte-standalone-sdk/lib/drivers/spi.c
kendryte-standalone-sdk/lib/drivers/include/spi.h
commit/reference observed: 02576ba67e8797444f3ee3f34c625b5ed048e707

K210 datasheet docs:
https://github.com/kendryte/kendryte-doc-datasheet

MaixPy SD driver:
https://github.com/sipeed/MaixPy-v1
components/drivers/sd_card/src/sdcard.c
components/drivers/sd_card/include/sdcard.h
```

## K210 peripheral address map relevant to SD/SPI

From Kendryte standalone SDK `lib/bsp/include/platform.h`:

```c
#define GPIOHS_BASE_ADDR    (0x38001000U)
#define GPIO_BASE_ADDR      (0x50200000U)
#define UART1_BASE_ADDR     (0x50210000U)
#define UART2_BASE_ADDR     (0x50220000U)
#define UART3_BASE_ADDR     (0x50230000U)
#define SPI_SLAVE_BASE_ADDR (0x50240000U)
#define FPIOA_BASE_ADDR     (0x502B0000U)
#define SYSCTL_BASE_ADDR    (0x50440000U)

#define SPI0_BASE_ADDR      (0x52000000U)
#define SPI1_BASE_ADDR      (0x53000000U)
#define SPI3_BASE_ADDR      (0x54000000U)
```

Important: SD is supposed to use SPI1, therefore the SPI register base is `0x53000000`, not an inferred or guessed address.

## K210 SPI register layout

From Kendryte standalone SDK `lib/drivers/include/spi.h`:

```c
typedef struct _spi
{
    volatile uint32_t ctrlr0;           /* 0x00 */
    volatile uint32_t ctrlr1;           /* 0x04 */
    volatile uint32_t ssienr;           /* 0x08 */
    volatile uint32_t mwcr;             /* 0x0c */
    volatile uint32_t ser;              /* 0x10 */
    volatile uint32_t baudr;            /* 0x14 */
    volatile uint32_t txftlr;           /* 0x18 */
    volatile uint32_t rxftlr;           /* 0x1c */
    volatile uint32_t txflr;            /* 0x20 */
    volatile uint32_t rxflr;            /* 0x24 */
    volatile uint32_t sr;               /* 0x28 */
    volatile uint32_t imr;              /* 0x2c */
    volatile uint32_t isr;              /* 0x30 */
    volatile uint32_t risr;             /* 0x34 */
    volatile uint32_t txoicr;           /* 0x38 */
    volatile uint32_t rxoicr;           /* 0x3c */
    volatile uint32_t rxuicr;           /* 0x40 */
    volatile uint32_t msticr;           /* 0x44 */
    volatile uint32_t icr;              /* 0x48 */
    volatile uint32_t dmacr;            /* 0x4c */
    volatile uint32_t dmatdlr;          /* 0x50 */
    volatile uint32_t dmardlr;          /* 0x54 */
    volatile uint32_t idr;              /* 0x58 */
    volatile uint32_t ssic_version_id;  /* 0x5c */
    volatile uint32_t dr[36];           /* 0x60..0xec */
    volatile uint32_t rx_sample_delay;  /* 0xf0 */
    volatile uint32_t spi_ctrlr0;       /* 0xf4 */
    volatile uint32_t resv;             /* 0xf8 */
    volatile uint32_t xip_mode_bits;    /* 0xfc */
    volatile uint32_t xip_incr_inst;    /* 0x100 */
    volatile uint32_t xip_wrap_inst;    /* 0x104 */
    volatile uint32_t xip_ctrl;         /* 0x108 */
    volatile uint32_t xip_ser;          /* 0x10c */
    volatile uint32_t xrxoicr;          /* 0x110 */
    volatile uint32_t xip_cnt_time_out; /* 0x114 */
    volatile uint32_t endian;
} spi_t;
```

For SPI0/SPI1, standalone SDK uses these bit offsets in `ctrlr0`:

```text
DFS/data bit length offset: 16
FRF/frame format offset:   21
WORK_MODE offset:          6
TMOD/transfer mode offset: 8
```

A correct standalone-style SPI init is not just raw writes to `ctrlr0`. It also enables the peripheral clock through SYSCTL and sets clock threshold.

## Standalone SDK SPI init sequence

From `kendryte-standalone-sdk/lib/drivers/spi.c`:

```c
static int spi_clk_init(uint8_t spi_num)
{
    if(spi_num == 3)
        sysctl_clock_set_clock_select(SYSCTL_CLOCK_SELECT_SPI3, 1);
    sysctl_clock_enable(SYSCTL_CLOCK_SPI0 + spi_num);
    sysctl_clock_set_threshold(SYSCTL_THRESHOLD_SPI0 + spi_num, 0);
    return 0;
}
```

For SPI0/SPI1:

```c
ctrlr0 = (work_mode << 6)
       | (frame_format << 21)
       | ((data_bit_length - 1) << 16);
```

For normal TX:

```c
spi_set_tmod(spi_num, SPI_TMOD_TRANS);
spi_handle->ssienr = 0x01;
spi_handle->ser = 1U << chip_select;
... write FIFO ...
while((spi_handle->sr & 0x05) != 0x04) ;
spi_handle->ser = 0x00;
spi_handle->ssienr = 0x00;
```

For receive with no command, standalone SDK uses RX mode and writes one dummy value to `dr[0]` to generate clocks:

```c
if(cmd_len == 0)
    spi_set_tmod(spi_num, SPI_TMOD_RECV);
...
spi_handle->ctrlr1 = (uint32_t)(v_rx_len - 1);
spi_handle->ssienr = 0x01;
if(cmd_len == 0)
{
    spi_handle->dr[0] = 0xffffffff;
    spi_handle->ser = 1U << chip_select;
}
```

## MaixPy SD wiring and init path

From MaixPy `components/drivers/sd_card/src/sdcard.c`:

```c
sdcard_config_t config = {
    28, 26, 27, 29, SD_CS_PIN,
};
```

Meaning:

```text
SD MOSI = IO28 -> FUNC_SPI1_D0
SD MISO = IO26 -> FUNC_SPI1_D1
SD SCLK = IO27 -> FUNC_SPI1_SCLK
SD CS   = IO29 -> GPIOHS via FUNC_GPIOHS0 + SD_CS_PIN
```

MaixPy SD init path:

```c
fpioa_set_function(config.sclk_pin, FUNC_SPI1_SCLK);
fpioa_set_function(config.mosi_pin, FUNC_SPI1_D0);
fpioa_set_function(config.miso_pin, FUNC_SPI1_D1);
fpioa_set_function(config.cs_pin, FUNC_GPIOHS0 + config.cs_gpio_num);

sd_lowlevel_init(0);
SD_CS_HIGH();

/* Send 80 dummy clocks with CS high. */
for (index = 0; index < 10; index++)
    frame[index] = 0xFF;
sd_write_data(frame, 10);

/* Then CMD0. */
sd_send_cmd(SD_CMD0, 0, 0x95);
```

MaixPy low-speed setup uses `spi_set_clk_rate(SD_SPI_DEVICE, 200000)` in low-level init and later also has a 400 kHz low-speed helper. Our current test path used 200 kHz in several patches.

## What has already been tried in this repository

Do not repeat these as if they are new ideas:

1. Lazy SD mount through SDK storage wrapper.
2. Dropping stale bad handles after failed mount.
3. MaixPy-like command order in the existing C++ SDK `sdcard.cpp`.
4. More diagnostic logging around `CMD0`, `CMD8`, `CMD55`, `ACMD41`, `CMD58`.
5. Full-duplex read hack through the SDK C++ SPI device wrapper. This hung the K210 during `SD_TEST`; it was reverted/disabled.
6. Changing C++ wrapper chip-select mask from `1` to `2` to match `SPI_CHIP_SELECT_1`. This did not change `CMD0 r=ff`.
7. Raw register SPI patch from inside `sdcard.cpp`. This was disabled because it was not based on a verified integration path and did not reproduce the standalone SDK clock/init wrapper correctly.

The currently useful diagnostic is:

```text
[sdcard] CMD0 r=ff
```

That means no valid SD SPI-mode response on the current K210 SD init path.

## What not to do next

Do not patch FatFs, file paths, ESP flashing, Wi-Fi, SHA, or KSD binary chunks while `CMD0 r=ff` persists.

Do not add raw SPI register writes unless they exactly reproduce the standalone SDK sequence:

1. correct `platform.h` address map;
2. correct `spi_t` register layout;
3. `sysctl_clock_enable(SYSCTL_CLOCK_SPI0 + spi_num)`;
4. `sysctl_clock_set_threshold(SYSCTL_THRESHOLD_SPI0 + spi_num, 0)`;
5. correct SPI0/SPI1 `ctrlr0` offsets;
6. correct transfer mode (`TMOD`) per operation;
7. correct `SER = 1U << chip_select`;
8. correct FPIOA function mapping;
9. correct GPIOHS CS drive mode and state;
10. LCD/DVP/SPI0 shared pin state must not override IO28 before SD init.

## Correct next implementation direction

The next clean implementation should port the known-good standalone/MaixPy path as a separate backend instead of continuing to mutate the C++ storage wrapper.

Recommended target structure:

```text
src/k210_maixpy_sd_port.c
src/k210_maixpy_sd_port.h
```

The port should copy/adapt this API surface from the standalone SDK / MaixPy path:

```c
spi_init()
spi_set_clk_rate()
spi_send_data_standard()
spi_receive_data_standard()
gpiohs_set_pin()
gpiohs_set_drive_mode()
fpioa_set_function()
```

Then wire KSD `SD_TEST`, `PUT`, and `GET` to this backend or expose it through a minimal FatFs diskio bridge.

Until `CMD0 r=01` is achieved, the milestone is not file IO. The milestone is:

```text
[sdcard] CMD0 r=01
```

After that, continue with:

```text
[sdcard] CMD8 r=01 ...
[sdcard] ACMD41 last r=00 ...
[sdcard] CMD58 r=00 ...
KSD:SD_OK rw
```

## Current safe test command

Use this fast test size while debugging SD init:

```bat
cd /d D:\w_space\K210_AI_V7s_Plus && git fetch origin main && git switch -f main && git reset --hard origin/main && py -3 tools\apply_fast_io_tuning.py --ksd-buf 4096 --ksd-stack 12288 --esp-baud 230400 --esp-block 4096 && py -3 tools\patch_build_apply_raw_spi.py && call build_k210.bat && call flash_k210.bat COM12 --no-build && cd /d D:\w_space\K210_ESP_SPI_WIFI && git fetch origin main && git switch -f main && git reset --hard origin/main && call run_pc_wifi_spi_sd_full_test.bat COM12 4096
```

`tools\patch_build_apply_raw_spi.py` currently disables the broken raw SPI build hook and should print:

```text
BUILD_RAW_SPI_PATCH_DISABLED_OK
```
