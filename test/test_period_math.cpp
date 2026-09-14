// Host-side tests for the billing-period math in src/period.cpp.
// No test framework: plain assert-ish checks with PASS/FAIL printf lines.
#include "period.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>

static int g_fail = 0;

static void check(const char* name, bool cond) {
    printf("[%s] %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) g_fail++;
}

// TIME_SANE_EPOCH mirrored from period.cpp (not exported; kept in sync manually).
static const uint32_t TIME_SANE_EPOCH = 1700000000UL;

// Build an epoch in the current TZ. Pass 1 sets TZ=UTC so this is a UTC
// epoch; pass 2 sets TZ=America/New_York to exercise DST-sensitive cases.
static uint32_t local(int year, int mon0, int mday, int hour = 12) {
    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon  = mon0;
    t.tm_mday = mday;
    t.tm_hour = hour;
    t.tm_isdst = -1;
    return (uint32_t)mktime(&t);
}

// Runs the TZ-independent cases (1-6, 8-11). Called once per TZ.
static void runCoreCases() {
    // 1. Mar 31 2025 end -> start is Feb 28 2025 (clamped, non-leap)
    {
        uint32_t end = local(2025, 2, 31); // March (0-indexed=2)
        struct tm t;
        time_t start = (time_t)periodStartFor(end);
        localtime_r(&start, &t);
        check("Mar31 2025 -> Feb28 (non-leap clamp)", t.tm_mon == 1 && t.tm_mday == 28);
    }

    // 2. Mar 31 2024 end -> start is Feb 29 2024 (clamped, leap)
    {
        uint32_t end = local(2024, 2, 31);
        struct tm t;
        time_t start = (time_t)periodStartFor(end);
        localtime_r(&start, &t);
        check("Mar31 2024 -> Feb29 (leap clamp)", t.tm_mon == 1 && t.tm_mday == 29);
    }

    // 3. May 31 end -> start is Apr 30 (30-day month clamp)
    {
        uint32_t end = local(2025, 4, 31);
        struct tm t;
        time_t start = (time_t)periodStartFor(end);
        localtime_r(&start, &t);
        check("May31 -> Apr30 (30-day clamp)", t.tm_mon == 3 && t.tm_mday == 30);
    }

    // 4. Jan 31 end -> start is Dec 31 previous year (year wrap, no clamp)
    {
        uint32_t end = local(2025, 0, 31);
        struct tm t;
        time_t start = (time_t)periodStartFor(end);
        localtime_r(&start, &t);
        check("Jan31 -> Dec31 prev year", t.tm_year == 2024 - 1900 && t.tm_mon == 11 && t.tm_mday == 31);
    }

    // 5. Jan 15 end -> start is Dec 15 previous year (year wrap)
    {
        uint32_t end = local(2025, 0, 15);
        struct tm t;
        time_t start = (time_t)periodStartFor(end);
        localtime_r(&start, &t);
        check("Jan15 -> Dec15 prev year", t.tm_year == 2024 - 1900 && t.tm_mon == 11 && t.tm_mday == 15);
    }

    // 6. Feb 29 2024 end -> start is Jan 29 2024 (leap-day end)
    {
        uint32_t end = local(2024, 1, 29);
        struct tm t;
        time_t start = (time_t)periodStartFor(end);
        localtime_r(&start, &t);
        check("Feb29 2024 -> Jan29 (leap-day end)", t.tm_mon == 0 && t.tm_mday == 29);
    }

    // 8. periodStartFor(0) and periodElapsedPct(0, now) -> 0, no crash
    {
        uint32_t s = periodStartFor(0);
        float pct = periodElapsedPct(0, (uint32_t)time(nullptr));
        check("periodStartFor(0) == 0", s == 0);
        check("periodElapsedPct(0, now) == 0", pct == 0.0f);
    }

    // 9. now below TIME_SANE_EPOCH -> 0
    {
        uint32_t end = local(2025, 5, 30);
        float pct = periodElapsedPct(end, TIME_SANE_EPOCH - 1);
        check("now below TIME_SANE_EPOCH -> 0", pct == 0.0f);
    }

    // 10. now exactly at start -> 0; at end -> 100; at midpoint -> ~50
    {
        uint32_t end = local(2025, 5, 30, 0);
        uint32_t start = periodStartFor(end);
        float pStart = periodElapsedPct(end, start);
        float pEnd   = periodElapsedPct(end, end);
        uint32_t mid = start + (end - start) / 2;
        float pMid   = periodElapsedPct(end, mid);
        check("now==start -> 0", pStart == 0.0f);
        check("now==end -> 100", pEnd == 100.0f);
        check("now==midpoint -> ~50", fabsf(pMid - 50.0f) < 1.0f);
    }

    // 11. now before start -> clamped to 0; after end -> clamped to 100
    {
        uint32_t end = local(2025, 5, 30, 0);
        uint32_t start = periodStartFor(end);
        float pBefore = periodElapsedPct(end, start - 3600);
        float pAfter  = periodElapsedPct(end, end + 3600);
        check("now before start -> clamped 0", pBefore == 0.0f);
        check("now after end -> clamped 100", pAfter == 100.0f);
    }
}

// Runs the measured-period-length cases (12-15). Called once per TZ.
static void runMeasuredCases() {
    static const uint32_t DAY = 24 * 3600UL;

    // 12. explicit 30-day periodSec: midpoint ~50, start 0, end 100
    {
        uint32_t end = local(2025, 5, 30, 0);
        uint32_t periodSec = 30 * DAY;
        uint32_t start = end - periodSec;
        uint32_t mid = start + periodSec / 2;
        check("measured 30d: start -> 0", periodElapsedPct(end, start, periodSec) == 0.0f);
        check("measured 30d: end -> 100", periodElapsedPct(end, end, periodSec) == 100.0f);
        check("measured 30d: midpoint -> ~50",
              fabsf(periodElapsedPct(end, mid, periodSec) - 50.0f) < 1.0f);
    }

    // 13. a measured length that differs from the calendar-month assumption
    // produces a different, correct result than periodSec=0 — proves the
    // measurement actually takes effect.
    {
        uint32_t end = local(2025, 4, 15, 0); // May 15 2025 (calendar-month back is Apr, 30 days)
        uint32_t periodSec = 28 * DAY;        // measured: 28 days, not calendar-month
        uint32_t now = end - 7 * DAY;         // 7 days before end

        float pctMeasured = periodElapsedPct(end, now, periodSec);
        float pctCalendar = periodElapsedPct(end, now, 0);

        float expected = 100.0f * (float)(periodSec - 7 * DAY) / (float)periodSec;
        check("measured 28d differs from calendar-month path",
              fabsf(pctMeasured - pctCalendar) > 1.0f);
        check("measured 28d matches expected elapsed pct",
              fabsf(pctMeasured - expected) < 0.5f);
    }

    // 14. periodSec >= periodEnd falls back to the calendar-month path
    // instead of underflowing.
    {
        uint32_t end = local(2025, 5, 30, 0);
        float pctFallback = periodElapsedPct(end, end, end); // periodSec == end
        float pctCalendar = periodElapsedPct(end, end, 0);
        check("periodSec >= periodEnd falls back (no underflow)",
              pctFallback == pctCalendar);
    }

    // 15. periodElapsedPct(end, now, 0) matches the 2-arg version exactly.
    {
        uint32_t end = local(2025, 5, 30, 0);
        uint32_t start = periodStartFor(end);
        uint32_t mid = start + (end - start) / 2;
        check("periodSec=0 matches 2-arg (start)",
              periodElapsedPct(end, start, 0) == periodElapsedPct(end, start));
        check("periodSec=0 matches 2-arg (mid)",
              periodElapsedPct(end, mid, 0) == periodElapsedPct(end, mid));
        check("periodSec=0 matches 2-arg (end)",
              periodElapsedPct(end, end, 0) == periodElapsedPct(end, end));
    }
}

int main() {
    // Pass 1: UTC, deterministic regardless of host TZ.
    setenv("TZ", "UTC", 1);
    tzset();
    printf("--- TZ=UTC ---\n");
    runCoreCases();
    runMeasuredCases();

    // Pass 2: America/New_York, to exercise DST-affected local-time math.
    setenv("TZ", "America/New_York", 1);
    tzset();
    printf("--- TZ=America/New_York ---\n");
    runCoreCases();
    runMeasuredCases();

    // 7. Period spanning the US DST spring-forward: end Apr 1 2025, start
    // should be Mar 1 2025, and the elapsed pct must stay within 0..100
    // despite the one-hour local-time gap on Mar 9 2025.
    {
        uint32_t end = local(2025, 3, 1, 12); // Apr 1 2025 12:00 local
        struct tm t;
        time_t start = (time_t)periodStartFor(end);
        localtime_r(&start, &t);
        bool startOk = (t.tm_mon == 2 && t.tm_mday == 1); // Mar 1
        check("DST spring-forward: start == Mar1", startOk);

        // sample a handful of points across the window, including the DST
        // transition itself (Mar 9 2025 02:00 local -> 03:00 local)
        uint32_t samples[] = {
            (uint32_t)start,
            local(2025, 2, 9, 1),   // just before the spring-forward
            local(2025, 2, 9, 4),   // just after
            local(2025, 2, 20, 12), // mid-window
            end,
        };
        bool allInRange = true;
        for (uint32_t s : samples) {
            float pct = periodElapsedPct(end, s);
            if (pct < 0.0f || pct > 100.0f) allInRange = false;
        }
        check("DST spring-forward: all samples in [0,100]", allInRange);
    }

    // 16. periodLooksPlausible: false outside ~20..45 days, true inside.
    {
        static const uint32_t DAY = 24 * 3600UL;
        check("plausible: 0 -> false", periodLooksPlausible(0) == false);
        check("plausible: 19d -> false", periodLooksPlausible(19 * DAY) == false);
        check("plausible: 46d -> false", periodLooksPlausible(46 * DAY) == false);
        check("plausible: 10y -> false", periodLooksPlausible(10UL * 365 * DAY) == false);
        check("plausible: 28d -> true", periodLooksPlausible(28 * DAY) == true);
        check("plausible: 30d -> true", periodLooksPlausible(30 * DAY) == true);
        check("plausible: 31d -> true", periodLooksPlausible(31 * DAY) == true);
    }

    printf("\n%d failing check(s)\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
