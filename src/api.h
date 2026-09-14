#pragma once
#include <stdint.h>

enum AcctKind : uint8_t { ACCT_PRO = 0, ACCT_ORG = 1 };

struct UsageData {
    float    h5;            // pro: 5h util % | org: spend vs. org limit %
    float    d7;            // pro: 7d util % | org: billing period elapsed %
    uint32_t h5ResetEpoch;  // pro: 5h reset | org: billing period end
    uint32_t d7ResetEpoch;  // pro: 7d reset | org: 0 (period bar has no countdown)
    bool     ok;
    uint8_t  acct;          // ACCT_PRO | ACCT_ORG
    char     status[24];    // rate-limit status string, "" if absent
    char     error[64];
};

bool fetchUsage(const char* token, UsageData& out);

const char* usageLabel(const UsageData& u, int idx);       // "5-HOUR"/"7-DAY" or "SPEND"/"PERIOD"
const char* usageLabelShort(const UsageData& u, int idx);  // "5H"/"7D" or "SP"/"PD"

// Projected end-of-period spend as a percent of the org limit: if the current
// burn rate holds, where the period lands. -1 when undefined (Pro/Max, or too
// early in the period for a rate to mean anything).
float usageProjectedPct(const UsageData& u);

// Caption for a bar's trailing slot — a countdown on Pro/Max, the projection
// on the org PERIOD bar.
const char* usageSlotCaption(const UsageData& u, int idx);       // "RESET" | "PROJ"
const char* usageSlotCaptionShort(const UsageData& u, int idx);  // "RST"   | "PRJ"

#ifdef PANEL_DEBUG
void apiSelfCheck();
#endif
