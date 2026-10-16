# blinky

ThinkPad ThinkLight as an agent status indicator.

`blinky` turns the lid flood light (`tpacpi::thinklight`) into a status lamp for
coding agents: a dimming heartbeat while an agent works, a triple blink when one
waits for your input, flashes when one finishes.

The ThinkLight is a **binary** LED (`max_brightness = 1`): no hardware dimming and
no hardware blink. `blinky` fakes a dimming heartbeat with software PWM and falls
back to plain on/off patterns when the machine cannot toggle fast enough.

```
 60 bpm heartbeat                 needs-input              running
    ▁▃▅▂▁▁▃▅▂▁▁                    ▌▌▌  ▁▁▁▁▁                ▁▃▅▇▅▃▁▁▃▅▇
 (PWM duty follows the envelope)  (triple blink)        (slow breathing)
```

## Install

```sh
make
sudo ./install.sh          # installs binary + udev rule + trigger modules
blinky calibrate           # verify write access and PWM feasibility
blinky pattern heartbeat   # look at the lid
```

Without root you can still build and run the test suite:

```sh
bash tests/run.sh
```

The udev rule grants write access to the LED (an LED is not sensitive, so `0666`).
The `ledtrig-*` modules are preloaded for the optional kernel-trigger mode.

## Usage

```sh
blinky set <agent> <state>     # running | needs-input | done | error | off
blinky pattern <name>          # force a pattern for testing
blinky off [agent]             # clear one agent or all
blinky pause | resume
blinky status                  # daemon + current pattern
blinky start | stop
blinky led on|off              # raw write, bypasses the daemon
blinky calibrate               # measure max toggle rate / PWM support
```

States are per agent and arbitrated by urgency, so several running agents in
several tmux panes collapse to one sensible light:

```
needs-input  >  error  >  running  >  done  >  off
```

`blinky set` starts the daemon on first use. A user service is provided in
`systemd/blinky.service` if you want it up from login.

## Agent integration

`blinky` is deliberately a dumb sink. Point your existing notify hook at it.
If you use `agent-notify.sh`, append one line:

```sh
[ -x "$HOME/.local/bin/blinky" ] && blinky set "$AGENT" "$STATE" &
```

That is enough for OpenCode (its plugin already calls `agent-notify.sh`). For the
rest, fire the same command from their lifecycle hooks:

| Agent | Where |
|---|---|
| opencode | `~/.config/opencode/plugins/` already calls `agent-notify.sh` |
| claude | `~/.claude/settings.json` hooks: `Stop`, `Notification`, `PreToolUse`, `SessionEnd` |
| codex | `~/.codex/hooks.json` (or its `notify` program) |
| agy / antigravity | `~/.gemini/config/hooks.json` (`PreInvocation` / `AfterInvocation`) |
| pi | `~/.pi/agent/extensions/*.ts`: `agent_start` → running, `ask_user_question` → needs-input, `agent_settled` → done, `session_shutdown` → off |

Direct use is fine too:

```sh
blinky set pi running
blinky set pi needs-input
blinky set pi done
```

## Patterns

| Pattern | Look | Used for |
|---|---|---|
| `heartbeat` | two-thump cardiac pulse, PWM dimming | manual / demo |
| `running` | slow breathing | agent working |
| `needs-input` | triple blink | agent blocked on you |
| `done` | three flashes then dark | agent finished |
| `error` | SOS | failure |
| `off` | dark | everything idle |

## Environment

| Variable | Meaning |
|---|---|
| `BLINKY_LED` | LED path (default `/sys/class/leds/tpacpi::thinklight/brightness`) |
| `BLINKY_BASE` | runtime dir override (default `$XDG_RUNTIME_DIR/blinky`) |
| `BLINKY_PWM` | `0` disables PWM, forcing binary patterns |
| `BLINKY_PWM_HZ` | carrier frequency, default `100` |
| `BLINKY_RECALIBRATE` | ignore the cached calibration and re-measure |

## How it works

- A foreground daemon owns the LED and a FIFO (`$XDG_RUNTIME_DIR/blinky/cmd`).
- Per-agent state is kept in memory; the most urgent state wins.
- Each frame (default 100 Hz) computes a duty cycle from the active pattern and,
  in PWM mode, holds the LED on for `duty * period` and off for the rest.
- Startup calibration measures the maximum toggle rate of the LED. PWM is used
  when the ceiling is at least `1.5 x 2 x carrier`; otherwise the same patterns
  are rendered as on/off. The result is cached in `$XDG_RUNTIME_DIR/blinky/calib`
  so the light does not flicker on every start.
- The `timer` / `pattern` kernel LED triggers are an optional zero-CPU
  alternative; `blinky` does not require them.

## Limitations

- **It is a binary LED.** Any "dimming" is PWM and shows some flicker at low
  carrier frequencies. There is no true brightness register.
- Every toggle is one ACPI call, so PWM costs CPU and EC traffic. Keep the
  carrier around 100 Hz; do not drive it at kHz.
- `Fn+PgUp` toggles the same LED; the daemon will override it. Use
  `blinky pause` before you want manual control.
- The LED may lose state across suspend/resume; re-issue the current state.
- Direct EC register poking can do a much faster PWM (see the `thinkled`
  project), but the ThinkLight's register is not publicly mapped on the X230 and
  the EC has documented init races. Not worth the risk for a status lamp.

See `REPORT.md` for the full firmware/kernel investigation.
