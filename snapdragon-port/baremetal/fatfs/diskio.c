/* diskio.c — FatFs media glue: drive 0 = the FFAT region inside userdata.
 * All access goes through the fenced eMMC driver (sdhci_msm.c) offset by the
 * region base from storage_gen6.c — FatFs can physically only ever touch the
 * FFAT region (double-fenced: this offset AND the emmc write window). */
#include "ff.h"
#include "diskio.h"
#include "../platform/platform.h"

DSTATUS disk_status(BYTE pdrv)
{
    return (pdrv == 0 && storage_ok()) ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv != 0) return STA_NOINIT;
    if (!storage_ok()) storage_init();
    return storage_ok() ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0 || !storage_ok()) return RES_NOTRDY;
    uint32_t base = storage_region_lba(2), n = storage_region_nblk(2);
    if (sector + count > n) return RES_PARERR;
    return emmc_read(base + (uint32_t)sector, count, buff) == 0 ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0 || !storage_ok()) return RES_NOTRDY;
    uint32_t base = storage_region_lba(2), n = storage_region_nblk(2);
    if (sector + count > n) return RES_PARERR;
    /* f_mkfs zeroes FATs with thousands of sequential writes — keep the
     * watchdog and dead-man fed through the burst or first-format reboots. */
    static uint32_t s_wr;
    if ((s_wr += count) >= 64u) { s_wr = 0; wdog_pet(); deadman_kick(); }
    return emmc_write(base + (uint32_t)sector, count, buff) == 0 ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv != 0) return RES_PARERR;
    switch (cmd) {
    case CTRL_SYNC:        return RES_OK;         /* PIO writes are synchronous */
    case GET_SECTOR_COUNT: *(LBA_t *)buff = storage_region_nblk(2); return RES_OK;
    case GET_SECTOR_SIZE:  *(WORD *)buff = 512;   return RES_OK;
    case GET_BLOCK_SIZE:   *(DWORD *)buff = 8;    return RES_OK;   /* 4 KiB */
    default:               return RES_PARERR;
    }
}
