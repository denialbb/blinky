# blinky

ThinkPad status LEDs and webcam indicator for coding agents.

`blinky` turns ThinkPad status LEDs (`tpacpi::power`, `tpacpi::thinkvantage`, `tpacpi::lid_logo_dot`, `tpacpi::standby`) and the integrated webcam indicator into live status lamps for coding agents:
- **Running / Heartbeat**: slow steady blink, smoothly fading in and out via software PWM
- **Needs-input**: fast triple-blink alert (and optional webcam LED solid indicator)
- **Done**: three clean finish flashes, then settles
- **Error**: Morse SOS pattern
- **Clean exit & idle**: restores the exact LED status that existed before the notification/toggle

```
 Slow steady blink (fade in & out)     needs-input               error (SOS)
        ▁▃▅▇█▇▅▃▁▁                     ▌▌▌  ▁▁▁▁▁                ▄ ▄ ▄  █ █ █  ▄ ▄ ▄
 (smooth gamma-corrected fade)       (triple blink)            (... --- ...)
```

---

## Hardware & Architecture

### ThinkPad xx30 (X230, T430, W530) ThinkLight Limitation
On xx30 ThinkPads, the ThinkLight (`tpacpi::thinklight`) **cannot be turned on via software**.
Lenovo removed the ACPI `lght` method, leaving `thinkpad_acpi` to use legacy UCMS/CMOS commands 12/13. In firmware, writing `1` is a silent no-op; only `0` (off) works from Linux. The flood light can only be turned on by the hardware EC hotkey (`Fn+Space` / `Fn+PgUp`).

### Supported LEDs
`blinky` drives the LEDs that *do* work:
- `tpacpi::power` (**default** — power button LED, visible next to keyboard)
- `tpacpi::thinkvantage` (blue ThinkVantage button LED)
- `tpacpi::lid_logo_dot` (red dot in the ThinkPad lid logo)
- `tpacpi::standby` (crescent moon sleep LED)

All probe at >400,000 toggles/s via sysfs and support smooth software PWM dimming.

### Webcam Indicator
The integrated webcam (Bison `5986:02d2`, `/dev/video0`) has no independent LED sysfs control (`V4L2_CID_PRIVACY` does not light it). It only lights when an active video stream is running. `blinky webcam watch` polls status and streams `v4l2-ctl` while agents are in urgent states (`needs-input,error`), providing a bright binary green indicator lamp.

---

## Install

```sh
make
sudo ./install.sh          # installs binary + udev rule + trigger modules
blinky probe               # verify LED access, toggles/s, and readback
blinky pattern running     # test the slow steady fade
```

Non-root install copies the binary to `~/.local/bin/blinky` and installs systemd user units to `~/.config/systemd/user/`.

Run the test suite anytime without root:

```sh
bash tests/run.sh
```

---

## Usage

```sh
blinky set <agent> <state>     # running | needs-input | done | error | off
blinky pattern <name>          # force a pattern: running | heartbeat | needs-input | done | error | off
blinky off [agent]             # clear one agent or all
blinky pause | resume          # temporarily suspend LED updates
blinky status                  # daemon status, active pattern, and webcam state
blinky start | stop            # start daemon in background / stop all daemons
blinky led on|off              # raw direct write, bypasses daemon
blinky calibrate               # measure max toggle rate and PWM feasibility
blinky probe                   # probe all candidate LEDs and toggle frequencies
blinky webcam on|off|watch|status [dev]  # manage webcam status indicator
```

### State Arbitration
Per-agent states are arbitrated by priority. Multiple agents running across terminal panes or tmux sessions collapse into the most urgent state:

```
needs-input  >  error  >  running  >  done  >  off
```

### Pre-existing State Restoration
When all agent notifications clear (`off` or `done`), or when the daemon exits (`blinky stop` or SIGTERM/SIGINT), `blinky` automatically restores every LED to its original pre-existing status (e.g. keeping `tpacpi::power` illuminated if it was ON before the agent ran).

---

## Systemd Services

Enable the daemon and optional webcam watcher at login:

```sh
systemctl --user enable --now blinky.service
systemctl --user enable --now blinky-webcam.service
```

---

## Agent Integration

`blinky` integrates directly with coding agents through `agent-notify.sh` or lifecycle hooks:

### Central Choke Point (`~/.local/bin/agent-notify.sh`)
Add one line near the top of `agent-notify.sh`:
```bash
if command -v blinky >/dev/null 2>&1; then
    blinky set "$AGENT" "$STATE" >/dev/null 2>&1 &
fi
```

### Direct Agent Integrations
- **Pi**: [`~/.pi/agent/extensions/blinky-indicator.ts`](file:///home/denial/.pi/agent/extensions/blinky-indicator.ts) listens to `agent_start` (running), `agent_settled` (done), and `session_shutdown` (off).
- **OpenCode**: [`~/.config/opencode/plugins/opencode-tmux-agent-indicator.js`](file:///home/denial/.config/opencode/plugins/opencode-tmux-agent-indicator.js) already invokes `agent-notify.sh`.
- **Claude / Codex / Antigravity**: invoke `blinky set <agent> <state>` from `PreInvocation`, `PostInvocation`, or lifecycle hook scripts.

---

## Environment Variables

| Variable | Default | Meaning |
|---|---|---|
| `BLINKY_LED` | `tpacpi::power` | LED target: short name, comma-separated list, `auto`, or sysfs path |
| `BLINKY_BASE` | `$XDG_RUNTIME_DIR/blinky` | Base runtime directory for FIFO, PID, and status files |
| `BLINKY_PWM` | `1` | Set `0` to disable PWM and force binary square-wave patterns |
| `BLINKY_PWM_HZ` | `100` | Carrier frequency for software PWM |
| `BLINKY_RECALIBRATE` | `0` | Set `1` to bypass cached calibration and re-measure |
| `BLINKY_WEBCAM_DEV` | `/dev/video0` | Video device node for webcam streaming indicator |
| `BLINKY_WEBCAM_STATES`| `needs-input,error` | Comma-separated agent states that trigger webcam light |

---

## Demo & Investigation

Run the interactive demo script to test all patterns and timings on physical hardware:

```sh
tools/demo.sh [led-path] [loops]
```
*(Automatically captures and restores initial LED state on exit).*

See [`REPORT.md`](file:///home/denial/Projects/blinky/REPORT.md) for full kernel source verification, firmware analysis, and benchmark measurements.
