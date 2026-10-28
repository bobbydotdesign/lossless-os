/*
 * LVGL's "A:" drive on Zephyr's file system, so the app reads fonts, covers
 * and the library the same way as on the Mac ("A:/SD:/Music/..." becomes
 * "/SD:/Music/..."). Folders come back with a leading '/', as LVGL's own
 * file drivers mark them. Reading only.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <lvgl.h>
#include <string.h>

#include "lv_drive_a.h"

static lv_fs_res_t result(int err)
{
	switch (err) {
	case 0:       return LV_FS_RES_OK;
	case -ENOENT: return LV_FS_RES_NOT_EX;
	case -ENOMEM: return LV_FS_RES_OUT_OF_MEM;
	case -EROFS:
	case -EACCES: return LV_FS_RES_DENIED;
	case -EBUSY:  return LV_FS_RES_BUSY;
	default:      return LV_FS_RES_HW_ERR;
	}
}

static void *open_cb(lv_fs_drv_t *drv, const char *path, lv_fs_mode_t mode)
{
	ARG_UNUSED(drv);
	if (mode & LV_FS_MODE_WR) {
		return NULL;
	}
	struct fs_file_t *f = lv_malloc(sizeof(*f));

	if (!f) {
		return NULL;
	}
	fs_file_t_init(f);
	if (fs_open(f, path, FS_O_READ)) {
		lv_free(f);
		return NULL;
	}
	return f;
}

static lv_fs_res_t close_cb(lv_fs_drv_t *drv, void *file)
{
	ARG_UNUSED(drv);
	int err = fs_close(file);

	lv_free(file);
	return result(err);
}

static lv_fs_res_t read_cb(lv_fs_drv_t *drv, void *file, void *buf, uint32_t btr, uint32_t *br)
{
	ARG_UNUSED(drv);
	ssize_t n = fs_read(file, buf, btr);

	if (n < 0) {
		*br = 0;
		return result((int)n);
	}
	*br = (uint32_t)n;
	return LV_FS_RES_OK;
}

static lv_fs_res_t seek_cb(lv_fs_drv_t *drv, void *file, uint32_t pos, lv_fs_whence_t whence)
{
	ARG_UNUSED(drv);
	int w = whence == LV_FS_SEEK_CUR ? FS_SEEK_CUR :
		whence == LV_FS_SEEK_END ? FS_SEEK_END : FS_SEEK_SET;

	return result(fs_seek(file, (off_t)pos, w));
}

static lv_fs_res_t tell_cb(lv_fs_drv_t *drv, void *file, uint32_t *pos)
{
	ARG_UNUSED(drv);
	off_t p = fs_tell(file);

	if (p < 0) {
		return result((int)p);
	}
	*pos = (uint32_t)p;
	return LV_FS_RES_OK;
}

static void *dir_open_cb(lv_fs_drv_t *drv, const char *path)
{
	ARG_UNUSED(drv);
	struct fs_dir_t *d = lv_malloc(sizeof(*d));

	if (!d) {
		return NULL;
	}
	fs_dir_t_init(d);
	if (fs_opendir(d, path)) {
		lv_free(d);
		return NULL;
	}
	return d;
}

static lv_fs_res_t dir_read_cb(lv_fs_drv_t *drv, void *dir, char *fn, uint32_t fn_len)
{
	ARG_UNUSED(drv);
	static struct fs_dirent entry;   /* large (long names); one reader at a time */

	for (;;) {
		int err = fs_readdir(dir, &entry);

		if (err) {
			fn[0] = '\0';
			return result(err);
		}
		if (entry.name[0] == '\0') {          /* end of the folder */
			fn[0] = '\0';
			return LV_FS_RES_OK;
		}
		if (!strcmp(entry.name, ".") || !strcmp(entry.name, "..")) {
			continue;
		}
		if (entry.type == FS_DIR_ENTRY_DIR) {
			snprintf(fn, fn_len, "/%s", entry.name);
		} else {
			snprintf(fn, fn_len, "%s", entry.name);
		}
		return LV_FS_RES_OK;
	}
}

static lv_fs_res_t dir_close_cb(lv_fs_drv_t *drv, void *dir)
{
	ARG_UNUSED(drv);
	int err = fs_closedir(dir);

	lv_free(dir);
	return result(err);
}

void lv_drive_a_register(void)
{
	static lv_fs_drv_t drv;

	lv_fs_drv_init(&drv);
	drv.letter = 'A';
	/* Read ahead 16 KB per open file: the card does far better with a few
	 * large reads than many small ones (JPEG and FLAC parsing read little
	 * pieces). */
	drv.cache_size = 16 * 1024;
	drv.open_cb = open_cb;
	drv.close_cb = close_cb;
	drv.read_cb = read_cb;
	drv.seek_cb = seek_cb;
	drv.tell_cb = tell_cb;
	drv.dir_open_cb = dir_open_cb;
	drv.dir_read_cb = dir_read_cb;
	drv.dir_close_cb = dir_close_cb;
	lv_fs_drv_register(&drv);
}
