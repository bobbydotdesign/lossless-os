/*
 * The boot file (bootfile.h): which paths the installer may change, and
 * what a boot file is (tools/mklosboot.py): "LOSB", header version 1, load
 * address, entry, image size, the image's CRC-32, the OS version, zeros to
 * 64 bytes, then the image. The bootloader (boot/stage2) makes the same
 * checks before it starts one.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>
#include "bootfile.h"
#include "los_sys.h"

#define HEADER     64
#define OS_START   0x81000000u   /* the image goes in RAM, below the bootloader (boot/stage2) */
#define OS_END     0x81e00000u

bool bootfile_path_ok(const char *path)
{
    if (!path)
        return false;
    return !strcmp(path, BOOTFILE_PATH) || !strcmp(path, BOOTFILE_PATH ".new") ||
           !strcmp(path, BOOTFILE_PATH ".old") || !strcmp(path, BOOTFILE_UPDATE);
}

static uint32_t get_le(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* The usual CRC-32 (as zlib's), a bit at a time */
static uint32_t crc32_add(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = crc >> 1 ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

void bootfile_check(const char *path, bootfile_info *i)
{
    memset(i, 0, sizeof(*i));
    void *f = los_file_open(path);
    if (!f)
        return;
    i->exists = true;
    static uint8_t buf[4096];
    uint8_t h[HEADER];
    size_t got = los_file_read(f, h, sizeof(h));
    if (got != sizeof(h) || memcmp(h, "LOSB", 4) || get_le(h + 4) != 1) {
        los_file_close(f);
        return;
    }
    uint32_t load = get_le(h + 8), entry = get_le(h + 12), size = get_le(h + 16);
    uint32_t want = get_le(h + 20), crc = 0, at = 0;
    while ((got = los_file_read(f, buf, sizeof(buf))) > 0) {
        crc = crc32_add(crc, buf, got);
        at += (uint32_t)got;
    }
    los_file_close(f);
    i->size = HEADER + at;
    i->crc = crc;
    i->version = get_le(h + 24);
    i->valid = at == size && crc == want && load >= OS_START && size <= OS_END - load &&
               entry >= load && entry < load + size;
}

bool bootfile_update_ours(const char *path)
{
    static const uint8_t mark[12] = { 'L', 'O', 'S', '2', 1, 0, 0, 0, 0x00, 0x00, 0xe0, 0x81 };
    void *f = los_file_open(path);
    if (!f)
        return false;
    bool ours = false;
    uint64_t size = los_file_size(f);
    static uint8_t buf[4096 + sizeof(mark)];
    size_t keep = 0, got;
    /* our update file is a small ISO 9660 image (about 200 KB; "CD001"
     * at 0x8001, the first volume descriptor): anything big, or not an
     * ISO image, is someone else's */
    uint8_t iso[5] = { 0 };
    bool is_iso = los_file_seek(f, 0x8001) && los_file_read(f, iso, sizeof(iso)) == sizeof(iso) &&
                  !memcmp(iso, "CD001", 5) && los_file_seek(f, 0);
    if (is_iso && size > 0 && size < 2u * 1024 * 1024) {
        while (!ours && (got = los_file_read(f, buf + keep, 4096)) > 0) {
            size_t n = keep + got;
            for (size_t k = 0; k + sizeof(mark) <= n; k++)
                if (!memcmp(buf + k, mark, sizeof(mark))) {
                    ours = true;
                    break;
                }
            keep = n < sizeof(mark) - 1 ? n : sizeof(mark) - 1;
            memmove(buf, buf + n - keep, keep);
        }
    }
    los_file_close(f);
    return ours;
}

#define ISO_SECTOR     2048u
#define BOOT_FLASH_MAX 0x80000u          /* the bootloader partition: 4 blocks of 128 KB */

static uint32_t le32_(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static bool iso_name_is(const uint8_t *name, unsigned n, const char *want)
{
    unsigned i = 0;
    for (; i < n && want[i]; i++) {
        char c = (char)name[i];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 32);
        if (c != want[i])
            return false;
    }
    return !want[i] && (i == n || name[i] == ';');
}

static bool read_at(void *f, uint64_t at, void *buf, size_t n)
{
    return los_file_seek(f, at) && los_file_read(f, buf, n) == n;
}

/* UBOOT.BIN in the image's root directory, as our bootloader finds it
 * (boot/stage2, update_from_card) */
static bool find_uboot(void *f, uint8_t *sec, uint32_t *at, uint32_t *len)
{
    if (!read_at(f, 16 * ISO_SECTOR, sec, ISO_SECTOR) || sec[0] != 1 || memcmp(sec + 1, "CD001", 5))
        return false;
    uint32_t dir = le32_(sec + 156 + 2) * ISO_SECTOR, dir_len = le32_(sec + 156 + 10);
    if (dir_len > 16 * ISO_SECTOR)
        return false;
    for (uint32_t off = 0; off < dir_len; off += ISO_SECTOR) {
        if (!read_at(f, (uint64_t)dir + off, sec, ISO_SECTOR))
            return false;
        for (uint32_t r = 0; r + 33 < ISO_SECTOR && sec[r];) {
            uint8_t rl = sec[r], nl = sec[r + 32];
            if (rl < 34 || r + rl > ISO_SECTOR)
                break;
            if (r + 33 + nl <= ISO_SECTOR && iso_name_is(sec + r + 33, nl, "UBOOT.BIN")) {
                *at = le32_(sec + r + 2) * ISO_SECTOR;
                *len = le32_(sec + r + 10);
                return true;
            }
            r += rl;
        }
    }
    return false;
}

bool bootfile_update_installed(const char *path,
                               bool (*flash_read)(uint32_t offset, void *buf, uint32_t len))
{
    static uint8_t a[ISO_SECTOR], b[ISO_SECTOR];
    void *f = los_file_open(path);
    if (!f)
        return false;
    uint32_t at = 0, len = 0;
    bool same = find_uboot(f, a, &at, &len) && len >= 0x1000 && len <= BOOT_FLASH_MAX;
    for (uint32_t done = 0; same && done < len; done += sizeof(a)) {
        uint32_t n = len - done < sizeof(a) ? len - done : (uint32_t)sizeof(a);
        same = read_at(f, (uint64_t)at + done, a, n) && flash_read(done, b, n) && !memcmp(a, b, n);
    }
    los_file_close(f);
    return same;
}

void bootfile_version_text(uint32_t v, char *buf, int n)
{
    if (!v)
        snprintf(buf, (size_t)n, "unknown");
    else if (v & 0xff)
        snprintf(buf, (size_t)n, "%u.%u.%u", (unsigned)(v >> 16), (unsigned)(v >> 8 & 0xff), (unsigned)(v & 0xff));
    else
        snprintf(buf, (size_t)n, "%u.%u", (unsigned)(v >> 16), (unsigned)(v >> 8 & 0xff));
}
