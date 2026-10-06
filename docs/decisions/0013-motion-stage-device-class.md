# 0013. Motorized stages are a separate device class behind `IMotionStage`

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
   - **Nothing moves at start-up.** This was decided by Gavin on
     2026-10-05, relayed by the merge-coordination session.
     - Application start-up only identifies the stage and reads its status.
     - The stage moves only when an operator presses **Home** (referencing,
       §6), or, once homed, on explicit move commands.
     - Discovery, reconnect, config reload and recording start never move the
       stage.
     - Automatic start-up referencing remains available as a configuration
       switch (`reference.on_startup`), which defaults to `false`. Turning it
       on for a rig is a reviewed change.
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
   - **Unknown until homed once per power-up.** Until Home has succeeded
     since the controller was last powered on, the position is reported and
     recorded as *unreferenced* (unknown).
     - No motion other than Home and Stop is accepted in that state by
       default.
     - A reference survives an application restart only if the controller
       proves it was not power-cycled (§6).

6. **Home is mid-travel, found by probing both limits.**
   - The procedure is:
     1. At the slow search speed, search for the negative limit switch and
        record the position. The search is a controller-bounded relative
        move of at most `expected_span + search_margin`, never an
        open-ended jog, so host timing cannot extend it (amended
        2026-10-06).
     2. Search for the positive limit the same way and record the position.
     3. Check that the measured span matches the stage profile (TBZF6-60:
        6000 µm ± tolerance).
     4. Move to the midpoint with the normal one-sided approach.
     5. Redefine that point as 0 µm by writing the coordinate, with no
        motion.
   - The Z frame is therefore symmetric: soft limits are ±(span/2 − margin).
   - Probing both switches on every Home also checks the switch wiring and
     the travel span.
   - The search bound exceeds the travel, so it limits timing errors, not
     wiring faults. A supervised check of each limit switch precedes the
     first real Home on any stage. Slice 8 makes Home require a
     per-controller "limits verified" record.
   - **Per-power-up persistence.**
     - When Home succeeds, the service writes a random non-zero token to a
       volatile, unsaved controller register. It persists that token with
       the controller serial and the reference span.
     - On a later connect, the stage counts as referenced only if the serial
       and the token both match. The ZC300 clears volatile registers and
       zeroes its position counter at power-up, so a power cycle reads back
       as unreferenced.
     - Front-panel moves made while the application is closed are counted by
       the controller, so they keep the frame valid. A stall or collision
       cannot be detected (the counter is open-loop) and needs a new Home.
     - The candidate register is reserved holding register 30054, which the
       manual marks read/write and not saved. Hardware acceptance must show
       that it is writable, keeps its value across a disconnect, clears at
       power-up, and is not touched by front-panel use. If no register
       qualifies, the reference is held for the application session only,
       which is stricter.
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
- Home moves the stage through its full 6 mm of travel, and it only
  happens when an operator asks for it. Whatever is mounted above the stage,
  such as an objective or sample holder, must clear that range before Home
  is pressed. The Home control therefore asks for confirmation.
- On the PZ7035 instrument ([ADR 0011](0011-yofo-studio-pz7035-instrument.md)),
  the stage is driven by the headless backend on the PS and operated from
  the remote React UI. That matches the facade/Tauri-only surface here, and
  the same driver runs unchanged.
- The E0 target ADR (landing as 0012 per ADR 0011) rule "boot must never
  start motion" is satisfied as-is: referencing is operator-initiated.
- On the instrument, autofocus is fed by PL result records (ADR 0011 §4). The
  follow-up focus-actuator ADR must take its focus metric from there, not
  from host-side frames.
- Getting the adapter
  recognised (`CONFIG_USB_SERIAL_FTDI_SIO`, USB host mode, the USB PHY reset
  on pin J16) is board-repo work tracked in pz7035-imx426.
