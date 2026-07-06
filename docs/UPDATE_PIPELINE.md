# K210/ESP update pipeline baseline

Goal: the robot must be updateable without opening it.  Upload transport must not matter: UART KSD, Wi-Fi TCP/HTTP, or any future transport can place files on the K210 SD card.  The actual flashing step is triggered by a small JSON job file stored on the same SD card.

## Rule 1: transport only stages files

A transport is only allowed to copy bytes to SD and write a pending job manifest.  It must not directly flash ESP or K210 while the connection is still streaming the payload.

Allowed staging paths:

```text
/fs/0/update/inbox/<upload-id>/...
/fs/0/update/pending.json
/fs/0/update/state.json
```

The same file set can be uploaded by UART today and by Wi-Fi later.

## Rule 2: pending job is written atomically

Never write `/fs/0/update/pending.json` directly.  Write temporary file first:

```text
/fs/0/update/pending.tmp
```

Then rename to:

```text
/fs/0/update/pending.json
```

The flashing code must immediately copy/disarm `pending.json` to avoid infinite reflash loops after reboot.

## Rule 3: manifest is the only command source

Example for ESP update:

```json
{
  "schema": 1,
  "job_id": "20260706-esp8285-hello",
  "once": true,
  "actions": [
    {
      "target": "esp8285",
      "op": "flash",
      "baud": 921600,
      "block": 1024,
      "reset_after": true,
      "parts": [
        { "file": "esp_boot.bin", "offset": "0x00000000", "sha256": "..." },
        { "file": "esp_part.bin", "offset": "0x00008000", "sha256": "..." },
        { "file": "esp_app.bin",  "offset": "0x00010000", "sha256": "..." },
        { "file": "wifi_config.json", "offset": "0x000e0000", "sha256": "..." }
      ]
    }
  ]
}
```

Legacy `/fs/0/flash.json` may stay supported, but new code should move toward `/fs/0/update/pending.json`.

## Rule 4: verify before flashing

Before touching ESP flash:

1. Check that every referenced file exists.
2. Check size is non-zero and within target limits.
3. Check SHA-256 if present.
4. Disarm the job.
5. Flash.
6. Reset ESP.
7. Wait for boot log / Wi-Fi-ready marker.
8. Write `/fs/0/update/state.json` with OK/FAIL result.

## ESP update state machine

```text
IDLE
  -> STAGED        files are present on SD
  -> VERIFIED      manifest and hashes accepted
  -> DISARMED      pending job disabled/renamed before flashing
  -> FLASHING      ESP bootloader/stub active
  -> RESETTING     ESP reset to normal boot
  -> BOOT_CHECK    wait for valid ESP boot/application marker
  -> OK or FAIL
```

If UART/Wi-Fi connection drops after STAGED, the robot can still run the job later because files are already on SD.

## K210 self-update requirement

K210 update over Wi-Fi must not be implemented as blind overwrite from the running app.  That can brick the robot on power loss.

Required safe design:

```text
K210 bootloader / updater stub
slot A: current app
slot B: candidate app
state record: active slot, candidate slot, boot counter, confirmed flag
```

Flow:

1. Upload new K210 image to SD or inactive flash slot.
2. Verify SHA-256/signature.
3. Mark candidate slot.
4. Reboot.
5. Bootloader starts candidate.
6. App must confirm healthy boot.
7. If no confirm after N boots, bootloader rolls back to previous slot.

Until this bootloader exists, Wi-Fi can stage K210 firmware files on SD, but final K210 self-flash should stay disabled or treated as unsafe.

## Practical next milestones

1. Keep current UART KSD staging working.
2. Rename current ESP `/fs/0/flash.json` flow into generic `/fs/0/update/pending.json` while preserving backward compatibility.
3. Add SHA-256 verification for staged ESP files.
4. Add Wi-Fi file upload endpoint on ESP that forwards/stages files to K210 SD through existing SPI/console protocol.
5. Add `APPLY_UPDATE` command that runs pending SD job without requiring the PC script to stay connected.
6. Design K210 bootloader with A/B slots before enabling K210 firmware update over Wi-Fi.
