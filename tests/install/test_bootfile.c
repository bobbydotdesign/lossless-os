/*
 * The installer's path rules and boot file checks (app/bootfile.c), on
 * the Mac (tests/install/run.sh).
 *
 *   test_bootfile ours.bin broken.bin other.bin our.upt their.upt [uboot.bin]
 *
 * With uboot.bin (the bootloader our.upt carries), our.upt is compared
 * with a pretend flash holding it, a changed copy, and one that can't be
 * read.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>
#include "bootfile.h"
#include "los_sys.h"

void *los_file_open(const char *path) { return fopen(path, "rb"); }
size_t los_file_read(void *f, void *buf, size_t n) { return fread(buf, 1, n, f); }
void los_file_close(void *f) { fclose(f); }
bool los_file_seek(void *f, uint64_t pos) { return fseek(f, (long)pos, SEEK_SET) == 0; }
uint64_t los_file_size(void *f)
{
    long at = ftell(f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, at, SEEK_SET);
    return size > 0 ? (uint64_t)size : 0;
}

/* the pretend flash: a file, with one byte changed if flip_at >= 0 */
static const char *flash_file;
static long flip_at = -1;
static bool flash_read(uint32_t offset, void *buf, uint32_t len)
{
    FILE *f = flash_file ? fopen(flash_file, "rb") : NULL;
    if (!f)
        return false;
    bool ok = !fseek(f, (long)offset, SEEK_SET) && fread(buf, 1, len, f) == len;
    fclose(f);
    if (ok && flip_at >= (long)offset && flip_at < (long)offset + (long)len)
        ((uint8_t *)buf)[flip_at - offset] ^= 0x01;
    return ok;
}

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)

int main(int argc, char **argv)
{
    /* the boot file, its .new and .old, and our update file */
    CHECK(bootfile_path_ok("/.lossless/boot/lossless.bin"));
    CHECK(bootfile_path_ok("/.lossless/boot/lossless.bin.new"));
    CHECK(bootfile_path_ok("/.lossless/boot/lossless.bin.old"));
    CHECK(bootfile_path_ok("/update.upt"));
    CHECK(bootfile_path_ok(BOOTFILE_PATH));
    CHECK(bootfile_path_ok(BOOTFILE_UPDATE));
    /* nothing else: the release itself is only read */
    CHECK(!bootfile_path_ok(BOOTFILE_PACKAGE));
    CHECK(!bootfile_path_ok("/Lossless/lossless.bin"));
    CHECK(!bootfile_path_ok("/.lossless/boot/other.bin"));
    CHECK(!bootfile_path_ok("/.lossless/boot/lossless.bin.bak"));
    CHECK(!bootfile_path_ok("/.lossless/boot/../boot/lossless.bin"));
    CHECK(!bootfile_path_ok("/lossless.bin"));
    CHECK(!bootfile_path_ok("/.lossless/settings.json"));
    CHECK(!bootfile_path_ok("/Music/a.flac"));
    CHECK(!bootfile_path_ok("/update.upt.old"));
    CHECK(!bootfile_path_ok("/UPDATE.UPT"));
    CHECK(!bootfile_path_ok("/uImage"));
    CHECK(!bootfile_path_ok("/linux_cmdline.txt"));
    CHECK(!bootfile_path_ok("update.upt"));
    CHECK(!bootfile_path_ok(""));
    CHECK(!bootfile_path_ok(NULL));

    char v[16];
    bootfile_version_text(0x010203, v, sizeof(v));
    CHECK(!strcmp(v, "1.2.3"));
    bootfile_version_text(0x000900, v, sizeof(v));
    CHECK(!strcmp(v, "0.9"));
    bootfile_version_text(0, v, sizeof(v));
    CHECK(!strcmp(v, "unknown"));

    if (argc == 6 || argc == 7) {
        bootfile_info i;
        bootfile_check(argv[1], &i);
        CHECK(i.exists && i.valid && i.version > 0);
        bootfile_check(argv[2], &i);
        CHECK(i.exists && !i.valid);                 /* one byte changed: the CRC fails */
        bootfile_check(argv[3], &i);
        CHECK(i.exists && !i.valid);                 /* not a boot file of ours */
        bootfile_check("/nonexistent/file", &i);
        CHECK(!i.exists && !i.valid);
        CHECK(bootfile_update_ours(argv[4]));
        CHECK(!bootfile_update_ours(argv[5]));       /* the factory's: never removed */
        CHECK(!bootfile_update_ours(argv[1]));
        flash_file = argv[1];
        CHECK(!bootfile_update_installed(argv[5], flash_read));   /* no bootloader in it */
        CHECK(!bootfile_update_installed("/nonexistent/file", flash_read));
        if (argc == 7) {
            flash_file = argv[6];
            CHECK(bootfile_update_installed(argv[4], flash_read));
            flip_at = 0x4100;                        /* the second stage, one bit */
            CHECK(!bootfile_update_installed(argv[4], flash_read));
            flip_at = 0x2b000;                       /* its copy at 0x20000 */
            CHECK(!bootfile_update_installed(argv[4], flash_read));
            flip_at = -1;
            flash_file = NULL;                       /* the flash can't be read: the file stays */
            CHECK(!bootfile_update_installed(argv[4], flash_read));
        } else {
            CHECK(!bootfile_update_installed(argv[4], flash_read));   /* no UBOOT.BIN in it */
        }
    } else {
        printf("(no files given: path rules only)\n");
    }

    printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails != 0;
}
