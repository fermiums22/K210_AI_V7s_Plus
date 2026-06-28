#ifndef _PROJECT_CFG_H_
#define _PROJECT_CFG_H_

/*
 * LCD pin configuration for the Maix Dock 8080/SPI0-octal panel.
 * See pinout.h for the full board map; values mirror it here so the
 * upstream jlt32009a transport stays self-contained.
 *
 * GPIOHS channel numbers double as the FUNC_GPIOHS0 offset used in
 * fpioa_set_function(io, FUNC_GPIOHS0 + <num>).
 */

/* IO pad numbers */
#define LCD_CS_IO         36   /* → FUNC_SPI0_SS3   */
#define LCD_WR_IO         39   /* → FUNC_SPI0_SCLK  */
#define LCD_DCX_IO        38   /* → GPIOHS (D/CX)   */
#define LCD_RST_IO        37   /* → GPIOHS (reset)  */
#define LCD_BL_IO         17   /* → GPIOHS (backlight, active-HIGH) */

/* GPIOHS channels (managed via /dev/gpio0) */
#define DCX_GPIONUM       31   /* IO38 — data/command select */
#define RST_GPIONUM       30   /* IO37 — hardware reset       */
#define BL_GPIONUM        26   /* IO17 — backlight            */

/* SPI0 hardware slave-select index: LCD CS is wired to SPI0_SS3 */
#define SPI_SLAVE_SELECT  3

#endif /* _PROJECT_CFG_H_ */
