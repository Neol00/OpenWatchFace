# OpenWatchFace → Fossil Gen 4 (bare-metal) port

Running the OpenWatchFace firmware (`../OpenWatchFace/`) **bare-metal on one Cortex-A7**
of the Fossil Gen 4 smartwatch (Snapdragon **Wear 2100**, msm8909w; AsteroidOS codename
**firefish** = ~44mm Q Explorist HR / sibling **ray** = ~40mm Q Venture HR; models
DW6xxx / DW7xxx). Both sizes are the SAME SoC (Wear 2100). Early notes said Wear 3100 —
that was a different product (the Fossil *Sport*), and is irrelevant here anyway: the
3100 is just this msm8909w A7 complex plus the unusable QCC1110 co-processor.

## Strategy

Not Wear OS, not AsteroidOS, not Linux at all. The Snapdragon Wear 3100 is an
msm8909w-class SoC: quad Cortex-A7 + Adreno GPU + WCN3620 radio + QCC1110
ultra-low-power co-processor. The QCC1110 is **not usable** (no public SDK; its
firmware is a signed Qualcomm blob; and it cannot reach the GPU or general display
path anyway), so this port targets the application cores directly:

```
[ aboot (stock Qualcomm bootloader) ]  <- kept as-is; loads our image as the "kernel"
[ FreeRTOS on ONE Cortex-A7        ]  <- GIC + arch timer + MMU; other 3 cores held off
[ ported MSM drivers (from Linux)  ]  <- DSI/panel, I2C/touch, SDHCI/eMMC, SPMI/PMIC, WCNSS
[ OpenWatchFace firmware (ours)    ]  <- setup()/loop() on the existing board layer
```

The vendor 3.18 kernel (GPL, released by Fossil on request; used by the AsteroidOS
firefish port) is the driver reference: every peripheral we need has a working,
readable Linux implementation plus a device tree that documents the exact panel,
touch controller, GPIOs, clocks and regulators of this watch. "Port the drivers,
lose the kernel."

**Fleet note:** the Gen 4 is the first of several Fossil targets on hand (a
Gen 6 — AsteroidOS "hoki", Snapdragon Wear 4100+, 64-bit Cortex-A53 — and
Fossil Q models). The layout anticipates them: `baremetal/platform/` is the
shared runtime + driver API, `baremetal/boards/<device>.h` holds per-device
addresses, and each watch gets its own `board_fossil_<device>.h` +
`BOARD_ID_FOSSIL_<DEVICE>` in the firmware's board layer. The Gen 6 will need
an AArch64 startup variant; many MSM driver ports (BLSP I2C, SDHCI, SPMI,
UARTDM) should carry across with new base addresses.

Why FreeRTOS and not a scratch scheduler: the firmware already talks FreeRTOS
(tasks, semaphores, queues) on the ESP32 targets, and FreeRTOS ships a Cortex-A
port (GIC + generic timer). Running the real thing means the firmware's threading
model works unchanged — no pthread shim like the Maix Linux port needed.

## Hardware access & recovery (before any code)

- **USB:** the Gen 4 has no USB connector. The charger pogo pads carry USB D+/D-;
  a DIY cable (documented by the AsteroidOS community) exposes fastboot/adb.
  Build/acquire this first — it is the only dev link.
- **Unlock:** `fastboot oem unlock` (wipes Wear OS; that's fine).
- **Never flash `aboot`/`sbl1`/`tz`/`rpm`.** Only the `boot` partition is ours.
  During development, don't even flash: `fastboot boot owf.img` runs a build once
  from RAM — a failed image is cured by rebooting. That is the iteration loop.
- **Recovery ladder:** reboot → fastboot (hold buttons) → EDL (9008) via QFIL as
  the last-resort unbrick. Dump every partition before first flash.
- **Debug output:** no UART is exposed. Plan A is the display itself (aboot leaves
  the panel initialized for its splash on most msm8909 devices — early bring-up can
  scribble into the splash framebuffer before we own DSI). Plan B: a persistent
  RAM ring-buffer log readable after reboot via fastboot RAM dump. Investigate
  whether any pogo pad is muxable to BLSP UART (some Fossil boards route one).

> **Verified hardware/boot facts live in [`HARDWARE.md`](HARDWARE.md)** — kernel
> repo/branch, boot.img parameters, UART address, panel/touch identification.
> Update it as device-in-hand dumps confirm entries.

## Roadmap

- [~] **Phase 0 — access:** DIY pogo-USB cable; unlock; full partition backup;
      confirm `fastboot boot` of a stock boot.img works. *(done without device:
      boot.img parameters recovered from AsteroidOS img_info → HARDWARE.md +
      tools/mk-bootimg.sh; remaining items need the watch.)*
- [~] **Phase 1 — sign of life:** `baremetal/` runtime BUILT AND PROVEN in QEMU:
      self-relocating startup.S (vectors, SVC, VFP/NEON, stacks, bss), linker
      script, UARTDM/PL011 drivers, post-mortem DDR ramlog, arch-timer
      heartbeat. Verified both load paths: ELF at link address AND raw binary
      at a foreign address (self-relocation + r2/DTB handoff exercised — the
      aboot scenario). gen4 boot.img packs via `./build.sh gen4` +
      `tools/mk-bootimg.sh` (bundled dependency-free mkbootimg_v0.py; payload
      is the RAW binary — aboot does not gunzip). *(Remaining on hardware:
      vibration-motor / splash-framebuffer sign of life.)*
      **Windows note:** run QEMU inside WSL (`wsl qemu-system-arm ...`). The
      winget/weilnetz QEMU builds are x86-64; under Windows-on-ARM64 x64
      emulation their TCG JIT dies instantly and silently — only a native
      ARM64 QEMU (WSL Debian: `apt install qemu-system-arm`) works.
- [x] **Phase 2 — runtime:** DONE in emulation. MMU flat map + caches (required:
      unaligned accesses fault with MMU off — LVGL/newlib need Normal memory),
      GICv2 driver + drift-free CNTV tick (CVAL deadline, not TVAL re-arm),
      FreeRTOS V11.2 (GCC/ARM_CA9 port) with per-task VFP context, newlib-nano
      malloc above the image, LVGL 9.5.0 (same tree as the Arduino builds,
      cached into liblvgl.a) rendering a demo watch face direct-mode into the
      framebuffer. Verified by QMP screendump: `build/qemu/shot-lvgl.png`.
      GIC priority-bit counts differ per board (QEMU GICv2 = 8 bits/256 levels,
      GIC-400 = 5 bits/32 levels) — set per-board in FreeRTOSConfig.h; the port
      self-checks at scheduler start.
      *(Board target `BOARD_ID_FOSSIL_GEN4` + `board_fossil_gen4.h` registered
      in the firmware's board layer, all features stubbed. Still open: the
      Arduino compat layer port — next.)*
- [~] **Phase 3 — display:** DSI half ported, UNTESTED (no device yet).
      `platform/msm_dsi_regs.h` (controller + 28nm PHY register map),
      `platform/msm_dsi.c` (PHY regulator/config + host init + DMA command TX,
      ported from `msm_mdss_io_8974.c` / `mdss_dsi_host.c`), and
      `platform/dsi_panel.c` (on/off command tables + `fb_init()`). The gen4
      target now LINKS and packs a boot image — previously it could not link at
      all (`fb_init` was QEMU-only). Every PHY data value (timing, lanecfg,
      strength, regulator, bist) is the verbatim DT property, not invented.
      `platform/msm_mdp3.c` (DMA_P config + command-mode frame push, ported
      from `mdp3_dma.c` `mdp3_dmap_config`/`mdp3_dmap_update`) closes the pixel
      path: `fb_init` programs DMA_P at the framebuffer, and LVGL's flush
      callback kicks `DMA_P_START` per refresh. Full chain now exists in code:
      LVGL → framebuffer → cache clean → MDP3 DMA_P → DSI → panel.
      **Still to do:** (a) the panel command table is the AUO 400p *template*,
      not Fossil's — swap it from the stock DTB. (b) assumes aboot left MDSS
      clocks/regulators on; owning GCC is Phase 6. (c) the flush is BLOCKING
      and full-frame — async-DMA + partial-ROI push are the perf follow-ups
      (HARDWARE.md items #1). (d) **nothing here has touched hardware** — DSI
      has no QEMU model, so first execution is on the watch.
- [~] **Phase 4 — input:** touch ported, UNTESTED. `platform/msm_i2c.c` (BLSP
      QUP v2 I2C master — polled FIFO mode, QUP v2 tag protocol, ported from
      `i2c-msm-v2.c`) and `platform/touch_raydium.c` (Raydium RM_TS PDA2
      register access + 11-byte-per-point report parsing, ported from
      `raydium_i2c_ts.c`). Registered as an LVGL pointer indev in `ui_demo.c`;
      touch failure is non-fatal (UI runs display-only). Polled from LVGL's
      read callback rather than the controller's INT line.
      **Still to do:** crown/buttons via TLMM GPIO + PMIC PON key; the crown →
      LVGL encoder mapping; INT-driven reads. Bus address and touch I2C
      address are the *reference-board* values — confirm from the stock DTB.
- [ ] **Phase 5 — storage & RTC:** sdhci-msm port for the eMMC; carve/reuse a FAT
      partition (userdata) for `storage_fs.h` (FatFs); PMIC RTC over SPMI for
      timekeeping across power-off (`BOARD_HAS_RTC_*` analogue).
- [ ] **Phase 6 — power:** cpufreq (A7 PLL scaling, floor it), WFI idle in the
      LVGL idle loop, unused-peripheral clock/regulator gating over SPMI. Then the
      deep-sleep analogue: SoC power collapse via RPM sleep-set messaging (port
      rpm-smd — SMEM + SMD first), PMIC RTC alarm + button as wake sources, so the
      existing `sleep_power.h` model (sleep → timed background wake) maps over.
- [ ] **Phase 7 — BLE (the big one):** WCNSS bring-up — PIL-load the signed
      wcnss firmware from the stock partition via TZ SCM calls, SMEM/SMD channels,
      then the BT HCI channel. Run the **NimBLE host** (already what the firmware
      speaks on ESP32) over an HCI-over-SMD transport → `ble_ancs.h` /
      `ble_gadgetbridge.h` / `ble_player_ams.h` largely unchanged.
- [ ] **Phase 8 — WiFi (optional/last):** wcn36xx-style WLAN over the same WCNSS,
      lwIP + mbedTLS for `notif_net.h`. Honest odds are long; BLE covers the
      watch's core phone link (ANCS + Gadgetbridge) without it.
- [ ] **Board target:** `board_fossil_gen4.h` (`BOARD_ID_FOSSIL`), round-screen
      flags (`BOARD_SCREEN_ROUND 1`), features switching on as phases land.

## Known cliffs (ranked)

1. **WCNSS/RPM are subsystem bring-ups, not drivers** — signed-firmware loading
   through TrustZone, shared-memory protocols (SMEM/SMD), and a remote processor
   each. Ported by reading the Linux implementations, but each is weeks, not days.
   Everything before Phase 7 works without them (WFI-only idle costs battery until
   Phase 6's RPM work lands).
2. **Panel init is fiddly and blind** — wrong DSI timing shows nothing. Mitigate by
   starting from aboot's already-lit panel and taking over incrementally.
3. **Debug visibility** — no UART/JTAG. The framebuffer console + RAM ring log has
   to be built early (Phase 1–2) and treated as core infrastructure.
4. **GPU:** deliberately unused. LVGL software rendering with NEON at 390×390 is
   fast; the Adreno needs clock/regulator/bus scaffolding that isn't worth it.

## Fallback

If a cliff proves terminal, the `maixcam-port` strategy (OpenWatchFace as the sole
userspace app on the AsteroidOS/vendor Linux kernel: fbdev/evdev + BlueZ +
suspend-to-RAM) remains viable on this same watch with the same unlocked
bootloader — Phases 0 and the board-layer work transfer to it directly.
