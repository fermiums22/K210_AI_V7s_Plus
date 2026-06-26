#pragma once
#include <platform.h>

/*
 * Maix Dock (M1W) full pin assignment
 * Derived from: hardware.md, config_maix_dock.py, live FPIOA probe
 * Format: <signal> IO<n>  FPIOA-function (function-number)
 *
 *  ┌─────────────────────────────────────────────────────────────────┐
 *  │ LCD ILI9341  (FPC, SPI0)                                        │
 *  │  CS   IO36  → FUNC_SPI0_SS3   (15)                             │
 *  │  DC   IO37  → FUNC_GPIOHS30   (54)  high=data, low=cmd         │
 *  │  MOSI IO38  → FUNC_SPI0_D0    ( 4)                             │
 *  │  SCLK IO39  → FUNC_SPI0_SCLK  (17)                             │
 *  │  BL   IO17  → FUNC_GPIO6      (62)  active-high backlight       │
 *  ├─────────────────────────────────────────────────────────────────┤
 *  │ Audio out (PT8211 DAC, I2S0 TX)                                 │
 *  │  WS   IO33  → FUNC_I2S0_WS    (89)  LRCLK                      │
 *  │  DATA IO34  → FUNC_I2S0_OUT_D1 (95) NOTE: D1, not D0!          │
 *  │  BCK  IO35  → FUNC_I2S0_SCLK  (88)  BCLK                      │
 *  ├─────────────────────────────────────────────────────────────────┤
 *  │ Microphone (MEMS, I2S0 RX)                                      │
 *  │  DATA IO20  → FUNC_I2S0_IN_D0  (90)                            │
 *  │  WS   IO30  → FUNC_I2S0_WS    (89)  shared clock               │
 *  │  BCK  IO32  → FUNC_I2S0_SCLK  (88)  shared clock               │
 *  ├─────────────────────────────────────────────────────────────────┤
 *  │ Amplifier PAM8403 (active-LOW mods)                             │
 *  │  MUTE IO9   → FUNC_GPIO1      (57)  0=muted,  1=sound          │
 *  │  SHDN IO10  → FUNC_GPIO2      (58)  0=shutdown,1=powered       │
 *  ├─────────────────────────────────────────────────────────────────┤
 *  │ SD card (FPC, SPI1)                                             │
 *  │  MOSI IO28  → FUNC_SPI1_D0    (19)                             │
 *  │  MISO IO26  → FUNC_SPI1_D1    (20)                             │
 *  │  CLK  IO27  → FUNC_SPI1_SCLK  (22)                             │
 *  │  CS   IO29  → FUNC_GPIOHS29   (53)  software CS                │
 *  ├─────────────────────────────────────────────────────────────────┤
 *  │ WiFi ESP8285 (UART2 from K210 side)                             │
 *  │  TX   IO6   → FUNC_UART2_TX   (  )  K210 → ESP                 │
 *  │  RX   IO7   → FUNC_UART2_RX   (  )  ESP → K210                 │
 *  │  EN   IO8   → FUNC_GPIO0      (56)  pull-low=reset ESP          │
 *  ├─────────────────────────────────────────────────────────────────┤
 *  │ UART debug / ISP  (UARTHS via USB CH340)                        │
 *  │  TX   IO5   → FUNC_UARTHS_TX  (18) (USB CH340 wired here)      │
 *  │  RX   IO4   → FUNC_UARTHS_RX  (19)                             │
 *  ├─────────────────────────────────────────────────────────────────┤
 *  │ STM32 lower board  (UART1 crossover, 3.3V)                      │
 *  │  TX   IO0   → FUNC_UART1_TX       K210 TX → STM32 RX (PA10)   │
 *  │  RX   IO1   → FUNC_UART1_RX       STM32 TX (PA9) → K210 RX    │
 *  │  BOOT0 IO2  → FUNC_GPIO2          K210 GPIO → STM32 BOOT0      │
 *  │  NRST IO3   → FUNC_GPIO3          K210 GPIO → STM32 NRST       │
 *  ├─────────────────────────────────────────────────────────────────┤
 *  │ RGB LED (PWM or GPIO)                                           │
 *  │  R IO12, G IO13, B IO14                                         │
 *  ├─────────────────────────────────────────────────────────────────┤
 *  │ Camera DVP                                                      │
 *  │  IO40-IO47  (VSYNC/HREF/PCLK/DATA0-4)                          │
 *  ├─────────────────────────────────────────────────────────────────┤
 *  │ Misc                                                            │
 *  │  BOOT  IO16  input, pull-up (press = ISP mode on power cycle)  │
 *  └─────────────────────────────────────────────────────────────────┘
 */

/* ── LCD ────────────────────────────────────────────────────────────────── */
#define PIN_LCD_CS       36
#define PIN_LCD_DC       37
#define PIN_LCD_MOSI     38
#define PIN_LCD_SCLK     39
#define PIN_LCD_BL       17
#define GPIOHS_LCD_DC    30      /* GPIOHS number, not IO number             */
#define GPIO_LCD_BL       6      /* GPIO number  */
#define LCD_W           320
#define LCD_H           240

/* ── Audio out ──────────────────────────────────────────────────────────── */
#define PIN_AUD_WS       33
#define PIN_AUD_DATA     34
#define PIN_AUD_BCK      35
#define AUD_SAMPLE_HZ  16000

/* ── Microphone (read-only, not yet used) ───────────────────────────────── */
#define PIN_MIC_DATA     20
#define PIN_MIC_WS       30
#define PIN_MIC_BCK      32

/* ── Amplifier ──────────────────────────────────────────────────────────── */
#define PIN_AMP_MUTE      9
#define PIN_AMP_SHDN     10
#define GPIO_AMP_MUTE     1     /* GPIO peripheral number */
#define GPIO_AMP_SHDN     2

/* ── SD card ────────────────────────────────────────────────────────────── */
#define PIN_SD_MOSI      28
#define PIN_SD_MISO      26
#define PIN_SD_CLK       27
#define PIN_SD_CS        29
#define GPIOHS_SD_CS     29     /* GPIOHS number for soft-CS */

/* ── WiFi ESP8285 ───────────────────────────────────────────────────────── */
#define PIN_ESP_TX        6     /* K210 → ESP RX (AT commands out)           */
#define PIN_ESP_RX        7     /* ESP TX → K210 (AT responses in)           */
#define PIN_ESP_EN        8     /* GPIO0: pull-low to reset, pull-high to run */
#define GPIO_ESP_EN       0

/* ── STM32 UART ─────────────────────────────────────────────────────────── */
#define PIN_STM_TX        0
#define PIN_STM_RX        1
#define PIN_STM_BOOT0     2
#define PIN_STM_NRST      3

/* ── Debug UART (USB CH340, UARTHS) ─────────────────────────────────────── */
#define PIN_DBG_TX        5
#define PIN_DBG_RX        4
#define DBG_BAUD     115200

/* ── RGB LED ─────────────────────────────────────────────────────────────── */
#define PIN_LED_R        12
#define PIN_LED_G        13
#define PIN_LED_B        14

/* ── Misc ────────────────────────────────────────────────────────────────── */
#define PIN_BOOT         16     /* input, pull-up, active-low BOOT button     */
