# ZC300Stage (motorized Z stage driver)

> `IMotionStage` driver for the Zolix ZC300 stepper controller and its
> TBZF6-60 lift stage, over the shared [[SerialBus]]. Literal and
> observe-only by default; soft limits, Home and backlash approach belong to
> [[StageService]] (ADR 0013, #464).

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
  meant for `StageService`'s once-per-power-up reference (ADR 0013 §6); its
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

One driver mutex serializes every public call, so multi-frame operations
(distance write plus opcode) are atomic with respect to other callers. The
bus session's call mutex is innermost (see [[SerialBus]]).

**Access is prioritized and bounded (`Zc300Stage::Access`).**
- Commands register as priority waiters. These are moves, `stop()`, writes,
  connect and token calls.
- Status polls (`readStatus`) step aside while any command is waiting.
- A command that cannot get the driver within 15 s returns `Busy`.
  `disconnect()` keeps trying, because teardown must not be skipped.
- Acquisition is `try_lock` in a short sleep loop, not `std::timed_mutex`,
  whose clock-based waits older TSan runtimes may not intercept.
- **Why:** with one back-to-back poller, an unfair `std::mutex` starved a
  move for 60 s on PR #511's TSan CI runner. Locally under TSan on two
  cores, three tight pollers made a move wait up to 2 s. With priority it
  waits ≤ 50 ms and `stop()` ≤ 30 ms (`backend.zc300_stage`
  "concurrency").

A `stop()` still waits behind the call in flight, at worst one read with its
retries (~2 s at the default timing). `StageService` owns polling threads.

## Gotchas

- `positionUm` is the controller's open-loop pulse counter. It is not a
  measurement, and it means nothing until Home (`StageService`). The driver
  always reports `referenced = false`.
- Moves overwrite the controller's front-panel step distance (30114), which
  is also the absolute-move target register.
- Only axis X is used on the ZC300-1A. On that model the Y/Z home bits float
  high.
- `SerialBus.cpp` is compiled into `oeabt_serial` (the shared native serial
  archive), so `zc300ctl` links without the backend and the Rust bridge's
  archive list is unchanged.
