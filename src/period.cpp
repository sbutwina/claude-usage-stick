#include "period.h"
#include <time.h>
#include <math.h>

// NTP sanity floor (mirrors src/history.cpp's TIME_SANE_EPOCH).
static const uint32_t TIME_SANE_EPOCH = 1700000000UL;

// A measured period must look like a real billing month to be trusted.
static const uint32_t PERIOD_MIN_SEC = 20UL * 24 * 3600;
static const uint32_t PERIOD_MAX_SEC = 45UL * 24 * 3600;

static bool isLeapYear(int year) {
    return (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0);
}

static int daysInMonth(int year1900, int mon) {
    static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int year = year1900 + 1900;
    if (mon == 1 && isLeapYear(year)) return 29;
    return kDays[mon];
}

// Enterprise spend limits are billed monthly; the headers expose only the
// period end, so the period start is that timestamp one calendar month
// earlier (day-of-month clamped to the shorter month).
uint32_t periodStartFor(uint32_t periodEnd) {
    // Stepping a month back from the epoch lands before 1970; the cast back to
    // uint32_t would wrap to a bogus far-future date. 0 in, 0 out.
    if (periodEnd == 0) return 0;

    time_t end = (time_t)periodEnd;
    struct tm tmEnd;
    localtime_r(&end, &tmEnd);

    tmEnd.tm_mon--;
    if (tmEnd.tm_mon < 0) {
        tmEnd.tm_mon = 11;
        tmEnd.tm_year--;
    }
    int maxDay = daysInMonth(tmEnd.tm_year, tmEnd.tm_mon);
    if (tmEnd.tm_mday > maxDay) tmEnd.tm_mday = maxDay;

    tmEnd.tm_isdst = -1;
    return (uint32_t)mktime(&tmEnd);
}

float periodElapsedPct(uint32_t periodEnd, uint32_t now) {
    return periodElapsedPct(periodEnd, now, 0);
}

float periodElapsedPct(uint32_t periodEnd, uint32_t now, uint32_t periodSec) {
    if (periodEnd == 0) return 0.0f;
    if (now < TIME_SANE_EPOCH) return 0.0f;

    time_t end = (time_t)periodEnd;
    time_t start;
    if (periodSec > 0 && periodSec < periodEnd) {
        start = (time_t)(periodEnd - periodSec);
    } else {
        start = (time_t)periodStartFor(periodEnd);
    }
    if (start <= 0 || end <= start) return 0.0f;

    float pct = 100.0f * (float)((time_t)now - start) / (float)(end - start);
    if (pct < 0.0f) return 0.0f;
    if (pct > 100.0f) return 100.0f;
    return pct;
}

bool periodLooksPlausible(uint32_t periodSec) {
    return periodSec >= PERIOD_MIN_SEC && periodSec <= PERIOD_MAX_SEC;
}
