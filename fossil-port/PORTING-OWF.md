# Porting OpenWatchFace onto the Fossil bare-metal runtime

Goal: compile, link, and run the real `OpenWatchFace/` firmware bare-metal on the
Fossil Gen 6 (hoki), production-grade. Approach modeled on the **Tuya T5 port**
(also a non-ESP ARM target): reuse `ArduinoCore-API`, add thin platform glue, and
stub everything above it — "stub first → compile → boot → fill in drivers."

Derived from an exhaustive 3-part API-surface audit of `OpenWatchFace/` (Arduino
core / ESP-IDF+RTOS+memory / third-party libs), 2026-08-02.

## What the board flags already neutralize (compile to stubs)

`board_fossil_gen6.h` sets `BOARD_PLATFORM_FOSSIL=1` and every capability OFF, so
these do NOT need real code for Milestone 1 — the firmware's own `#if` guards drop
them: **BLE** (NimBLE ANCS/AMS/NUS → `ble_compat_stubs.h`), **WiFi/HTTP/TLS**,
**camera + esp_jpeg**, **ULP/LP step blobs**, **audio (ES8311/I2S)**, **SD_MMC/FFat
storage**, **PSRAM tiers**, **PMU (AXP2101)**, **RTC (PCF85063)**, **IMU (QMI8658)**.
These are later milestones, driver by driver.

## The compat layer (in `fossil-port/baremetal/compat/`)

Tier 1 — Arduino base (mandatory, mostly reused):
- `arduino-api/` — VENDORED `ArduinoCore-API` (String, Print, Stream, IPAddress…). Done.
- `Arduino.h` + `arduino_glue.cpp` — `millis/micros/delay/delayMicroseconds` → `timer.c`;
  `pinMode/digitalWrite/digitalRead/analogWrite/ledcAttach/ledcWrite` → GPIO stubs
  (no GPIO driver yet — safe no-ops that log); `HIGH/LOW/INPUT/OUTPUT/INPUT_PULLUP`;
  `boolean`/`byte` typedefs. Modeled on TuyaOpen `cores/tuya_open/wiring*.cpp`.
- `HWCDC.h` + `Serial` — a `Print` subclass writing to `con_puts`/`uart_msm`.
- `Wire.h` + `TwoWire` — I2C. STUB first (endTransmission→fail), real driver = `msm_i2c.c` later.

Tier 2 — ESP/IDF shims referenced regardless of feature flags (mandatory):
- `esp_system.h` — `esp_reset_reason()` (return cold-boot), `esp_restart()` → `reboot_now()`.
- `RTC_DATA_ATTR` / `RTC_NOINIT_ATTR` — map to a `.noinit`/plain section (no deep sleep
  yet, so no true retention needed for Milestone 1; state simply resets).
- `esp_heap_caps.h` — `heap_caps_*` → newlib malloc; keep SPIRAM vs INTERNAL as the
  same pool for now but distinguishable API (adapt `tuya/compat/esp_heap_caps.h`).
- `esp_timer.h` — `esp_timer_get_time()` → `timer_ticks()` µs.
- `Preferences.h` — settings_store.h needs it heavily. Back with an in-RAM KV first
  (adapt `tuya/compat/Preferences.h`), persist to eMMC later.
- time: `settimeofday/gettimeofday/localtime_r/mktime/strftime` come from newlib;
  `configTzTime` → stub (no SNTP without WiFi).
- `esp_sleep.h`, `driver/gpio.h`, `driver/rtc_io.h`, `esp_clk_stubs.h`, `esp_mac.h`,
  `esp_bt.h`, `esp_wifi.h`, `FS.h`/`FFat.h` — adapt the Tuya stub versions.

Tier 3 — the runtime integration (`ArduinoMain`):
- Provide `setup()`/`loop()` call harness as a FreeRTOS task in the fossil-port runtime
  (replaces `ui_demo.c`), with LVGL tick → `millis`, flush_cb → `fb_splash`/`fb_flush`,
  indev read_cb → touch stub.

## Milestones
1. **Compiles + links** for `BOARD_ID_FOSSIL_GEN6` with all of the above stubbed. ✔ done
2. `setup()`/`loop()` run in the runtime; real UI renders into the splash framebuffer. ✔ done
3. Real drivers in order: I2C/Wire ✔ → touch ✔ (works; tap-hold timing under
   investigation) → RTC/time ✔ → haptics ✔ → eMMC storage + FFat ✔ (mounts rw) →
   PMU/battery ✔ → USB device mode ✔ (CDC-ACM log console; see BUILD-GEN6.md) →
   IMU/steps ✖ (behind the ADSP sensor hub) → BLE ✖ / WiFi ✖ (behind WCNSS).
4. Shippable image on the **boot** partition as the permanent OS.

> **Subsystem-blocked, not merely unported:** the IMU/step counter sits behind
> the ADSP sensor core, BLE/WiFi behind WCNSS, and the **rotating crown** behind
> the BG co-processor (`qcom,bg-rsb` → glink channel `RSB_CTRL` over BGCOM/SPI).
> The crown's *push* is a plain GPIO and works; its *rotation* is not AP-readable
> without bringing up a second processor. See `HARDWARE-GEN6.md`.

## Building

**See [`BUILD-GEN6.md`](BUILD-GEN6.md)** for the canonical build + pack commands,
image verification, and a reference for every build flag in the tree.

The short version — and note that two of these flags are load-bearing, not
diagnostics:

```sh
cd fossil-port/baremetal
CFLAGS_EXTRA="-DWDOG_TRACE -DDISPLAY_BISECT -DPLAT_I2C_RETEST -DTOUCH_DIAG" \
LVGL_DIR=$HOME/Arduino/libraries/lvgl sh build-owf-image.sh
sh tools/mk-bootimg-gen6.sh build/gen6-owf/owf.bin ../sda429-hoki.dtb
```

- `-DWDOG_TRACE` is the only thing that compiles `wdog_pet()` into the main
  loop. Omit it and the watch reboots ~30 s into every boot.
- `-DPLAT_I2C_RETEST` — without it `boards/fossil_gen6.h` sets
  `PLAT_I2C_DISABLED 1` and touch is completely dead.
- The DTB argument to the packer is **optional and defaults to none**. Omitting
  it silently produces a ~232 KB-smaller image that will not run correctly.

Do **not** ship `-DSTORAGE_STAIRS` / `-DSTORAGE_DIAG`: they are the storage
colour ladder, and `STORAGE_STAIRS` additionally makes `storage_init()` inert.

## Toolchain notes
- C++ needs: `new`/`delete`, `std::shared_ptr`/`unique_ptr`/`make_shared`, virtual dtors.
  NO exceptions, NO RTTI, NO `std::function`, NO STL containers, NO PROGMEM/`F()`.
  Build with `-fno-exceptions -fno-rtti -fno-use-cxa-atexit`; verify libstdc++ shared_ptr
  (atomic refcount) links on single-core bare-metal.
- Build `.ino` as C++ (`-x c++`), one big TU, same `-mcpu=cortex-a7` flags as the runtime.
