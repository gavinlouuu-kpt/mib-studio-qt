# StageService (motorized Z stage)

> Owns the Zolix ZC300 / TBZF6-60 Z stage: read-only start-up, operator Home
> (mid-travel referencing), soft limits, one-sided approach and a reference
> that lasts one controller power-up. ADR 0013, #464.

**Source:**
- `include/backend/services/StageService.h`, `src/backend/services/StageService.cpp`
- config: `include/backend/services/StageConfig.h`, `src/backend/services/StageConfig.cpp`

The driver underneath is [[ZC300Stage]].

**Tests:**
- `tests/backend/stage_service_test.cpp`
- `tests/backend/stage_service_stress_test.cpp` (TSan lane)
- `tests/backend/stage_config_test.cpp`
- `tests/integration/e2e_stage_startup_test.cpp` (through a real `AppBackend`)
- `tests/support/stage_rig.h` (fake controller + real bus + shared reference store)

**Related:** [[ZC300Stage]], [[SerialBus]], [[../architecture/AppBackend]],
[[AutofocusService]] (the piezo is a separate focus actuator).

## Policy (decided by Gavin, 2026-10-05)

- **Start-up is read-only.** `startup()` connects only when `stage.enabled`.
  - It identifies the controller, checks the TBZF6-60 profile and reads the
    power-up token. It writes nothing and never moves the stage.
  - `reference.on_startup` (default false) is the only way a rig can opt into
    homing at start-up.
- **The stage moves only after an operator presses Home.** Until Home has
  succeeded for the current controller power-up, only `reference()` and
  `stop()` are accepted. A rig may allow small jogs before Home with
  `max_unreferenced_jog_um`, which defaults to 0.
- **Nothing else moves the stage.** Discovery, reconnect, config changes and
  recording start never do.

## Home (`reference()`)

1. **Search for the negative limit switch** at `search_speed_um_s` (at most
   2000 µm/s; 1000 µm/s by default).
   - The search is a relative move (opcode 0x65) of at most
     `expected_span + search_margin` (6500 µm), never an open-ended jog.
   - The ZC300 itself ends the move at the bound, or earlier at a tripped
     limit switch, so host stalls (load, OS sleep granularity, USB latency)
     cannot extend it. Host polling can only stop it earlier.
   - If it ends without the switch active: `ReferenceFailed`, "check the
     limit-switch wiring".
2. Search for the positive limit switch the same way.
3. Check the span: `|span − expected_span| ≤ span_tolerance`, otherwise
   `ReferenceFailed`.
4. Move to the midpoint with the one-sided approach.
5. `setPosition(0)`. The stage is now referenced, with soft limits of
   ±(span/2 − `soft_limit_margin_um`).
6. Write a random token to controller register 30054 and store
   {controller serial, token, span} in `<dataDir>/stage_reference.json`.

On the next `connect()`, the reference is restored only if the serial and
token both match. A power cycle clears the register, so the stage needs Home
again. With `power_up_token_register: 0`, the reference lasts for the session
only.

**The 6500 µm bound fixes timing, not wiring.** It is larger than the
TBZF6-60's ~6000 µm travel, so on a stage whose limit switches are unwired
or broken, a search reaches the mechanical hard stop before the bound. Before
the first real Home on any stage, someone must do a **supervised slow jog to
each limit, with Gavin present**, to confirm the switches (plan, slice 8).
Slice 8 will add a per-controller "limits verified" record that Home
requires.

### Limits-verified gate (#464 slice 4)

`reference()` is refused with `LimitsUnverified` unless
`<dataDir>/stage_limits_verified.json` holds a record for the connected
controller's serial. That includes the `on_startup` opt-in.
- **When it's read:** on every Home request and again when Home starts, so a
  record written while the app runs counts without a reconnect.
- **Who writes it:** only the supervised bench procedure,
  `zc300ctl verify-limits --supervised --allow-motion --data-dir <dataDir>`
  (`stage::verifyLimits`, `include/backend/stage/LimitVerification.h`).
  No facade, bridge, server or UI path can write it.
- **What the procedure does:**
  - It is operator-paced. It confirms each direction, then moves in
    controller-bounded steps of at most 500 µm at 200 µm/s, with travel per
    direction capped at span + 500 µm.
  - Enter or Ctrl-C stops it.
  - It checks that the correct switch trips at each end (catching swapped
    wiring within one step) and that the span is right.
  - It returns to the start position, and records only on success.
- The snapshot exposes `limitsVerified`.

## Panel

The Tauri panel is described in [[../architecture/Desktop-Shell]] (Z stage
panel). It mirrors these rules in the UI and shows why a control is disabled.

## Operations

- **One at a time on the worker.** `moveTo` (absolute), `moveBy` (relative to
  the µm grid point nearest the current position) and `reference` are each
  validated twice: before they are queued, and again when they start. The
  checks are connection, configuration, reference, soft limits, whole
  micrometres, and no e-stop or alarm.
- **Each operation returns an id**, and ends in exactly one terminal state:
  `Completed`, `Failed`, `Cancelled` or `TimedOut`.
- **One-sided approach.** When the final motion would run against
  `approach.direction`, the move first overshoots by `overshoot_um`, clamped
  to the soft limits, then approaches. Unreferenced jogs move directly.
- **Deadline** = distance / speed × `move_timeout_margin` + 1 s. A missed
  deadline stops the axis and reports `TimedOut`.
- **Failures that may desync the open-loop counter drop the reference** and
  delete the record: limit switch, e-stop, driver alarm, lost ack, timeout,
  and transport or protocol errors. An operator Stop keeps the reference.

## Threading

- **One worker thread** runs connect/disconnect, operations and the idle
  status poll (`poll_ms.idle`; `poll_ms.moving` inside operations). It is the
  driver's only regular caller.
- **`stop()` from any thread** cancels the active operation and calls the
  driver's `stop()` directly; the driver mutex serializes it.
- **Stop epoch.** Every `stop()` bumps a counter after the driver call. An
  operation that sees the counter change since its start ends `Cancelled`.
  This covers a Stop that raced the operation's first command.
- **`shutdown()`** cancels the active operation, queues a disconnect (which
  stops a moving axis), joins the worker and refuses further work.
  `AppBackend::shutdown()` calls it after the pulse generator, while the bus
  is alive.
- **No call waits behind a running operation.** The bridge runs one command
  at a time, and `stage_stop` needs the same lock, so any call that waited
  for a move would hold Stop for the move's whole duration.
  - `connect()` and `applyProfile()` are *exclusive* jobs: refused at once
    with `Busy` while an operation is active or another exclusive job is
    queued, and operations are refused (`Busy`) while one is pending, so
    nothing can slip in ahead of it.
  - `disconnect()` marks the service `disconnecting_` under the admission
    lock, cancels the active operation and stops the axis immediately, then
    queues the disconnect. The operation ends `Cancelled` at its next poll
    (≤ `poll_ms.moving`); no new operation is admitted meanwhile.
  - Found while designing the Tauri panel: before this, `applyProfile()`
    during a 3 s move blocked 2.9 s and then *applied and saved* the profile.

## Gotchas

- `StageConfig` comes from `parseStageConfig()` (the `stage` block). Wiring it
  into profile apply and the facade is slice 4. Today the shell, or a test,
  calls `setConfig()` and then `startup()`.
- `setConfig()` is refused while connected; disconnect first to change the
  endpoint.
- Positions are the open-loop counter. Missed steps from a collision cannot
  be detected, so if in doubt press Home.
- **Bench hold (2026-10-05):** the real stage stays read-only until the
  unexplained −6565 µm reading is resolved. Tests use the fake controller
  only.
