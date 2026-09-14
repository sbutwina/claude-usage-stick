#pragma once
#include <stdint.h>

// Enterprise spend limits are billed monthly; the rate-limit headers expose
// only the period end, so the start is that timestamp one calendar month
// earlier (day-of-month clamped to the shorter month).
uint32_t periodStartFor(uint32_t periodEnd);

// Percent of the billing period elapsed at `now`, 0..100. Returns 0 when
// periodEnd is 0, when `now` is below the NTP sanity floor, or when the
// derived window is degenerate.
float periodElapsedPct(uint32_t periodEnd, uint32_t now);

// Percent of the billing period elapsed at `now`, 0..100. `periodSec` is the
// measured period length when one has been observed, or 0 to fall back to the
// calendar-month assumption.
float periodElapsedPct(uint32_t periodEnd, uint32_t now, uint32_t periodSec);

// A measured period is only trusted if it looks like a real billing month.
// Guards against clock skew, a changed account, or a garbage header.
bool periodLooksPlausible(uint32_t periodSec);   // true for ~20..45 days
