#include "log.h"

#include <dmac.h>
#include <platform.h>
#include <spi.h>
#include <stdint.h>
#include <string.h>
#include <sysctl.h>
#include <FreeRTOS.h>
#include <task.h>

#ifndef DMAC_BASE_ADDR
#define DMAC_BASE_ADDR 0x50000000UL
#endif

#define RW_SPI3_CS_MASK        0x01u
#define RW_CMD_READ            0x03u
#define RW_CMD_WREN            0x06u
#define RW_CMD_PP              0x02u
#define RW_CMD_SE4K            0x20u
#define RW_CMD_RDSR1           0x05u
#define RW_CMD_RDSR2           0x35u

#define RW_SPI3_TIMEOUT        100000u
#define RW_SPI3_WIP_TIMEOUT    8000000u
#define RW_SPI3_FLUSH_LIMIT    128u

#define RW_SR1_WIP             0x01u
#define RW_SR1_WEL             0x02u

#define RW_SPI3_CLK_SELECT_PLL0 1u
#define RW_SPI3_CLK_THRESHOLD   2u
#define RW_SPI3_BAUDR           16u
#define RW_SPI3_ENDIAN_NORMAL   0u

#define RW_SPI3_SR_BUSY        0x01u
#define RW_SPI3_SR_TFNF        0x02u
#define RW_SPI3_SR_TFE         0x04u
#define RW_SPI3_SR_RFNE        0x08u

#define RW_SPI3_MOD_OFF        8u
#define RW_SPI3_DFS_OFF        0u
#define RW_SPI3_TMOD_OFF       10u
#define RW_SPI3_FRF_OFF        22u

#define RW_SPI_TMOD_TX_ONLY    1u
#define RW_SPI_TMOD_EEPROM     3u

#define RW_SPI_CTRL_FRAME_STD  (0u << RW_SPI3_FRF_OFF)
#define RW_SPI_CTRL_MODE0      (0u << RW_SPI3_MOD_OFF)
#define RW_SPI_CTRL_DFS8       (7u << RW_SPI3_DFS_OFF)

#define RW_DMAC_CH             0u
#define RW_DMAC_CH_MASK        (1ull << RW_DMAC_CH)
#define RW_DMAC_CH_WE          (1ull << (RW_DMAC_CH + 8u))

static volatile spi_t *const RW_SPI3 = (volatile spi_t *)SPI3_BASE_ADDR;
static volatile dmac_t *const RW_DMAC = (volatile dmac_t *)DMAC_BASE_ADDR;

static void rw_dma_disable(void)
{
    RW_DMAC->chen = RW_DMAC_CH_WE;
    RW_DMAC->channel[RW_DMAC_CH].intclear = 0xffffffffull;
}

static void rw_flush_rx(void)
{
    for (uint32_t n = 0; n < RW_SPI3_FLUSH_LIMIT; ++n) {
        if ((RW_SPI3->sr & RW_SPI3_SR_RFNE) == 0)
            break;
        (void)RW_SPI3->dr[0];
    }
}

static int rw_wait_mask(uint32_t mask, uint32_t value)
{
    for (uint32_t n = 0; n < RW_SPI3_TIMEOUT; ++n) {
        if ((RW_SPI3->sr & mask) == value)
            return 0;
    }
    return -1;
}

static void rw_deassert(void)
{
    RW_SPI3->ser = 0;
    (void)rw_wait_mask(RW_SPI3_SR_BUSY, 0);
    RW_SPI3->dmacr = 0;
    RW_SPI3->ssienr = 0;
    rw_flush_rx();
}

static void rw_prepare_std(uint32_t tmod)
{
    sysctl_clock_set_clock_select(SYSCTL_CLOCK_SELECT_SPI3, RW_SPI3_CLK_SELECT_PLL0);
    sysctl_clock_set_threshold(SYSCTL_THRESHOLD_SPI3, RW_SPI3_CLK_THRESHOLD);
    sysctl_clock_enable(SYSCTL_CLOCK_SPI3);
    sysctl_clock_enable(SYSCTL_CLOCK_DMA);

    rw_dma_disable();
    (void)rw_wait_mask(RW_SPI3_SR_BUSY, 0);

    RW_SPI3->ser = 0;
    RW_SPI3->ssienr = 0;
    RW_SPI3->baudr = RW_SPI3_BAUDR;
    RW_SPI3->imr = 0;
    RW_SPI3->dmacr = 0;
    RW_SPI3->dmatdlr = 0x10;
    RW_SPI3->dmardlr = 0;
    RW_SPI3->rx_sample_delay = 0;
    RW_SPI3->endian = RW_SPI3_ENDIAN_NORMAL;
    RW_SPI3->spi_ctrlr0 = 0;
    RW_SPI3->ctrlr1 = 0;
    RW_SPI3->ctrlr0 = RW_SPI_CTRL_MODE0 | RW_SPI_CTRL_FRAME_STD | RW_SPI_CTRL_DFS8 |
                      (tmod << RW_SPI3_TMOD_OFF);
    (void)RW_SPI3->icr;
    rw_flush_rx();
}

static int rw_tx_only(const uint8_t *tx, uint32_t tx_len)
{
    if (!tx || tx_len == 0)
        return -1;

    rw_prepare_std(RW_SPI_TMOD_TX_ONLY);
    RW_SPI3->ssienr = 1;
    RW_SPI3->ser = RW_SPI3_CS_MASK;

    for (uint32_t i = 0; i < tx_len; ++i) {
        if (rw_wait_mask(RW_SPI3_SR_TFNF, RW_SPI3_SR_TFNF) != 0) {
            rw_deassert();
            return -2;
        }
        RW_SPI3->dr[0] = tx[i];
    }

    if (rw_wait_mask(RW_SPI3_SR_TFE, RW_SPI3_SR_TFE) != 0) {
        rw_deassert();
        return -3;
    }
    if (rw_wait_mask(RW_SPI3_SR_BUSY, 0) != 0) {
        rw_deassert();
        return -4;
    }

    rw_deassert();
    return 0;
}

static int rw_read_cmd(const uint8_t *cmd, uint32_t cmd_len, uint8_t *rx, uint32_t rx_len)
{
    uint32_t got = 0;
    if (!cmd || cmd_len == 0 || (!rx && rx_len))
        return -1;
    if (rx_len == 0)
        return 0;

    rw_prepare_std(RW_SPI_TMOD_EEPROM);
    RW_SPI3->ctrlr1 = rx_len - 1u;
    RW_SPI3->ssienr = 1;

    for (uint32_t i = 0; i < cmd_len; ++i) {
        if (rw_wait_mask(RW_SPI3_SR_TFNF, RW_SPI3_SR_TFNF) != 0) {
            rw_deassert();
            return -2;
        }
        RW_SPI3->dr[0] = cmd[i];
    }

    RW_SPI3->ser = RW_SPI3_CS_MASK;
    while (got < rx_len) {
        uint32_t progressed = 0;
        for (uint32_t n = 0; n < RW_SPI3_TIMEOUT; ++n) {
            while ((RW_SPI3->sr & RW_SPI3_SR_RFNE) && got < rx_len) {
                rx[got++] = (uint8_t)RW_SPI3->dr[0];
                progressed = 1;
            }
            if (progressed)
                break;
        }
        if (!progressed) {
            rw_deassert();
            return -3;
        }
    }

    rw_deassert();
    return 0;
}

static int rw_read_sr1(uint8_t *sr1)
{
    uint8_t cmd = RW_CMD_RDSR1;
    if (!sr1)
        return -1;
    *sr1 = 0xffu;
    return rw_read_cmd(&cmd, 1, sr1, 1);
}

int app_spi3_read_status(uint8_t *sr1, uint8_t *sr2)
{
    int rc1 = 0;
    int rc2 = 0;
    if (sr1) {
        uint8_t cmd = RW_CMD_RDSR1;
        *sr1 = 0xffu;
        rc1 = rw_read_cmd(&cmd, 1, sr1, 1);
    }
    if (sr2) {
        uint8_t cmd = RW_CMD_RDSR2;
        *sr2 = 0xffu;
        rc2 = rw_read_cmd(&cmd, 1, sr2, 1);
    }
    if (rc1 != 0)
        return -10 + rc1;
    if (rc2 != 0)
        return -20 + rc2;
    return 0;
}

static int rw_wren(void)
{
    uint8_t cmd = RW_CMD_WREN;
    uint8_t sr1 = 0xffu;
    int rc = rw_tx_only(&cmd, 1);
    if (rc != 0)
        return -10 + rc;
    rc = rw_read_sr1(&sr1);
    LOGF("APP_SPI3_WREN rc=%d sr1=0x%02x", rc, (unsigned)sr1);
    if (rc != 0)
        return -20 + rc;
    if ((sr1 & RW_SR1_WEL) == 0)
        return -30;
    return 0;
}

static int rw_wait_done(const char *tag)
{
    uint8_t last_sr1 = 0xffu;
    for (uint32_t n = 0; n < RW_SPI3_WIP_TIMEOUT; ++n) {
        uint8_t sr1 = 0xffu;
        int rc = rw_read_sr1(&sr1);
        last_sr1 = sr1;
        if (rc != 0) {
            LOGF("APP_SPI3_%s_SR_FAIL rc=%d sr1=0x%02x", tag, rc, (unsigned)sr1);
            return -1;
        }
        if (sr1 == 0xffu) {
            LOGF("APP_SPI3_%s_BUS_FF sr1=0x%02x", tag, (unsigned)sr1);
            return -6;
        }
        if ((sr1 & RW_SR1_WIP) == 0) {
            LOGF("APP_SPI3_%s_DONE sr1=0x%02x", tag, (unsigned)sr1);
            return 0;
        }
        if ((n & 0x3ffu) == 0)
            taskYIELD();
    }
    LOGF("APP_SPI3_%s_TIMEOUT sr1=0x%02x", tag, (unsigned)last_sr1);
    return -2;
}

int app_spi3_sector_4k(uint32_t flash_offset)
{
    uint8_t cmd[4] = {
        RW_CMD_SE4K,
        (uint8_t)((flash_offset >> 16) & 0xffu),
        (uint8_t)((flash_offset >> 8) & 0xffu),
        (uint8_t)(flash_offset & 0xffu)
    };
    int rc;

    if ((flash_offset & 0xfffu) != 0)
        return -1;

    rc = rw_wren();
    if (rc != 0)
        return -10 + rc;

    rc = rw_tx_only(cmd, sizeof(cmd));
    LOGF("APP_SPI3_ERASE_TX_ONLY off=0x%08lx rc=%d", (unsigned long)flash_offset, rc);
    if (rc != 0)
        return -20 + rc;

    rc = rw_wait_done("ERASE");
    if (rc != 0)
        return -30 + rc;
    return 0;
}

int app_spi3_program_page(uint32_t flash_offset, const void *src, uint32_t len)
{
    const uint8_t *in = (const uint8_t *)src;
    uint8_t cmd[4 + 256];
    int rc;

    if (!in || len == 0 || len > 256u)
        return -1;
    if (((flash_offset & 0xffu) + len) > 256u)
        return -2;

    cmd[0] = RW_CMD_PP;
    cmd[1] = (uint8_t)((flash_offset >> 16) & 0xffu);
    cmd[2] = (uint8_t)((flash_offset >> 8) & 0xffu);
    cmd[3] = (uint8_t)(flash_offset & 0xffu);
    memcpy(&cmd[4], in, len);

    rc = rw_wren();
    if (rc != 0)
        return -10 + rc;

    rc = rw_tx_only(cmd, len + 4u);
    LOGF("APP_SPI3_PROG_TX_ONLY off=0x%08lx len=%lu rc=%d",
         (unsigned long)flash_offset, (unsigned long)len, rc);
    if (rc != 0)
        return -20 + rc;

    rc = rw_wait_done("PROG");
    if (rc != 0)
        return -30 + rc;
    return 0;
}

int app_spi3_read_raw(uint32_t flash_offset, void *dst, uint32_t len)
{
    uint8_t cmd[4];
    uint8_t *out = (uint8_t *)dst;
    if (!out && len)
        return -1;
    if (len == 0)
        return 0;

    cmd[0] = RW_CMD_READ;
    cmd[1] = (uint8_t)((flash_offset >> 16) & 0xffu);
    cmd[2] = (uint8_t)((flash_offset >> 8) & 0xffu);
    cmd[3] = (uint8_t)(flash_offset & 0xffu);
    return rw_read_cmd(cmd, sizeof(cmd), out, len);
}
