/* ============================================================================
 *  plat_soc_tier.h — the SoC tier macros, and NOTHING else.
 *
 *  WHY THIS IS ITS OWN HEADER. PLAT_SOC_MSM / PLAT_SOC_MSM8909 are DERIVED
 *  from the PLAT_BOARD_* the build passes; they are not themselves -D flags.
 *  They used to be derived inside platform.h, which the runtime .c files all
 *  include — but the compat layer (compat/Arduino.h, compat/owf_meminfo.h)
 *  does NOT include platform.h, so in those translation units the tier macros
 *  silently evaluated to "not defined".
 *
 *  That is a nasty failure mode: `#if defined(PLAT_SOC_MSM8909)` in a file
 *  that cannot see the definition does not warn, it just takes the #else. It
 *  shipped a TicWatch C2 that reported itself as an SDA429W with 1 GB of DDR,
 *  and — worse — moving the Fossil Gen 4's guards up to the SoC tier silently
 *  broke THOSE too, because PLAT_BOARD_FOSSIL_GEN4 was a real command-line
 *  define while PLAT_SOC_MSM8909 was not.
 *
 *  So the derivation lives here, in a header with no dependencies, and both
 *  platform.h and the compat headers include it. One source of truth, visible
 *  from every translation unit that needs it. Anything added here must stay
 *  free of includes so it can be pulled in from anywhere, including C++.
 * ========================================================================== */
#pragma once

/* All the MSM watches: shared driver model (SPMI, GIC-400, msm reboot). */
#if defined(PLAT_BOARD_FOSSIL_GEN4) || defined(PLAT_BOARD_FOSSIL_GEN5) || \
    defined(PLAT_BOARD_FOSSIL_GEN6) || defined(PLAT_BOARD_TICWATCH_C2)
#define PLAT_SOC_MSM 1
#endif

/* The Snapdragon Wear 2100 (msm8909w / APQ8009W): Fossil Gen 4 (firefish) and
 * Mobvoi TicWatch C2 (skipjack). Same silicon, same registers, same fuse rows,
 * DDR at 0x80000000 — different vendors' watches around it.
 *
 * NOT the same amount of DDR, though this comment used to say so: the Gen 5 in
 * this tier has 1 GB, and the C2 ships in both 512 MB and 1 GB (C2+) variants
 * that share a board key. Anything needing the real size calls
 * ddr_size_detect() / plat_ddr_size(); nothing may infer it from this tier. */
/* The Gen 5 (triggerfish, Wear 3100) belongs here: the Wear 3100 is the Wear
 * 2100 AP plus an always-on co-processor, so every AP-side register in this
 * tier is the same silicon. Its PMIC is NOT the same, which is exactly why the
 * PMIC-side drivers ask about the PMIC rather than about the SoC tier. */
#if defined(PLAT_BOARD_FOSSIL_GEN4) || defined(PLAT_BOARD_FOSSIL_GEN5) || \
    defined(PLAT_BOARD_TICWATCH_C2)
#define PLAT_SOC_MSM8909 1
#endif
