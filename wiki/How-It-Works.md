# How it works

The whole device is a loop: ask the Anthropic API a trivial question, read the rate-limit headers off the answer, draw them.

## The polling loop

1. The device sends a minimal request to the Anthropic Messages endpoint using your OAuth token — `max_tokens: 1`, so it costs essentially nothing.
2. It ignores the response body and reads the rate-limit headers, in one of two shapes depending on account type:
   - **Pro/Max:** `anthropic-ratelimit-unified-5h-utilization` and `anthropic-ratelimit-unified-7d-utilization`
   - **Enterprise / org spend limit:** `anthropic-ratelimit-unified-overage-utilization` and `anthropic-ratelimit-unified-overage-reset`, read when the 5h header is absent
3. It draws those percentages as bars, along with the reset countdowns.
4. It sleeps until the next poll — the interval is configurable from 30 s to 5 min.

On [Mango](The-UI) firmware the device also fetches model health from [status.claude.com](https://status.claude.com) and draws the Haiku / Sonnet / Opus / Fable mascots.

On [Dust](The-UI#what-dust-adds) firmware (v3) two more things happen:

- Every successful poll drops one sample into a **7-day history ring** (one slot per 30 minutes, ~0.7 KB) persisted on the device's own flash — that's what the chart screen and the panel's chart draw. It records whichever header pair is live (5h/7d or spend/period) and clears itself if the account type changes. Time the device spends off shows up as gaps, honestly.
- Every 6 hours it streams the **Anthropic news feed** from `raw.githubusercontent.com`, reads just the first five headlines (~10 KB of a ~200 KB file) and hangs up.

## Where your token goes

Nowhere except Anthropic. There is no backend, no telemetry, and no cloud service in the middle — the device talks straight to `api.anthropic.com` over HTTPS. The token itself is stored encrypted on the device's own flash; see [Security](Security).

## Rate-limit headers and your plan

Two account types publish usable headers, and the device draws the same two bars for both:

- **Pro/Max** subscriptions return the unified 5h/7d headers. Bar 1 is **5-HOUR**, bar 2 is **7-DAY**, each with its own reset countdown.
- **Enterprise accounts with an organization spend limit** return the overage headers instead: `anthropic-ratelimit-unified-overage-utilization` (spend against the org's limit) and `anthropic-ratelimit-unified-overage-reset` (epoch end of the monthly billing period). Bar 1 becomes **SPEND** (percent of the org limit consumed, counting down to period end) and bar 2 becomes **PERIOD** (percent of the billing cycle elapsed, trailing with a projected end-of-period spend instead of a countdown: `SPEND% ÷ PERIOD%`, e.g. `~140%`, capped at `~999%` — over 100% means the org is on pace to blow through its limit before the period ends). That projection is undefined until at least 5% of the period has elapsed (a single call on day one would project to thousands of percent), so it shows `--` until then. The headers only expose the period's end, so until the device has seen a full period roll over, it assumes the period started one calendar month earlier — Anthropic's spend-limit `period` field documents "monthly" as the only value today. Watching SPEND against PERIOD shows pace: SPEND ahead of PERIOD means the org is burning faster than the month is passing.
  - Note: these headers carry no dollar amounts, only the 0.0–1.0 ratios above — no limit value, no remaining budget.
  - **Measuring the real period length:** that calendar-month assumption is only a bootstrap. The device remembers the reset epoch from `anthropic-ratelimit-unified-overage-reset`, and when it changes, that's a rollover — the gap between the old and new epoch is the org's actual period length. If it's plausible (roughly 20–45 days) it's persisted to NVS and used from then on; an implausible gap is rejected and the calendar-month assumption stays in effect. The measurement is logged to serial and exposed in the web panel's `/api/state` as `period_sec` and `period_measured`.

### Why there's no dollar figure

It would be nice to show "$340 of $500" instead of a percentage, but nothing available to the device can produce that safely:

- The rate-limit headers themselves carry no currency — only the ratios above.
- There is an endpoint that returns real currency, `GET /api/oauth/usage`, whose response carries a `spend` object with `used`/`limit` as `{amount_minor, currency, exponent}`. The repo's optional `server/usage_proxy.py` (below) reads it successfully using the Claude Code OAuth token from the macOS Keychain. But the token this device carries — created with `claude setup-token` — is refused by that same endpoint, returning HTTP 403 and then 429 (`rate_limit_error`) on repeated attempts, while the Keychain token succeeded from the same machine at the same time. So the firmware doesn't use it, and the device shows percentages only.
- The Admin API's `GET /v1/organizations/cost_report` does return real USD, but needs an `sk-ant-admin01-...` admin key or an `org:admin` OAuth token — a credential that can manage org members, workspaces and API keys, which doesn't belong on this device. It also reports spend without the limit, so it couldn't render "$340 of $500" anyway.

**API-billed accounts still emit neither header set** — the request succeeds with HTTP 200, but no usage headers come back, and the device can't show anything.

If your device reports `no_usage_h_200`, that's what happened: the token is valid, but the account behind it doesn't publish unified or overage usage. See [Troubleshooting](Troubleshooting#the-device-shows-no_usage_h_200).

## Optional local proxy

The repo ships `server/usage_proxy.py`, a small caching proxy that reads the token from the macOS Keychain. It's useful if you'd rather not put a token on the device at all, or if you want several devices sharing one upstream poll. It is entirely optional — the device works standalone without it.
