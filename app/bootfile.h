/*
 * Lossless OS: its boot file, as our bootloader (boot/stage2) and the
 * installer see it (docs/h2-hardware.md, "The boot file").
 *
 * At power-on the bootloader starts /.lossless/boot/lossless.bin, or, when
 * that's missing or doesn't check out, /Lossless/lossless.bin: the release
 * copied to the card. The file is a 64-byte header and the image
 * (tools/mklosboot.py).
 *
 * The installer keeps the boot file, its .new while writing and its .old
 * (the version before) in /.lossless/boot. Outside /.lossless it may only
 * remove /update.upt, our bootloader's update file, once that has done its
 * job (bootfile_path_ok; the platform checks every path against it).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOS_BOOTFILE_H
#define LOS_BOOTFILE_H

#include <stdbool.h>
#include <stdint.h>

#define BOOTFILE_DIR     "/.lossless/boot"
#define BOOTFILE_PATH    BOOTFILE_DIR "/lossless.bin"
#define BOOTFILE_PACKAGE "/Lossless/lossless.bin"
#define BOOTFILE_UPDATE  "/update.upt"

/* A path on the card (from its root, starting with '/') the installer may
 * change: the boot file with "", ".new" or ".old" after it, and
 * /update.upt. Nothing else. */
bool bootfile_path_ok(const char *path);

typedef struct {
    bool exists;
    bool valid;          /* header and CRC check out: the bootloader would start it */
    uint32_t version;    /* major << 16 | minor << 8 | patch (0 if unknown) */
    uint32_t size, crc;
} bootfile_info;

/* Reads the whole file (path as for los_file_open) */
void bootfile_check(const char *path, bootfile_info *i);

/* Our bootloader's update file (tools/mkupdate.py), not anyone else's:
 * an ISO 9660 image holding our second stage's header ("LOS2", version
 * 1, linked at 0x81e00000) */
bool bootfile_update_ours(const char *path);

/* Whether the bootloader in an update file of ours (UBOOT.BIN in the ISO
 * image) is the one in the flash, byte for byte: only then has the file
 * done its job. flash_read reads the flash's bootloader partition (0 to
 * 512 KB); false if the file has no bootloader, or anything can't be
 * read, so the file stays. */
bool bootfile_update_installed(const char *path,
                               bool (*flash_read)(uint32_t offset, void *buf, uint32_t len));

/* "1.2" or "1.2.3" from a packed version; "unknown" for 0 */
void bootfile_version_text(uint32_t v, char *buf, int n);

#endif
