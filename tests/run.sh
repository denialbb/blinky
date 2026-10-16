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

# webcam tests using mock v4l2-ctl
mkdir -p "$tmp/bin"
cat <<'EOF' > "$tmp/bin/v4l2-ctl"
#!/bin/sh
exec sleep 1000
EOF
chmod +x "$tmp/bin/v4l2-ctl"

ORIG_PATH="$PATH"
export PATH="$tmp/bin:$PATH"

./blinky webcam on
./blinky webcam status | grep -q "streaming" || fail "webcam status should be streaming"
[ -f "$tmp/blinky/webcam.pid" ] || fail "webcam.pid missing"

./blinky webcam off
./blinky webcam status | grep -q "off" || fail "webcam status should be off"
[ ! -f "$tmp/blinky/webcam.pid" ] || fail "webcam.pid should be removed"

# test webcam watch reacting to status file
./blinky start
./blinky webcam watch &
WATCH_PID=$!
sleep 0.4
[ -f "$tmp/blinky/webcam_watch.pid" ] || fail "webcam_watch.pid missing"

./blinky set agent1 running
sleep 0.6
./blinky webcam status | grep -q "off" || fail "webcam should be off during running state"

./blinky set agent2 needs-input
sleep 0.6
./blinky webcam status | grep -q "streaming" || fail "webcam should stream during needs-input"

./blinky off agent2
sleep 0.6
./blinky webcam status | grep -q "off" || fail "webcam should stop after clearing needs-input"

kill "$WATCH_PID" 2>/dev/null || true
wait "$WATCH_PID" 2>/dev/null || true
./blinky stop >/dev/null

export PATH="$ORIG_PATH"

# test LED restoration: initial status 1 must be restored on exit
echo "1" > "$tmp/led"
./blinky start
sleep 0.2
./blinky set alpha running
sleep 0.4
./blinky stop >/dev/null
[ "$(cat "$tmp/led")" = "1" ] || fail "LED status 1 was not restored on stop (got $(cat "$tmp/led"))"

# test demo.sh restores initial status
echo "1" > "$tmp/led"
tools/demo.sh "$tmp/led" 1 >/dev/null 2>&1
[ "$(cat "$tmp/led")" = "1" ] || fail "demo.sh did not restore LED status 1 (got $(cat "$tmp/led"))"

echo "ok: blinky integration test passed"
