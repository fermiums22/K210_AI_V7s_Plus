#include "k210_maixpy_sd_port.h"

#include "log.h"
#include "pinout.h"

#include <fpioa.h>
#include <gpiohs.h>
#include <platform.h>
#include <spi.h>
#include <sysctl.h>

#include <stddef.h>
#include <stdint.h>

#define K210_SD_SPI_CHIP_SELECT 1u

#define K210_SPI_TMOD_TRANS 1u
#define K210_SPI_TMOD_RECV  2u

#define K210_SPI_SR_BUSY    0x01u
#define K210_SPI_SR_TFE     0x04u

#define K210_SPI_FIFO_DEPTH 32u
#define K210_SPI_WAIT_LIMIT 1000000u

static volatile spi_t *const s_spi1 = (volatile spi_t *)SPI1_BASE_ADDR;
static volatile gpiohs_t *const s_gpiohs = (volatile gpiohs_t *)GPIOHS_BASE_ADDR;

static void sd_cs_high(void)
{
    s_gpiohs->output_val.u32[0] |= (1u << GPIOHS_SD_CS);
}

static void sd_cs_low(void)
{
    s_gpiohs->output_val.u32[0] &= ~(1u << GPIOHS_SD_CS);
}

static void sd_gpiohs_cs_init(void)
{
    const uint32_t mask = (1u << GPIOHS_SD_CS);

    /*
     * GPIOHS is used as software CS exactly like MaixPy.  Do the output value
     * first so the card sees inactive CS as soon as the output driver is enabled.
     */
    s_gpiohs->output_val.u32[0] |= mask;
    s_gpiohs->output_en.u32[0] |= mask;
    s_gpiohs->input_en.u32[0] &= ~mask;
    s_gpiohs->iof_en.u32[0] &= ~mask;
    s_gpiohs->output_xor.u32[0] &= ~mask;
}

static void sd_pinmux_maixpy(void)
{
    fpioa_set_function(PIN_SD_CLK,  FUNC_SPI1_SCLK);
    fpioa_set_function(PIN_SD_MOSI, FUNC_SPI1_D0);
    fpioa_set_function(PIN_SD_MISO, FUNC_SPI1_D1);
    fpioa_set_function(PIN_SD_CS,   FUNC_GPIOHS0 + GPIOHS_SD_CS);
    fpioa_set_io_pull(PIN_SD_CS, FPIOA_PULL_DOWN);
    sd_gpiohs_cs_init();
}

static void spi1_set_tmod(uint32_t tmod)
{
    s_spi1->ctrlr0 = (s_spi1->ctrlr0 & ~(3u << 8)) | ((tmod & 3u) << 8);
}

static void spi1_disable(void)
{
    s_spi1->ser = 0;
    s_spi1->ssienr = 0;
}

static void spi1_drain_rx(void)
{
    uint32_t guard = K210_SPI_WAIT_LIMIT;
    while (s_spi1->rxflr && guard--)
        (void)s_spi1->dr[0];
}

static void spi1_init_8bit_mode0(void)
{
    sysctl_clock_enable(SYSCTL_CLOCK_SPI1);
    sysctl_clock_set_threshold(SYSCTL_THRESHOLD_SPI1, 0);

    spi1_disable();
    s_spi1->imr = 0;
    s_spi1->dmacr = 0;
    s_spi1->dmatdlr = 0x10;
    s_spi1->dmardlr = 0;
    s_spi1->spi_ctrlr0 = 0;
    s_spi1->endian = 0;

    /*
     * Standalone SDK SPI0/SPI1 ctrlr0 layout:
     *   work mode offset 6, TMOD offset 8, data bits offset 16,
     *   frame format offset 21.
     */
    s_spi1->ctrlr0 = ((8u - 1u) << 16);
    spi1_drain_rx();
}

static uint32_t spi1_set_clk_rate(uint32_t target_hz)
{
    uint32_t base = sysctl_clock_get_freq(SYSCTL_CLOCK_SPI1);
    if (base == 0)
        base = 400000000u;

    uint32_t div = base / target_hz;
    if (div < 2)
        div = 2;
    else if (div > 65534)
        div = 65534;

    s_spi1->baudr = div;
    return base / div;
}

static int spi1_wait_not_busy(void)
{
    uint32_t guard = K210_SPI_WAIT_LIMIT;
    while (((s_spi1->sr & (K210_SPI_SR_BUSY | K210_SPI_SR_TFE)) != K210_SPI_SR_TFE) && guard--)
        ;
    return guard != 0;
}

static int spi1_write_bytes(const uint8_t *data, size_t len)
{
    size_t i = 0;
    uint32_t guard = K210_SPI_WAIT_LIMIT;

    if (!data || len == 0)
        return 1;

    spi1_disable();
    spi1_drain_rx();
    spi1_set_tmod(K210_SPI_TMOD_TRANS);
    s_spi1->ssienr = 1;
    s_spi1->ser = 1u << K210_SD_SPI_CHIP_SELECT;

    while (i < len && guard--) {
        if (s_spi1->txflr < K210_SPI_FIFO_DEPTH) {
            s_spi1->dr[0] = data[i++];
            guard = K210_SPI_WAIT_LIMIT;
        }
    }

    if (i != len || !spi1_wait_not_busy()) {
        spi1_disable();
        return 0;
    }

    spi1_disable();
    spi1_drain_rx();
    return 1;
}

static int spi1_read_bytes(uint8_t *data, size_t len)
{
    size_t i = 0;
    uint32_t guard = K210_SPI_WAIT_LIMIT;

    if (!data || len == 0)
        return 1;

    spi1_disable();
    spi1_drain_rx();
    spi1_set_tmod(K210_SPI_TMOD_RECV);
    s_spi1->ctrlr1 = (uint32_t)(len - 1u);
    s_spi1->ssienr = 1;
    s_spi1->dr[0] = 0xffffffffu;
    s_spi1->ser = 1u << K210_SD_SPI_CHIP_SELECT;

    while (i < len && guard--) {
        if (s_spi1->rxflr) {
            data[i++] = (uint8_t)s_spi1->dr[0];
            guard = K210_SPI_WAIT_LIMIT;
        }
    }

    spi1_disable();

    if (i != len)
        return 0;

    return 1;
}

static void sd_write_data(const uint8_t *data, size_t len)
{
    spi1_init_8bit_mode0();
    (void)spi1_write_bytes(data, len);
}

static uint8_t sd_read_byte(void)
{
    uint8_t b = 0xff;
    spi1_init_8bit_mode0();
    if (!spi1_read_bytes(&b, 1))
        return 0xff;
    return b;
}

static void sd_send_cmd0(void)
{
    static const uint8_t cmd0[6] = {
        0x40u | 0u,
        0x00u, 0x00u, 0x00u, 0x00u,
        0x95u
    };

    sd_cs_low();
    sd_write_data(cmd0, sizeof(cmd0));
}

static void sd_end_cmd(void)
{
    static const uint8_t one_ff = 0xffu;

    sd_cs_high();
    sd_write_data(&one_ff, 1);
}

static uint8_t sd_get_response(void)
{
    uint8_t r = 0xff;

    /*
     * MaixPy uses 0xFFFF here.  Keep the same retry order but every raw SPI
     * receive has its own bounded guard, so a bad bus cannot hang K210.
     */
    for (uint32_t timeout = 0; timeout < 0xffffu; timeout++) {
        r = sd_read_byte();
        if (r != 0xffu)
            return r;
    }

    return 0xffu;
}

uint8_t k210_maixpy_sd_probe_cmd0(void)
{
    uint8_t dummy[10];

    LOG("[sdcard] PROBE begin");
    sd_pinmux_maixpy();
    spi1_init_8bit_mode0();

    uint32_t actual = spi1_set_clk_rate(200000u);
    LOGF("[sdcard] SPI1 low clock %lu Hz", (unsigned long)actual);

    sd_cs_high();
    for (size_t i = 0; i < sizeof(dummy); i++)
        dummy[i] = 0xffu;
    sd_write_data(dummy, sizeof(dummy));

    LOG("[sdcard] CMD0 send");
    sd_send_cmd0();
    uint8_t r = sd_get_response();
    LOGF("[sdcard] CMD0 r=%02x", (unsigned int)r);
    sd_end_cmd();

    return r;
}
