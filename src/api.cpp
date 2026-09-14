#include "api.h"
#include "config.h"
#include "certs.h"
#include "period.h"
#include "settings.h"
#include "app_state.h"
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

        // The reset epoch changing IS a period rollover; the gap between the
        // old and new epoch IS the real period length. Measure it once,
        // persist it, and use it thereafter instead of assuming a calendar
        // month.
        // A missing or unparseable reset header gives 0 — never let that
        // overwrite a learned baseline, or the next rollover can't be measured.
        uint32_t lastReset = (uint32_t)g_settings.lastResetEpoch;
        if (out.h5ResetEpoch != 0 && lastReset != 0 && out.h5ResetEpoch != lastReset) {
            if (out.h5ResetEpoch > lastReset) {
                uint32_t measured = out.h5ResetEpoch - lastReset;
                bool plausible = periodLooksPlausible(measured);
                Serial.printf("[API] period rolled over: measured %us (%ud) %s\n",
                              (unsigned)measured, (unsigned)(measured / 86400),
                              plausible ? "accepted" : "rejected as implausible");
                if (plausible && (int32_t)measured != g_settings.periodSec) {
                    g_settings.periodSec = (int32_t)measured;
                    settingsPutInt("period_sec", g_settings.periodSec);
                }
            } else {
                // reset epoch went backwards: account change or clock issue.
                // treat as a new baseline, don't measure anything from it.
                Serial.printf("[API] org reset epoch went backwards (was %u, now %u); "
                              "treating as new baseline\n",
                              (unsigned)lastReset, (unsigned)out.h5ResetEpoch);
            }
        }
        if (out.h5ResetEpoch != 0 && out.h5ResetEpoch != lastReset) {
            g_settings.lastResetEpoch = (int32_t)out.h5ResetEpoch;
            settingsPutInt("last_reset", g_settings.lastResetEpoch);
        }

        out.d7           = periodElapsedPct(out.h5ResetEpoch, (uint32_t)time(nullptr),
                                             (uint32_t)g_settings.periodSec);
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
