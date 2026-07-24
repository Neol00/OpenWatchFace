# Supported devices

One page per ported device. Each page is a **complete, self-contained install
guide** for that device — libraries, patches, board/partition selection, build
settings, tunable options, and flashing — so you only need to read the page for
the hardware you actually own.

| Device | SoC | Display | Page |
|---|---|---|---|
| Waveshare ESP32-S3-Touch-AMOLED-2.06 | ESP32-S3R8 | 410×502 CO5300 AMOLED (QSPI) | [Install guide](s3-touch-amoled-2.06.md) |
| Waveshare ESP32-S3-Touch-AMOLED-1.8 | ESP32-S3R8 | 368×448 SH8601 AMOLED (QSPI) | [Install guide](s3-touch-amoled-1.8.md) |
| Waveshare ESP32-C6-Touch-LCD-1.47 | ESP32-C6 | 172×320 JD9853 LCD (SPI) | [Install guide](c6-touch-lcd-1.47.md) |
| Waveshare ESP32-S3-Touch-LCD-1.47 | ESP32-S3 | 172×320 JD9853 LCD (SPI) | [Install guide](s3-touch-lcd-1.47.md) |
| Waveshare T5-E1-Touch-AMOLED-1.75 | Tuya T5-E1 | 466×466 CO5300 AMOLED (QSPI) | [Install guide](tuya-t5-amoled-1.75.md) |
| Sipeed MaixCam-Pro | SG2002 (Linux) | MaixCDK-owned panel | [Install guide](maixcam-pro.md) |
| Fossil Gen 4 (firefish/ray) | Snapdragon Wear 2100 | 390×390 AMOLED (MSM DSI) | [Install guide](fossil-gen4.md) |

## Which toolchain does my device use?

The install flow differs by platform, and it is worth knowing which one you are
in before you start:

- **Arduino IDE + ESP32 core** — the four ESP32 boards (S3-2.06, S3-1.8, C6-1.47,
  S3-1.47). Bundled libraries, out-of-tree patches, `board.h` + partition selection,
  then Verify/Upload.
- **Arduino IDE + TuyaOpen core** — the Tuya T5-E1. Still the Arduino IDE, but a
  different board package (installed via a boards-manager URL), a prebuilt
  `libtuyaos.a` swap, and the `tyutool` flasher instead of the ESP32 toolchain.
- **MaixCDK** — the MaixCam-Pro. A native Linux application, not firmware.
- **Bare-metal** — Fossil Gen 4. Custom runtime; see its page for status.

## Shared reference

These apply to every device and are documented once, centrally:

- [Library patches](../../patches/README.md) — what each out-of-tree patch changes and why
- [Repo layout, architecture, apps, deep sleep](../../README.md) — the main README
