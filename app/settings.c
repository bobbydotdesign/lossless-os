/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "settings.h"
#include "los_sys.h"

static void paths(char *file, size_t n, char *dir, size_t dn)
{
    snprintf(dir, dn, "%s/.lossless/state", los_card_path());
    snprintf(file, n, "%s/settings.txt", dir);
}

bool settings_load(los_settings *s)
{
    char file[176], dir[160], tmp[184];   /* the folder, then /settings.txt, then .tmp */
    static char text[1600];            /* as much as settings_save writes, and more */
    memset(s, 0, sizeof(*s));
    s->dim_s = -1;
    s->wheel_volume = true;            /* the default (as on an iPod) */
    int version = 1;                   /* files before "settings=2" */
    paths(file, sizeof(file), dir, sizeof(dir));
    void *f = los_file_open(file);
    if (!f) {   /* a save cut short leaves the new one as .tmp */
        snprintf(tmp, sizeof(tmp), "%s.tmp", file);
        f = los_file_open(tmp);
    }
    if (!f)
        return false;
    size_t n = los_file_read(f, text, sizeof(text) - 1);
    los_file_close(f);
    text[n] = '\0';
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        int v = atoi(eq + 1);
        if (!strcmp(line, "volume")) {
            s->volume_db = v;
            s->have_volume = true;
        } else if (!strcmp(line, "repeat")) {
            s->repeat = v >= 0 && v <= 2 ? v : 0;
        } else if (!strcmp(line, "shuffle")) {
            s->shuffle = v != 0;
        } else if (!strcmp(line, "bluetooth")) {
            s->bluetooth = v != 0;
        } else if (!strcmp(line, "brightness")) {
            s->brightness = v >= 5 && v <= 100 ? v : 0;
        } else if (!strcmp(line, "dim")) {
            s->dim_s = v >= 0 ? v : -1;
        } else if (!strcmp(line, "clock24")) {
            s->clock24 = v != 0;
        } else if (!strcmp(line, "settings")) {
            version = v;
        } else if (!strcmp(line, "wheel")) {
            s->wheel_volume = v == 1;
        } else if (!strcmp(line, "eq")) {
            s->eq = v != 0;
        } else if (!strcmp(line, "usb")) {
            s->usb = v >= 0 && v <= 3 ? v : 0;
        } else if (!strcmp(line, "eqsel")) {
            snprintf(s->eq_sel, sizeof(s->eq_sel), "%s", eq + 1);
        } else if (!strncmp(line, "eqc", 3) && line[3] >= '1' && line[3] <= '3' && !line[4]) {
            int *g = s->eq_custom[line[3] - '1'];
            if (sscanf(eq + 1, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", &g[0], &g[1], &g[2], &g[3],
                       &g[4], &g[5], &g[6], &g[7], &g[8], &g[9]) != 10)
                memset(g, 0, 10 * sizeof(int));
        } else if (!strcmp(line, "eqp") && s->eq_n_edited < 12) {
            /* a preset as changed: its name, then its ten bands */
            char *comma = strchr(eq + 1, ',');
            int *g = s->eq_edited[s->eq_n_edited].gains;
            if (comma && comma - (eq + 1) < 16 &&
                sscanf(comma + 1, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", &g[0], &g[1], &g[2], &g[3],
                       &g[4], &g[5], &g[6], &g[7], &g[8], &g[9]) == 10) {
                *comma = '\0';
                snprintf(s->eq_edited[s->eq_n_edited].name, 16, "%s", eq + 1);
                s->eq_n_edited++;
            }
        } else if (!strcmp(line, "eqg")) {
            int *g = s->eq_gains;
            int n = sscanf(eq + 1, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", &g[0], &g[1], &g[2], &g[3],
                           &g[4], &g[5], &g[6], &g[7], &g[8], &g[9]);
            if (n == 5) {
                /* saved by the five-band EQ (100 Hz shelf, 250, 1k, 4k,
                 * 10 kHz shelf): spread over the ten */
                int o[5];
                memcpy(o, g, sizeof(o));
                int ten[10] = { o[0], o[0], o[0], o[1], (o[1] + o[2]) / 2, o[2],
                                (o[2] + o[3]) / 2, o[3], o[4], o[4] };
                memcpy(g, ten, sizeof(ten));
            } else if (n != 10) {
                memset(s->eq_gains, 0, sizeof(s->eq_gains));
            }
        }
    }
    /* before version 2 the wheel's default was scrubbing, and every file
     * saved it: take the new default once */
    if (version < 2)
        s->wheel_volume = true;
    return true;
}

bool settings_save(const los_settings *s)
{
    char file[176], dir[160];
    static char text[1536];
    if (!los_writes_ok())
        return false;
    paths(file, sizeof(file), dir, sizeof(dir));
    char top[160];
    snprintf(top, sizeof(top), "%s/.lossless", los_card_path());
    los_mkdir(top);
    los_mkdir(dir);
    int n = snprintf(text, sizeof(text),
                     "volume=%d\nrepeat=%d\nshuffle=%d\nbluetooth=%d\nbrightness=%d\ndim=%d\n"
                     "settings=2\nclock24=%d\nwheel=%d\neq=%d\neqg=%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n"
                     "usb=%d\n",
                     s->volume_db, s->repeat, s->shuffle ? 1 : 0, s->bluetooth ? 1 : 0,
                     s->brightness, s->dim_s, s->clock24 ? 1 : 0, s->wheel_volume ? 1 : 0,
                     s->eq ? 1 : 0, s->eq_gains[0], s->eq_gains[1], s->eq_gains[2], s->eq_gains[3],
                     s->eq_gains[4], s->eq_gains[5], s->eq_gains[6], s->eq_gains[7], s->eq_gains[8],
                     s->eq_gains[9], s->usb);
    for (int c = 0; c < 3 && n > 0 && n < (int)sizeof(text); c++) {
        const int *g = s->eq_custom[c];
        n += snprintf(text + n, sizeof(text) - n, "eqc%d=%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n", c + 1,
                      g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7], g[8], g[9]);
    }
    for (int e = 0; e < s->eq_n_edited && e < 12 && n > 0 && n < (int)sizeof(text); e++) {
        const int *g = s->eq_edited[e].gains;
        n += snprintf(text + n, sizeof(text) - n, "eqp=%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
                      s->eq_edited[e].name, g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7],
                      g[8], g[9]);
    }
    if (n > 0 && n < (int)sizeof(text))
        n += snprintf(text + n, sizeof(text) - n, "eqsel=%s\n", s->eq_sel);
    if (n <= 0 || n >= (int)sizeof(text))
        return false;
    return los_file_save(file, text, (size_t)n);
}
