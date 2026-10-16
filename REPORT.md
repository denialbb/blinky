# ThinkPad X230 "blinky" — feasibility & implementation report

Machine: ThinkPad X230 (2325CN3), kernel `7.2.5-3-omarchy`, `thinkpad_acpi` loaded.
Goal: drive the lid ThinkLight (top flood light) as an agent status indicator for
pi / agy / opencode / claude / codex, and use the other LEDs where useful.

---

## 1. Verdict

| Question | Answer |
|---|---|
| Can we control the lights? | **Yes.** `tpacpi::thinklight` is exposed as a standard Linux LED. On/off via sysfs or `/proc/acpi/ibm/light`. |
| Can we modulate brightness? | **No true dimming.** `max_brightness = 1` → binary on/off only. |
| Can we blink it? | **Yes, software blink.** Kernel `timer`/`heartbeat` triggers, or a tiny userspace loop. |
| Hardware-accelerated blink? | **No for ThinkLight.** Only the numbered indicator LEDs (power/battery/standby/thinkvantage) have a `blink_set` EC path. |
| Pseudo-dimming (PWM)? | **Maybe.** Userspace software-PWM (toggle fast, vary duty). Viable rate unknown on this unit — must be measured. Realistic: 50–200 Hz, enough for "dim / mid / bright" on a notification light, not for smooth fades. |
| Root needed? | **Once**, to install a udev rule (no passwordless sudo currently). After that, fully user-level. |

**Bottom line:** a binary-but-blinking ThinkLight is very feasible and cheap. Treat it as
"lamp + rhythm", not as an RGB strip. Use on/off *patterns* (heartbeat, double-blink,
SOS) as the primary language.

---

## 2. What the hardware/driver actually gives us

### 2.1 ThinkLight (the flood light)

```
/sys/class/leds/tpacpi::thinklight/
  brightness      = 0 | 1        (root:root 0644, currently)
  max_brightness  = 1
  trigger         = [none] usb-gadget ... disk-activity ... panic AC-online ...
/proc/acpi/ibm/light             → echo on / echo off
```

Kernel source (`drivers/platform/x86/lenovo/thinkpad_acpi.c`):

```c
static struct tpacpi_led_classdev tpacpi_led_thinklight = {
    .led_classdev = {
        .name = "tpacpi::thinklight",
        .max_brightness = 1,
        .brightness_set_blocking = &light_sysfs_set,   /* one ACPI call per toggle */
        .brightness_get = &light_sysfs_get,
    }
};
```

Consequences:
- Every write is one ACPI method call (`_SB.LHHT`-style / `lght` handle). Blocking, synchronous.
- **No `blink_set`** → the kernel cannot offload blinking to the EC for this LED.
- Rate limit = ACPI call latency. A pre-opened fd + tight C loop should be much
  faster than `echo` (fork/exec ~1–2 ms each). Measure before promising fades.

### 2.2 Other controllable LEDs on this unit

| LED | max | Notes |
|---|---|---|
| `tpacpi::thinklight` | 1 | Flood light. Binary, software blink only. |
| `tpacpi::lid_logo_dot` | 255 | Red dot on lid. **Hardware blink** supported (EC-driven). Sweet spot for "needs input". |
| `tpacpi::power` | 255 | Power-button LED. HW blink (1 Hz) via timer trigger, delay_on/off=0. |
| `tpacpi::standby` | 255 | Crescent moon LED. HW blink. |
| `tpacpi::thinkvantage` | 255 | Black-button LED. HW blink. |
| `platform::mute`, `platform::micmute` | 1 | Owned by the audio driver; leave alone. |
| `phy0-led`, `mmc0::`, `input3::capslock/numlock/scrolllock` | — | Unrelated to agents; leave alone. |

For the numbered LEDs the kernel doc is explicit:

> These LEDs can blink using hardware acceleration. To request that a ThinkPad
> indicator LED should blink in hardware accelerated mode, use the "timer"
> trigger, and leave the delay_on and delay_off parameters set to zero.

So the EC itself can blink `power`/`standby`/`thinkvantage`/`lid_logo_dot` for free —
zero CPU, survives suspend better than a userspace loop.

### 2.3 Triggers

`CONFIG_LEDS_TRIGGER_*` are built as **modules** (`ledtrig-timer.ko`, `ledtrig-heartbeat.ko`,
`ledtrig-pattern.ko`, `ledtrig-oneshot.ko`, `ledtrig-transient.ko`, `ledtrig-netdev.ko`, …)
and none are loaded yet. Writing the trigger name (as root) autoloads it.

- `timer` → blink with ms precision, no daemon needed.
- `heartbeat` → nice "thinking" pulse.
- `pattern` → multi-step HW blink sequences (great for "needs input" vs "done").
- `oneshot` / `transient` → fire-once flashes from a simple write.

---

## 3. Modulation deep-dive

### 3.1 Free and reliable: blink patterns
`timer` trigger, kernel-side:

```sh
echo timer > /sys/class/leds/tpacpi::thinklight/trigger
echo 100 > /sys/class/leds/tpacpi::thinklight/delay_on
echo 900 > /sys/class/leds/tpacpi::thinklight/delay_off
```

Kernel toggles the light; no per-toggle fork, negligible CPU. This is the recommended
default for anything that must run for hours.

### 3.2 Pseudo-brightness by software PWM
Toggle the LED faster than the eye integrates (~>60 Hz) with varying duty cycle.
Evidence it works on ThinkPads: `thinkled` drives the lid dot at **200 Hz software PWM**
to fake brightness (but via direct EC register `0x0C` writes, not sysfs).

For the ThinkLight the sysfs path is ACPI-bound, so the ceiling is lower. Plan a
10-minute experiment: pre-open the fd in C, loop ON 1× / OFF 1×, measure achievable
frequency, then judge whether 60–100 Hz PWM is stable. If not, ship patterns only.

**Do not** hack EC registers for the ThinkLight. The X230 EC is a MEC1619, the light's
register is not publicly mapped like the lid dot, and coreboot bug #647 shows EC
init/state races on this exact model. High risk, low reward.

### 3.3 Interaction with Fn+PgUp
The hotkey and sysfs poke the *same* state. If the daemon holds a pattern, a manual
Fn+PgUp appears to "not work" (the daemon overrides it next tick). Provide a
`blinky pause` / manual override.

---

## 4. Permissions (one-time root)

Currently `brightness` and `trigger` are `root:root 0644`, and there is no passwordless
sudo for anything relevant. Clean fix — a udev rule, applied to the LED class device:

```
# /etc/udev/rules.d/70-blinky-thinkpad-leds.rules
SUBSYSTEM=="leds", KERNEL=="tpacpi::thinklight",   RUN+="/bin/chmod 0666 /sys/class/leds/tpacpi::thinklight/brightness"
SUBSYSTEM=="leds", KERNEL=="tpacpi::thinklight",   RUN+="/bin/chmod 0666 /sys/class/leds/tpacpi::thinklight/trigger"
SUBSYSTEM=="leds", KERNEL=="tpacpi::lid_logo_dot", RUN+="/bin/chmod 0666 /sys/class/leds/tpacpi::lid_logo_dot/brightness"
```

`chmod 0666` on an LED attribute is a tiny relaxation (any local process can toggle a
lamp). Alternative: `chown` to your user, or a single narrow sudoers entry for a
`blinky` helper. Also persist the trigger modules:

```
# /etc/modules-load.d/blinky.conf
ledtrig-timer
ledtrig-pattern
ledtrig-heartbeat
```

Reload: `sudo udevadm control --reload && sudo udevadm trigger -s leds`.

Note: writing `trigger` from userspace then requires the chmod above too.

---

## 5. High-level implementation options

### Option A — no daemon (simplest, patterns only)
A shell CLI that selects a kernel trigger / sets delay_on/off:

```sh
blinky set thinklight running     # timer 80/920
blinky set thinklight needs-input # timer 120/120 (double via pattern)
blinky set thinklight done        # 3 flashes then off
```

Zero processes. Limitation: cannot express per-agent arbitration or smooth fades, and
re-setting a trigger is a root write unless udev-chmod'ed.

### Option B — tiny daemon + CLI (recommended)
One small C program (or ~150-line Python if you accept ~1 core at 100 Hz):

- Opens the LED fds once, `lseek(0)`+`write()` per toggle.
- Reads the desired state from a **FIFO** (`~/.cache/blinky/cmd`) — one line per update.
- Owns a per-agent state map, applies "most urgent wins":
  `needs-input > error > running > done > idle`.
- Drives `thinklight` with patterns, uses the *hardware* blink on `lid_logo_dot` /
  `power` for things that must be free.

Control surface:
```
blinky set <agent> <state> [detail]
blinky on | off | pause | status
```
CLI just appends a line to the FIFO — no protocol, no socket, no config.

### Option C — hardware-blink only
Set `lid_logo_dot`/`power` to 1 Hz HW blink for "needs input", leave the ThinkLight
solid ON while any agent is running, OFF when all idle/settled. Trivially reliable,
survives load and suspend; the ThinkLight becomes a "busy lamp".
This is the laziest complete answer if full patterns are not needed.

**Recommendation:** start with **C**, ship as **B** (adds arbitration + patterns).
Fall back to **A** if you want zero moving parts.

---

## 6. Seamless integration into *your* setup

You already have the perfect single choke point:

```
~/.local/bin/agent-notify.sh <agent> <state> [tool] [cmd]
    states: running | needs-input | done | off
    → updates tmux-agent-indicator, sends ntfy push
```

Everything that calls `agent-notify.sh` gets the LEDs for free if you add ~5 lines at
the end of that script:

```sh
# 6. Drive ThinkPad LEDs
[ -x "$HOME/.local/bin/blinky" ] && "$HOME/.local/bin/blinky" set "$AGENT" "$STATE" &
```

Current wiring (from inspecting the configs):

| Agent | Hook file | Status |
|---|---|---|
| **opencode** | `~/.config/opencode/plugins/opencode-tmux-agent-indicator.js` | **Already calls `agent-notify.sh`** on busy / permission.asked|updated / session.idle / session.error / question. → LEDs free. |
| **claude** | `~/.claude/settings.json` | Only herdr `SessionStart` today. Add `Stop`, `Notification`, `PreToolUse`, `SessionEnd` command hooks → `agent-notify.sh claude <state>`. |
| **codex** | `~/.codex/hooks.json` | Only herdr `SessionStart` today. Add a hook / `notify` program → `agent-notify.sh codex <state>`. |
| **agy** (antigravity) | `~/.gemini/config/hooks.json` | Only herdr `PreInvocation`. Add `PreInvocation`/`AfterInvocation` hooks calling `agent-notify.sh agy <state>`; `agent-notify.sh` already parses the antigravity `toolCall` stdin JSON. |
| **pi** | `~/.pi/agent/extensions/` | **Not wired at all.** Write `thinklight-status.ts`: `session_start`/`agent_start` → running, `tool_call` on `ask_user_question`-style tools → needs-input, `agent_settled`/`turn_end` → done, `session_shutdown` → off. |

If you prefer not to touch each agent, an alternative is one shared wrapper
(`blinky wrap -- pi …`) that exports a state and installs atexit handlers — but the
hook map above is more accurate and you already own `agent-notify.sh`.

Herdr note: your herdr integrations report *pane session* state over a unix socket,
not lifecycle. Keep herdr for resume, keep `agent-notify.sh` for lifecycle → LEDs.

---

## 7. Feature ideas (ranked by value/effort)

1. **State language on the ThinkLight** — running = slow pulse (timer 80/920);
   needs-input = fast double-blink; done = 3 flashes; error = SOS.
2. **Most-urgent-wins across panes** — one daemon, N agents, `needs-input` always wins.
   This is the single most useful feature when 3+ agents run.
3. **Second channel without losing the first** — `lid_logo_dot` 1 Hz HW blink = "needs
   input" independent of the flood light; `power` LED = "done".
4. **Manual override** — `blinky pause` / `blinky on`; restores daemon after a timeout.
5. **Morse** — spell a short tag (session name, repo, error code) in Morse, like
   `thinkmorse`. Fun for "which agent finished".
6. **Suspend/resume reapply** — `systemd-sleep` hook re-arms patterns (ACPI may drop state).
7. **Quiet hours / ambient mode** — suppress `done` flashes at night; only `needs-input`.
8. **PWM "dim" level** — if the experiment succeeds, encode urgency as brightness
   (dim = background agent, full = blocked).
9. **CLI + optional status widget** — re-use the state files for your omarchy bar
   (`denial.agents`) so screen and lid agree.

---

## 8. Risks & caveats

- **Binary LED** — no true brightness. Any "dimming" is PWM and needs a real measurement.
- **ACPI cost** — every toggle is an ACPI call. Userspace PWM costs CPU; kernel `timer`
  trigger does not. Prefer the trigger for long-running states.
- **EC/ACPI stress** — keep PWM ≤ ~200 Hz; do not hammer at kHz. Do not poke EC registers
  directly; the X230 has known EC state races (coreboot #647).
- **Fn+PgUp conflict** — daemon and hotkey fight; needs a pause/override.
- **Suspend/resume** — state may be lost; reapply on resume.
- **Permissions** — a one-time udev rule; `chmod 666` lets any local process toggle the
  LED (low impact).
- **Battery** — the ThinkLight draws real power; a permanently-on lamp on an X230 is
  ~measurable. Default to blinking, not solid.

---

## 9. Verification checklist (needs root once)

```sh
# 1. Does the LED respond at all?
sudo sh -c 'echo 1 > /sys/class/leds/tpacpi::thinklight/brightness'; sleep 1
sudo sh -c 'echo 0 > /sys/class/leds/tpacpi::thinklight/brightness'

# 2. Software blink via kernel timer
sudo modprobe ledtrig-timer
sudo sh -c 'echo timer > /sys/class/leds/tpacpi::thinklight/trigger; \
            echo 100 > /sys/class/leds/tpacpi::thinklight/delay_on; \
            echo 900 > /sys/class/leds/tpacpi::thinklight/delay_off'
# reset: echo none > .../trigger

# 3. Hardware blink on a numbered indicator LED
sudo sh -c 'echo timer > /sys/class/leds/tpacpi::lid_logo_dot/trigger; \
            echo 0 > /sys/class/leds/tpacpi::lid_logo_dot/delay_on; \
            echo 0 > /sys/class/leds/tpacpi::lid_logo_dot/delay_off'

# 4. PWM ceiling (the only real unknown) — pre-opened fd in C,
#    toggle 1/0 in a tight loop, count Hz. If <60 Hz stable → patterns only.
```

---

## 10. Suggested first milestone

1. udev rule + modules-load (root, one time).
2. `blinky` daemon (Option B) with FIFO control and three patterns.
3. 5-line patch to `agent-notify.sh` → instant coverage for opencode and any future caller.
4. Hooks for claude/codex/agy; a pi extension last.
5. Only then try PWM dimming.
