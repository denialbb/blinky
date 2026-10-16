#!/usr/bin/env bash
# Human-observable LED capability demo.  Run it and watch the lid light.
# Each phase is separated by a dark gap so you can tell them apart.
#
#   tools/demo.sh [led-path] [loops]
#
# Phases per loop:
#   SOLID   : on 2s, off 1s
#   BLINK   : 4 x (0.6s on / 0.6s off)
#   TRIGGER : kernel timer trigger, 100ms on / 900ms off, 4s
#   PWM100  : 100 Hz software PWM, 2s breathing sine, 6s
#   PWM10   : 10 Hz software PWM, 2s breathing sine, 6s

set -u
LED="${1:-/sys/class/leds/tpacpi::power/brightness}"
TRIG="${LED%/brightness}/trigger"
LOOPS="${2:-2}"

ORIG_VAL=$(cat "$LED" 2>/dev/null || echo 0)
ORIG_TRIG=""
if [ -f "$TRIG" ]; then
    ORIG_TRIG=$(sed -n 's/.*\[\([^]]*\)\].*/\1/p' "$TRIG" 2>/dev/null || echo none)
fi

cleanup() {
    if [ -n "$ORIG_TRIG" ] && [ -w "$TRIG" ]; then
        echo "$ORIG_TRIG" > "$TRIG" 2>/dev/null || true
    fi
    printf '%s\n' "$ORIG_VAL" > "$LED" 2>/dev/null || true
}
trap cleanup EXIT INT TERM HUP

w() { printf '%s\n' "$1" > "$LED" 2>/dev/null || return 1; }

pwm() { # pwm <hz> <duration_s>
    python3 - "$LED" "$1" "$2" <<'PY'
import math, os, sys, time
led, hz, dur = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
try:
    fd = os.open(led, os.O_WRONLY)
except OSError as e:
    print(f"pwm: {e}", file=sys.stderr); raise SystemExit(1)
period = 1.0 / hz
t0 = time.monotonic()
while time.monotonic() - t0 < dur:
    t = time.monotonic() - t0
    duty = 0.5 - 0.5 * math.cos(2 * math.pi * t / 2.0)
    on = period * duty
    os.lseek(fd, 0, os.SEEK_SET); os.write(fd, b'1\n')
    time.sleep(on)
    os.lseek(fd, 0, os.SEEK_SET); os.write(fd, b'0\n')
    time.sleep(period - on)
os.close(fd)
PY
}

echo "led=$LED trigger=$TRIG loops=$LOOPS" >&2
for ((l = 1; l <= LOOPS; l++)); do
    echo "[loop $l] SOLID" >&2
    w 1; sleep 2; w 0; sleep 1

    echo "[loop $l] BLINK" >&2
    for _ in 1 2 3 4; do w 1; sleep 0.6; w 0; sleep 0.6; done
    sleep 0.6

    echo "[loop $l] TRIGGER" >&2
    if [ -w "$TRIG" ]; then
        echo timer > "$TRIG" 2>/dev/null
        echo 100 > "${TRIG%/trigger}/delay_on" 2>/dev/null
        echo 900 > "${TRIG%/trigger}/delay_off" 2>/dev/null
        sleep 4
        echo none > "$TRIG" 2>/dev/null
        w 0
    else
        echo "  (trigger not writable)" >&2
    fi
    sleep 0.6

    echo "[loop $l] PWM100" >&2
    pwm 100 6
    w 0; sleep 0.6

    echo "[loop $l] PWM10" >&2
    pwm 10 6
    w 0; sleep 2
done
echo "demo done" >&2
