# Library patches

This firmware needs **5 small modifications** to three third-party libraries. They live
outside the sketch (in your Arduino `libraries/` folder and the ESP32 core), so a fresh
install won't have them — you must apply these patches once, or the build will fail /
the watch will misbehave (crash on BLE toggle, render on one core, no screen cache).

All changes are clearly marked in-source with a `LOCAL PATCH (ESP32-S3-WatchFace)`
comment, and each is GPL-compatible (the upstream licenses are MIT / Apache-2.0).

## What each patch does

| # | Patch | Library (version) | License | Effect |
|---|-------|-------------------|---------|--------|
| 01 | `01-lvgl-freertos-corepin.patch` | LVGL **9.5.0** | MIT | Pins LVGL's SW render thread to core 0 so the 2nd draw unit uses the idle core (dual-core rendering). |
| 02 | `02-lv_conf-snapshot.patch` | LVGL **9.5.0** config | MIT | Enables `LV_USE_SNAPSHOT` so the PSRAM screen cache can pre-render screens. |
| 03 | `03-gfx-qspi-dma.patch` | Arduino_GFX **1.6.5** | MIT | Pipelined (async-DMA) QSPI flush: overlaps pixel-format conversion with transmission for higher FPS. |
| 04 | `04-gfx-qspi-header.patch` | Arduino_GFX **1.6.5** | MIT | Adds the second SPI transaction struct the pipelined flush needs. |
| 05 | `05-esp32-ble-gap.patch` | ESP32 Arduino core **3.3.8** | Apache-2.0 | Unregisters the custom GAP event listener in `BLEDevice::deinit()` — fixes a crash when BLE is toggled off then on again. |

> **Versions matter.** These patches were generated against the exact versions above.
> If your libraries differ, a patch may not apply cleanly — install the matching
> versions (LVGL 9.5.0, Arduino_GFX 1.6.5, ESP32 core 3.3.8), or apply the change by
> hand using the `LOCAL PATCH` markers as a guide.

## Where the files live

- **LVGL** (`lvgl/`) and its `lv_conf.h` — in your Arduino `libraries/` folder
  (typically `Documents/Arduino/libraries/`). `lv_conf.h` sits **next to** the `lvgl`
  folder, not inside it.
- **Arduino_GFX** (`GFX_Library_for_Arduino/`) — same `libraries/` folder.
- **ESP32 core BLE** — inside the installed core, e.g.
  `…/Arduino15/packages/esp32/hardware/esp32/3.3.8/libraries/BLE/`.

## How to apply

### Automatic (recommended)

From this `patches/` directory, run the script for your OS and point it at your
Arduino `libraries/` folder and your ESP32 core folder:

**Windows (PowerShell):**
```powershell
./apply_patches.ps1 -LibrariesDir "$HOME\Documents\Arduino\libraries" `
                    -Esp32CoreDir "$env:LOCALAPPDATA\Arduino15\packages\esp32\hardware\esp32\3.3.8"
```

**Linux / macOS (bash, needs `patch`):**
```bash
./apply_patches.sh ~/Arduino/libraries \
  ~/.arduino15/packages/esp32/hardware/esp32/3.3.8
```

Both scripts **dry-run first** and refuse to apply if any patch wouldn't apply cleanly,
so they won't half-patch your tree. They're also idempotent-aware: if a patch is already
applied they tell you and skip it.

### Manual

From each library's root, apply with `patch -p1`:
```bash
cd <libraries>/lvgl                       && patch -p1 < 01-lvgl-freertos-corepin.patch
cd <libraries>                            && patch -p1 < 02-lv_conf-snapshot.patch   # lv_conf.h is here
cd <libraries>/GFX_Library_for_Arduino    && patch -p1 < 03-gfx-qspi-dma.patch
cd <libraries>/GFX_Library_for_Arduino    && patch -p1 < 04-gfx-qspi-header.patch
cd <esp32-core>/libraries/BLE             && patch -p1 < 05-esp32-ble-gap.patch
```

## After applying

Clear the Arduino build cache so the patched libraries recompile, then rebuild:
- **Windows:** delete `%LOCALAPPDATA%\arduino\sketches\*`
- **Linux/macOS:** delete `~/.cache/arduino/sketches/*` (path varies by IDE version)
