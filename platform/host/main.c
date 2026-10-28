/*
 * Lossless OS on the Mac: the simulator.
 *
 *   los-sim                      a 320x240 window at 2x; arrow keys, Return
 *                                (play/select), Esc or Backspace (back),
 *                                Home (menu), + and - (volume),
 *                                L (Power tap: key lock), U (plug a computer
 *                                in; again: unplug)
 *   los-sim --script FILE        no window: play a button script and save
 *                                screenshots (wait MS, sleep MS (real time),
 *                                key NAME, hold NAME MS, vol DB, usb N (0 out, 1 charger,
 *                                2 computer, 3 eject), dump, quit)
 *
 * Options: --music DIR (library), --assets DIR (fonts), --shots DIR.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <SDL.h>
#include "lvgl.h"
#include "los.h"

static const char *music_dir = "build/sim-music";   /* tools/sim_library.sh */
static const char *assets_dir = "assets/fonts";
static const char *shots_dir = "sim/shots";
static char music_abs[1024], assets_abs[1024];

const char *los_assets_path(void) { return assets_abs; }
const char *los_music_path(void) { return music_abs; }
const char *los_player_name(void) { return "Lossless OS simulator"; }
const char *los_card_path(void) { return music_abs; }

bool los_storage(uint64_t *used, uint64_t *total)
{
    struct statvfs s;
    if (statvfs(music_abs, &s) != 0)
        return false;
    *total = (uint64_t)s.f_blocks * s.f_frsize;
    *used = *total - (uint64_t)s.f_bavail * s.f_frsize;
    return true;
}


/* ---- scripted keypad ---------------------------------------------------------------- */

static uint32_t script_key;
static int script_phase;   /* 0 idle, 1 pressed, 2 released */
static bool script_held;   /* "hold": pressed until let go */

static void script_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->key = script_key;
    if (script_held) {
        data->state = LV_INDEV_STATE_PRESSED;
    } else if (script_phase == 1) {
        data->state = LV_INDEV_STATE_PRESSED;
        script_phase = 2;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        script_phase = 0;
    }
}

static void run_for(int ms)
{
    for (int t = 0; t < ms; t += 5) {
        lv_tick_inc(5);
        lv_timer_handler();
    }
}

static void press(uint32_t key)
{
    script_key = key;
    script_phase = 1;
    run_for(60);
}

static void save_bmp(const char *path)
{
    lv_draw_buf_t *buf = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB888);
    if (!buf) {
        fprintf(stderr, "snapshot failed\n");
        return;
    }
    int w = buf->header.w, h = buf->header.h, stride = buf->header.stride;
    /* what's on the top layer (the lock screen, say) goes over it */
    if (lv_obj_get_child_count(lv_layer_top())) {
        lv_draw_buf_t *top = lv_snapshot_take(lv_layer_top(), LV_COLOR_FORMAT_ARGB8888);
        if (top && top->header.w == (uint32_t)w && top->header.h == (uint32_t)h) {
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    const uint8_t *t = top->data + y * top->header.stride + x * 4;   /* B G R A */
                    uint8_t *d = buf->data + y * stride + x * 3;
                    for (int c = 0; c < 3; c++)
                        d[c] = (uint8_t)((t[c] * t[3] + d[c] * (255 - t[3])) / 255);
                }
        }
        if (top)
            lv_draw_buf_destroy(top);
    }
    int row = (w * 3 + 3) & ~3;
    FILE *f = fopen(path, "wb");
    if (f) {
        uint8_t hdr[54] = { 'B', 'M' };
        uint32_t size = 54 + row * h;
        memcpy(hdr + 2, &size, 4);
        hdr[10] = 54;
        hdr[14] = 40;
        memcpy(hdr + 18, &w, 4);
        memcpy(hdr + 22, &h, 4);
        hdr[26] = 1;
        hdr[28] = 24;
        fwrite(hdr, 1, 54, f);
        uint8_t pad[4] = { 0 };
        for (int y = h - 1; y >= 0; y--) {
            fwrite(buf->data + y * stride, 1, w * 3, f);   /* stored B, G, R */
            fwrite(pad, 1, row - w * 3, f);
        }
        fclose(f);
        printf("%s\n", path);
    }
    lv_draw_buf_destroy(buf);
}

void los_sim_usb(int s);   /* sys.c */

static uint32_t key_named(const char *name)
{
    if (!strcmp(name, "up")) return LV_KEY_UP;
    if (!strcmp(name, "down")) return LV_KEY_DOWN;
    if (!strcmp(name, "left")) return LV_KEY_LEFT;
    if (!strcmp(name, "right")) return LV_KEY_RIGHT;
    if (!strcmp(name, "enter")) return LV_KEY_ENTER;
    if (!strcmp(name, "back")) return LV_KEY_ESC;
    if (!strcmp(name, "menu")) return LV_KEY_HOME;
    if (!strcmp(name, "lock")) return LOS_KEY_LOCK;
    return 0;
}

static void flush_nothing(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)a; (void)px;
    lv_display_flush_ready(d);
}

static int run_script(const char *file, const char *name)
{
    static uint8_t frame[320 * 240 * 2] __attribute__((aligned(64)));   /* LVGL checks */
    lv_display_t *disp = lv_display_create(320, 240);
    lv_display_set_buffers(disp, frame, NULL, sizeof(frame), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, flush_nothing);
    lv_indev_t *kp = lv_indev_create();
    lv_indev_set_type(kp, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(kp, script_read);
    los_app_start(disp, kp);
    run_for(100);

    FILE *f = fopen(file, "r");
    if (!f) {
        perror(file);
        return 1;
    }
    char line[128];
    int shot = 0;
    while (fgets(line, sizeof(line), f)) {
        char cmd[32] = "", arg[64] = "";
        int n = 1;
        if (line[0] == '#' || sscanf(line, "%31s %63s %d", cmd, arg, &n) < 1)
            continue;
        if (!strcmp(cmd, "wait"))
            run_for(atoi(arg));
        else if (!strcmp(cmd, "sleep")) {          /* real time, for playback */
            uint32_t end = SDL_GetTicks() + (uint32_t)atoi(arg);
            while (SDL_GetTicks() < end) {
                run_for(10);
                SDL_Delay(10);
            }
        }
        else if (!strcmp(cmd, "hold")) {           /* hold NAME MS: pressed that long */
            script_key = key_named(arg);
            script_held = true;
            run_for(n > 0 ? n : 1000);
            script_held = false;
            run_for(60);
        }
        else if (!strcmp(cmd, "usb"))
            los_sim_usb(atoi(arg));
        else if (!strcmp(cmd, "vol"))
            los_app_volume(atoi(arg));
        else if (!strcmp(cmd, "key")) {
            uint32_t k = key_named(arg);
            for (int i = 0; i < (n > 0 ? n : 1) && k; i++)
                press(k);
        } else if (!strcmp(cmd, "dump")) {
            char path[1100];
            run_for(30);
            snprintf(path, sizeof(path), "%s/%s-%02d.bmp", shots_dir, name, ++shot);
            save_bmp(path);
        } else if (!strcmp(cmd, "quit"))
            break;
    }
    fclose(f);
    return 0;
}

/* ---- interactive window --------------------------------------------------------------- */

static uint32_t sdl_ms(void) { return SDL_GetTicks(); }

/* The keyboard, with + and - taken out as the volume buttons */
static lv_indev_read_cb_t keyboard_read;

static void keys_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    static bool vol_held;
    keyboard_read(indev, data);
    bool vol = data->key == '+' || data->key == '=' || data->key == '-';
    if (vol && data->state == LV_INDEV_STATE_PRESSED) {
        if (!vol_held)
            los_app_volume(data->key == '-' ? -2 : +2);
        vol_held = true;
    } else
        vol_held = false;
    if (vol) {
        data->key = 0;
        data->state = LV_INDEV_STATE_RELEASED;
    }
    if (data->key == 'l' || data->key == 'L')
        data->key = LOS_KEY_LOCK;
    /* U: plug a computer in; again: eject and unplug */
    static bool u_held, usb_in;
    bool u = data->key == 'u' || data->key == 'U';
    if (u && data->state == LV_INDEV_STATE_PRESSED && !u_held) {
        usb_in = !usb_in;
        los_sim_usb(usb_in ? 2 : 0);
    }
    u_held = u && data->state == LV_INDEV_STATE_PRESSED;
    if (u) {
        data->key = 0;
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static int run_window(void)
{
    lv_tick_set_cb(sdl_ms);
    lv_display_t *disp = lv_sdl_window_create(320, 240);
    lv_sdl_window_set_zoom(disp, 2);
    lv_sdl_window_set_title(disp, "Lossless OS");
    lv_indev_t *kb = lv_sdl_keyboard_create();
    keyboard_read = lv_indev_get_read_cb(kb);
    lv_indev_set_read_cb(kb, keys_read);
    los_app_start(disp, kb);
    for (;;) {
        uint32_t wait = lv_timer_handler();
        SDL_Delay(wait < 5 ? wait : 5);
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *script = NULL, *name = "shot";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--script") && i + 1 < argc) script = argv[++i];
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--music") && i + 1 < argc) music_dir = argv[++i];
        else if (!strcmp(argv[i], "--assets") && i + 1 < argc) assets_dir = argv[++i];
        else if (!strcmp(argv[i], "--shots") && i + 1 < argc) shots_dir = argv[++i];
    }
    if (!realpath(music_dir, music_abs))
        snprintf(music_abs, sizeof(music_abs), "%s", music_dir);
    if (!realpath(assets_dir, assets_abs))
        snprintf(assets_abs, sizeof(assets_abs), "%s", assets_dir);

    lv_init();
    if (!script)
        return run_window();
    /* leave without tearing down: the audio and Bluetooth threads are
     * still running and would trip over freed state */
    int code = run_script(script, name);
    fflush(stdout);
    fflush(stderr);
    _exit(code);
}
