#!/usr/bin/env bash
# One-time setup.  Root is only needed for the udev rule / modules; the binary
# itself runs as your user.
set -euo pipefail
cd "$(dirname "$0")"

make

if [ "$(id -u)" -eq 0 ]; then
    install -Dm755 blinky /usr/local/bin/blinky
    install -Dm644 udev/70-blinky.rules /etc/udev/rules.d/70-blinky.rules
    install -Dm644 modules-load/blinky.conf /etc/modules-load.d/blinky.conf
    udevadm control --reload
    udevadm trigger --subsystem-match=leds
    modprobe ledtrig-timer ledtrig-pattern ledtrig-heartbeat 2>/dev/null || true
    echo "installed system-wide."
    echo "verify:  blinky calibrate"
    echo "try:     blinky pattern heartbeat"
else
    install -Dm755 blinky "$HOME/.local/bin/blinky"
    install -Dm644 systemd/blinky.service "$HOME/.config/systemd/user/blinky.service"
    install -Dm644 systemd/blinky-webcam.service "$HOME/.config/systemd/user/blinky-webcam.service"
    systemctl --user daemon-reload 2>/dev/null || true
    echo "binary installed to ~/.local/bin/blinky"
    echo "systemd units installed to ~/.config/systemd/user/"
    echo
    echo "LED access still needs one root run (if not already done):"
    echo "  sudo $(pwd)/install.sh"
fi
