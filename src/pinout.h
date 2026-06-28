#pragma once
#include <platform.h>

/*
 * Maix Dock (M1W) full pin assignment — verified from schematic + FPIOA dump
 *
 * LCD ILI9341 — 8080 8-bit parallel interface (J3 "8bit MCU LCD")
 *  CS   IO36 → GPIOHS27   active-LOW chip select
 *  RST  IO37 → GPIOHS30   active-LOW hardware reset
 *  DC   IO38 → GPIOHS31   LOW=command, HIGH=data
 *  WR   IO39 → GPIOHS28   active-LOW write strobe (rising edge latches data)
 *  DB0  IO18 → GPIOHS0    data bit 0 (LSB)
 *  DB1  IO19 → GPIOHS1
 *  DB2  IO20 → GPIOHS2
 *  DB3  IO21 → GPIOHS3
 *  DB4  IO22 → GPIOHS4
 *  DB5  IO23 → GPIOHS5
 *  DB6  IO24 → GPIOHS6
 *  DB7  IO25 → GPIOHS7    data bit 7 (MSB)
 *  BL   IO17 → GPIO6      backlight, active-HIGH
 *
 * Audio out PT8211 DAC — I2S0 TX
 *  WS   IO33 → I2S0_WS    (LRCLK, fn89)
 *  DATA IO34 → I2S0_OUT_D1 (fn95, NOT D0!)
 *  BCK  IO35 → I2S0_SCLK  (fn88)
 *
 * Amplifier PAM8403 (active-LOW mods)
 *  MUTE IO9  → GPIO1  LOW=muted,     HIGH=sound
 *  SHDN IO10 → GPIO2  LOW=shutdown,  HIGH=powered
 *
 * SD card — SPI1
 *  MOSI IO28 → SPI1_D0
 *  MISO IO26 → SPI1_D1
 *  CLK  IO27 → SPI1_SCLK
 *  CS   IO29 → GPIOHS29  software CS
 *
 * WiFi ESP8285 — UART2
 *  TX   IO6  → UART2_TX  K210 → ESP
 *  RX   IO7  → UART2_RX  ESP → K210
 *  EN   IO8  → GPIO0     LOW=reset, HIGH=run
 *
 * Debug UART (USB CH340, UARTHS)
 *  RX   IO4  → UARTHS_RX
 *  TX   IO5  → UARTHS_TX
 *
 * STM32 lower board — UART1 (crossover, 3.3V)
 *  TX   IO0  → UART1_TX
 *  RX   IO1  → UART1_RX
 *  BOOT0 IO2 → GPIO
 *  NRST IO3  → GPIO
 *
 * RGB LED
 *  R IO12  G IO13  B IO14
 *
 * Boot button
 *  BOOT IO16  input, pull-up, active-LOW → ISP mode on power cycle
 */

/* ── LCD — SPI0 hardware (schematic: IO28=SPI0_D0→FPC LCD_D0) ─────────────── */
#define PIN_LCD_CS        36   /* SPI0_SS3    */
#define PIN_LCD_RST       37   /* GPIOHS30    */
#define PIN_LCD_DC        38   /* GPIOHS31    */
#define PIN_LCD_WR        39   /* SPI0_SCLK   */
#define PIN_LCD_MOSI      28   /* SPI0_D0, wired to FPC LCD_D0 inside DAN module */
#define PIN_LCD_BL        17   /* GPIO6, active-HIGH backlight */

/* GPIOHS numbers for RST/DC */
#define GPIOHS_LCD_RST    30
#define GPIOHS_LCD_DC     31

#define GPIO_LCD_BL        6   /* GPIO peripheral instance number */

#define LCD_W            320
#define LCD_H            240

/* ── Audio out ──────────────────────────────────────────────────────────── */
#define PIN_AUD_WS        33
#define PIN_AUD_DATA      34
#define PIN_AUD_BCK       35
#define AUD_SAMPLE_HZ  16000

/* ── Microphone ─────────────────────────────────────────────────────────── */
#define PIN_MIC_DATA      23   /* I2S0_IN_D0 → FUNC_I2S0_IN_D0 */
#define PIN_MIC_WS        19
#define PIN_MIC_BCK       18

/* ── Amplifier ──────────────────────────────────────────────────────────── */
#define PIN_AMP_MUTE       9
#define PIN_AMP_SHDN      10
#define GPIO_AMP_MUTE      1   /* GPIO1 */
#define GPIO_AMP_SHDN      2   /* GPIO2 */

/* ── SD card ─────────────────────────────────────────────────────────────── */
#define PIN_SD_MOSI       28
#define PIN_SD_MISO       26
#define PIN_SD_CLK        27
#define PIN_SD_CS         29
#define GPIOHS_SD_CS      29

/* ── WiFi ESP8285 ────────────────────────────────────────────────────────── */
#define PIN_ESP_TX         6
#define PIN_ESP_RX         7
#define PIN_ESP_EN         8
#define GPIO_ESP_EN        0

/* ── STM32 ───────────────────────────────────────────────────────────────── */
#define PIN_STM_TX         0
#define PIN_STM_RX         1
#define PIN_STM_BOOT0      2
#define PIN_STM_NRST       3

/* ── Debug UART ──────────────────────────────────────────────────────────── */
#define PIN_DBG_TX         5
#define PIN_DBG_RX         4
#define DBG_BAUD       115200

/* ── RGB LED ──────────────────────────────────────────────────────────────── */
#define PIN_LED_R         12
#define PIN_LED_G         13
#define PIN_LED_B         14

/* ── Misc ─────────────────────────────────────────────────────────────────── */
#define PIN_BOOT          16
