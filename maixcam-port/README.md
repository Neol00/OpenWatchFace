# OpenWatchFace → MaixCam-Pro port

Running the OpenWatchFace firmware (`../OpenWatchFace/`) natively on a **Sipeed
MaixCam-Pro** (Sophgo SG2002 RISC-V, Linux).

## Strategy

Not bare metal, not MaixPy (MicroPython). Build a **native C++ MaixCDK app**: it links
against Sipeed's working display/touch drivers (via the `maix::` API + bundled LVGL),
owns the framebuffer directly, and is set as the device boot target instead of the
MaixPy launcher. "Rip out their UI, keep their drivers."

```
[ Linux + Sophgo MMF/VO drivers ]   <- reused as-is (the hard part Sipeed already solved)
[ MaixCDK  (maix::display/touch)  ]  <- C++ SDK, bundles LVGL + maix::lvgl_init() glue
[ OpenWatchFace firmware (ours)   ]  <- setup()/loop() on top of an Arduino shim
```

## Layout

- `MaixCDK/` — the SDK (shallow clone). Key bits:
  - `examples/gui_lvgl/` — the canonical LVGL-app template we mirror.
  - `components/3rd_party/lvgl/` — bundled LVGL **9.x** + `maix::lvgl_init(Display*, TouchScreen*)`
    which wires flush (`monitor_flush`), touch indev (`pointing_device`), tick (`time::ticks_ms`).
  - `components/vision/include/maix_{display,touchscreen,image}.hpp` — the driver API.
- `openwatch/` — our app. **Step 1** is a minimal display+touch+LVGL sanity UI
  (`main/src/main.cpp`). The firmware integrates into this same skeleton.

## Build

```sh
cd openwatch
maixcdk run                 # desktop SDL build — quickest sanity check on a PC
# or
maixcdk build -p maixcam    # cross-compile for the device, then copy the binary over
```

## Roadmap

- [x] Clone MaixCDK; confirm bundled LVGL + `lvgl_init` glue; confirm driver API.
- [x] **Step 1:** LVGL→MaixCDK render+touch path proven (`openwatch/` test UI renders + takes touch).
- [x] **Step 2 (partial):** `compat/Arduino.h` + `compat/arduino_shim.cpp` — core shim:
      `millis/micros/delay`→`maix::time`, `Serial`/`USBSerial`→stdout, GPIO/ADC/PWM no-ops, math,
      a `String` class. Grows as the build surfaces more symbols. STILL TODO in compat: stub
      headers for `Wire.h`, `SPI.h`, `esp_sleep.h`, `esp_system.h`, `Preferences.h` (→file-backed),
      `WiFi.h`, `HTTPClient.h`, `HWCDC.h`, `heap_caps`/`ps_malloc`→malloc, FreeRTOS→pthread/no-op.
- [x] **Step 3:** new board target `OpenWatchFace/board_maix_linux.h` (`BOARD_ID_MAIX`), all
      `BOARD_HAS_*` OFF; `board.h` wired with `BOARD_DISPLAY_MAIX`/`BOARD_TOUCH_MAIX`/`BOARD_PLATFORM_MAIX`.
- [x] **Step 4:** first-pass integration done:
      - `OpenWatchFace.ino`: gated the firmware-owned LVGL/display/touch bring-up (the panel-flush
        + touch-read callbacks, and the `lv_init`…`lv_indev` setup block) behind `#if !BOARD_PLATFORM_MAIX`;
        the Maix path adopts `lv_display_get_default()` from `lvgl_init`. BLE includes gated behind
        `BOARD_HAS_BLE` with `compat/ble_compat_stubs.h` standing in for the public BLE API.
      - `compat/` shims: `Arduino.h`/`arduino_shim.cpp`, `Wire.h`, `Preferences.h` (in-memory),
        `esp_sleep.h`, `esp_system.h`, `esp_timer.h`, `esp_heap_caps.h`, `esp_mac.h`, `esp32-hal-cpu.h`,
        `esp_freertos_hooks.h`, `driver/gpio.h`, `freertos/{FreeRTOS,task,semphr}.h`, `HWCDC.h`,
        `WiFi.h`/`WiFiClientSecure.h`/`HTTPClient.h` (no-op).
      - `openwatch/main/CMakeLists.txt`: compat-first include order, compiles the `.ino` as C++,
        `-DBOARD_SELECT=3`. `main.cpp` runs `lvgl_init` → `setup()` → `loop()`.
- [x] **Step 4b:** iterated the desktop build to green — **it compiles, links, and runs the full
      firmware UI under SDL** (`maixcdk run -p linux`). Added the remaining compat (ESP `clk`/`bt`/`wifi`
      stubs, `fs::File` over POSIX, `ESP` object, time helpers, gpio/rtc_io stubs, LVGL `lv_label_set_recolor`
      shim + Montserrat fonts enabled in MaixCDK's lv_conf), gated the rest of the raw-`gfx` paths, and
      compiled the icon/font `.c` arrays (stripped the LVGL-9.5-only `.static_bitmap` field).
- [x] **Step 4c:** mapped the MaixCam-Pro **User button → firmware BOOT button**: `main.cpp` polls
      `maix::peripheral::key::Key` and drives `g_owf_boot_level`, which `digitalRead(BOOT_BTN_GPIO)`
      returns — so the existing single-tap-menu / double-tap-sleep logic works unchanged.
- [ ] **Step 4d (next):** cross-compile for the device — `maixcdk build -p maixcam` — and run on hardware.
      The pinned cross-toolchain avoids the host-GCC-16 issues; watch for device-only surprises.
- [ ] **Step 5:** boot replacement — point the device init at our binary, disable the MaixPy launcher.
- [ ] **Later:** WiFi notifications (`notif_net.h`) → sockets/libcurl; BLE (`ble_*.h`) → BlueZ.

## Host build patches (Artix / GCC 16, desktop `-p linux` build only)

MaixCDK's vendored 3rd-party libs predate GCC 16; the desktop build needs these
SDK-side patches (re-apply if `MaixCDK/` is re-cloned):

- `tools/cmake/compile_flags.cmake`: C flags get `-include stdint.h -std=gnu17`,
  CXX flags get `-include cstdint`. Fixes missing-`<cstdint>` and C23 `()`→`(void)`
  conflicts across yaml-cpp / omv / zxing / etc.
- `components/3rd_party/omv/omv.hpp`: `#include <omp.h>` before the `extern "C"`
  block (GCC 16's `<omp.h>` has C++ templates that can't have C linkage).
- `components/3rd_party/opencv/CMakeLists.txt`: `REMOVE_ITEM` `opencv_viz` **and**
  `opencv_rgbd` `opencv_structured_light` `opencv_cvv`. Arch's `opencv_rgbd`/`structured_light`
  imported targets transitively re-add `opencv_viz` (→VTK, undefined refs), so removing only
  `viz` is not enough — the whole chain must go. Requires system OpenCV: `sudo pacman -S opencv`.

(The device build `-p maixcam` uses a pinned cross-toolchain and needs none of these.)

## Known wrinkles

- **Color depth:** bundled LVGL is `LV_COLOR_DEPTH==32` (BGRA8888, hard `assert`); the
  firmware is authored for 16bpp (RGB565). Plan: adopt 32bpp (LVGL widgets + 4bpp alpha
  fonts/icons are depth-agnostic); the firmware's custom RGB565 DMA flush is discarded
  (we use `monitor_flush`). Audit for explicit RGB565 canvas/image buffers.
- **Panel size:** MaixCam-Pro is 640×480 vs the S3's 410×502 — `UI_PX()`/geometry flags
  should absorb it; verify layouts.
- **`lv_conf.h`:** MaixCDK uses its own (`components/3rd_party/lvgl/conf/`), replacing the
  firmware's carefully RAM-tuned one. Fine on Linux (plenty of RAM).
