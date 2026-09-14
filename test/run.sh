#!/bin/sh
# Host-side tests for the billing-period math (no hardware, no PlatformIO).
set -e
cd "$(dirname "$0")/.."
g++ -std=c++17 -Wall -Wextra -Isrc src/period.cpp test/test_period_math.cpp -o /tmp/period_test
/tmp/period_test
