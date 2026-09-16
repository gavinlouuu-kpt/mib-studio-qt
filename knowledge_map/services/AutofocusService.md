# AutofocusService

> Closed-loop autofocus: drives OEABT or CoreMOR piezo nanopositioners using
> ring-ratio feedback from [[ProcessingService]].

**Source:** `src/backend/services/AutofocusService.cpp`,
`include/backend/services/AutofocusService.h`,
`src/backend/nanopositioner/NanopositionerBackends.cpp`,
`src/backend/nanopositioner/oeabt/OeabtProtocol.cpp`,
`include/backend/services/AutofocusMath.h` (pure control math)
**Tests:** `tests/backend/autofocus_math_test.cpp`,
`tests/backend/oeabt_protocol_test.cpp`,
`tests/backend/autofocus_backend_safety_test.cpp`,
`tests/backend/oeabt_serial_pty_test.cpp`
**Related:** [[ProcessingService]], [[../frontend/NanopositionerTab]],
[[../domain/Glossary]] (ring ratio)

## Responsibility

- Manage backend-neutral endpoint discovery and connection (`availableEndpoints`,
  `probeEndpoint`, `connect`, `disconnect`). Legacy COM APIs wrap CoreMOR.
  Since #419 the static enumeration/probe pair is consumed by the
  `nanopositioner` provider of [[DeviceDiscoveryService]]; the UI never calls
  them directly. `setBackendFactory(factory)` swaps the driver factory while
  disconnected (test seam for fake stages behind a real `AppBackend`).
- Select OEABT (Linux/Windows) or CoreMOR (Windows) through
  `INanopositionerBackend`; require OEABT protocol identity before connection.
- Consume ring-ratio samples via `onRingRatio(ringRatio, timestampNs)`
  (wired by [[../architecture/AppBackend]]).
- Run a control loop on its own thread; manual voltage control
  (`increaseVoltage` / `decreaseVoltage`) too.
- Expose running statistics to the UI: `getAverageRingRatio`,
  `getMedianRingRatio`, `getLastRingRatioUpdateUs`.
- Emit human-readable status via `StatusCallback`.

## Config — `AutofocusService::Config`

- `focusSetpoint`, `focusRange` — target ring-ratio and tolerance
- `voltageStep`, `fineVoltageStep`, `maxVoltage`, `minVoltage`
- `initialVoltage` remains readable for config compatibility but connect is
  observe-only and does not apply it.
- `manualVoltageStep`
- `ringRatioStaleMs` — drop samples older than this
- `requireNewSamplePerStep`, `minSamplesPerStep`
- `safeShutdownVoltage` — applied on disconnect
- `focusDirection` — whether increasing voltage increases ring ratio

## Threading

Three threads are involved in the autofocus path:

| Thread | Lifetime | Role |
|---|---|---|
| Caller (ProcessingService realtime) | external | Calls `onRingRatio(ringRatio, ts)` on every valid frame — O(1) push into `pendingSamples_` + atomic freshness markers + `notify_one`. No sort, no deque work, no allocator pressure on the realtime thread. |
| `statsThread_` | constructor → destructor | `statsLoop()` drains `pendingSamples_` under `pendingSamplesMutex_`, writes into the `std::deque<double>` ring-ratio buffer (`ringRatioMutex_`), trims to `MAX_BUFFER_SIZE` (1000), and refreshes `{median, average, min, max}RingRatio_` atomics. Wake-rate is capped at ~100 Hz via a 10 ms min-drain interval so the O(n log n) sort amortises across a batch. |
| `controlThread_` | connect → disconnect | `controlLoop()` at ~20 Hz: reads stats atomics, owns all selected-backend I/O, and applies explicit manual or automatic voltage steps. |

Two mutexes: `pendingSamplesMutex_` (producer ↔ `statsThread_`) and
`ringRatioMutex_` (`statsThread_` ↔ `controlThread_`). The
producer-consumer split means the ProcessingService realtime thread
never touches `ringRatioMutex_` or the sort, so autofocus work can never
delay the [[TriggerService]] CV wake-up.

### Post-step buffer clear

After a voltage step the control loop clears **both** `pendingSamples_`
and `ringRatioBuffer_` (`std::scoped_lock` over both mutexes atomically)
so pre-step samples sitting in the inbox don't leak into post-step
statistics. Same pattern on `disconnect()`.

## Control math (`AutofocusMath.h`)

The voltage decision is extracted into pure, device-free functions in
`backend::services::autofocus` so it can be unit tested without a CoreMOR:

- `computeFocusVoltage(medianRingRatio, currentVoltage, FocusParams)` — given
  `deviation = median - setpoint`: a **coarse** `voltageStep` outside the
  acceptable range (`|deviation| > range`), a **fine** `fineVoltageStep` inside
  the range but beyond half the band (`|deviation| > range/2`), otherwise
  **hold**. `focusDirection` flips the sign. The result is always clamped to
  `[minVoltage, maxVoltage]`.
- `clampVoltage(v, lo, hi)` — clamps, but passes the value through untouched if
  the limits are inverted (`hi < lo`) rather than fabricating a bound.

`controlLoop()` delegates to `computeFocusVoltage`. `connect()` only identifies
the controller and reads limits/current voltage. Every write is validated
against configured limits and the controller-reported maximum; invalid values
are rejected rather than silently clamped at the transport boundary.

## Gotchas

- Candidate VID/PID values do not establish identity. OEABT requires the
  `Oeabt pzt controller` response before connection or any voltage write.
- `probeEndpoint` is static and must not run against the active endpoint.
- A read-only session disconnects without writing. After the first successful
  manual/autofocus write, intentional disconnect applies the validated
  `safeShutdownVoltage`.
- Native serial operations run on the calling thread; a mutex-backed proxy
  serializes complete operations, including multi-command voltage writes.
- **Resting stage reads slightly negative.** A CoreMorrow controller at 0 V
  returns about -1 mV (-0.0004 .. -0.002 V on the bench). The probe window
  (backend read validation) is therefore -0.05 V, not 0: with a floor of exactly
  0 the single-retry probe failed about one run in four, `hardware.nanopositioner`
  flaked, and at boot `DeviceInitManager` logged "saved nanopositioner COMn did
  not validate; scanning all ports" before connecting anyway (2026-09-08).
- `probeComPort` is **static** and requires the service to be
  **disconnected** — it opens/closes the port itself for discovery.
- `focusDirection` inverts the sign of voltage adjustments — wrong value
  causes runaway.
- Ring-ratio buffer does not track stream ID; if camera restarts, consider
  clearing state.
- `statsThread_` starts in the constructor and stops in the destructor —
  **not** in `connect` / `disconnect`. This is intentional: the UI still
  expects statistics even when the nanopositioner is not connected, and
  the realtime pipeline pushes samples regardless.
- `ringRatioSequence_` and `lastRingRatioUpdateUs_` are updated in
  `onRingRatio` (inline) so the control loop's freshness gate sees data
  arrival immediately even if `statsLoop` is a few milliseconds behind
  on the sort. The median may lag by up to ~10 ms (a single drain
  interval); the 50 ms control-loop tick absorbs this.

## Third-party

See `include/Coremor/` for the optional Windows XMT DLL and
`docs/integration/oeabt-nanopositioner.md` for the clean-room OEABT protocol
evidence and hardware acceptance gate.

## Platform behavior

- **Linux**: full autofocus service plus OEABT native serial backend; Linux's
  standard `ch341` driver handles the USB bridge.
- **Windows**: OEABT uses the existing native serial interface. CoreMOR is
  additionally available when `MIB_HAS_COREMOR=1`.
- `MIB_HAS_COREMOR` and `MIB_HAS_EGRABBER` are independent compile guards.


### PR #413 discovery integration

The shared vendor registry now uses native nanopositioner endpoints and includes
both OEABT and CoreMorrow/XMT probes. Startup scans all candidates on its worker,
auto-connects only a unique validated match, and Refresh repeats discovery.
Connection and serial/vendor controls are disabled while scanning. A legacy
COM-only setting retains its port preference but defaults to automatic vendor
selection; an explicit saved vendor is preserved. Discovery and connection are
observe-only, with no voltage or mode writes.
