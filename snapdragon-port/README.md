# snapdragon-port — OpenWatchFace bare-metal on Qualcomm smartwatches

The OpenWatchFace firmware (`../OpenWatchFace/`) running **bare-metal on the
application cores** of Qualcomm-based smartwatches: a FreeRTOS runtime, MSM
drivers ported from the vendor kernels, a thin Arduino-compatible layer, and the
unchanged firmware on top. No Linux, no Wear OS. The stock bootloader (`aboot`)
loads our image as the "kernel"; the `boot` partition is the only thing ever
written.

| Watch | SoC | State |
|---|---|---|
| Fossil Gen 4 (firefish / ray) | Snapdragon Wear 2100, APQ8009W | **daily**: display, touch, crown, storage, WiFi, BLE, OTA, dual core, ~6 mA deep sleep |
| Mobvoi TicWatch C2 / C2+ (skipjack) | Snapdragon Wear 2100, APQ8009W | **daily**: same feature set (no crown, one pusher) |
| Mobvoi TicWatch S2 / E2 (tunny) | Snapdragon Wear 2100, APQ8009W | same board as the C2 with a 400×400 panel; image built, less tested |
| Fossil Gen 6 (hoki) | Snapdragon Wear 4100+, SDA429W | boots and runs the UI from the splash framebuffer; WiFi blocked by TrustZone (`pas_init_image` -13), BLE on the WCN3990 UART unfinished |

**Start with the install guides** in [`../docs/devices/`](../docs/devices/):
[Gen 4](../docs/devices/fossil-gen4.md), [C2](../docs/devices/ticwatch-c2.md),
[S2](../docs/devices/ticwatch-s2.md), [Gen 6](../docs/devices/fossil-gen6.md).
They carry the host tools, the flashing steps, every build flag and the
troubleshooting tables.

## Layout

| Path | What |
|---|---|
| `baremetal/` | the port: `platform/` (runtime + drivers, shared by all boards), `boards/<watch>.h` (addresses and per-device facts), `compat/` (Arduino/ESP-IDF surface the firmware expects), `rtos/`, `lwip_port/`, `nimble_port/`, `third_party/` |
| `baremetal/build-owf-image-<board>.sh` | compile + link the firmware for one board; `tools/mk-bootimg*.sh` packs the result into an Android boot image with the right DTB |
| `dtbs/` | every device tree the build or the docs use, dumped from the watches ([README](dtbs/README.md)) |
| `twrp/` | the TWRP recovery images used to back up the stock partitions before touching a watch |
| `notes/` | working findings; `C2PLUS-FINDINGS.md` is the record of how the deep sleep was brought to parity with stock, read on a rooted C2+ |
| `BUILD-GEN4.md`, `BUILD-GEN6.md` | build references: flags, the watchdog staircase, memory maps, recovery |
| `HARDWARE.md`, `HARDWARE-GEN6.md` | verified hardware and boot facts per SoC (panel, buses, PMIC, boot.img parameters) |

Not in the repository, by design: device dumps, stock firmware partitions and
the vendor kernel checkouts (`dumps/`, `firmware/`, `kernels/`), all multi-GB
and obtainable from the watches or their upstreams.

## How the Wear 2100 port works, in one paragraph

Startup, MMU, GIC and the arch timer bring FreeRTOS up on core 0; core 1 is
cold-booted through TrustZone for frame pushes. The display is aboot's own MDP3
DMA_P pipe, taken over rather than re-initialised, so the boot splash flows into
the UI. Touch, the crown, the charger and the sensors are direct I²C/SPI on the
buses the vendor kernel used. The eMMC holds NVS, FFat and the log inside
`userdata`. WiFi and BLE are the WCNSS subsystem: firmware loaded through
TrustZone's PAS with the watch's NV blob, a wcn36xx HAL, a WPA2 STA on lwIP,
NimBLE over HCI-on-SMD. Deep sleep is the vendor kernel's `l2-pc` level done by
hand: the SAW sequencers programmed as the kernel programs them, the cluster
collapsed through TERMINATE_PC, the RPM sleep set applied with the crystal
released, wake on the PMIC button or RTC alarm. Updates come from GitHub
releases over HTTPS and are written to `boot` by the watch itself.

## Building (short form)

```sh
cd snapdragon-port/baremetal
export LVGL_DIR=$PWD/../../libraries/lvgl
F="-DWDOG_TRACE -DSLEEP_NO_WDOG -DSYS_PC_8909 -DSYS_PC_STAGE=6 -DL2_SAW_AP_ENABLE -DSYS_PC_XO_SHUTDOWN"
CFLAGS_EXTRA="$F" sh build-owf-image-c2.sh   && sh tools/mk-bootimg-c2.sh build/c2-owf/owf.bin
CFLAGS_EXTRA="$F" sh build-owf-image-s2.sh   && sh tools/mk-bootimg-s2.sh build/s2-owf/owf.bin
CFLAGS_EXTRA="$F" sh build-owf-image-gen4.sh && sh tools/mk-bootimg.sh build/gen4-owf/owf.bin ../dtbs/firefish-stock.dtb
```

`fastboot boot build/<board>/owf-boot.img` runs it from RAM without writing
anything (Wear 2100 only; the Gen 6 has to be flashed). The flags are explained
in the install guides; `-DWDOG_TRACE` alone boots but sleeps at ~45 mA.
