# Fossil Gen 6 (hoki) — verified hardware & boot facts

Collected 2026-08-01 **from public sources only** (AsteroidOS `meta-hoki` +
the Fossil-released GPL kernel). Device is in hand but **nothing here has been
confirmed against it yet** — every entry below came from the decompiled DTB
that AsteroidOS already recovered and published.

> **Headline:** unlike the Gen 4, the Gen 6's device tree did NOT need to be
> dumped by us. AsteroidOS already dumped it, decompiled it, and shipped it as
> a kernel patch. A verbatim copy is checked in at
> [`sda429-hoki-decompiled.dts`](sda429-hoki-decompiled.dts) (11,779 lines,
> fully flattened — every node inline, no `#include`s to chase).

## Sources

```
kernel:  https://github.com/fossil-engineering/kernel-msm-fossil-cw
branch:  fossil-android-msm-hoki-lw1.2-4.14
SRCREV:  c0b4c201f2d5a641defe19958a9b4c16f40d866b
DTS:     AsteroidOS meta-smartwatch/meta-hoki/recipes-kernel/linux/files/
         0001-dts-Add-hoki-device-trees.patch
         -> arch/arm/boot/dts/sda429-hoki/sda429-hoki.dts
```

Fossil released these sources only after a ~4-month GPL request escalated by the
AsteroidOS community — do NOT assume a future Fossil target will be as easy.

## SoC / platform

- **Snapdragon Wear 4100+**, part **SDA429W** (`qcom,sdm429w-qrd`), 12nm.
- `model = "Fossil Hoki based on SDA429 BG WTP"`
- `qcom,msm-id = <0x1b5 0x00>` (437), `qcom,board-id = <0x10b 0x0a>`
- **Quad Cortex-A53.** A53 is ARMv8, but see the mode note below — this is
  **NOT** an AArch64 port.
- Kernel 4.14 (vs the Gen 4's 3.18).

### CRITICAL: the Gen 6 runs in AArch32, not AArch64

The earlier project assumption ("Gen 6 is 64-bit A53 — needs an AArch64 startup
variant") is **wrong**, and this is the single most consequential correction here:

- The device tree is added under **`arch/arm/boot/dts/`**, not `arch/arm64/`.
- AsteroidOS `hoki.conf` tunes for `cortexa7` / `DEFAULTTUNE = "armv7vehf-neon"`
  — the exact same tune as the Gen 4.
- `boot.img base_addr = 0x80000000` with a gzip `zImage` (32-bit ARM format).

So the stock bootloader boots a **32-bit ARM kernel** on the A53 cluster. The
existing ARMv7 `startup.S`, short-descriptor MMU, GICv2 driver and the FreeRTOS
`GCC/ARM_CA9` port all carry over **unchanged in architecture**. This removes
what was assumed to be the largest single blocker for the Gen 6.

(The A53 runs AArch32 at EL1 perfectly well; ARMv8-A's AArch32 state is
ARMv7-A-compatible. NEON/VFP is present.)

## boot.img parameters (from meta-hoki `img_info`)

```
page_size      = 4096          # Gen 4 was 2048
base_addr      = 0x80000000    # Gen 4 was 0x00000000  <-- link address differs!
kernel_offset  = 0x00008000
ramdisk_offset = 0x01000000
tags_offset    = 0x0000100
format         = gzip
board          = "hoki"
```

**`base_addr` is the big one:** DRAM starts at 0x80000000 here vs 0x0 on the
Gen 4, so `platform/linker.ld` and the self-relocation path need a per-board
load address. The Phase 1 self-relocating startup already handles running from
a foreign address, so this should be a linker-script constant, not new code.

Stock cmdline:
```
console=ttyMSM0,115200,n8 androidboot.console=ttyMSM0
androidboot.selinux=permissive androidboot.hardware=hoki user_debug=30
msm_rtb.filter=0x237 ehci-hcd.park=3 androidboot.bootdevice=7824900.sdhci
earlycon=msm_serial_dm,0x78b0000 vmalloc=300M androidboot.usbconfigfs=true
lpm_levels.sleep_disabled=1 ...
```

- **Debug UART @ 0x78b0000** (`serial@78b0000`, `qcom,msm-uartdm-v1.4`,
  alias `blsp1_uart2`). Gen 4's was 0x78af000. Same UARTDM v1.4 IP → the
  existing `uart_msm.c` should work with only the base address changed.

## Hardware access — MUCH easier than the Gen 4

- **A full 4-pin USB connection is exposed on the charger pads.** No DIY
  pogo-pin cable, no disassembly. This was the Gen 4's Phase 0 gate.
- **Bootloader unlocks normally:** `adb reboot bootloader` (or a button combo)
  then `fastboot oem unlock`. Wipes the watch; that is fine.
- **Do NOT relock the bootloader** once non-stock software is installed.
- AsteroidOS only ever flashes `boot` and `userdata` — same blast radius as our
  Gen 4 plan. **The dev loop is `fastboot flash boot`, NOT `fastboot boot`:**
  RAM-boot does not work on this device (partial transfer, never runs).
  Confirmed 2026-08-07. See BUILD-GEN6.md "Booting, flashing and recovery".

## Peripherals (from the decompiled DTB — addresses are verbatim)

| Block | Fact | vs Gen 4 |
|---|---|---|
| Display ctrl | **MDSS / MDP5** (`qcom,mdss_mdp@1a00000`, reg offset 0x1000, VIG/RGB/DMA/cursor pipes, 2 mixers, 2 pingpongs) | **DIFFERENT** — Gen 4 is MDP3. `msm_mdp3.c` does **not** carry over. |
| DSI host | `mdss_dsi_ctrl0@1a94000` (ctrl 0x1a94000+0x300, phy 0x1a94400+0x400, misc 0x193e000) | Different base; same DSI host IP family — `msm_dsi.c` sequences largely reusable, values are not. |
| DSI PHY/PLL | **`qcom,mdss_dsi_pll_12nm`** @0x1a94400 | **DIFFERENT** — Gen 4 is a 28nm PHY. PHY init is a rewrite. |
| Panel | **NOT IN THIS DTB.** Only `qcom,mdss_wb_panel` (writeback, 640×640 @24bpp) is present. | Same situation as Gen 4 — the real panel node must come from the device. |
| Touch | **NOT IN THIS DTB.** No touch node on any I2C bus. | Must be dumped from the device. |
| I2C | 8× `qcom,i2c-msm-v2`: BLSP1 @0x78b5000/6000/7000/8000, BLSP2 @0x7af5000/6000/7000/8000 | Same **i2c-msm-v2** IP → `msm_i2c.c` carries over with new bases. |
| NFC | `nq@28` on `i2c@7af5000` (the only populated I2C child in the DTB) | — |
| eMMC | `sdhci@7824900` (boot device), `sdhci@7864900` | Same SDHCI-MSM family. |
| PMIC | **PM660** over SPMI @0x200f000 (`qcom,pm660@0` / `@1`) | **DIFFERENT** — Gen 4 is PM8916-class. |
| RTC | `qcom,pm660_rtc` — rw@6000, alarm@6100 | Phase 5 target; alarm reg present. |
| Haptics | `qcom,pm660-haptics` | Gen 4 used a plain motor GPIO. |
| Buttons | `qcom,qpnp-power-on` @0x800 — kpdpwr + resin | Same qpnp-pon as Gen 4. |
| Radio | wcnss subsystem present in SMEM table (`wcnss@3`) | Same WCNSS story; Phase 7 either way. |

### The two gaps that DO need the device

The published DTB is missing exactly the two nodes we most need:

1. **Panel** — no `dsi-panel-*` node, no command tables, no resolution. The
   `mdss_wb_panel` 640×640 is the *writeback* buffer, which is a plausible hint
   at panel size but is **not** the panel spec. (Gen 6 is widely reported as a
   1.28" 416×416 AMOLED — treat 640×640 as unconfirmed until dumped.)
2. **Touch controller** — no touch node at all; vendor, I2C bus and address all
   unknown.

Both are almost certainly in a **dtbo overlay** (4.14-era devices split base DTB
+ `dtbo` overlays; the Gen 4's 3.18 tree did not do this). So the device-side
dump must grab **`dtbo` as well as `boot`**, and the highest-value single
artifact is `/proc/device-tree` from the running stock OS or AsteroidOS, which
is the *already-merged* tree.

## What carries over from the Gen 4 port

| Reusable as-is (arch) | Needs new values | Needs a rewrite |
|---|---|---|
| `startup.S`, MMU, GICv2, timer | `linker.ld` base 0x80000000 | `msm_mdp3.c` → MDP5 |
| FreeRTOS ARM_CA9 port, newlib, LVGL | `uart_msm.c` base 0x78b0000 | DSI **PHY** 28nm → 12nm |
| `msm_i2c.c` (i2c-msm-v2 IP) | I2C bus bases | `touch_raydium.c` (vendor unknown) |
| `reboot_msm.c`, `ramlog.c` | GIC priority bits (check GIC-400 vs -500) | PMIC: PM8916 → PM660 |

## Port status (2026-08-01)

The `gen6` target BUILDS. What is implemented:

| Area | State | Notes |
|---|---|---|
| Startup / MMU / GIC / timer | **reused unchanged** | GIC-400 @0xb000000 and the 19.2 MHz timer are byte-identical to the Gen 4 |
| UART (UARTDM v1.4) | **reused, new base** | 0x78b0000; it is the stock earlycon so aboot leaves its clocks on |
| Reboot + dead-man recovery | **reused unchanged** | `restart@4ab000` reg and imem `restart_reason@65c` verified identical to the Gen 4 |
| Vibration (sign of life) | **new driver** | PM660 haptics, SPMI **slave id 1** — a different block from the Gen 4's PM8916 vibrator; ALSO wired into the firmware's haptics engine (virtual pin 200) |
| Display | **new: splash reuse** | `fb_splash.c` — writes into aboot's live framebuffer; no DSI/PHY/panel code |
| SPMI reads | **implemented** | `spmi_arb.c` — obsrvr region 0x2c00000, 1-8 byte read/write, bounded |
| RTC / wall clock | **implemented** | `pmic_rtc.c` — pm8941-class map @0x6000 sid 0; seeds gettimeofday, best-effort write-back (verify-after-write catches secure lockdown) |
| Battery / charger | **implemented** | `pmic_fg.c` — PM660 FG-GEN3 MSOC/VBATT/IBATT/temp + SMB2 charging/USB status; wired into board_power.h |
| Buttons | **implemented** | `pmic_pon.c` — qpnp-pon RT_STS: power/crown = firmware BOOT button (virtual pin 201), resin = 202 |
| I2C | **implemented (bus level)** | `msm_i2c.c` multi-bus + real buffered `TwoWire`; all 8 QUP bases in the board header; no known peripheral to talk to until the dtbo dump |
| eMMC | **read-only implemented** | `sdhci_msm.c` — reuses aboot's controller state, CMD17 polled PIO + GPT partition lookup; NO write path by design until tested |
| Touch | **implemented (bus auto-probed)** | **Raydium U128BLA03** — identified from the AsteroidOS hoki defconfig (`CONFIG_TOUCHSCREEN_RAYDIUM_U128BLA03_CHIPSET=y`), no dtbo needed for the part. `touch_raydium.c` now serves both gens (wt030 deltas: PAGE reg 0x0A, seq ack); the bus is probed across all 8 QUPs at init (addr 0x39/0x5A). Wired into the firmware's LVGL indev. |
| IMU / steps | **blocked (sensor hub)** | defconfig says `CONFIG_SENSORS_SSC=y` — sensors live behind the ADSP Snapdragon Sensor Core, not a kernel I2C driver. Needs DSP subsystem bring-up; same bucket as WCNSS. |
| BLE / WiFi (WCNSS) | **not implemented** | needs WCNSS firmware loading — out of scope for now |

### Display strategy: reuse the splash framebuffer, don't port DSI

The Gen 4 taught the lesson: a from-scratch DSI + MDP + panel stack has no
observable intermediate state, so a single wrong value looks exactly like "the
code never ran". The Gen 6 makes that avoidable. aboot initialises MDSS, the
12nm PHY and the panel for its boot logo and hands over with the panel **still
lit and scanning** out of `splash_region` (0x90000000, 20 MB) — that is what the
`qcom,cont-splash-memory` node exists for.

So `fb_splash.c` programs **nothing**. It reads back the geometry aboot
actually set (MDP5 mixer `LAYER_0 OUT_SIZE`, falling back to the DMA pipe's
`SRC_SIZE`), clears the buffer, and hands it to LVGL. Cache-clean on flush is
the only hardware interaction. This means:

- first boot can show pixels with **zero** display-driver risk,
- the real panel resolution is **discovered**, not guessed — which matters
  because the panel node is not in any DTB we have,
- if aboot did **not** light the panel, `fb_init()` returns NULL and you get
  3 buzzes instead of an ambiguous dark screen.

Porting MDP5 + the 12nm PHY properly is the follow-up, once there is a working
screen to debug it on.

## Build & test loop

**Full instructions and the complete build-flag reference live in
[`BUILD-GEN6.md`](BUILD-GEN6.md).** Summary:

```sh
cd fossil-port/baremetal

# 1. build the REAL firmware (build.sh builds the demo/QEMU targets, not this)
CFLAGS_EXTRA="-DWDOG_TRACE -DDISPLAY_BISECT -DPLAT_I2C_RETEST -DTOUCH_DIAG" \
LVGL_DIR=$HOME/Arduino/libraries/lvgl sh build-owf-image.sh

# 2. pack — the DTB argument is NOT optional in practice
sh tools/mk-bootimg-gen6.sh build/gen6-owf/owf.bin ../sda429-hoki.dtb

# 3. flash to the RECOVERY slot (the scratch slot; boot is the firmware's
#    eventual permanent home)
fastboot flash recovery build/gen6-owf/owf-real-<TAG>-recovery.img
```

Three things that have each cost a wasted test cycle:

- **`-DWDOG_TRACE`** is the only thing that compiles `wdog_pet()` into the main
  loop — omit it and the watch reboots ~30 s into every boot.
- **`-DPLAT_I2C_RETEST`** — without it `boards/fossil_gen6.h` sets
  `PLAT_I2C_DISABLED 1` and touch is completely dead.
- **The DTB defaults to none.** `mk-bootimg-gen6.sh`'s second argument is
  optional; leaving it off yields a ~232 KB-smaller image. Verify every packed
  image with the checker in `BUILD-GEN6.md` before flashing.

The toolchain is now a system install on Linux (`/usr/bin/arm-none-eabi-gcc`);
the old Windows/Git-Bash `PATH` export is obsolete.

Both failure modes are non-destructive: a rejected image simply never runs, and
the 30-second dead-man returns the watch to fastboot even if ours does run and
hangs.

### Reading the result (buzz codes)

There is no UART pad and the display is unproven, so the motor is the primary
channel — same vocabulary as the Gen 4:

| You feel | Meaning | Where the bug is |
|---|---|---|
| nothing | never reached `main()` | boot.img params, load address, `startup.S`, or aboot rejected it |
| 1 long | reached `main()`; MMU + GIC + timer up | everything downstream |
| 1 long, then 2 short | `fb_init()` got a live framebuffer | if the screen is still dark: scanout/cache/format, not bring-up |
| 1 long, then 3 short | `fb_init()` failed | aboot did not leave the splash panel lit |

## Open questions (need the device)

- [ ] Panel vendor/model, real resolution, DSI command tables — from `dtbo` or
      `/proc/device-tree`.
- [ ] Touch controller vendor, bus, address, IRQ GPIO.
- [x] Does `fastboot boot <img>` work, or is it restricted to `fastboot flash`?
      **ANSWERED 2026-08-07: restricted to `fastboot flash`.** `fastboot boot`
      sends only a partial image and never runs it.
- [ ] GIC version/priority-bit count (A53 + 4.14 suggests GIC-400 like the Gen 4,
      but confirm — the FreeRTOS port self-checks this at scheduler start).
- [ ] Whether aboot leaves the panel lit (the Gen 4 splash-framebuffer trick).
- [ ] Is the exposed 4-pin USB genuinely fastboot-capable without a cradle mod?
