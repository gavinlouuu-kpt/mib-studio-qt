# ZC300 Z stage: supervised hardware acceptance

A checklist to run **with Gavin present, in his session, step by step.** Nothing here
has been run. Each step says what it does to the real controller. A step that writes or
moves needs Gavin's explicit OK for that step; read-only steps are marked so.

Background: [ADR 0013](../decisions/0013-motion-stage-device-class.md) (Amendment 1:
the stage is never homed), [evidence](../integration/zc300-z-stage.md),
[plan](../exec-plans/active/2026-09-30-zc300-z-stage.md). Stage: Zolix ZC300-1A +
TBZF6-60 (6 mm travel, 0.4375 µm/pulse), FT232R `A10RB8XC`.

> **The counter is open-loop.** Nothing knows where the stage physically is. Everything
> below depends on someone watching the stage and on what is mounted above it.

## 0. Before anything

- [ ] Gavin is present and has the controller's power switch within reach.
- [ ] Nothing fragile is in the stage's reach: objective, sample holder and tubing are clear
      of ±1.5 mm around the current position (the first steps move at most ±1 mm).
- [ ] The app is **closed** (the serial port has one owner at a time), or the steps that
      use the bench client say otherwise.
- [ ] Open items acknowledged: the driver is set to 1.6 A for a 1.3 A motor (reduce with
      the SR2-plus DIP switches, power off); limit and home sensor wiring and polarity are
      unproven (a 2026-10-06 read-only check found the home bit floating on all three axes
      and the limit bits at 0 even on the two unconnected axes).
- [ ] Abort rule for every step: anything unexpected (motion that was not asked for, noise,
      a bit that flips unexpectedly) means **Stop in the app, or cut the controller's
      power**. Then stop and write down what happened.

## 1. Read-only baseline (no writes, no motion)

```bash
zc300ctl list
zc300ctl info   --usb-serial A10RB8XC
zc300ctl status --usb-serial A10RB8XC
```

- [ ] Identity `ZC300-1A`, serial 26017, firmware 1.2; "profile 'tbzf6-60': matches".
- [ ] Note the position counter (it is not a position), `state: idle`, e-stop 0, alarm 0.
- [ ] Raw 30015 and the token register, read-only (FC04 / FC03), with the pyserial client:

```bash
cd ~/Developer/zc300-tools && python3 - <<'EOF'
import zc300
z = zc300.ZC300()
s = z.read_u16(30015)[0]
print(f"30015 = 0x{s:04X} = {s:016b}")
print("30054 (power-up token) =", z.read_u16(30054)[0])
z.close()
EOF
```

- [ ] 30054 reads **0** on a controller that was just powered up and never zeroed.
- [ ] Record the 30015 word. Bits per axis: positive limit, negative limit, home (X = bits
      0, 1, 2); e-stop bit 9; alarms from bit 10.

### 1b. Optional, read-only: watch the limit bits while a switch is pressed by hand

No motion: the stage is not commanded. Gavin presses each limit switch (or passes a flag
through the photo-interrupter) by hand while the word is watched:

```bash
watch -n 0.3 'zc300ctl status --usb-serial A10RB8XC | grep -E "limits|emergency"'
```

- [ ] Positive switch: which bit flips, and is it active-high or active-low?
- [ ] Negative switch: same.
- [ ] Do the two switches map to the right directions (not swapped)?
- [ ] Does the home bit move at all when its sensor is actuated (it floats today)?

Write the results into the evidence doc. This only informs the "wiring unverified" badge;
nothing in the software depends on it.

## 2. The power-up token register (needs Gavin's OK: one volatile register write, no motion)

The software assumes 30054 is a volatile scratch register that **survives a serial
disconnect and is cleared to 0 by a controller power cycle**. The whole power-cycle
detection rests on that, so it is checked before trusting it.

- [ ] Gavin OKs one write of `0x1234` to 30054 (volatile, not saved; no motion):

```bash
cd ~/Developer/zc300-tools && python3 - <<'EOF'
import zc300
z = zc300.ZC300()
z.write_u16(30054, 0x1234)
print("30054 after write =", hex(z.read_u16(30054)[0]))
z.close()
EOF
```

- [ ] Reads back `0x1234`.
- [ ] Close and reopen the port (run the read-only snippet from step 1 again): still `0x1234`
      (survives a disconnect).
- [ ] Optional: use the controller's front panel (no motion) and read again: still `0x1234`.
- [ ] **Power-cycle the controller** (switch off, wait, on). Read again: **0**. If it is not 0,
      stop: the power-cycle detection does not hold on this controller (see "If a check
      fails" below).

## 3. First Set zero, through the app (needs Gavin's OK: writes the position counter and 30054, no motion)

Start MIB Studio, Connect tab, Z stage panel. The app's mode must be Service /
Commissioning and the panel armed (arming is one-shot).

- [ ] Connect only: the panel shows the controller identity, "Zero not set", position
      "unknown until zero is set". Nothing moved (watch the stage).
- [ ] Gavin chooses where zero is (the current position is fine) and OKs **Set zero here**.
      Leave "mid-travel" unticked for the first run.
- [ ] Panel: "Zero set · travel -1000 to 1000 µm (mid-travel not declared)". Position ~0 µm.
- [ ] The stage did not move.
- [ ] Disconnect in the app, then read back with the bench client (counter and token):

```bash
cd ~/Developer/zc300-tools && python3 - <<'EOF'
import zc300
z = zc300.ZC300()
print("30054 =", z.read_u16(30054)[0], "(non-zero, not 0x1234)")
print("position counter =", z.position("X"))
z.close()
EOF
```

- [ ] Counter ~0; 30054 is non-zero and equals `token` in `<app data dir>/stage_reference.json`.
- [ ] Reconnect in the app: the zero is **restored** (same power-up) and the panel says so.

## 4. Tiny moves inside ±1000 µm (needs Gavin's OK for the run; one-shot arming per move)

Gavin watches the stage; the step size starts at 1 µm.

- [ ] Jog +1 µm, −1 µm. Position changes by ~1 µm (±0.5), nothing else.
- [ ] Jog ±10 µm, then ±100 µm. The counter moves by 23 and 229 pulses (0.4375 µm/pulse).
- [ ] Go to 0 µm: the position returns to ~0 and the stage is where it started (a
      mechanical check by eye or dial indicator, if there is one: this is the only real
      position check there is).
- [ ] A target of 1001 µm (or beyond the shown travel) is **refused** by the panel, and a
      refusal does not use up the arming.
- [ ] Press **Stop** during a move (start a ±500 µm move and press right away): the axis
      stops within about a second.
- [ ] Optional: after a power cycle of the controller **while the app stays connected**, the
      panel drops to "Zero not set" within a second or two and moves are refused until Set
      zero is done again.

## 5. Wrap-up

- [ ] Disconnect in the app; power state noted.
- [ ] Results (tables, bit words, the 30054 observations, anything unexpected) go into
      [the evidence doc](../integration/zc300-z-stage.md) in the same PR.
- [ ] Remaining open items listed: driver current, limit wiring and polarity, whether the
      limit switches stop the stage (not tested here: that needs `zc300ctl verify-limits
      --supervised`, which moves the stage toward its ends and is optional).

## If a check fails

- **30054 does not read 0 after a power cycle:** the software would treat a power cycle as
  "same lifetime or uncertain". Do not rely on the zero surviving a controller power cycle;
  keep the app in its default mode and tell the coordinator. A different register, or
  `stage.reference.power_up_token_register: 0` with `allow_session_only_zero` (acceptance
  mode, which shows an alert and detects no power cycles), is the fallback.
- **Motion that was not asked for:** Stop, cut power, do not continue. The counter is no
  longer trustworthy; set zero again only after looking at the stage.
- **Counter and physical position disagree after step 4's return to 0:** the driver current
  or the mechanics are suspect (missed steps). Stop and investigate before any further motion.
