# Robot controller architecture v2

## Objective

Build a deterministic C/FreeRTOS firmware stack for the K210 + ESP8285 robot:

- low-latency robot commands and telemetry over Wi-Fi;
- bulk transfer of firmware, pictures, models, sounds, and configuration;
- camera frames from K210 to the network;
- audio playback and bounded microphone capture;
- LCD rendering without blocking camera/audio/network state machines;
- Wi-Fi update of K210 and STM32, plus a controlled ESP recovery/update path;
- 115200 UART used only for human-readable logs and recovery diagnostics.

The v2 build is independent. It lives under `firmware_v2` and does not compile
legacy `src`, KSD, `esp_spi_link`, runtime SPI scanners, fast-tuning scripts, or
SD-staged update code.

## Non-negotiable rules

1. Every hardware peripheral has exactly one owner task.
2. Drivers never sleep to make races disappear. Completion is interrupt/DMA or
   a protocol state transition.
3. A deadline detects a failed operation; increasing a deadline is not a fix.
4. No hidden automatic retries. A CRC/protocol/write error aborts the operation,
   records the first failing sequence/offset, and reports it end-to-end.
5. The only repeated wire cell is an unconsumed cell held for flow control. It
   has the same sequence number and is never applied twice.
6. A PC success response is emitted only after the final K210/STM32/ESP target
   has committed and verified the requested operation.
7. Logs stay at UARTHS 115200. Bulk data never shares the log stream.
8. ESP runs only as a station. There is no SoftAP, captive portal, fallback AP,
   provisioning mode, or saved Wi-Fi profile.
9. SD is application-owned removable storage. Boot, recovery, K210 update, ESP
   update, and STM32 update never initialize, mount, probe, or depend on SD.

## Repository layout

```text
K210_AI_V7s_Plus/
  firmware_v2/              K210 application, linked at 0x80100000
  protocol/                 KLINK and network protocol definitions/tests
  docs/                     architecture and verified hardware map

K210_AI_V7s_Plus_Boot/
  firmware_v2/              minimal A/B validator and chainloader
  protocol/                 image/metadata format shared with the app

K210_ESP_SPI_WIFI/
  esp8266_rtos_v2/bridge/   ESP8266_RTOS_SDK bridge firmware
  protocol/                 exact KLINK wire definition and test vectors
  tools/robotctl/           PC control/update/benchmark tool
```

Each firmware project names all sources explicitly. No source glob may pull a
legacy file into v2 by accident.

## Runtime ownership model

| Owner | Priority | Owns | Receives |
|---|---:|---|---|
| `klink_task` | highest service | logical KLINK state and SPI cells | channel TX/RX queues |
| `spi1_owner_task` | high | SPI1, ESP/SD FPIOA maps, DMA | ESP polls and bounded SD leases |
| `stm32_task` | high | UART1, STM32 BOOT0/NRST | robot commands, telemetry, STM update stream |
| `audio_task` | high | I2S0, audio DMA, amp state | PCM playback/capture buffers |
| `media_mux_task` | medium-high | SPI0/DVP mux and IO18..IO25 state | camera/LCD/mic leases |
| `camera_task` | medium | DVP frame buffers | capture requests/stream policy |
| `display_task` | medium | framebuffer and dirty rectangles | UI/render messages |
| `flash_task` | medium | SPI3 update writer and metadata | verified update stream |
| `router_task` | medium | command authorization and dispatch | decoded network/KLINK messages |
| `health_task` | low | watchdog votes and counters | task heartbeat snapshots |

Application code cannot call FPIOA, SPI, UART, I2S, DVP, flash, or FatFs APIs.
It posts typed messages to the owner.

## KLINK v1 physical transport

- K210 is SPI master, ESP8285 is SPI slave.
- ESP8266 HSPI uses fixed buffer commands: `2` writes K210→ESP and `3` reads
  ESP→K210, each with an 8-bit zero address and one 64-byte logical KLINK cell.
- The added ESP GPIO0↔K210 IO15 wire is the boot strap during reset and a READY
  handshake after boot. ESP asserts READY only after the complete MISO cell is
  stable; K210 never guesses with sleeps.
- K210 SPI1 on fixed IO0..IO3, mode 0, MSB first.
- One exchange is a READY-gated 64-byte read-buffer operation followed by a
  64-byte write-buffer operation. ESP processes each write exactly once from
  the HSPI done interrupt; it never free-runs over a shared buffer.
- Production has one fixed frequency selected by the hardware qualification
  test. It never scans frequency, mode, CS, pin order, magic offset, or length.
- Idle polling is periodic for bounded command latency. When either side has
  credits/data, polling is continuous with no per-cell delay.

### Cell format (64 bytes, little-endian fields)

```text
offset size field
0      2    magic = 0x4b4c ("LK")
2      1    version = 1
3      1    channel_type: channel[7:5], type[4:0]
4      2    sequence
6      2    acknowledgement
8      1    payload_length (0..48)
9      1    ack_channel[7:5], receive_credit[4:0] (0..31 cells)
10     2    flags
12     48   payload
60     4    CRC32 over bytes 0..59
```

Reliable channels keep a cell stable until its sequence is acknowledged. If
the master polls before the slave has armed a new cell, the same sequence can be
observed again and is discarded as an already-consumed cell. A CRC error, an
unexpected new sequence, or a credit violation faults the stream immediately;
there is no error retry loop.

Channels:

| ID | Use | Policy |
|---:|---|---|
| 0 | link control, capabilities, time, fault | reliable |
| 1 | robot command/telemetry | reliable, priority over bulk |
| 2 | firmware/files/models/assets | reliable, credit limited |
| 3 | audio playback | realtime, stale data dropped |
| 4 | microphone uplink | realtime, gaps counted |
| 5 | camera/preview | realtime, complete frames preferred |
| 6 | diagnostics/benchmark | explicit test mode only |
| 7 | reserved | disabled |

## ESP network model

ESP is a stateless transport endpoint, not the robot application.

- Station mode only: SSID `Fermiums_2.4`, credentials compiled into this private
  board build, Wi-Fi storage in RAM, and no NVS profile or AP fallback.
- On every power-up ESP starts the station and connects immediately. Network
  readiness is advertised to K210 only after DHCP has supplied an address and
  TCP port 21002 is listening.
- TCP 21002 carries framed control, telemetry, file and firmware operations.
  `TCP_NODELAY` is enabled. Every operation has a length and integrity digest;
  `OK queued` is forbidden.
- KLINK channel 1 retains priority for robot control and telemetry. Channel 2
  carries files and firmware under receiver credits. Realtime media use their
  dedicated KLINK channels and explicit sequence numbers.
- UART at 115200 is used for human-readable logs. ESP ROM recovery begins at
  115200, loads a stub, and then switches to the separately qualified high rate.

The ESP SPI task is higher priority than socket workers. Socket workers write to
bounded rings using KLINK credits as backpressure, so TCP naturally stops the
sender when K210 cannot accept more data. A disconnect or malformed frame aborts
the current operation at its first failing offset; the firmware does not hide it
with reconnect/retry loops.

## SD ownership and file service

Only the running K210 application owns the SD card. The boot manager and all
recovery/update builds are linked without FatFs/SD sources and never remap SPI1
to IO26..IO29.

The APP file-service sequence is:

1. quiesce KLINK bulk traffic at a cell boundary and obtain an SPI1 SD lease;
2. map SPI1 to the SD pins, initialize and mount once;
3. stream a bounded read or write while reporting the exact byte offset on the
   first card/protocol error;
4. for writes, flush the file and filesystem metadata before reporting success;
5. close all handles, unmount, return the pins/controller to the ESP mapping,
   and resume KLINK.

Before entering BOOT the APP rejects new SD requests, drains the active request,
flushes, closes, unmounts, releases the lease, and only then reboots. A failed SD
shutdown blocks that transition and is reported; BOOT never attempts to repair
or remount the card. Thus a card that requires a physical power cycle cannot
break Wi-Fi control or any controller update.

## K210 flash layout (16 MiB SPI3)

```text
0x000000..0x0fffff  immutable/recovery boot manager region (1 MiB)
0x100000..0x5fffff  application slot A (5 MiB)
0x600000..0xafffff  application slot B (5 MiB)
0xb00000..0xfdffff  models/assets/cache (4992 KiB)
0xfe0000..0xfeffff  metadata journal 0 (64 KiB)
0xff0000..0xffffff  metadata journal 1 (64 KiB)
```

The running application always writes only the inactive slot. Upload flow:

1. PC sends manifest: image size, load/entry address, version, SHA-256.
2. K210 validates bounds and erases the inactive slot ahead of the stream.
3. K210 writes aligned pages directly to SPI3; SD is not in the path.
4. K210 computes streaming SHA-256 and then computes a readback SHA-256.
5. Only if both match does K210 append a `pending` metadata record.
6. PC receives `VERIFIED_PENDING`; reboot requires a separate commit command.
7. Boot manager loads the pending slot once. The app confirms only after core
   service tasks and the watchdog are healthy.
8. An unconfirmed boot selects the previously confirmed slot on the next reset.

Metadata is append-only with magic, schema, generation, slot state, image
properties, SHA-256, and record CRC32. Two journals prevent a power loss during
metadata erase from destroying the last confirmed state.

## STM32 update

STM32 control and update use the dedicated UART1 owner. Normal robot traffic is
921600 8N1 with binary framing and CRC. Update flow quiesces robot control,
asserts BOOT0, pulses NRST, performs the STM32 ROM protocol, verifies, restores
BOOT0, resets, and waits for an application identity frame. Failure reports the
first ROM command/address and leaves motors disabled.

## ESP update and recovery

Normal ESP firmware images arrive by Wi-Fi/KLINK and are stored in the K210
asset/cache region. A dedicated recovery state machine then:

1. quiesces KLINK and closes network sessions;
2. selects ESP UART ROM mode with GPIO0/EN;
3. connects once at 115200 and loads the flasher stub;
4. switches to the highest separately qualified UART rate;
5. writes blocks once, verifies target MD5/SHA result, and aborts on first error;
6. resets ESP and requires a matching build ID before declaring success.

Initial recovery when ESP has no valid image uses a separate minimal K210
`recovery_v2` build. It embeds the ESP image and contains no LCD, camera, audio,
SD, KSD, or application code.

## Media scheduling

The board cannot continuously expose camera/LCD/microphone pins at the same
time. The router requests bounded leases:

- camera capture fills RAM buffer A/B, then releases DVP pins;
- display consumes a completed buffer or dirty rectangles and releases SPI0;
- microphone capture obtains scheduled windows and fills a PCM ring;
- audio output continues through the audio owner when the I2S configuration is
  compatible; otherwise the owner reports the unsupported combination.

Policy is visible and configurable (for example `camera-priority`,
`voice-priority`, `display-priority`). There are no implicit pin remaps inside
device drivers.

## Performance and acceptance gates

Every stage has counters and a pass condition before the next stage is enabled:

1. Host protocol unit tests and fixed test vectors pass.
2. ESP/K210 64-byte logical cells pass a long deterministic pattern at each
   frequency step with zero CRC/sequence/credit errors.
3. Reliable 1 MiB RAM-to-RAM transfer matches SHA-256 and reports throughput.
4. Direct inactive-slot write/readback matches SHA-256 and survives power loss
   before metadata commit.
5. A/B boot confirms a healthy image and rolls back one unconfirmed image.
6. Control latency remains bounded while bulk transfer is saturated.
7. Audio underflow, microphone gap, camera drop, and display lease counters are
   zero or explicitly within the selected policy budget.
8. A complete Wi-Fi update returns success only after booting and confirming the
   new K210 build ID.

The release report must also contain destructive/non-destructive 1 MiB tests as
applicable: ESP network ingress/egress, K210 RAM write/read, K210 SPI3 flash
erase/write/readback, and APP-owned SD write/read. Every row records payload
size, SHA-256, elapsed time, payload throughput, and exact error counters. ESP
SPI-flash-over-Wi-Fi is accepted only after the restarted ESP reports the
expected build ID and reconnects in STA mode.

No throughput number is accepted from a PC socket alone. Measurements are
reported for PC->ESP, ESP->K210, K210 flash, and end-to-end commit separately.
