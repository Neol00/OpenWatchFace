/* mmu.c — flat (identity) MMU map + caches for ARMv7-A.
 *
 * Why this exists: with the MMU off, ARMv7 treats ALL memory as strongly-
 * ordered, and unaligned accesses FAULT. LVGL/newlib rely on ordinary
 * unaligned loads, so the runtime cannot host them until DDR is mapped as
 * Normal memory. Caches ride along for free (essential on real silicon;
 * QEMU's TCG models them as no-ops).
 *
 * Map: 4096 x 1MB sections, VA == PA.
 *   - DDR (PLAT_DDR_BASE..+SIZE): Normal, write-back write-allocate.
 *   - everything else:            Device (peripherals stay strongly ordered).
 */
#include "platform.h"

static uint32_t s_l1[4096] __attribute__((aligned(16384)));

#define SECT_DEVICE   0x00000C12u  /* section, AP=rw, XN=1, TEX=000 C=0 B=0 (strongly-ordered) */
#define SECT_NORMAL   0x00015C0Eu  /* section, AP=rw, XN=0, S=1, TEX=101 C=1 B=1 (WBWA) */

static inline void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }
static inline void isb(void) { __asm__ volatile("isb" ::: "memory"); }

void mmu_enable_flat(void)
{
    uint32_t ddr_first = PLAT_DDR_BASE >> 20;
    uint32_t ddr_last  = (PLAT_DDR_BASE + PLAT_DDR_SIZE - 1) >> 20;

    for (uint32_t i = 0; i < 4096; i++) {
        uint32_t base = i << 20;
        s_l1[i] = base | ((i >= ddr_first && i <= ddr_last) ? SECT_NORMAL : SECT_DEVICE);
    }
    dsb();

    /* invalidate stale state: I-cache, branch predictor, TLB, L1 D-cache */
    __asm__ volatile("mcr p15, 0, %0, c7, c5, 0" :: "r"(0));   /* ICIALLU  */
    __asm__ volatile("mcr p15, 0, %0, c7, c5, 6" :: "r"(0));   /* BPIALL   */
    __asm__ volatile("mcr p15, 0, %0, c8, c7, 0" :: "r"(0));   /* TLBIALL  */
    /* A7 L1D: 32KB 4-way 64B lines -> 128 sets. (Full CLIDR walk when the
     * hardware bring-up demands L2 handling — aboot hands L2 over clean.) */
    for (uint32_t way = 0; way < 4; way++)
        for (uint32_t set = 0; set < 128; set++) {
            uint32_t v = (way << 30) | (set << 6);
            __asm__ volatile("mcr p15, 0, %0, c7, c6, 2" :: "r"(v)); /* DCISW */
        }
    dsb();

    __asm__ volatile("mcr p15, 0, %0, c3, c0, 0" :: "r"(0x1));      /* DACR: dom0 client */
    __asm__ volatile("mcr p15, 0, %0, c2, c0, 2" :: "r"(0));        /* TTBCR: TTBR0 only */
    __asm__ volatile("mcr p15, 0, %0, c2, c0, 0" :: "r"((uint32_t)(uintptr_t)s_l1 | 0x48)); /* TTBR0: WBWA walk */
    isb();

    uint32_t sctlr;
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(sctlr));
    sctlr |= (1u << 0)    /* M: MMU */
           | (1u << 2)    /* C: D-cache */
           | (1u << 11)   /* Z: branch prediction */
           | (1u << 12);  /* I: I-cache */
    sctlr &= ~(1u << 1);  /* A: no strict alignment (Normal memory allows unaligned) */
    __asm__ volatile("mcr p15, 0, %0, c1, c0, 0" :: "r"(sctlr));
    isb();
}
