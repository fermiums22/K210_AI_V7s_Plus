# V2 bring-up test report — 2026-07-12

This is a factual intermediate report. `BLOCKED` and `NOT RUN` rows are not
release passes.

## Host protocol tests

| Test | Result |
|---|---|
| KLINK 64-byte cell, fixed vector, CRC/fault | PASS, CRC `0x8b8eb547` |
| KLINK reliable link, credits, duplicate suppression | PASS, tx=19 rx=18 duplicates=1 |
| KUPDATE v2 framing and CRC | PASS |
| streaming SHA-256, 1 MiB at 1 MiB/4096/48/1 byte chunks | PASS, SHA-256 `52dddfbe09ad4d43d025b58f1105b3471b8968c2aaf9a84c070cd0f1a7ecb706` |

## K210 hardware tests

Payload for all 1 MiB rows is the same deterministic byte pattern. Timings are
payload timings on the controller and exclude UART logging and kflash upload.

| Target | Operation | Bytes | Time | Throughput | Integrity | Result |
|---|---|---:|---:|---:|---|---|
| K210 RAM | write pattern | 1,048,576 | 59 ms | 17,355 KiB/s | byte verify | PASS |
| K210 RAM | read + verify + SHA | 1,048,576 | 278 ms | 3,683 KiB/s | SHA-256 `52dd…b706` | PASS |
| K210 SPI3 flash `0xB00000` | erase | 1,048,576 | 11,330 ms | 90 KiB/s | all 256 sectors completed | PASS |
| K210 SPI3 flash `0xB00000` | program | 1,048,576 | 2,288 ms | 447 KiB/s | page writes completed | PASS |
| K210 SPI3 flash `0xB00000` | read + byte verify + SHA | 1,048,576 | 496 ms | 2,064 KiB/s | SHA-256 `52dd…b706` | PASS |
| K210 SPI1 / KLINK physical | 64-byte cells at requested 20 MHz | qualification stream | — | 146,722 payload B/s | zero CRC/sequence/fault; actual SCK 19.5 MHz | PASS |
| K210 boot v2 | SPI3 slot A load and jump | 452,480 | 204 ms | 2,166 KiB/s | header valid, boot SHA `be63add281b76dbd42c2f20abdce2a3339ef267c7f60cae9ca75db667af37725`, APP heartbeat | PASS (factory path) |
| K210 boot v2 | pending boot/confirm/rollback | — | — | — | metadata journal state transitions | NOT RUN |

The boot-v2 ELF is 34,129 bytes of linked text/data/BSS, its flash binary is
small enough for the 1 MiB boot region, and its symbol table contains no FatFs,
SD, disk, or SPI1 symbols.

## Network, SD, and ESP update gates

| Required test | State | First blocking evidence |
|---|---|---|
| ESP STA 1 MiB receive/send | BLOCKED | ESP active scan reports `count=0`; disconnect reason 201 (`NO_AP_FOUND`) while the PC sees `Fermiums_2.4` on channel 10 at strong signal |
| Wi-Fi -> ESP -> KLINK -> inactive A/B slot -> verify -> boot | BLOCKED | updater halts at `ESP marker missing`; it does not erase a slot |
| APP-owned SD 1 MiB write/read over Wi-Fi | NOT RUN | APP-only SD service is not integrated into v2 yet; boot/update have already been separated from SD |
| ESP SPI-flash firmware over Wi-Fi | BLOCKED | requires a working STA ingress; direct K210 UART recovery with stub/MD5 is separately proven |

## Root causes fixed during this run

1. Minimal bare-metal images originally allowed both K210 harts to enter one
   `main` using the same stack. Hart 1 is now parked before BSS/stack use.
2. Bare-metal boot/selftest used newlib formatting without a complete reent/TLS
   runtime. It now uses direct UARTHS formatting with no malloc or scheduler.
3. SHA-256 now processes complete 64-byte input blocks directly and retains
   only the partial tail, validated across four chunk sizes.
4. An updater cannot overwrite the confirmed fallback while metadata contains
   a pending slot; it rejects that state and selects `confirmed_slot ^ 1`.
