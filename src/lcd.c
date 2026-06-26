#include "lcd.h"
#include "pinout.h"
#include <fpioa.h>
#include <platform.h>
#include <sysctl.h>
#include <sleep.h>
#include <stddef.h>

/* ── GPIOHS direct register access ─────────────────────────────────────── */
static volatile gpiohs_t *const GPIOHS =
    (volatile gpiohs_t *)GPIOHS_BASE_ADDR;

/* GPIO6 for backlight: use regular GPIO peripheral                         */
static volatile gpio_t *const GPIO_PERIPH =
    (volatile gpio_t *)GPIO_BASE_ADDR;

static inline void dc_cmd(void)  { GPIOHS->output_val.u32[0] &= ~(1u << LCD_GPIOHS_DC); }
static inline void dc_data(void) { GPIOHS->output_val.u32[0] |=  (1u << LCD_GPIOHS_DC); }

/* ── SPI0 direct register access ───────────────────────────────────────── */
static volatile spi_t *const SPI0 = (volatile spi_t *)SPI0_BASE_ADDR;

#define SPI_BAUDR    8      /* divider → ~50 MHz / 8 = 6.25 MHz (safe for ILI9341) */
#define SPI_CTRLR0_FRF_MOTO   0
#define SPI_CTRLR0_TMOD_TX    1     /* transmit only */

static void spi_cs(int on)
{
    /* SPI0_SS3 is driven by the SPI hardware slave-select                  */
    if (on) SPI0->ser |=  (1u << 3);
    else    SPI0->ser &= ~(1u << 3);
}

static void spi_init_hw(void)
{
    sysctl_clock_enable(SYSCTL_CLOCK_SPI0);
    sysctl_reset(SYSCTL_RESET_SPI0);

    SPI0->ssienr = 0;                /* disable while configuring            */
    /* 8-bit, Motorola SPI, CPOL=0, CPHA=0, TX-only                        */
    SPI0->ctrlr0 = (0x07u)          /* DFS = 8 bits (value = bits-1)        */
                 | (SPI_CTRLR0_FRF_MOTO << 4)
                 | (SPI_CTRLR0_TMOD_TX  << 8);
    SPI0->baudr  = SPI_BAUDR;
    SPI0->txftlr = 0;
    SPI0->rxftlr = 0;
    SPI0->imr    = 0;               /* mask all interrupts                   */
    SPI0->dmacr  = 0;
    SPI0->ssienr = 1;               /* enable                                */
}

static void spi_write8(uint8_t b)
{
    while (!(SPI0->sr & 0x2))       /* wait TX FIFO not full                */
        ;
    SPI0->dr[0] = b;
    while (SPI0->sr & 0x1)          /* wait not busy                        */
        ;
}

static void spi_write16(uint16_t v)
{
    spi_write8(v >> 8);
    spi_write8(v & 0xFF);
}

/* ── ILI9341 commands ───────────────────────────────────────────────────── */
static void cmd(uint8_t c)
{
    dc_cmd();
    spi_cs(1);
    spi_write8(c);
    spi_cs(0);
}

static void cmd_data(uint8_t c, const uint8_t *d, int n)
{
    cmd(c);
    dc_data();
    spi_cs(1);
    for (int i = 0; i < n; i++) spi_write8(d[i]);
    spi_cs(0);
}

/* ── Minimal ILI9341 init sequence ─────────────────────────────────────── */
void lcd_init(void)
{
    /* FPIOA assignments */
    fpioa_set_function(PIN_LCD_MOSI, FUNC_SPI0_D0);
    fpioa_set_function(PIN_LCD_SCLK, FUNC_SPI0_SCLK);
    fpioa_set_function(PIN_LCD_CS,   FUNC_SPI0_SS3);
    fpioa_set_function(PIN_LCD_DC,   FUNC_GPIOHS30);

    /* Backlight via regular GPIO6 */
    fpioa_set_function(PIN_LCD_BL, FUNC_GPIO6);
    GPIO_PERIPH->direction.u32[0]   |= (1u << GPIO_LCD_BL);
    GPIO_PERIPH->data_output.u32[0] |= (1u << GPIO_LCD_BL);  /* BL on */

    /* DC as GPIOHS30 output */
    GPIOHS->output_en.u32[0]  |= (1u << LCD_GPIOHS_DC);
    GPIOHS->input_en.u32[0]   &= ~(1u << LCD_GPIOHS_DC);

    spi_init_hw();

    /* Hardware reset via DC line toggling + delay (no RST pin wired) */
    dc_cmd();
    msleep(10);
    dc_data();
    msleep(10);

    /* ILI9341 init */
    cmd(0x01);              /* software reset */
    msleep(120);
    cmd(0x11);              /* sleep out      */
    msleep(5);

    uint8_t d1[] = {0x39, 0x2C, 0x00, 0x34, 0x02};
    cmd_data(0xCB, d1, 5);

    uint8_t d2[] = {0x00, 0xC1};
    cmd_data(0xCF, d2, 2);

    uint8_t d3[] = {0x85, 0x00, 0x78};
    cmd_data(0xE8, d3, 3);

    uint8_t d4[] = {0x39, 0x2C, 0x00};
    cmd_data(0xEA, d4, 3);

    uint8_t d5[] = {0x10};
    cmd_data(0xED, d5, 1);

    uint8_t d6[] = {0x20};
    cmd_data(0xF6, d6, 1); /* pump ratio */

    uint8_t d7[] = {0x23};
    cmd_data(0xC0, d7, 1); /* power ctrl 1: VRH=4.60V */

    uint8_t d8[] = {0x10};
    cmd_data(0xC1, d8, 1); /* power ctrl 2 */

    uint8_t d9[] = {0x3E, 0x28};
    cmd_data(0xC5, d9, 2); /* VCOM ctrl 1 */

    uint8_t d10[] = {0x86};
    cmd_data(0xC7, d10, 1); /* VCOM ctrl 2 */

    /* Memory access: landscape, RGB order */
    uint8_t madctl[] = {0x48};  /* MX|BGR */
    cmd_data(0x36, madctl, 1);

    uint8_t pf[] = {0x55};      /* 16 bits/pixel */
    cmd_data(0x3A, pf, 1);

    uint8_t fc[] = {0x00, 0x18};
    cmd_data(0xB1, fc, 2);

    uint8_t df[] = {0x08, 0x82, 0x27};
    cmd_data(0xB6, df, 3);

    uint8_t gs[] = {0x00};
    cmd_data(0xF2, gs, 1);

    cmd_data(0x26, (uint8_t[]){0x01}, 1); /* gamma set */

    /* Positive/negative gamma — ILI9341 defaults */
    uint8_t pgam[] = {0x0F,0x31,0x2B,0x0C,0x0E,0x08,0x4E,0xF1,0x37,0x07,0x10,0x03,0x0E,0x09,0x00};
    cmd_data(0xE0, pgam, 15);
    uint8_t ngam[] = {0x00,0x0E,0x14,0x03,0x11,0x07,0x31,0xC1,0x48,0x08,0x0F,0x0C,0x31,0x36,0x0F};
    cmd_data(0xE1, ngam, 15);

    cmd(0x11);   /* sleep out */
    msleep(120);
    cmd(0x29);   /* display on */

    lcd_fill(rgb(0, 0, 100));   /* blue — proves LCD works on boot */
}

void lcd_set_window(int x, int y, int w, int h)
{
    int x2 = x + w - 1, y2 = y + h - 1;
    cmd_data(0x2A, (uint8_t[]){x>>8, x&0xFF, x2>>8, x2&0xFF}, 4);
    cmd_data(0x2B, (uint8_t[]){y>>8, y&0xFF, y2>>8, y2&0xFF}, 4);
    cmd(0x2C);   /* start write */
    dc_data();
    spi_cs(1);
}

void lcd_write_pixels(const uint16_t *buf, int count)
{
    for (int i = 0; i < count; i++) spi_write16(buf[i]);
    spi_cs(0);
}

void lcd_fill(uint16_t color)
{
    lcd_set_window(0, 0, LCD_W, LCD_H);
    for (int i = 0; i < LCD_W * LCD_H; i++) spi_write16(color);
    spi_cs(0);
}

void lcd_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    lcd_set_window(x, y, w, h);
    for (int i = 0; i < w * h; i++) spi_write16(color);
    spi_cs(0);
}
