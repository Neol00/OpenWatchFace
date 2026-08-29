# Fossil Gen 4 (firefish/ray) — verified hardware & boot facts

Collected 2026-07-21 from the AsteroidOS meta-smartwatch layer and the public
firefish kernel branch. This file is the ground truth the bare-metal port codes
against; update it as the stock boot.img / device tree dump (device-in-hand)
confirms or corrects entries.

## Kernel source (the driver reference — public, no Fossil request needed)

```
repo:   https://android.googlesource.com/kernel/msm
branch: android-msm-firefish-3.18-oreo-wear-dr
pinned: 20d62df1b6b88de89184cbd1bf826291f43ddec8   (AsteroidOS SRCREV)
```

Don't clone the full tree (~GBs); gitiles serves per-directory tarballs:
`.../kernel/msm/+archive/refs/heads/android-msm-firefish-3.18-oreo-wear-dr/<subdir>.tar.gz`
and single files via `.../+/refs/heads/<branch>/<path>?format=TEXT` (base64).

AsteroidOS's kernel patches (meta-smartwatch/meta-ray/recipes-kernel/linux/files/)
are small and worth mirroring — notably 0003 (raydium wakeup delay) and 0005
(mdp3 overlay release), which hint where the drivers are fragile.

## boot.img parameters (from AsteroidOS img_info, firefish)

```
page_size      = 2048
base_addr      = 0x00000000
kernel_offset  = 0x00008000
ramdisk_offset = 0x02000000
tags_offset    = 0x01e00000
format         = gzip (zImage-dtb: appended DTB image)
```

Stock cmdline (facts embedded in it):
```
androidboot.hardware=ray console=ttyHSL0,115200,n8
earlycon=msm_hsl_uart,0x78af000 msm_rtb.filter=0x237
lpm_levels.sleep_disabled=1 selinux=0 ...
```
- **Debug UART exists**: MSM HSL UART @ phys 0x78af000 (BLSP1 UART). Whether it
  reaches any accessible pad is unknown — but the peripheral + protocol is known,
  and a bare-metal putc against it is ~20 lines.
  **DO NOT TOUCH IT (2026-08-28).** The first Gen 4 image that ran died
  instantly, and disabling `uart_init()` was what made the firmware boot.
  Nothing proves aboot leaves BLSP1 UART1 clocked at handoff, and a read of a
  clock-gated MSM block does not fail — the AHB transaction never completes and
  the CPU hangs there. `boards/fossil_gen4.h` now sets `PLAT_UART_DISABLED`;
  re-enable only together with a GCC driver for the BLSP1 UART clocks AND a pad
  that something can actually listen on (open question #3, still open).
- Both variants report `androidboot.hardware=ray`.

## SoC / platform

- **Snapdragon Wear 2100** (msm8909w/APQ8009w) — CORRECTED 2026-07-21. The
  ENTIRE Gen 4 line (Q Explorist HR ~44mm, Q Venture HR ~40mm) is Wear 2100;
  both watch sizes are the SAME SoC. The Wear 3100 of that era was a different
  product, the Fossil *Sport* (not a Gen 4). The 3100 = this same msm8909w
  quad-A7 complex PLUS a QCC1110 ambient co-processor; the 2100 has no QCC1110.
  Since this port only uses the A7 cluster, 2100 vs 3100 is immaterial — and
  the AsteroidOS table always listed `msm8909w` (= Wear 2100), which is what
  every address/driver here is built against.
  Quad Cortex-A7, ARMv7VE + NEON-VFPv4 (AsteroidOS tune: `armv7vehf-neon`).
- Wear DTBs built by the kernel: `apq8009w[-1gb][-nowgr]-swoctp[-circpanel].dtb`
  and msm8909w equivalents (circpanel = round display; 1gb vs 512MB;
  nowgr = no WiFi/GPS variant). Fossil boards match `qcom,msm-id <265>/<301>`,
  board-id e.g. `<8 0xc>` for SWOC-circular.
- **The shipping Fossil DTB is NOT in the public tree** (no raydium node, no
  Fossil board file). It lives in the stock boot.img → recover it device-in-hand
  (`fastboot`/TWRP dump, or /proc/device-tree under AsteroidOS) and decompile
  with dtc. Until then the Qualcomm swoctp-circpanel trees are the template.

## Peripherals (kernel/DT evidence so far)

| Block | Fact | Source |
|---|---|---|
| Display path | MDP3 + MIPI-DSI (mdss_mdp; AsteroidOS patches `video/mdp3`) | patch 0005, msm8909-mdss*.dtsi |
| Panel | **CONFIRMED from the stock DTB: "AUO h139 AMOLED command mode dsi panel" (pref-prim-pan phandle 0x9b), round 454×454 (0x1c6), 24bpp, framerate 45, `rgb_swap_rgb`, active area at column offset 10.** Earlier notes said 390×390 — that was a pre-dump guess and is WRONG. The DTB is checked in at `firefish-stock.dtb`/`.dts`; the command tables are ported verbatim in `baremetal/platform/dsi_panel.c`. | stock DTB |
| Touch | **Raydium RM_TS** I2C (`CONFIG_TOUCHSCREEN_RM_TS=y`; it7260/synaptics disabled). Driver: `drivers/input/touchscreen/raydium/` | defconfig |
| Touch bus (ref boards) | BLSP1 QUP5 i2c @ 0x78b9000 (reference wear DT; confirm Fossil's from stock DTB) | apq8009w-swoctp.dtsi |
| PMIC | PM8916-class over SPMI (`msm-pm8916.dtsi` on wear refs; charger ext.) | swoctp dtsi |
| Charger | SMB231 linear charger on i2c_4 (`qcom,smb231-lbc` @0x12) on wear refs | swoctp dtsi |
| NFC | NQ-NCI (qcom,nq-nci) on BLSP1 QUP2 i2c @ 0x78b6000 (ref boards) | swoctp dtsi |
| WLAN/BT | WCN3620-class via WCNSS (machine conf `MACHINE_HAS_WLAN=true`; bluebinder) | firefish.conf |
| Console | ttyHSL0 @ 0x78af000, 115200n8 | cmdline |

## Rendering: software (NEON), GPU out of scope

Decision: LVGL **software rendering with NEON** (already enabled in startup.S) —
NOT because it's better, but because the GPU is out of reach. This is an
accepted LOSS on two axes, not a free choice:
  - Animation: the Adreno 304 IS the right engine for UI transitions (full-screen
    fades/scale/rotate/arc = per-pixel alpha-blend at 60fps — exactly a GPU's
    strength). NEON can do 390×390 but only by pinning the A7 at high clock for
    the whole animation; expect transitions to be the weak spot of software
    rendering even when the static face is smooth.
  - Power: msm8909w is a 28nm quad-A7 clocked far above watch needs (it gets
    physically HOT under sustained use — observed on real hardware; ~1-day
    battery). Pushing pixels on the CPU holds that oversized core at high V²
    power — the worst way to spend the budget. A GPU blend costs a fraction of
    the energy; offloading display work off the big cores is exactly how
    week-plus-battery watches do it.
Why we accept it anyway: the Adreno's only bare-metal path is porting
freedreno/drm-msm + Mesa off a Linux kernel (SMMU, GEM, command submission,
GDSC/GCC/bus bring-up) — a multi-month subproject with no freestanding entry
point. Not worth it for this port.

What protects the STATIC UI regardless: the tiny workload — 390×390 (~152k px,
~10% of 720p), dirty-region redraw, face changes ~1×/sec. And note clock speed
is a poor predictor of software-render performance anyway (it's bandwidth/pixel-
plumbing bound): this project's own ports show the 480 MHz T5-E1 only MATCHING
the 240 MHz ESP32-S3, and the >1 GHz MaixCam-Pro running WORSE than the S3.

Because the GPU (the power-efficient path) is off the table, battery must be won
elsewhere — these are now HEADLINE work, not polish:
  - RPM SoC power-collapse + dynamic A7 clock scaling. CRITICAL ARCHITECTURE
    NOTE: on msm8909w the A7 does NOT own its own clocks/voltages — they belong
    to the **RPM**, a separate always-on Cortex-M3 power processor. "Setting a
    frequency" = sending a vote to the RPM over a shared-memory mailbox
    (SMEM + SMD/RPM-SMD), which drives the PMIC SPMI rails. So freq/voltage
    scaling is NOT a CPU-register poke like the ESP ports — it appears only
    AFTER the RPM comms path is ported (same subsystem as power-collapse/BLE).
    Proof it works on THIS watch: AsteroidOS ships an `underclock` service
    (meta-ray) that clocks the Gen 4 down via the normal cpufreq→RPM path — the
    knob is real and community-proven, just behind the RPM, not a fuse.
    Dynamic (idle-low / active-high) scaling is natural here: the RPM is
    vote/sleep-set based and built to arbitrate exactly that.
  - CAVEAT vs the ESP undervolt approach: the S3's arbitrary dig_dbias voltage
    trim does NOT port. The RPM exposes discrete, pre-characterized (freq,
    voltage) operating points, not a free voltage dial — pick the lowest blessed
    corner, don't hand-tune millivolts. (Safer anyway.)
  - NOTE: the CPU cores are STOCK ARM Cortex-A7 (licensed IP), NOT Qualcomm
    custom cores (Kryo/Krait) — those are high-end Snapdragon only. Best-
    documented ARM core there is; no custom-core quirks either way.
  - MDP3 2D blitter (the ONE reachable graphics-offload block — fixed-function,
    NOT the Adreno) + async-DMA flush: every blend the MDP does is one the hot
    A7 doesn't. Now a POWER win, not just an FPS win.
  - UI-level: cheaper transitions / lower-framerate ambient states.

What will actually determine Fossil render performance (measure THESE on
hardware, not a CPU benchmark), highest-leverage first:
  1. DSI/MDP3 flush path — panel is COMMAND-MODE (we push every frame; no
     self-scanout). A blocking flush = bandwidth-bound = mediocre regardless of
     the A7. Port the S3's async-DMA-flush (overlap transfer w/ next-frame
     compute) — the single biggest lever.
  2. Framebuffer memory attributes — cached vs write-combine for the LVGL draw
     buffers + DSI-DMA buffer, and cache-maintenance cost per flush. A
     bare-metal knob (ties into mmu.c section descriptors) the ESP boards never
     exposed; wrong choice stalls a fast CPU every frame.
  3. Native panel color format — render in the panel's byte order to skip the
     per-frame software byte-swap (as on the S3).
  4. DDR/bus clock — msm8909w LPDDR3; if RPM leaves these low in minimal
     bring-up, blits crawl. A spot where "wake the minimum" can bite.
  5. CPU throughput — last, and NEON mostly settles it.

## AsteroidOS machine facts (fallback path / cross-check)

- `KERNEL_IMAGETYPE = zImage-dtb`, serial console ttyHSL0.
- Machine confs: `firefish.conf` ("Fossil Gen 4") and `ray.conf` ("Skagen
  Falster 2") — same layer, different kernel recipe (`linux-firefish` vs
  `linux-ray`, branches `...firefish-3.18-oreo...` / `..._n` for ray).
- They ship an `underclock` service — confirms thermal/battery headroom is tight
  at stock clocks; our cpufreq floor plan matches.
- Fastboot entry gesture: boot, wait for vibration to stop, then immediately
  touch top-left + bottom-right screen edges.

## Confirmed on hardware (2026-08-28, first boots of the real firmware)

- **The firmware boots and renders.** The OpenWatchFace UI is on the glass, via
  the MDP3 DMA_P takeover of aboot's splash engine (`platform/fb_mdp3.c`): no
  DSI, PHY, PLL or panel-init code of our own runs at all. Build and flag
  details: [`BUILD-GEN4.md`](BUILD-GEN4.md).
- **The panel wants RGB pack order** (`MDP3_DMA_OUTPUT_PACK_PATTERN_RGB`,
  0x21), NOT BGR. The DT's `rgb_swap_rgb` and `mdp3_ctrl.c`'s BGR-for-8888 rule
  both pointed at BGR; on the glass that gave red-as-blue and bright-blue-as-
  orange with black and white correct, i.e. R and B swapped once too often.
- **`uart_init()` is fatal** — see the boot.img section above.
- **The APPS watchdog must be petted from the first instruction.** aboot arms
  it at an 11 s bark (`qcom,bark-time = 0x2af8`), so a build without
  `-DWDOG_TRACE` warm-resets into Wear OS a few seconds in, looking exactly
  like an image that never ran.
- **Three MDP3 register/enum errors** were found by re-reading the vendor
  source rather than trusting the earlier port: `DMA_P_FETCH_CFG` is at 0x90074
  (not 0x90048), `PACK_PATTERN_BGR` is 0x12 (0x06 is GBR), and
  `color_comp_out_bits` is three 2-bit fields = 0x3F (not a bare 3, which
  leaves two components at 4 bits).

## Open questions

1. ~~Exact panel controller + init table~~ — ANSWERED: AUO h139, 454x454,
   command mode; table decoded verbatim into `dsi_panel.c`. (Only needed for
   the `-DGEN4_DSI_INIT` fallback; the normal path never sends it.)
2. Fossil's touch/pin map deltas vs the swoctp reference DT → ANSWERED from the
   dump: touch is Raydium @0x39 on i2c@78b9000 (the DT's `i2c5` = BLSP1 QUP5,
   pins gpio18/19, function `blsp_i2c5` = func-sel 2), reset GPIO 12, IRQ GPIO
   13. Whether the controller actually answers is not yet confirmed.
3. Is the HSL UART bonded out anywhere reachable (pogo pads / test points)?
4. Which DTB does Fossil's aboot actually select (board-id) — affects our
   appended-DTB packaging.
5. firefish vs ray: which variant the purchased unit is (DW6xxx = firefish,
   DW7xxx = ray) — affects dock geometry and kernel branch.
