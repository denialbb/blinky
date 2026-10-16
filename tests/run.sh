#!/usr/bin/env bash
# Integration test without root: point the daemon at a regular file and check
# state arbitration plus that the PWM engine actually toggles the "LED".
set -euo pipefail
cd "$(dirname "$0")/.."
make -s

tmp=$(mktemp -d)
cleanup() {
    BLINKY_BASE="$tmp" BLINKY_LED="$tmp/led" ./blinky stop >/dev/null 2>&1 || true
    rm -rf "$tmp"
}
trap cleanup EXIT

export BLINKY_BASE="$tmp"
export BLINKY_LED="$tmp/led"
: > "$tmp/led"

fail() { echo "FAIL: $*" >&2; exit 1; }

./blinky pattern heartbeat
sleep 0.4
grep -q "heartbeat" "$tmp/blinky/status" || fail "status not heartbeat: $(cat "$tmp/blinky/status")"
grep -q "pwm=on" "$tmp/blinky/status" || fail "PWM not enabled on fast device"

seen_on=0
seen_off=0
for _ in $(seq 1 200); do
    v=$(cat "$tmp/led" 2>/dev/null || true)
    case "$v" in 1*) seen_on=1 ;; 0*) seen_off=1 ;; esac
    sleep 0.005
done
[ "$seen_on" = 1 ] || fail "PWM never turned the LED on"
[ "$seen_off" = 1 ] || fail "PWM never turned the LED off"

# arbitration: needs-input outranks running
./blinky set alpha running
./blinky set beta needs-input
sleep 0.1
grep -q "needs-input" "$tmp/blinky/status" || fail "needs-input should win"

./blinky off beta
sleep 0.1
grep -q "running" "$tmp/blinky/status" || fail "running should take over after clearing"

./blinky off
sleep 0.1
grep -q "off" "$tmp/blinky/status" || fail "off should clear all agents"

./blinky pause
./blinky set gamma running
sleep 0.1
grep -q "off" "$tmp/blinky/status" || fail "pause should suppress output"
./blinky resume
sleep 0.1
grep -q "running" "$tmp/blinky/status" || fail "resume should re-enable output"

./blinky stop >/dev/null
echo "ok: blinky integration test passed"
