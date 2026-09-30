# 0008. Motorized stages are a separate device class behind `IMotionStage`

Date: 2026-09-30
Status: proposed

Issue: [#464](https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/464) · Plan: [2026-09-30-zc300-z-stage](../exec-plans/active/2026-09-30-zc300-z-stage.md) ·
Evidence: [ZC300 + TBZF6-60 integration](../integration/zc300-z-stage.md)

## Context

A Zolix ZC300-1A stepper controller driving a TBZF6-60 wedge lift stage
(6 mm travel, 0.4375 µm/pulse, ≤ 5 µm backlash) has been added to the bench
for Z/focus. It is a Modbus RTU device over USB serial, so it could reuse the
Qt-free [SerialBus](../../knowledge_map/services/SerialBus.md) layer that the syringe pump and pulse
generator already use.

The only focus actuator MIB Studio knows about today is the piezo
**nanopositioner** (`INanopositionerBackend`: connect, read/set voltage,
maximum voltage). A stepper stage does not fit that interface:

- It moves over time. A move has a start, completion, and failure modes
  (limit, e-stop, stall, driver alarm), and it must be stoppable.
- It has a reference state. Its position is an open-loop pulse count that is
  meaningless until the stage is homed, and after any collision or power cycle.
- It has physical units and hysteresis. Moves are in µm, not volts, and
  backlash means the approach direction matters.
- It is more dangerous than a piezo. It can drive 6 mm into an objective at
  up to 7 mm/s, while a piezo moves a few µm at most.

The piezo and the stage are also complementary rather than alternatives:
coarse Z over millimetres, fine Z over micrometres. Autofocus currently works
in volts. No recording stores any focus state, so the "cells in different
focus" corpus is labelled only by nominal voltage.

## Decision

1. **New device class, new module.** `backend::stage` sits beside
   `backend::nanopositioner` inside `mib_backend`. It is Qt-free, uses only
   C++17 value types in its public headers, and exposes no vendor handles.
   `INanopositionerBackend` is not widened.

2. **`IMotionStage` speaks physical units and explicit state.** Positions and
   distances are `double` micrometres (or degrees for rotary axes). The driver
   reports its step size (`resolution()`), whether the position is
   referenced to home, and a status snapshot: moving, the positive, negative
   and home switches, e-stop, and driver alarm. Moves return immediately; a
   separate completion wait is bounded by a deadline. `stop()` is always
   accepted and is idempotent. Each driver declares a command quantum, and
   targets off that grid are rejected, not rounded. For the ZC300 the
   quantum is 1 µm with the controller in mm mode. Any unit conversion stays
   inside the driver. A controller whose unit, stage type or lead disagrees
   with the configured stage profile stays connected read-only and refuses
   motion. Only an explicit profile-apply action writes those settings.

3. **Layered like OEABT, as its own CMake targets.**
   - `stage_zc300_protocol` is pure: register map, frame construction,
     status decoding and quantization rules, all testable without I/O.
   - `stage_zc300` is the driver, built on a `ModbusBusSession`.
   - `zc300ctl` is a CLI that is read-only unless `--allow-motion` is given.

   These targets form the reusable library: the same code serves the desktop
   app, the PZ7035 embedded runtime (#441) and bench bring-up. There is no
   separate repository and no Python package; the pyserial bench client
   remains a diagnostic.

4. **Shared Modbus layer gains function code 0x04.** `ModbusRtu.h` gains FC 04
   (read input registers) framing, length prediction and correlation. This is
   a generic addition that the pump and pulse generator tests must keep
   passing. The ZC300 is a point-to-point USB device, but it still goes
   through `SerialBusManager`, so port ownership, busy detection and I/O
   threading stay in one place.

5. **Motion safety is part of the contract, not the UI.**
   - Discovery and connect are **observe-only**. Discovery reads the model
     and serial registers only; connect reads status and configuration and
     writes nothing.
   - **Start-up referencing is the only automatic motion.**
     - At application start-up, the startup coordinator references the stage
       (§6) so that the system starts in a known Z frame. It runs after the
       stage is uniquely identified and its profile matches.
     - It can be disabled in config, and it runs as a visible operation that
       can be cancelled; cancelling stops the stage.
     - Everything else moves only on explicit command: discovery, reconnect,
       config reload and recording start never move the stage, and re-homing
       after start-up is an explicit, confirmable action.
     - "Start-up" means the MIB Studio startup sequence, not OS boot.
   - **Soft limits.** Every move target is checked against the referenced
     travel window before it reaches the wire. Hardware limit switches are the
     backstop, not the policy.
   - **Motion opcodes are sent at most once.** A lost acknowledgement is
     reconciled by reading status and position. Stop and save are
     idempotent and may be retried.
   - **Stop wins.** Disconnect, service shutdown, the frontend `Stop`, and a
     move that misses its deadline all issue an immediate stop before
     releasing the port.
   - **Unreferenced positions are labelled.** Until referencing succeeds in
     the current session, positions are reported and recorded as unreferenced.

6. **Home is mid-travel, found by probing both limits.**
   - The procedure is:
     1. Drive at search speed to the negative limit switch and record the
        position.
     2. Drive to the positive limit and record the position.
     3. Check that the measured span matches the stage profile (TBZF6-60:
        6000 µm ± tolerance).
     4. Move to the midpoint with the normal one-sided approach.
     5. Redefine that point as 0 µm by writing the coordinate, with no
        motion.
   - The Z frame is therefore symmetric: soft limits are ±(span/2 − margin).
   - Probing both switches on every start-up also checks the switch wiring
     and catches a stage that stalled or shifted since the last run.
   - The stage's home sensor is not used for the reference; it may be read as
     a consistency check.
   - A span outside tolerance, a limit not reached within the expected
     travel, an e-stop or a driver alarm aborts referencing. The stage is
     stopped and left unreferenced.

7. **Backlash is handled in the service, not the driver.** An optional
   one-sided approach applies to target moves: overshoot past the target,
   then approach from a fixed direction. It is on by default for focus use.
   The driver stays literal.

8. **One service owns the stage.** `StageService` is wired through
   `AppBackend`. It owns connection, a status poll thread, move sequencing and
   referencing. Moves and referencing are **operations** under ADR 0004 (operation ID,
   progress, exactly one terminal state, cancel = stop). The discovery
   provider, facade commands and bridge enums follow ADR 0004 and ADR 0005,
   with additive changes only.

9. **Focus state becomes recording provenance.** Recordings and experiments
   stamp the stage identity, the Z position at start and end, and the
   referenced flag. Stage-driven sweeps record Z per frame. Files without a
   stage read as "no Z information", never as Z = 0.

10. **Autofocus is not changed by this decision.** Using the stage for coarse
   focus, the piezo for fine focus, or a common focus-actuator abstraction
   will get its own ADR. That ADR should rest on µm-labelled sweep data this
   work produces, not on an interface guessed today.

## Consequences

- Adding another stage (a different Zolix axis, a PI or Thorlabs stage, or a
  Z motor on the PZ7035) means adding a driver behind `IMotionStage` plus a
  discovery provider. The service, safety rules, operations, UI and
  provenance are inherited.
- FC 04 support is new shared code on the RS485 path. It lands with its own
  unit tests, and the pump/pulse-generator bus tests must stay green.
- The bridge ABI bumps once, for the new discovery kind, command type,
  operation kinds and stage status snapshot.
- Recording files gain optional attributes and, for sweeps, a per-frame Z
  column. That is a schema change, so it needs round-trip and
  fault-injection tests under the coverage matrix. Readers must tolerate
  their absence.
- Two focus actuators can be connected at once. Anything that commands both
  (future autofocus, sweeps) must name which one it drives. Neither service
  may command the other's device.
- The Qt shell gets no stage UI. Stage control is exposed only through
  `BackendFacade` and the Tauri shell, in line with ADR 0001.
- Every application start moves the stage through its full 6 mm travel.
  Whatever is mounted above it, such as an objective or sample holder, must
  clear the whole travel range. Rigs where it cannot must disable start-up
  referencing.
- On the PZ7035, the #441 target rule "boot must never start motion" needs an
  explicit amendment for application-start referencing, or referencing must
  wait for the first operator session there.
- On the PZ7035 the same driver runs unchanged. Getting the adapter
  recognised (`CONFIG_USB_SERIAL_FTDI_SIO`, USB host mode, the USB PHY reset
  on pin J16) is board-repo work tracked in pz7035-imx426.
