# Camera delivery-mode hardware validation

Hardware acceptance procedure for the frame delivery modes (`Every Frame` /
`Latest Frame`) introduced by the delivery-mode epic
([#328](https://github.com/KPT1020/mib-studio-qt/issues/328)). Run it per
camera backend (EGrabber, MindVision) whenever backend queue policy, buffer
counts, or startup sequencing changes. The automated counterparts live in
`tests/camera/delivery_mode_overload_test.cpp` and
`tests/camera/delivery_mode_contract_test.cpp` (simulated backend); this
runbook covers what CI cannot: real SDK queues and transports.

## Setup

1. Connect the camera and load the profile under test. Confirm the status-bar
   badge shows the intended mode; the badge shows the backend-confirmed mode,
   not the requested one.
2. Note the configuration for the report:
   - camera frame rate and exposure
   - delivery mode
   - announced/configured SDK buffer count (`config.json` → capture buffers)
   - `BufferPartCount` (EGrabber only; must be 1 in Latest Frame mode)
3. Enable diagnostics logging so the per-second acquisition-queue stats are
   recorded (queue depth, intentional discards, transport loss, underruns —
   see [status-metrics.md](status-metrics.md)).

## Runs

Execute each scenario in **both** modes:

| Scenario | Condition |
|---|---|
| Nominal | normal processing load, no artificial delay |
| Overload | injected processing delay greater than the frame period (e.g. heavy algorithm settings or artificially high frame rate) |

For each run capture at least 60 s of steady state and record:

- average, p95, p99, and maximum frame age (host dequeue − device timestamp;
  only meaningful on backends that report host-comparable timestamps, i.e.
  EGrabber on Windows)
- SDK output queue depth over time
- intentional discards
- transport loss / incomplete frames / underruns
- CPU usage and copy/processing duration

## Expected results

- **Every Frame, nominal**: queue depth ~0, zero intentional discards, frame
  age ≈ one frame period.
- **Every Frame, overload**: queue depth grows to the announced buffer count,
  then underruns accumulate; delivered frames stay in order; intentional
  discards stay zero. Frame age grows toward
  `bufferCount × framePeriod`.
- **Latest Frame, nominal**: behavior identical to Every Frame (no stale
  buffers to discard); intentional discards ≈ 0.
- **Latest Frame, overload**: intentional discards accumulate; frame age stays
  bounded near `framePeriod + processingTime`; queue depth stays near 0 after
  each grab; underruns stay near zero (input buffers keep circulating).
- **Counters reconcile**: sequence gaps observed downstream must equal
  intentional discards + transport losses; anything unexplained is a bug.

## Startup check (EGrabber)

After a fresh connect, verify from the log that the data stream starts before
the remote `AcquisitionStart` (single `grabber->start()` call — see
[safe-start-stop-egrabber.md](safe-start-stop-egrabber.md)) and that the first
delivered frame's timestamp postdates the start call: the first frame must not
be residue from a camera-first acquisition window.

## Mode-switch check

Toggle the mode from the Connect tab while live view is running, then restart
capture. Verify: the badge flips only after the backend confirms, no stale
frame from the previous mode is displayed after the restart, and repeated
switches do not shrink the available buffer pool (queue depth + input buffers
should still equal the announced count).

## Reporting

Attach the recorded table per backend/mode/scenario to the validation issue
and archive the diagnostics log. Test performance metrics go to MLflow
(`mlflow.yofo.bio`) via the `MLFLOW_TRACKING_USERNAME` /
`MLFLOW_TRACKING_PASSWORD` environment variables.
