# ZC300Stage (motorized Z stage driver)

> `IMotionStage` driver for the Zolix ZC300 stepper controller and its
> TBZF6-60 lift stage, over the shared [[SerialBus]]. Literal and
> observe-only by default; the travel envelope, "Set zero here" and backlash
> approach belong to [[StageService]] (ADR 0013 + Amendment 1, #464). The
> stage is never homed; the driver's `setPosition(0)` is the operator's "Set
> zero here".

**Source:**
- interface: `include/backend/stage/{IMotionStage,StageTypes,StageProfiles}.h`
- factory and `toString`: `src/backend/stage/StageFactory.cpp`
- pure protocol: `include/backend/stage/zc300/Zc300Protocol.h`, `src/backend/stage/zc300/Zc300Protocol.cpp` (`stage_zc300_protocol`)
- driver: `include/backend/stage/zc300/Zc300Stage.h`, `src/backend/stage/zc300/Zc300Stage.cpp` (`stage_zc300`)
- CLI: `tools/zc300ctl/main.cpp` (`MIB_BUILD_STAGE_TOOLS`)

**Tests:**
- `tests/backend/zc300_protocol_test.cpp`: vendor known-answer frames, decoding, µm encoding.
- `tests/backend/zc300_stage_test.cpp`: driver over a real `ModbusBusSession`.
- `tests/support/fake_zc300.h`: simulated controller with the bench quirks.

**Related:** [[SerialBus]], [[ISerialPort]], [[AutofocusService]] (the piezo
focus actuator; separate device class). Evidence:
`docs/integration/zc300-z-stage.md`. Plan:
`docs/exec-plans/active/2026-09-30-zc300-z-stage.md`.

## Responsibility

- `connect(endpoint, profile, …)` is **observe-only**. It:
  - reads the model, serial and firmware over FC04;
  - reads the axis unit, stage type, lead and pulses/rev;
  - compares them with the `StageProfile`.

  It writes nothing. A mismatch leaves the stage connected but
  `isConfigured() == false`, and every motion call returns `Misconfigured`
  until `applyProfile()`.
- **Units are micrometres.**
  - The controller runs in mm mode. Distances are sent as
    `round(um)/1000 + 1e-5` mm, which defeats the controller's 0.001 mm
    truncation and float32 loss.
  - Targets that are not whole micrometres return `OffGrid` and are never
    rounded.
  - Readback keeps pulse resolution (0.4375 µm for the TBZF6-60).
- **Motion:**
  - `moveAbsolute` (opcode 0x64) and `moveRelative` (0x65) put the distance in
    the step-distance register, then send the opcode with P/N.
  - `jog` is 0x66. `stop` is 0x68 and is allowed even when misconfigured.
  - `setPosition` writes 30059 and redefines the coordinate without motion.
  - `setSpeed` takes µm/s and µm/s².
- `applyProfile` is the only configuration write. It sets the stage type,
  lead, pulses/rev and unit, then saves to flash (allowing a 3 s ack for the
  save). It is refused while the axis moves.
- `read/writePowerUpToken` use the volatile reserved register 30054. It is
  meant for `StageService`'s once-per-power-up zero (ADR 0013 Amendment 1); its
  hardware behaviour is still unverified.

## Wire rules

- Reads and idempotent writes (including `stop` and the save) are retried
  up to 3 times on silence, because the controller drops one request right
  after a move ends.
- **Motion opcodes are sent exactly once.** On a lost reply the driver reads
  status: if the axis is moving, the command was accepted; otherwise it
  returns `LostAck` and does not re-send.
- Exception codes map to errors: 06 `Busy`, 07 `LimitSwitch`, 08
  `EmergencyStop`, 09 `NotEnabled`, 04 (save while moving) `Busy`.
- `disconnect()` stops the axis only if it is moving; an idle disconnect writes
  nothing.

## Threading

One driver lock serializes every public call, so multi-frame operations
(distance write plus opcode) are atomic with respect to other callers. The
bus session's call mutex is innermost (see [[SerialBus]]).

**The lock is fair, prioritized and bounded (`Zc300Stage::Access`).**
- Waiters queue, and a release **hands the driver straight to the next
  waiter**: **Stop first**, then commands and teardown (FIFO), then status
  polls (FIFO). Nobody can jump the queue.
- **Stop has its own queue**, served before everything else. It is sent right
  after the call in flight, never behind queued moves or teardown. To keep a
  Stop storm from starving a waiting Disconnect, at most four Stops are
  granted in a row while anything else waits.
- Commands are moves, writes, connect and token calls. Polls are
  `readStatus`. `stop()` is its own kind.
  Every `stop()` bumps `stopGeneration()` before it waits for the driver, and
  `moveAbsolute(target, generation)` returns `Stopped` under the driver lock
  if a Stop arrived after the caller read the generation: a move decided
  before a Stop never starts motion after it. The generation is checked again
  right before **every** motion opcode (move, relative move, jog), after the
  target write, and without a caller-supplied value it is read when the call
  starts, so a Stop queued behind a call in flight still wins.
- **Stop latency.** While a Stop waits for the driver, the retries of the call in
  flight give way between attempts (`Stopped`) instead of burning through
  4 × `transactionMs`, so a Stop sits behind at most one transaction. A profile
  Save (up to 3 s) is not interruptible, but it only runs on an idle axis with
  no operation (the service admits nothing meanwhile), so no motion is pending.
- **Test hook:** `enableGrantLog()` / `grantLog()` record the order in which calls
  were *granted* (S stop, C command, P poll, L lifecycle); tests assert on it,
  not on the order threads happened to return (#532).
- A command or poll that cannot get the driver within 15 s returns `Busy`.
  `disconnect()` waits its turn however long it takes, because teardown must
  not be skipped.
- The queue state sits behind a small `gate_` mutex that is never held during
  bus I/O. Waits use a condition variable's `wait_for`, not
  `std::timed_mutex`, whose clock-based waits older TSan runtimes may not
  intercept.
- **History:**
  - A plain `std::mutex` is unfair: with one back-to-back poller it starved
    a move for 60 s on PR #511's TSan runner.
  - #511's fix (commands as priority waiters, polls yield) used a `try_lock`
    plus 200 µs sleep loop. That is unfair too: a thread that re-locks within
    nanoseconds keeps the driver while sleepers lose every race. PR #516's
    plain Linux lane saw a poll refused after 15 s, with commands fine
    (slowest 40 ms).
  - The queue fixes both. `backend.zc300_stage` "fairness" runs six tight
    pollers for 1.5 s: each must get at least a quarter of the average
    share, and none may wait 2 s. Before: fewest 1 of a mean 25 and waits
    of 5–8 s. After: 25 of 25 and a worst wait of ~60 ms.
- **Not only a test problem:** a UI or server polling `readStatus` in a
  tight loop on real hardware could have starved Stop for seconds the same
  way. In production the worker thread is the only regular poller, so this
  is defence in depth, not a live defect.

A `stop()` still waits behind the call in flight, at worst one read with its
retries (~2 s at the default timing). `StageService` owns polling threads.

## Gotchas

- `positionUm` is the controller's open-loop pulse counter. It is not a
  measurement, and it means nothing until the operator sets zero
  (`StageService`); even then a hand move or stall is invisible. The driver
  always reports `zeroSet = false`.
- Moves overwrite the controller's front-panel step distance (30114), which
  is also the absolute-move target register.
- Only axis X is used on the ZC300-1A. On that model the Y/Z home bits float
  high, and a read-only bench check (2026-10-06, register 30015 = `0x0124`)
  found the home bit set on all three axes and the limit bits clear on all
  three, so the limit wiring is unproven. The software ignores the home bit
  and uses the limit bits only as a stop-while-moving backstop.
- `SerialBus.cpp` is compiled into `oeabt_serial` (the shared native serial
  archive), so `zc300ctl` links without the backend and the Rust bridge's
  archive list is unchanged.
