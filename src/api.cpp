#include "api.h"
#include "config.h"
#include "certs.h"
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>

static const char* RL_HEADERS[] = {
    "anthropic-ratelimit-unified-5h-utilization",
    "anthropic-ratelimit-unified-5h-reset",
    "anthropic-ratelimit-unified-7d-utilization",
    "anthropic-ratelimit-unified-7d-reset",
    "anthropic-ratelimit-unified-overage-utilization",
    "anthropic-ratelimit-unified-overage-reset",
    "anthropic-ratelimit-unified-status",
    "anthropic-ratelimit-unified-5h-status",
};
static const int RL_HEADER_COUNT = sizeof(RL_HEADERS) / sizeof(RL_HEADERS[0]);

// NTP sanity floor (mirrors src/history.cpp's TIME_SANE_EPOCH).
static const uint32_t TIME_SANE_EPOCH = 1700000000UL;

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
static time_t periodStartFor(time_t end) {
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
    return mktime(&tmEnd);
}

static float periodElapsedPct(uint32_t periodEnd) {
    if (periodEnd == 0) return 0.0f;
    time_t now = time(nullptr);
    if ((uint32_t)now < TIME_SANE_EPOCH) return 0.0f;

    time_t end = (time_t)periodEnd;
    time_t start = periodStartFor(end);
    if (start <= 0 || end <= start) return 0.0f;

    float pct = 100.0f * (float)(now - start) / (float)(end - start);
    return constrain(pct, 0.0f, 100.0f);
}

bool fetchUsage(const char* token, UsageData& out) {
    out.status[0] = '\0';
    out.acct = ACCT_PRO;

    WiFiClientSecure client;
    client.setCACert(CA_BUNDLE);

    HTTPClient https;
    if (!https.begin(client, MESSAGES_ENDPOINT)) {
        strlcpy(out.error, "https_init", sizeof(out.error));
        out.ok = false;
        return false;
    }

    https.addHeader("Authorization", String("Bearer ") + token);
    https.addHeader("anthropic-version", ANTHROPIC_VERSION);
    https.addHeader("anthropic-beta", "oauth-2025-04-20");
    https.addHeader("content-type", "application/json");
    https.addHeader("User-Agent", "claude-code/2.1.5");
    https.setTimeout(API_TIMEOUT_MS);
    https.collectHeaders(RL_HEADERS, RL_HEADER_COUNT);

    String body = "{\"model\":\"" PROBE_MODEL "\","
                  "\"max_tokens\":1,"
                  "\"messages\":[{\"role\":\"user\",\"content\":\".\"}]}";

    Serial.printf("[API] POST %s\n", MESSAGES_ENDPOINT);
    int code = https.POST(body);
    Serial.printf("[API] HTTP %d\n", code);

    if (code <= 0) {
        snprintf(out.error, sizeof(out.error), "http_%d", code);
        out.ok = false;
        https.end();
        return false;
    }

    String h5u = https.header("anthropic-ratelimit-unified-5h-utilization");
    String h5r = https.header("anthropic-ratelimit-unified-5h-reset");
    String d7u = https.header("anthropic-ratelimit-unified-7d-utilization");
    String d7r = https.header("anthropic-ratelimit-unified-7d-reset");
    String ovu = https.header("anthropic-ratelimit-unified-overage-utilization");
    String ovr = https.header("anthropic-ratelimit-unified-overage-reset");
    String stU = https.header("anthropic-ratelimit-unified-status");
    String st5 = https.header("anthropic-ratelimit-unified-5h-status");

    // dump every collected header so a real org account's names can be confirmed
    for (int i = 0; i < RL_HEADER_COUNT; i++) {
        String v = https.header(RL_HEADERS[i]);
        Serial.printf("[API] %s: %s\n", RL_HEADERS[i], v.length() ? v.c_str() : "(empty)");
    }

    https.end();

    bool isPro = h5u.length() > 0;
    bool isOrg = !isPro && ovu.length() > 0;

    if (!isPro && !isOrg) {
        if (code == 401) {
            strlcpy(out.error, "auth_failed", sizeof(out.error));
        } else {
            snprintf(out.error, sizeof(out.error), "no_usage_h_%d", code);
        }
        out.ok = false;
        return false;
    }

    if (isPro) {
        // utilization is 0.0–1.0, convert to percentage
        out.h5 = h5u.toFloat() * 100.0f;
        out.d7 = d7u.toFloat() * 100.0f;
        out.h5ResetEpoch = (uint32_t)h5r.toInt();
        out.d7ResetEpoch = (uint32_t)d7r.toInt();
        out.acct = ACCT_PRO;
        strlcpy(out.status, st5.c_str(), sizeof(out.status));
    } else {
        out.h5           = ovu.toFloat() * 100.0f;
        out.h5ResetEpoch = (uint32_t)ovr.toInt();
        out.d7           = periodElapsedPct(out.h5ResetEpoch);
        out.d7ResetEpoch = 0;
        out.acct         = ACCT_ORG;
        strlcpy(out.status, stU.c_str(), sizeof(out.status));
    }

    out.ok = true;
    return true;
}

const char* usageLabel(const UsageData& u, int idx) {
    static const char* kPro[2] = {"5-HOUR", "7-DAY"};
    static const char* kOrg[2] = {"SPEND",  "PERIOD"};
    if (idx < 0 || idx > 1) return "";
    return (u.acct == ACCT_ORG) ? kOrg[idx] : kPro[idx];
}

const char* usageLabelShort(const UsageData& u, int idx) {
    static const char* kPro[2] = {"5H", "7D"};
    static const char* kOrg[2] = {"SP", "PD"};
    if (idx < 0 || idx > 1) return "";
    return (u.acct == ACCT_ORG) ? kOrg[idx] : kPro[idx];
}

// Below this much of the period elapsed, the burn rate is noise — a single
// early call would project to thousands of percent.
static const float PACE_MIN_ELAPSED = 5.0f;

float usageProjectedPct(const UsageData& u) {
    if (u.acct != ACCT_ORG || u.d7 < PACE_MIN_ELAPSED) return -1.0f;
    return u.h5 * 100.0f / u.d7;
}

const char* usageSlotCaption(const UsageData& u, int idx) {
    static const char* kReset[2] = {"RESET", "RESET"};
    static const char* kOrg[2]   = {"RESET", "PROJ"};
    if (idx < 0 || idx > 1) return "";
    return (u.acct == ACCT_ORG) ? kOrg[idx] : kReset[idx];
}

const char* usageSlotCaptionShort(const UsageData& u, int idx) {
    static const char* kReset[2] = {"RST", "RST"};
    static const char* kOrg[2]   = {"RST", "PRJ"};
    if (idx < 0 || idx > 1) return "";
    return (u.acct == ACCT_ORG) ? kOrg[idx] : kReset[idx];
}

#ifdef PANEL_DEBUG
// Runnable check for periodElapsedPct's calendar-month step-back.
// Call apiSelfCheck() from setup() manually when debugging.
void apiSelfCheck() {
    // 1. zero period end -> 0
    float r1 = periodElapsedPct(0);
    Serial.printf("[SELFCHECK] periodElapsedPct(0)==0: %s (%f)\n", r1 == 0.0f ? "PASS" : "FAIL", r1);

    // 2. Mar 31 period end -> start clamps to Feb 28/29, not Mar 2/3.
    struct tm tmMar31 = {};
    tmMar31.tm_year = 2025 - 1900;
    tmMar31.tm_mon  = 2; // March (0-indexed)
    tmMar31.tm_mday = 31;
    tmMar31.tm_hour = 12;
    tmMar31.tm_isdst = -1;
    time_t marEnd = mktime(&tmMar31);
    time_t start = periodStartFor(marEnd);
    struct tm tmStart;
    localtime_r(&start, &tmStart);
    bool pass2 = (tmStart.tm_mon == 1) && (tmStart.tm_mday == 28 || tmStart.tm_mday == 29);
    Serial.printf("[SELFCHECK] Mar31 step-back clamps to Feb 28/29: %s (mon=%d mday=%d)\n",
                  pass2 ? "PASS" : "FAIL", tmStart.tm_mon, tmStart.tm_mday);

    // 3. far-future period end -> result in [0,100]
    time_t future = time(nullptr) + (3600L * 24 * 20); // ~20 days out
    if ((uint32_t)time(nullptr) < TIME_SANE_EPOCH) future = TIME_SANE_EPOCH + (3600L * 24 * 20);
    float pct = periodElapsedPct((uint32_t)future);
    bool pass3 = pct >= 0.0f && pct <= 100.0f;
    Serial.printf("[SELFCHECK] far-future pct in [0,100]: %s (%f)\n", pass3 ? "PASS" : "FAIL", pct);

    // 4. Pro/Max account -> projection undefined regardless of h5/d7
    UsageData uPro = {};
    uPro.acct = ACCT_PRO;
    uPro.h5 = 50.0f;
    uPro.d7 = 50.0f;
    float r4 = usageProjectedPct(uPro);
    Serial.printf("[SELFCHECK] Pro acct projection==-1: %s (%f)\n", r4 == -1.0f ? "PASS" : "FAIL", r4);

    // 5. org acct, too early in period -> undefined
    UsageData uEarly = {};
    uEarly.acct = ACCT_ORG;
    uEarly.h5 = 10.0f;
    uEarly.d7 = 2.0f;
    float r5 = usageProjectedPct(uEarly);
    Serial.printf("[SELFCHECK] org early-period projection==-1: %s (%f)\n", r5 == -1.0f ? "PASS" : "FAIL", r5);

    // 6. org acct, past the noise floor -> h5/d7*100
    UsageData uOrg = {};
    uOrg.acct = ACCT_ORG;
    uOrg.h5 = 60.0f;
    uOrg.d7 = 30.0f;
    float r6 = usageProjectedPct(uOrg);
    bool pass6 = fabsf(r6 - 200.0f) < 0.01f;
    Serial.printf("[SELFCHECK] org projection 60/30->200: %s (%f)\n", pass6 ? "PASS" : "FAIL", r6);
}
#endif
