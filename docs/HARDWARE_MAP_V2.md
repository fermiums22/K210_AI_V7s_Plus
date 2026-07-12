# Hardware map v2 — Maix Dock M1W robot controller

This file is the hardware source of truth for the v2 firmware. A signal must not
appear in production code until it is listed here as `CONFIRMED`. Old comments
in `src/pinout.h` and `docs/hardware.md` are historical notes, not authority.

## Evidence levels

- `CONFIRMED-SCHEMATIC`: fixed by the Sipeed M1W/Dock design.
- `CONFIRMED-BENCH`: observed on this board with a focused hardware test.
- `CURRENT-WIRING`: intentionally rewired for this robot but still needs a v2
  continuity/loopback test.
- `UNVERIFIED`: do not enable in production.

Primary references:

- Sipeed Maix Dock: https://wiki.sipeed.com/soft/maixpy/en/develop_kit_board/maix_dock.html
- Sipeed M1W routing warning: https://wiki.sipeed.com/hardware/maixface/zh/mf_ml_module/mf_precautions.html
- Sipeed ESP8285 update guide: https://wiki.sipeed.com/soft/maixpy/en/get_started/upgrade_esp8285_firmware.html
- Maix Dock schematic: https://dl.sipeed.com/fileList/MAIX/HDK/Sipeed-Maix-Dock/Maix-Dock_11.27/Maix-Dock_11.27-schematic.pdf

## Fixed M1W connections

| Function | K210 IO | Peripheral mapping | State | Notes |
|---|---:|---|---|---|
| ESP SPI CS | 0 | SPI1_SS0 in v2 | CONFIRMED-SCHEMATIC | M1W internal connection to ESP GPIO15 |
| ESP SPI CLK | 1 | SPI1_SCLK in v2 | CONFIRMED-SCHEMATIC | M1W internal connection to ESP GPIO14 |
| ESP SPI MISO | 2 | SPI1_D1 in v2 | CONFIRMED-SCHEMATIC | ESP GPIO12 to K210 |
| ESP SPI MOSI | 3 | SPI1_D0 in v2 | CONFIRMED-SCHEMATIC | K210 to ESP GPIO13 |
| Debug RX | 4 | UARTHS_RX | CONFIRMED-BENCH | CH340, logs only, 115200 8N1 |
| Debug TX | 5 | UARTHS_TX | CONFIRMED-BENCH | CH340, logs only, 115200 8N1 |
| ESP UART TX | 6 | UART2_RX | CONFIRMED-BENCH | Signal name is ESP-side; K210 receives |
| ESP UART RX | 7 | UART2_TX | CONFIRMED-BENCH | Signal name is ESP-side; K210 transmits |
| ESP EN | 8 | GPIO0 | CONFIRMED-BENCH | Active high |
| Amplifier MUTE | 9 | GPIO1 | CONFIRMED-BENCH | Low is mute |
| Amplifier SHDN | 10 | GPIO2 | CONFIRMED-BENCH | Low is shutdown |
| RGB LED | 12,13,14 | GPIO/PWM | CONFIRMED-SCHEMATIC | These pins are electrically loaded by LEDs |
| ESP GPIO0/BOOT/READY | 15 | GPIO3 | CONFIRMED-BENCH | Viktor's added wire to ESP corner pad; K210 drives strap during reset, then releases input; ESP drives READY at runtime |
| K210 BOOT | 16 | input | CONFIRMED-SCHEMATIC | Active low at power-up |
| LCD backlight | 17 | GPIO6 | CONFIRMED-BENCH | Active high |
| Microphone BCK | 18 | I2S0_SCLK | CONFIRMED-SCHEMATIC | Shares physical media pin group |
| Microphone WS | 19 | I2S0_WS | CONFIRMED-SCHEMATIC | Shares physical media pin group |
| Microphone DATA | 20 | I2S0_IN_D0 | CONFIRMED-SCHEMATIC | Old `PIN_MIC_DATA=23` is rejected |
| SD MISO | 26 | SPI1_D1 | CONFIRMED-SCHEMATIC | SPI1 is shared with ESP in v2 |
| SD CLK | 27 | SPI1_SCLK | CONFIRMED-SCHEMATIC | SPI1 is shared with ESP in v2 |
| SD MOSI | 28 | SPI1_D0 | CONFIRMED-SCHEMATIC | SPI1 is shared with ESP in v2 |
| SD CS | 29 | GPIOHS29 | CONFIRMED-SCHEMATIC | Software CS |
| Audio WS | 33 | I2S0_WS | CONFIRMED-BENCH | PT8211 output |
| Audio DATA | 34 | I2S0_OUT_D1 | CONFIRMED-BENCH | D1, not D0 |
| Audio BCK | 35 | I2S0_SCLK | CONFIRMED-BENCH | PT8211 output |
| LCD CS | 36 | SPI0_SS3 | CONFIRMED-BENCH | 8080/octet transport |
| LCD RESET | 37 | GPIOHS30 | CONFIRMED-BENCH | Active low |
| LCD DC | 38 | GPIOHS31 | CONFIRMED-BENCH | Command/data |
| LCD WR | 39 | SPI0_SCLK | CONFIRMED-BENCH | Write strobe |
| Camera control/data | 40..47 plus DVP data mux | DVP/SCCB | CONFIRMED-BENCH | GC0328 VGA path |

Sipeed explicitly reserves IO0..IO3 for the internal K210/ESP8285 SPI link on
M1W. They must never be assigned to STM32, LEDs, buttons, or generic GPIO.

## Robot STM32 wiring

STM32 has intentionally been removed from IO0..IO3 to free the fixed M1W SPI
link, but the replacement wires have not yet been soldered. The intended future
mapping is:

| Function | K210 IO | State | Risk |
|---|---:|---|---|
| STM32 NRST | 11 | UNCONNECTED | Solder and continuity-test later |
| K210 TX -> STM32 PA10 RX | 12 | UNCONNECTED | Shares the green/red/blue LED pin group |
| K210 RX <- STM32 PA9 TX | 13 | UNCONNECTED | LED load may affect 921600 signal integrity |
| STM32 BOOT0 | 14 | UNCONNECTED | Shares the LED pin group |

Production enablement requires one v2 test that checks reset, BOOT0, UART RX/TX
at 115200 and 921600, and reports framing/error counters. If 921600 is not clean,
the solution is a wiring change or buffer, not retries or a longer timeout.

## Hardware sharing that software must arbitrate

### SPI1 owner

The ESP internal SPI pins (IO0..IO3) and SD pins (IO26..IO29) use one K210 SPI1
controller in v2. They are different physical wires but cannot be transacted at
the same time. Only the running APP `spi1_owner_task` may reconfigure SPI1/FPIOA.
Normal mode is ESP. APP SD work is an explicit, bounded lease; KLINK is quiesced
before remapping, and the card is flushed/closed/unmounted before returning the
controller to ESP.

BOOT and recovery images do not contain an SD driver and never map or probe the
SD pins. K210, STM32, and ESP firmware updates never use SD, so a wedged card or
a card requiring a physical power cycle cannot block update or rollback.

### Media pin owner

LCD/camera traffic uses the K210 SPI0/DVP data mux. The onboard microphone uses
IO18..IO20 in the same physical media group. Only `media_mux_task` may switch the
mux. Supported modes are:

- `MEDIA_CAMERA`: capture a complete camera frame into one of two RAM buffers;
- `MEDIA_LCD`: render a complete or dirty-rectangle LCD DMA operation;
- `MEDIA_MIC`: capture a bounded audio window into an I2S ring;
- `MEDIA_IDLE`: safe GPIO/input state.

No driver is allowed to call `sysctl_set_spi0_dvp_data()` or remap IO18..IO25
directly. Continuous simultaneous camera, LCD, and microphone operation is not a
capability of this board wiring. V2 time-slices them explicitly and exposes the
active mode and missed-window counters to telemetry.

### I2S0 owner

PT8211 playback and microphone capture share I2S0 clocks. `audio_task` owns I2S0
and DMA. Application/UI/network tasks only exchange timestamped PCM buffers.

## Flash and RAM facts

- External SPI3 flash: 16 MiB (JEDEC `EF 60 18` observed on this board).
- K210 SRAM: 8 MiB; v2 reserves the first 1 MiB for the boot manager and links
  the application at `0x80100000`.
- A v2 application image must fit within 5 MiB and must be position-linked for
  `0x80100000`.

## Required first hardware tests

1. STM32 IO11..IO14 continuity and UART integrity.
2. ESP SPI cells on SPI1, mode 0, fixed pin map: command 2 write + command 3 read,
   8-bit address, 64-byte logical cell, GPIO0/IO15 READY handshake.
3. Frequency ladder 2/4/8/10/16/20 MHz with CRC and exact error counters.
4. SPI1 ownership switch ESP -> SD -> ESP without runtime pin scanning.
5. Media ownership switch camera -> LCD -> microphone -> LCD.

Bench qualification passed all steps with zero CRC/fault through an actual
19.5 MHz clock (20 MHz requested). Production firmware never scans modes, pin
maps, CS variants, or frame lengths.
