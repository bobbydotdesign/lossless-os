/*
 * Dates from a count of seconds (local time, no time zone), and back.
 * Days from 1970-01-01 to a civil date and back, after the method in
 * Howard Hinnant's "chrono-Compatible Low-Level Date Algorithms"
 * (http://howardhinnant.github.io/date_algorithms.html), written here.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOS_CALENDAR_H
#define LOS_CALENDAR_H

#include <stdint.h>

typedef struct { int year, month, day, hour, minute, second, weekday; } los_date;  /* weekday 0 = Sunday */

static inline int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int yoe = (int)(y - era * 400);
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static inline los_date date_from_seconds(int64_t s)
{
    los_date t;
    int64_t z = s >= 0 ? s / 86400 : (s - 86399) / 86400;
    int64_t secs = s - z * 86400;
    t.hour = (int)(secs / 3600);
    t.minute = (int)(secs / 60 % 60);
    t.second = (int)(secs % 60);
    t.weekday = (int)((z % 7 + 11) % 7);          /* 1970-01-01 was a Thursday */
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = (int)(z - era * 146097);
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    t.day = doy - (153 * mp + 2) / 5 + 1;
    t.month = mp < 10 ? mp + 3 : mp - 9;
    t.year = (int)(yoe + era * 400) + (t.month <= 2);
    return t;
}

static inline int64_t seconds_from_date(const los_date *t)
{
    return days_from_civil(t->year, t->month, t->day) * 86400 + t->hour * 3600 + t->minute * 60 +
           t->second;
}

static inline int days_in_month(int y, int m)
{
    static const int days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : days[m - 1];
}

#endif
