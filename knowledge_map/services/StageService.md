# StageService (motorized Z stage)

> Owns the Zolix ZC300 / TBZF6-60 Z stage: read-only start-up, the operator's
> "Set zero here", the travel envelope around that zero, one-sided approach and
> a zero that lasts one controller power-up. **The stage is never homed**
> (ADR 0013 Amendment 1, 2026-10-06). #464.

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

## Policy (Gavin, 2026-10-05 and 2026-10-06)

> **Known limitation.** The position is an open-loop pulse counter with no
> encoder. Without homing, nothing knows where the stage physically is: a hand
> move, a stall or a collision silently shifts the real position against the
> counter, and the software cannot detect it. The envelope below only bounds
> the exposure.

- **Start-up is read-only.** `startup()` only calls `connect()` when
  `stage.enabled`.
  - It identifies the controller, checks the TBZF6-60 profile and reads the
    power-up token. It writes nothing and never moves the stage.
  - There is no Home and no option to move at start-up. Old `on_startup`,
    `search_*`, `span_tolerance_um`, `require_reference` and
    `max_unreferenced_jog_um` keys are ignored.
- **Moves need the operator's zero.** Until `setZero()` has succeeded for the
  current controller power-up, only `setZero()` and `stop()` are accepted
  (`ZeroNotSet`).
- **Nothing else moves the stage or writes the position.** Discovery,
  reconnect, config changes and recording start never do.

## Set zero (`setZero(midTravel)`)

One write to position register 30059 through the driver's `setPosition(0)`:
**no motion, no opcode.** It runs on the worker as an exclusive job.
- **Refused** with `Busy` while an operation is active or the axis moves
  (even if this service did not start it), with `EmergencyStop` /
  `DriverAlarm`, `Misconfigured`, or `NotConnected`.
- **Window rule.** Without `midTravel`, when a zero is already set, the
  current position must be inside the *window* set by the first zero of this
  power-up, otherwise `OutOfSoftLimits` ("declare mid-travel only if the stage
  really is there, or power-cycle"). The window is stored in the current
  zero's coordinates and shifted by every re-zero, so repeated re-zeroing can
  never walk the envelope along the stage.
- **Fail closed.** Any uncertainty about what is on the controller or on disk
  (a save failed, a token write was not acknowledged, the token is unknown, a
  fault was seen while it was unknown) leaves the stage in "zero invalid, window
  uncertain": moves and an undeclared re-zero are refused, and the way out is an
  explicit operator action: Set zero **with the mid-travel declaration** (or a
  controller power cycle). That state is persisted.
- **Order of writes, safe at every crash point** (with a power-up token):
  1. An **interim record** is saved, for the *first* zero too: `{window,
     zeroValid = false, frameUncertain = true, token = the controller's,
     nextToken = the new one}`. A reconnect accepts the controller holding either
     token as "the same power-up", so a crash never loses the window. If it
     cannot be stored, Set zero is refused and nothing has changed.
  2. A **fresh token goes to controller register 30054**, guaranteed different
     from the one it replaces (and never 0), so no stored record, however stale
     or undeletable, can match it. If the write fails or is not acknowledged the
     controller may hold either token: the interim record (accepting both) is
     kept, nothing is trusted, and Set zero must be repeated with a declaration.
  3. Register 30059 is written (the counter). A failure leaves the stage
     uncertain, as above.
  4. The final record `{serial, token, midTravelDeclared, window, zeroValid}` is
     saved (three attempts) and only then is the zero used. If it cannot be
     stored the call fails: the counter is already new, so nothing is trusted.
  The store is never trusted to have deleted anything, and
  `FileStageReferenceStore::save` writes, flushes, fsyncs and closes before it
  renames, so a close-time failure cannot replace the record with a truncated file.
- **Power-up detection.** The token is compared with the controller's on every
  idle status poll, before every Set zero and before every motion opcode, and
  read as one of three verdicts (`classifyToken`):
  - **0 = a new power-up** (a power cycle clears the register): the zero, the
    window and the stored record are all gone; the next zero gets a fresh window.
  - **matches the record's `token` or `nextToken`** (an interrupted Set zero is
    the second case, including the *first* zero, whose interim record has token 0):
    the same power-up; the record is kept as stored.
  - **non-zero and matches nothing**: the token was changed by something that
    never reached the disk (a rotation whose replacement record was not stored)
    or by another host. That is **not** a power cycle: the zero is dropped and the
    window is uncertain, so an undeclared re-zero is refused until the operator
    declares mid-travel. The same applies at reconnect, also with no record at
    all, and is persisted.
  A token that cannot be read fails the move; at reconnect it is *unknown*: the
  record is kept, motion refused, and the next successful poll (or Set zero)
  decides. Relies on the register being cleared at power-up: part of hardware
  acceptance.
- **`power_up_token_register: 0` is hardware-acceptance only.** Config refuses
  it unless `reference.allow_session_only_zero: true` is set too (`setConfig`
  refuses it as well). It turns power-cycle detection off and persists nothing;
  the snapshot says `sessionOnlyZero` and the panel shows an alert.
- On the next `connect()` the record is restored only if the serial and token
  both match, and the zero only if it was valid and no e-stop or driver alarm
  is active. A power cycle clears the register, so the stage needs Set zero
  again, with a fresh window.
- **The zero is dropped, the window is not.** An e-stop or driver alarm seen at
  any poll, `applyProfile()` and the operation failures listed under
  Operations only mark the record `zeroValid = false` and keep the window of
  this power-up's first zero (also across an application restart). The next
  undeclared Set zero must still be inside it, so a fault cannot be used to
  start a fresh ±1000 µm. Only a new power-up (token change) or a
  mid-travel declaration starts a new window. An operator Stop keeps the zero.
- **No file I/O under the service lock.** Deleting a stored record is queued
  (`storeClearPending_`) and done by the worker right after, outside `mutex_`
  (`flushStoreClear`; a successful save of a newer record supersedes a deletion still
  pending, so a retry never erases it), as are saves, the port enumeration and the limit-record
  read, so a slow disk cannot hold `stop()` or `snapshot()`.
- **Set zero publishes the status it reads**, exactly as a poll would: a fault seen
  there drops the existing zero too, even if it clears before the next poll.
- **The invalidation is made durable by the worker** after the job or poll that
  caused it (`persistInvalidation`). If the store refuses the write, the
  controller token is rotated (once), so a stale "valid" record can never match
  again, and the replacement record is **retried on every poll until it is
  stored** (also tried before a Disconnect). If the store never recovers, a
  restart cannot know this power-up's window: that is reported in `lastError`.
- **A fault seen while the stored zero is not trusted yet** (token unknown at
  reconnect) still costs it, even if the fault clears before the token can be
  checked.

## Travel envelope

Moves must stay inside `[envelopeMin, envelopeMax]` µm around the zero. The
edges round inward to whole micrometres.
- **Default ±1000 µm** (`envelope.default_um`, 1–1000: a rig may only lower it),
  intersected with the window above.
- **±(span/2 − `soft_limit_margin_um`) = ±2900 µm** only when the operator
  declared "the stage is at mid-travel" for this zero. The declaration is
  cleared by the next Set zero. It replaces what Home used to prove, so it is
  the only protection against the hard end stops.
- **Refused, never clamped.** A target outside the envelope is refused
  (`OutOfSoftLimits`), and so is a move whose approach overshoot waypoint
  would leave it.

## Limit bits: a backstop that only stops

- A move toward an active limit switch is refused before anything is sent
  (`LimitSwitch`), and a limit that becomes active in the direction of travel
  during a move stops the axis (host-side stop on every poll). A stage
  sitting on a switch can always move away.
- The bits gate nothing else: not Set zero, not other moves, not widening the
  envelope. The home bit is a floating input (reads 1 on all three axes of the
  bench unit) and is ignored.
- The record `<dataDir>/stage_limits_verified.json` (written only by
  `zc300ctl verify-limits --supervised --allow-motion --data-dir <dataDir>`,
  `stage::verifyLimits`) only clears the panel's "wiring unverified" badge.
  It is re-read on idle polls, so a record written while the app runs counts
  without a reconnect. No facade, bridge, server or UI path can write it. The
  procedure is operator-paced, bounded to ≤ 500 µm controller-bounded steps at
  200 µm/s, checks that the right switch trips at each end, and returns to the
  start. The snapshot exposes `limitsVerified`.

## Panel

The Tauri panel is described in [[../architecture/Desktop-Shell]] (Z stage
panel). It mirrors these rules in the UI and shows why a control is disabled.

## Operations

- **Fresh facts before every opcode.** Admission uses the cached status; each
  `moveAndWait` leg re-reads the status, publishes it (an e-stop or alarm drops
  the zero), refuses an e-stop, alarm or already-moving axis, re-checks the zero
  and the envelope, and compares the power-up token, all before the opcode.
- **An experiment and a moving stage exclude each other (#533).** Experiment Start is
  refused (`Busy`, "a Z stage operation is active") while a stage operation is active
  (`ExperimentCoordinator::setStageBusyProbe`, checked under the coordinator lock, which
  is also held while a stage move is queued, so check and queueing cannot interleave).
  The worker re-checks `setMotionGate` (the coordinator's idle gate) outside every
  service lock right before each opcode, every leg; a refusal ends the operation `Busy`
  with the zero kept. Lock order: coordinator, then stage; the gate is never called with
  the service mutex held.
- **Stop beats a queued move.** `stop()` bumps the service's stop epoch before
  and after it reaches the driver, the driver counts every `stop()`
  (`IMotionStage::stopGeneration`), and `moveAbsolute(target, generation)`
  fails with `Stopped` under the driver lock if a Stop arrived after the
  generation was read at the start of the leg. The service also checks
  cancellation immediately before the opcode.
- **One at a time on the worker.** `moveTo` (absolute) and `moveBy` (relative
  to the µm grid point nearest the current position) are each validated
  twice: before they are queued, and again when they start. The checks are
  connection, configuration, zero set, travel envelope (including the
  approach overshoot), limit direction, whole micrometres, and no e-stop or
  alarm.
- **Each operation returns an id**, and ends in exactly one terminal state:
  `Completed`, `Failed`, `Cancelled` or `TimedOut`.
- **One-sided approach.** When the final motion would run against
  `approach.direction`, the move first overshoots by `overshoot_um`, then
  approaches. The overshoot must itself stay inside the envelope, or the move
  is refused.
- **Deadline** = distance / speed × `move_timeout_margin` + 1 s. A missed
  deadline stops the axis and reports `TimedOut`.
- **Failures that may desync the open-loop counter drop the zero** and
  delete the record: limit switch, e-stop, driver alarm, lost ack, timeout,
  and transport or protocol errors. An operator Stop keeps the zero.

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
  - `connect()`, `applyProfile()` and `setZero()` are *exclusive* jobs: refused at once
    with `Busy` while an operation is active or another exclusive job is
    queued, and operations are refused (`Busy`) while one is pending, so
    nothing can slip in ahead of it.
  - `disconnect()` counts itself in `pendingDisconnects_` under the admission
    lock, cancels the active operation and stops the axis immediately, then
    queues the disconnect. The operation ends `Cancelled` at its next poll
    (≤ `poll_ms.moving`). While any Disconnect is pending, no operation,
    Connect or ApplyProfile is admitted. It is a counter, not a flag, so the
    first of two overlapping `disconnect()` calls cannot reopen admission
    while the second is still queued.
  - Found while designing the Tauri panel: before this, `applyProfile()`
    during a 3 s move blocked 2.9 s and then *applied and saved* the profile.

## Gotchas

- `StageConfig` comes from `parseStageConfig()` (the `stage` block). Wiring it
  into profile apply and the facade is slice 4. Today the shell, or a test,
  calls `setConfig()` and then `startup()`.
- `setConfig()` is refused while connected; disconnect first to change the
  endpoint.
- Positions are the open-loop counter. Missed steps from a collision or a
  hand move cannot be detected, so if in doubt set zero again (it needs a
  declaration to leave the first window).
- **Bench hold (2026-10-05):** the real stage stays read-only until the
  unexplained −6565 µm reading is resolved. Tests use the fake controller
  only.
