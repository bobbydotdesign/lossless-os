/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "startlog.h"
#include "los_sys.h"
#include "../lib/lbt/lbt_platform.h"   /* lbt_platform_ms: milliseconds since the kernel started */

#define MARKS 24

static struct { unsigned ms; char what[56]; } marks[MARKS];
static int nmarks;
static bool written;

void startlog_mark(const char *fmt, ...)
{
    unsigned ms = lbt_platform_ms();
    los_lock();
    if (!written && nmarks < MARKS) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(marks[nmarks].what, sizeof(marks[nmarks].what), fmt, ap);
        va_end(ap);
        marks[nmarks++].ms = ms;
    }
    los_unlock();
}

void startlog_write(void)
{
    static char text[MARKS * 72 + 32];
    los_lock();
    if (written) {
        los_unlock();
        return;
    }
    written = true;
    int n = snprintf(text, sizeof(text), "--- start ---\n");
    for (int i = 0; i < nmarks && n < (int)sizeof(text); i++)
        n += snprintf(text + n, sizeof(text) - n, "%6u.%02u s  %s\n", marks[i].ms / 1000,
                      marks[i].ms % 1000 / 10, marks[i].what);
    los_unlock();
    if (n >= (int)sizeof(text))
        n = (int)sizeof(text) - 1;
    char path[160];
    snprintf(path, sizeof(path), "%s/.lossless/logs", los_card_path());
    los_mkdir(path);
    strncat(path, "/start.txt", sizeof(path) - strlen(path) - 1);
    los_file_append(path, text, (size_t)n);
}
