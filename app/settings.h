/*
 * What the player remembers between starts, in the card's
 * /.lossless/state/settings.txt (one "name=value" a line). Saved only when
 * the card can be written (los_writes_ok).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOS_SETTINGS_H
#define LOS_SETTINGS_H

#include <stdbool.h>

typedef struct {
    bool have_volume;
    int volume_db;
    int repeat;        /* 0 off, 1 all, 2 one */
    bool shuffle;
    bool bluetooth;    /* on at start */
    int brightness;    /* percent; 0 if not saved */
    int dim_s;         /* -1 if not saved; 0 never */
    int idle_off_min;  /* switch off after this long idle; -1 if not saved, 0 never */
    bool clock24;
    bool wheel_volume;
    bool lock_wheel;   /* the wheel when locked: volume */
    bool battery_pct;  /* the battery's percent in the header always (or only while plugged in) */
    bool eq;
    bool mono;         /* both sides averaged into each (off: stereo, bit-perfect) */
    int eq_gains[10];  /* 31 Hz .. 16 kHz */
    int usb;           /* plugged into a computer: 0 ask, 1 transfer files, 2 charge only, 3 DAC */
    int eq_custom[3][10];  /* Custom EQ 1 to 3 */
    struct { char name[16]; int gains[10]; } eq_edited[12];  /* presets changed from their own */
    int eq_n_edited;
    char skin[24];     /* the skin's name (skins.h); "" if not saved */
    char eq_sel[24];   /* the EQ chosen: "custom1" to "custom3", or a preset's name; "" if not saved */
} los_settings;

/* False if there's nothing saved (defaults then) */
bool settings_load(los_settings *s);
bool settings_save(const los_settings *s);

#endif
