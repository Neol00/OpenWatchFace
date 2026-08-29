# Supported devices

One page per ported device. Each page is a **complete, self-contained install
guide** for that device. Libraries, patches, board/partition selection, build
settings, tunable options, and flashing, so you only need to read the page for
the hardware you actually own.

| Device | SoC | Display | Page |
|---|---|---|---|
| Waveshare ESP32-S3-Touch-AMOLED-2.06 | ESP32-S3R8 | 410×502 CO5300 AMOLED (QSPI) | [Install guide](s3-touch-amoled-2.06.md) |
| Waveshare ESP32-S3-Touch-AMOLED-1.8 | ESP32-S3R8 | 368×448 SH8601 AMOLED (QSPI) | [Install guide](s3-touch-amoled-1.8.md) |
| Waveshare ESP32-S3-Touch-AMOLED-1.64 | ESP32-S3R8 | 280×456 SH8601 AMOLED (QSPI) | [Install guide](s3-touch-amoled-1.64.md) |
| Waveshare ESP32-C6-Touch-LCD-1.47 | ESP32-C6 | 172×320 JD9853 LCD (SPI) | [Install guide](c6-touch-lcd-1.47.md) |
| Waveshare ESP32-S3-Touch-LCD-1.47 | ESP32-S3 | 172×320 JD9853 LCD (SPI) | [Install guide](s3-touch-lcd-1.47.md) |
| Waveshare ESP32-S3-Touch-LCD-2 | ESP32-S3 | 240×320 ST7789T3 LCD (SPI) | [Install guide](s3-touch-lcd-2.md) |
| Waveshare ESP32-S3-Touch-LCD-1.69 | ESP32-S3 | 240×280 ST7789V2 LCD (SPI) | [Install guide](s3-touch-lcd-1.69.md) |
| Waveshare T5-E1-Touch-AMOLED-1.75 | Tuya T5-E1 | 466×466 CO5300 AMOLED (QSPI) | [Install guide](tuya-t5-amoled-1.75.md) |
| Sipeed MaixCam-Pro | SG2002 (Linux) | MaixCDK-owned panel | [Install guide](maixcam-pro.md) |
| Fossil Gen 6 (hoki) | Snapdragon Wear 4100 (SDA429W) | 416×416 AMOLED (MSM DSI) | [Install guide](fossil-gen6.md) |
| Fossil Gen 4 (firefish/ray) | Snapdragon Wear 2100 (APQ8009W) | 454×454 AMOLED (MSM DSI) | [Install guide](fossil-gen4.md) |
| Mobvoi TicWatch C2 / C2+ (skipjack) | Snapdragon Wear 2100 (APQ8009W) | 360×360 round AMOLED (MSM DSI) | [Install guide](ticwatch-c2.md) |

## Which toolchain does my device use?

The install flow differs by platform, and it is worth knowing which one you are
in before you start:

- **Arduino IDE + ESP32 core** the five ESP32 boards (S3-2.06, S3-1.8, S3-1.64,
  C6-1.47, S3-1.47). Bundled libraries, out-of-tree patches, `board.h` + partition
  selection, then Verify/Upload.
- **Arduino IDE + TuyaOpen core** the Tuya T5-E1. Still the Arduino IDE, but a
  different board package (installed via a boards-manager URL), a prebuilt
  `libtuyaos.a` swap, and the `tyutool` flasher instead of the ESP32 toolchain.
- **MaixCDK** the MaixCam-Pro. A native Linux application, not firmware.
- **Bare-metal** Fossil Gen 6, Fossil Gen 4 and TicWatch C2. A custom FreeRTOS
  runtime built by a shell script and run with `fastboot` as an Android boot
  image, no Arduino IDE involved. All three have full install guides. The two
  Wear 2100 watches (Gen 4, C2) also support `fastboot boot`, so the firmware can
  be **RAM-booted without overwriting Wear OS**; the Gen 6 has to be flashed.

## Shared reference

These apply to every device and are documented once, centrally:

- [Library patches](../../patches/README.md) what each out-of-tree patch changes and why
- [Repo layout, architecture, apps, deep sleep](../../README.md) the main README
