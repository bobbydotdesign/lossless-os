/*
 * Lossless OS on Zephyr: start the display and the app, then run the UI.
 *
 * What differs per board (the card, buttons, power) comes through the
 * los_board_* hooks in board.h; the defaults here suit the emulator, which
 * has none of them.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <lvgl.h>
#include "los.h"
#include "board.h"

#ifdef CONFIG_FILE_SYSTEM
#include <zephyr/fs/fs.h>
#include "lv_drive_a.h"
#endif

/* Paths on the player's card */
const char *los_assets_path(void) { return "/SD:/.lossless/fonts"; }
const char *los_music_path(void) { return "/SD:"; }   /* the whole card: any layout */
__weak const char *los_player_name(void) { return "Lossless OS player"; }

bool los_storage(uint64_t *used, uint64_t *total)
{
#ifdef CONFIG_FILE_SYSTEM
    struct fs_statvfs st;

    if (fs_statvfs("/SD:", &st) == 0 && st.f_blocks) {
        *total = (uint64_t)st.f_blocks * st.f_frsize;
        *used = *total - (uint64_t)st.f_bfree * st.f_frsize;
        return true;
    }
#endif
    (void)used; (void)total;
    return false;
}

/* Board defaults: nothing to set up, no buttons */
__weak void los_board_start(void) {}

__weak bool los_board_key(uint32_t *key, bool *pressed, bool *more)
{
    (void)key; (void)pressed; (void)more;
    return false;
}

static void read_keys(lv_indev_t *indev, lv_indev_data_t *data)
{
    static uint32_t last_key;
    static bool last_pressed;
    uint32_t key = last_key;
    bool pressed = last_pressed, more = false;

    (void)indev;
    if (los_board_key(&key, &pressed, &more)) {
        last_key = key;
        last_pressed = pressed;
    }
    data->key = last_key;
    data->state = last_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->continue_reading = more;
}

int main(void)
{
    printk("Lossless OS 0.1 on %s\n", CONFIG_BOARD_TARGET);

    const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    if (!device_is_ready(display)) {
        printk("display not ready\n");
        return 0;
    }

#ifdef CONFIG_FILE_SYSTEM
    lv_drive_a_register();
#endif
    los_board_start();

    lv_indev_t *keys = lv_indev_create();
    lv_indev_set_type(keys, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(keys, read_keys);

    los_app_start(lv_display_get_default(), keys);
    display_blanking_off(display);
    printk("home screen built\n");

    for (;;) {
        uint32_t wait = lv_timer_handler();
        k_msleep(wait < 10 ? wait : 10);
    }
}
