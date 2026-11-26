/*
 * FatFs's disk interface (its diskio.h) on our card reader: one drive,
 * read only.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ff.h"
#include "diskio.h"
#include "stage2.h"

static DSTATUS state = STA_NOINIT;

DSTATUS disk_status(BYTE pdrv)
{
	return pdrv ? STA_NOINIT : state;
}

DSTATUS disk_initialize(BYTE pdrv)
{
	if (pdrv)
		return STA_NOINIT;
	if (state & STA_NOINIT) {
		int err = sd_init();
		if (err) {
			note(0xcd000000u | (uint32_t)err);
			return state;
		}
		state = STA_PROTECT;
	}
	return state;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
	if (pdrv || (state & STA_NOINIT))
		return RES_NOTRDY;
	int err = sd_read(buff, (uint32_t)sector, count);
	if (err) {
		note(0xce000000u | ((uint32_t)-err & 0xff) << 16 | ((uint32_t)sector & 0xffff));
		return RES_ERROR;
	}
	return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
	(void)buff;
	if (pdrv)
		return RES_PARERR;
	return cmd == CTRL_SYNC ? RES_OK : RES_PARERR;
}
