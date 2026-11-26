/*
 * FatFs options for the second stage (through FatFs's own override hook,
 * ZEPHYR_CONFIG_OVERRIDE in its ffconf.h): read only, long names (the boot
 * file sits in /.lossless), code page 437, the smaller buffer layout. And
 * exFAT as well as FAT32: cards of 64 GB and more come as exFAT, and the
 * original firmware reads both, so owners shouldn't have to format.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#undef FF_FS_READONLY
#define FF_FS_READONLY 1
#undef FF_FS_MINIMIZE
#define FF_FS_MINIMIZE 1
#undef FF_CODE_PAGE
#define FF_CODE_PAGE 437
#undef FF_USE_LFN
#define FF_USE_LFN 1
#undef FF_FS_TINY
#define FF_FS_TINY 1
#undef FF_FS_NORTC
#define FF_FS_NORTC 1
#undef FF_FS_EXFAT
#define FF_FS_EXFAT 1

/* the sector window's alignment (Zephyr names it): words */
#define FS_FATFS_WINDOW_ALIGNMENT 4
