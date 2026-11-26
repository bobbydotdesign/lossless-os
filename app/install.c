/*
 * Installing (install.h). Every change to the boot file and to
 * /update.upt goes through los_boot_*, which refuse any other path
 * (bootfile_path_ok).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "install.h"
#include "los_sys.h"
#include "stockfw.h"

#define MIN_BATTERY   30         /* percent, unless on USB power */
#define PATH_MAX_     256
#define FAILED_STARTS 3

/* snprintf that says whether it all fit: a path cut short could name
 * another file, so a path that doesn't fit is never used */
#define FITS(buf, ...) (snprintf((buf), sizeof(buf), __VA_ARGS__) < (int)sizeof(buf))

static const char *card(void) { return los_card_path(); }

/* card() + rel into buf; false (and buf names nothing) if it doesn't fit */
#define CARD_PATH(buf, rel) card_path((buf), sizeof(buf), (rel))
static bool card_path(char *buf, size_t n, const char *rel)
{
    if (snprintf(buf, n, "%s%s", card(), rel) < (int)n)
        return true;
    buf[0] = '\0';
    return false;
}

static bool exists(const char *path)
{
    void *f = los_file_open(path);
    if (f)
        los_file_close(f);
    return f != NULL;
}

static void make_dirs(void)
{
    static const char *const dirs[] = { "/.lossless", "/.lossless/logs", "/.lossless/state", BOOTFILE_DIR };
    char p[PATH_MAX_];
    for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
        if (CARD_PATH(p, dirs[i]))
            los_mkdir(p);
}

static void log_line(const char *fmt, const char *arg)
{
    char line[300], p[PATH_MAX_];
    snprintf(line, sizeof(line), fmt, arg ? arg : "");
    strncat(line, "\n", sizeof(line) - strlen(line) - 1);
    make_dirs();
    if (CARD_PATH(p, "/.lossless/logs/install.txt"))
        los_file_append(p, line, strlen(line));
}

void install_look(install_state *s)
{
    char p[PATH_MAX_];
    memset(s, 0, sizeof(*s));
    if (CARD_PATH(p, BOOTFILE_PATH))
        bootfile_check(p, &s->current);
    if (CARD_PATH(p, BOOTFILE_PATH ".old"))
        bootfile_check(p, &s->previous);
    if (CARD_PATH(p, BOOTFILE_PACKAGE))
        bootfile_check(p, &s->package);
}

enum install_offer install_offer(const install_state *s)
{
    /* only a newer version: the same version (another build of it) or an
     * older one, as a Lossless folder left from an earlier install, is
     * never offered over what runs now */
    if (s->package.valid && s->current.valid && s->package.version > s->current.version)
        return INSTALL_UPDATE;
    return INSTALL_NOTHING;
}

/* ---- copying ---------------------------------------------------------------------------- */

static uint8_t buf_a[4096], buf_b[4096];

/* src to dst (a boot path, or one in /.lossless), then read back and compared */
static bool copy_checked(const char *src, const char *dst, bool boot)
{
    void *in = los_file_open(src);
    if (!in)
        return false;
    bool ok = true;
    size_t got;
    while (ok && (got = los_file_read(in, buf_a, sizeof(buf_a))) > 0)
        ok = boot ? los_boot_append(dst, buf_a, got) : los_file_append(dst, buf_a, got);
    los_file_close(in);
    if (!ok)
        return false;
    /* what's on the card now, against what it came from */
    void *a = los_file_open(src), *b = los_file_open(dst);
    ok = a && b;
    while (ok) {
        size_t na = los_file_read(a, buf_a, sizeof(buf_a));
        size_t nb = los_file_read(b, buf_b, sizeof(buf_b));
        if (na != nb || memcmp(buf_a, buf_b, na))
            ok = false;
        if (!na)
            break;
    }
    if (a)
        los_file_close(a);
    if (b)
        los_file_close(b);
    return ok;
}

static void copy_fonts(void)
{
    char dst_dir[220], src_dir[230], entry[160];
    snprintf(dst_dir, sizeof(dst_dir), "%s/.lossless", card());
    los_mkdir(dst_dir);
    snprintf(dst_dir, sizeof(dst_dir), "%s/.lossless/fonts", card());
    los_mkdir(dst_dir);
    snprintf(src_dir, sizeof(src_dir), "A:%s/Lossless/fonts", card());
    lv_fs_dir_t d;
    if (lv_fs_dir_open(&d, src_dir) != LV_FS_RES_OK)
        return;
    while (lv_fs_dir_read(&d, entry, sizeof(entry)) == LV_FS_RES_OK && entry[0]) {
        if (entry[0] == '/' || entry[0] == '.')
            continue;
        char src[PATH_MAX_ + 200], dst[PATH_MAX_ + 200];
        if (!FITS(src, "%s/Lossless/fonts/%s", card(), entry) || !FITS(dst, "%s/%s", dst_dir, entry))
            continue;
        void *a = los_file_open(src), *b = los_file_open(dst);
        bool same = a && b && los_file_size(a) == los_file_size(b);
        if (a)
            los_file_close(a);
        if (b)
            los_file_close(b);
        if (same)
            continue;
        los_file_remove(dst);
        if (!copy_checked(src, dst, false)) {
            los_file_remove(dst);            /* /Lossless/fonts still works */
            log_line("fonts: couldn't copy %s", entry);
        }
    }
    lv_fs_dir_close(&d);
}

static const char *power_problem(void)
{
    int percent;
    bool charging, usb;
    if (!los_battery(&percent, &charging, &usb))
        return "Battery level unknown: plug into USB power first";
    if (percent < MIN_BATTERY && !usb && !charging)
        return "Charge to 30% or plug in first";
    return NULL;
}

/* ---- installing ------------------------------------------------------------------------- */

/* The release into /.lossless/boot: .new written and checked, the one
 * there kept as .old, .new renamed in */
static const char *install_copy(void)
{
    install_state s;
    install_look(&s);
    if (!s.package.valid)
        return "The Lossless folder's boot file is damaged: copy the release again";
    char boot[PATH_MAX_], fresh[PATH_MAX_], old[PATH_MAX_], package[PATH_MAX_];
    if (!CARD_PATH(boot, BOOTFILE_PATH) || !CARD_PATH(fresh, BOOTFILE_PATH ".new") ||
        !CARD_PATH(old, BOOTFILE_PATH ".old") || !CARD_PATH(package, BOOTFILE_PACKAGE))
        return "The card's name is too long: nothing was changed";
    copy_fonts();
    make_dirs();

    /* 1. ours beside it, read back, compared and checked */
    bootfile_info n;
    los_boot_remove(fresh);
    if (!copy_checked(package, fresh, true)) {
        los_boot_remove(fresh);
        log_line("install: writing %s failed", fresh);
        return "Couldn't write the boot file (is the card full?): nothing was changed";
    }
    bootfile_check(fresh, &n);
    if (!n.valid) {
        los_boot_remove(fresh);
        log_line("install: %s didn't check out", fresh);
        return "The boot file didn't check out after writing: nothing was changed";
    }

    /* 2. the one there kept as .old (a broken one just goes) */
    bool ok = true;
    if (s.current.valid)
        ok = los_boot_remove(old) && los_boot_rename(boot, old);
    else if (s.current.exists)
        ok = los_boot_remove(boot);
    if (!ok) {
        los_boot_remove(fresh);
        log_line("install: keeping %s failed", boot);
        return "Couldn't keep the version there now: nothing was changed";
    }

    /* 3. ours in its place */
    if (!los_boot_rename(fresh, boot))
        log_line("install: rename failed (finished at the next start)", NULL);
    char v[16];
    bootfile_version_text(n.version, v, sizeof(v));
    log_line("install: Lossless OS %s starts at power-on", v);
    return NULL;
}

const char *install_run(void)
{
    if (!los_writes_ok())
        return "The card can't be written now";
    const char *p = power_problem();
    return p ? p : install_copy();
}

/* ---- at start --------------------------------------------------------------------------- */

static const char *starts_path(void)
{
    static char p[PATH_MAX_];
    CARD_PATH(p, "/.lossless/state/starts.txt");
    return p;
}

static int read_starts(void)
{
    void *f = los_file_open(starts_path());
    if (!f)
        return 0;
    char t[16] = { 0 };
    los_file_read(f, t, sizeof(t) - 1);
    los_file_close(f);
    return atoi(t);
}

static void save_starts(int n)
{
    char t[16];
    make_dirs();
    snprintf(t, sizeof(t), "%d\n", n);
    los_file_save(starts_path(), t, strlen(t));
}

/* A swap cut short: the boot file missing, its .new or .old there */
static void finish_swap(void)
{
    char boot[PATH_MAX_], fresh[PATH_MAX_], old[PATH_MAX_];
    if (!CARD_PATH(boot, BOOTFILE_PATH) || !CARD_PATH(fresh, BOOTFILE_PATH ".new") ||
        !CARD_PATH(old, BOOTFILE_PATH ".old"))
        return;
    if (exists(boot)) {
        if (exists(fresh))
            los_boot_remove(fresh);                  /* left from a copy that never finished */
        return;
    }
    bootfile_info n, o;
    bootfile_check(fresh, &n);
    bootfile_check(old, &o);
    if (n.valid && los_boot_rename(fresh, boot))
        log_line("start: finished installing", NULL);
    else if (o.valid && los_boot_rename(old, boot))
        log_line("start: put the version before back", NULL);
}

/* The version before back in place of this one */
static bool put_previous_back(void)
{
    char boot[PATH_MAX_], old[PATH_MAX_];
    bootfile_info o, now;
    if (!CARD_PATH(boot, BOOTFILE_PATH) || !CARD_PATH(old, BOOTFILE_PATH ".old"))
        return false;
    bootfile_check(old, &o);
    if (!o.valid || !los_boot_remove(boot) || !los_boot_rename(old, boot))
        return false;
    bootfile_check(boot, &now);
    return now.valid;
}

bool install_at_start(void)
{
    if (!los_writes_ok())
        return false;                                /* Back held, or the check failed */
    finish_swap();

    install_state s;
    install_look(&s);
    if (!s.current.valid && s.package.valid) {
        /* started from the release: it installs itself */
        const char *err = install_copy();
        log_line(err ? "start: first install: %s" : "start: first install done%s", err ? err : "");
        install_look(&s);
    }
    char upt[PATH_MAX_];
    if (s.current.valid && CARD_PATH(upt, BOOTFILE_UPDATE) && bootfile_update_ours(upt) &&
        bootfile_update_installed(upt, los_boot_flash_read)) {
        /* the bootloader in it is the one in the flash: it has done its
         * job, and Vol+ would only write it again. Until then it stays (a
         * Vol+ start the battery check turned down, say). */
        if (los_boot_remove(upt))
            log_line("start: removed our update.upt (its bootloader is installed)", NULL);
    }

    install_bt_patch(false);

    int n = read_starts();
    if (n < FAILED_STARTS) {
        save_starts(n + 1);
        return false;
    }
    save_starts(0);
    if (put_previous_back()) {
        log_line("start: three starts in a row failed: the version before is back", NULL);
        return true;
    }
    return false;
}

void install_started_ok(void)
{
    static bool done;
    if (done || !los_writes_ok())
        return;
    done = true;
    if (read_starts())
        save_starts(0);
}

/* ---- the Bluetooth chip's patch --------------------------------------------------------- */

#define BT_PATCH_MAX  (64 * 1024)
#define BT_PATCH_TRIED "/.lossless/state/bt-patch-tried.txt"

static bool card_patch_ok(const char *path, uint8_t *buf)
{
    void *f = los_file_open(path);
    if (!f)
        return false;
    size_t n = los_file_read(f, buf, BT_PATCH_MAX);
    los_file_close(f);
    return n < BT_PATCH_MAX && stockfw_hcd_ok(buf, n);
}

bool install_bt_patch(bool thorough)
{
    static uint8_t buf[BT_PATCH_MAX];
    char path[PATH_MAX_], tried[PATH_MAX_], dir[PATH_MAX_], note[160];

    if (!CARD_PATH(path, LOS_BT_PATCH) || !CARD_PATH(tried, BT_PATCH_TRIED) ||
        !CARD_PATH(dir, LOS_BT_PATCH_DIR))
        return false;
    if (card_patch_ok(path, buf))
        return true;                                 /* the player's, or one put there by hand */
    if (!los_writes_ok() || (!thorough && exists(tried)))
        return false;                                /* at start, once: Bluetooth tries again */

    size_t n = stockfw_bt_patch(buf, sizeof(buf), thorough, note, sizeof(note));
    make_dirs();
    if (!n) {
        log_line("bluetooth: no patch in the original firmware: %s", note);
        los_file_save(tried, note, strlen(note));
        return false;
    }
    los_mkdir(dir);
    if (!los_file_save(path, buf, n)) {
        log_line("bluetooth: couldn't save the patch to the card", NULL);
        return false;
    }
    log_line("bluetooth: copied the player's own patch from its original firmware (%s)", note);
    los_file_remove(tried);
    return card_patch_ok(path, buf);
}
