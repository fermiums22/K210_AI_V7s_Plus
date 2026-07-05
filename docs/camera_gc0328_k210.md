# K210 + GC0328 camera bring-up notes

This document records the working camera configuration and the important dead ends from the GC0328 debugging work. Keep it as the reference before changing `src/camera.c`, `src/gc0328_regs.h`, or `lib/bsp/device/dvp.cpp`.

## Current working state

Repository: `fermiums22/K210_AI_V7s_Plus`

Current tested `main` camera commits:

- `6d5f787d5bbb31b713d8189ac7f1be67c874ef12` — `camera: port tested DVP burst driver to main`
- `21f08d0dfdebd9243ddf0aa29a95ec9b615a25d7` — `camera: port working GC0328 capture to main`
- `9991aa3461f9c8d649a45a9d4b8e2f94f4f47607` — `camera: switch GC0328 capture to VGA`

Current output mode:

- Sensor: GC0328
- Capture size: `640 x 480`
- Pixel format returned by `CAM_CAPTURE`: `RGB565`
- Output file size: `614400` bytes
- Log marker: `maixpy-poll VGA`

Successful VGA log signature:

```text
[cam] after-vga-fix regs id=9d f1=07 f2=01 17=14 4b=8b 50=01 55=01 56=e0 57=02 58=80 59=11 5a=02
[cam] DVP cfg maixpy-poll VGA DATA_MUX1 GC0328_VGA ... fb_bytes=614400 ai_bytes=921600
[cam] maixpy skip_frames poll done ok=8 ...
[cam] maixpy-poll VGA frame 0 ... rows=480 firstrow=0 lastrow=479 ... nonzero=307200 ... ai: ... nonzero=921600
KSD:CAPTURE_OK capture.rgb565 614400 640 480 RGB565
```

Earlier successful QVGA signature:

```text
[cam] DVP cfg maixpy-poll DATA_MUX1 GC0328_QVGA_FIX ... fb_bytes=153600 ai_bytes=230400
[cam] maixpy skip_frames poll done ok=8 ...
[cam] maixpy-poll frame 0 ... rows=240 firstrow=0 lastrow=239 ... nonzero=76800 ... ai: ... nonzero=230400
KSD:CAPTURE_OK capture.rgb565 153600 320 240 RGB565
```

## Hardware pins

The board wiring is fixed:

| Signal | K210 IO | FPIOA function |
|---|---:|---|
| SCCB SDA | IO40 | `FUNC_SCCB_SDA` |
| SCCB SCL | IO41 | `FUNC_SCCB_SCLK` |
| RST | IO42 | `FUNC_CMOS_RST` |
| VSYNC | IO43 | `FUNC_CMOS_VSYNC` |
| PWDN | IO44 | `FUNC_CMOS_PWDN` |
| HREF | IO45 | `FUNC_CMOS_HREF` |
| XCLK | IO46 | `FUNC_CMOS_XCLK` |
| PCLK | IO47 | `FUNC_CMOS_PCLK` |

Important board-specific detail:

```c
if (fpioa_get_io(43, &cfg) == 0) {
    cfg.di_inv = 1;
    fpioa_set_io(43, &cfg);
}
```

This `VSYNC di_inv = 1` is required on this hardware. Attempts to remove it to match MaixPy source more literally caused missing or unusable DVP IRQs.

Also required:

```c
sysctl_set_spi0_dvp_data(1);  // route SPI0/DVP shared data pads to camera
```

After capture, route pads back to LCD/display path:

```c
sysctl_set_spi0_dvp_data(0);
```

## MaixPy reference

Working reference firmware:

```text
maixpy_v0.6.3_2_gd8901fd22_openmv_kmodel_v4_with_ide_support...
```

Exact MaixPy source commit:

```text
d8901fd2272f000226f1c1037c1eb7c412b88e66
```

Important MaixPy files:

```text
components/micropython/port/src/omv/gc0328.c
components/micropython/port/src/omv/sensor.c
```

MaixPy GC0328 rules that matter:

- `gc0328_set_pixformat()` itself returns 0; real DVP format selection is in the higher-level sensor/DVP code.
- For `PIXFORMAT_RGB565` on GC0328, MaixPy uses K210 DVP `DVP_CFG_YUV_FORMAT`, not `DVP_CFG_RGB_FORMAT`.
- `sensor.snapshot()` uses a software finish flag, then waits until DVP frame finish.
- MaixPy IRQ logic is effectively:

```c
if (FRAME_FINISH) {
    clear START | FINISH;
    g_dvp_finish_flag = 1;
} else {
    if (g_dvp_finish_flag == 0)
        dvp_start_convert();
    clear START;
}
```

The working project follows this logic, but also polls `dvp->sts` as a safety net because this RTOS BSP can leave DVP status bits pending while the callback counters do not advance in the current wait window.

## DVP configuration

Current driver behavior in `lib/bsp/device/dvp.cpp`:

- `dvp_config(width, height, false)` — single-frame, not auto mode.
- Uses burst-capable DVP transfer.
- For width divisible by 32, enables `DVP_CFG_BURST_SIZE_4BEATS`.
- For VGA `640 x 480`, observed DVP config:

```text
cfg=0x1e01430f
```

Earlier QVGA working config:

```text
cfg=0x0f00a30f
```

Do not revert the DVP driver back to a non-burst implementation. Non-burst tests produced IRQs but zero/empty output buffers.

## Working GC0328 register modes

### VGA / current maximum

Current maximum used by `main` is VGA `640 x 480`:

```c
{0xfe, 0x00},
{0x4b, 0x8b},
{0x50, 0x01},
{0x51, 0x00},
{0x52, 0x00},
{0x53, 0x00},
{0x54, 0x00},
{0x55, 0x01},
{0x56, 0xe0},
{0x57, 0x02},
{0x58, 0x80},
{0x59, 0x11},
{0x5a, 0x02},
{0x5b, 0x00},
{0x5c, 0x00},
{0x5d, 0x00},
{0x5e, 0x00},
{0x5f, 0x00},
{0x60, 0x00},
{0x61, 0x00},
{0x62, 0x00},
```

This is the native maximum exposed by the MaixPy GC0328 table. If a square image is needed, use a software crop from VGA, for example central `480 x 480`. Do not expect a native `400 x 400` mode from GC0328/MaixPy.

### QVGA / previous stable mode

Earlier stable QVGA mode was `320 x 240` with a board-specific fix:

```text
0x17 = 0x14
0x50 = 0x01
0x59 = 0x22
0x5a = 0x03
```

Important: MaixPy QVGA table writes `0x5a = 0x00`, but this board needed `0x5a = 0x03` for the working QVGA path.

## Memory sizes and layout

Current VGA memory sizes:

```text
RGB565 buffer: 640 * 480 * 2 = 614400 bytes
AI buffer:     640 * 480 * 3 = 921600 bytes
```

Earlier QVGA sizes:

```text
RGB565 buffer: 320 * 240 * 2 = 153600 bytes
AI buffer:     320 * 240 * 3 = 230400 bytes
```

The buffers are static globals, not stack allocations:

```c
static uint32_t s_fb[CAM_FB_WORDS]   __attribute__((aligned(128)));
static uint32_t s_snap[CAM_FB_WORDS] __attribute__((aligned(128)));
static uint8_t  s_ai[CAM_PIXELS * 3] __attribute__((aligned(64)));
```

Known-good QVGA memory log showed no overlap:

```text
ai=0x8006c5c0 + 230400 = 0x800a49c0
fb=0x800a4a00
```

VGA successful log:

```text
fb=0x8014d780 snap=0x801e3780 ai=0x8006c740 fb_bytes=614400 ai_bytes=921600
```

## Capture flow

Current working flow:

1. Configure pins and power banks.
2. Route shared data pads to DVP/camera with `sysctl_set_spi0_dvp_data(1)`.
3. Open `/dev/dvp0` and `/dev/sccb0`.
4. Set XCLK to 24 MHz.
5. Power up sensor and reset it.
6. Read GC0328 ID; expected `0x9d` or `0x9b`.
7. Write `sensor_default_regs`.
8. Write VGA register table.
9. Force `0x17 = 0x14`.
10. Configure DVP as YUV format with AI output and RGB565 display output enabled.
11. Enable frame begin/end events.
12. Clear pending DVP frame flags.
13. Run MaixPy-like `skip_frames` warm-up.
14. Capture a frame by clearing software finish flag and waiting for DVP finish.
15. Reverse 32-bit pixel word byte order with `cam_reverse_u32pixel()` before returning/saving.
16. Route pads back to LCD/display with `sysctl_set_spi0_dvp_data(0)`.

## Why polling exists

The RTOS BSP DVP callback path is not fully reliable for this use case. Logs showed `dvp->sts` changing to values such as:

```text
sts=0x00210000
```

while callback counters in the wait window stayed at zero:

```text
begin_irq=0 end_irq=0 finish_flag=0
```

The fix is `cam_poll_maixpy_irq()`: it reads `dvp->sts` directly and applies the same START/FINISH rules as MaixPy. This is what made both `skip_frames` and snapshot stable.

Do not remove the polling safety net unless the BSP ISR/clear logic is fixed and validated again on hardware.

## Failed experiments / do not repeat blindly

These were tested and should not be repeated as blind experiments:

- DATA_MUX0: hung or unusable. Use DATA_MUX1.
- Removing `VSYNC di_inv = 1`: IRQs disappeared or frame completion was not observed correctly.
- Non-burst DVP: stable-looking IRQs, but frame buffers were all zero.
- `DVP_CFG_RGB_FORMAT` for GC0328 RGB565 path: wrong for this MaixPy-compatible flow. Use `DVP_CFG_YUV_FORMAT`.
- Pure callback-only MaixPy flow without polling: DVP status bits appeared, but wait-window callback counters missed them.
- Starting DVP manually from the snapshot function: produced partial frames, for example about 65 nonzero rows in QVGA.
- `dummy-sync` two-pass capture: not needed and not MaixPy-equivalent.
- BSP ISR experiment `7acefbf57c0ed960d34be26003a5b43d1f33ae31`: bad, it could hang K210 at start. Do not use as a baseline.
- QVGA with `0x5a = 0x00`: broke the board-specific QVGA path. QVGA needed `0x5a = 0x03`.
- Auto-stream / auto mode: produced IRQ activity but zero buffers in earlier tests. Current working mode is single-frame with MaixPy-style START/FINISH handling plus polling.

## Byte order / image viewing

The saved `.rgb565` file is visually correct when interpreted as big-endian RGB565 words. If it is opened as little-endian RGB565, colors look wrong.

The current code calls:

```c
cam_reverse_u32pixel(s_fb, CAM_FB_WORDS);
```

This matches the earlier MaixPy/OpenMV path that reverses 32-bit pixel words after DVP capture. If external tools expect little-endian RGB565, add a separate converter or save path instead of breaking the working DVP capture path.

## Test commands

Build, flash, capture from `main`:

```bat
cd /d D:\w_space\K210_AI_V7s_Plus && git fetch origin main && git switch -f main && git reset --hard origin/main && call build_k210.bat && call flash_k210.bat COM12 --no-build && py -3 tools\ksd_cmd.py --port COM12 --baud 921600 --connect-timeout 30 --cmd "CAM_CAPTURE capture.rgb565" --get capture.rgb565 --out logs\capture.rgb565
```

Convert the resulting raw file on PC with a tool that treats RGB565 words as big-endian, or use the Python/Pillow conversion script from the chat history.

## Recommended next steps

Keep VGA `640 x 480` as the camera baseline.

If KPU or LCD needs a square frame, add an explicit crop mode from VGA, for example:

- `480 x 480` center crop from VGA for square preview/inference.
- `320 x 320` or model-specific resize/crop for KPU input.

Do not change the low-level DVP capture while adding crop/resize. First keep raw VGA capture stable, then transform the image in a separate step.
