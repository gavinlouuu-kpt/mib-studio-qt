# ADR 0005 — Device discovery is a backend job service with providers

- Status: accepted
- Date: 2026-09-15
- Issue: #419 (related #246, #272, #278, #307, #417, #418)
- Plan: [2026-09-15-device-discovery-service](../exec-plans/active/2026-09-15-device-discovery-service.md)

## Context

Device discovery ran from five independent places: `DeviceInitManager`'s
QtConcurrent workers (camera + nanopositioner with retries), `ConnectTab`'s
constructor/Refresh and its synchronous fallback on the UI thread,
`NanopositionerTab` Refresh, `ConfigTabs`' own `std::thread` pulse-generator
scan, and `BackendFacade::fetchCameraDiscovery` pulled synchronously by the
Tauri shell. Each had its own cancellation model (or none), its own timeout
policy and its own shutdown path; adding a device meant adding a sixth. The
headless/React–Tauri frontend could not reuse the Qt scheduling or the
auto-selection policy at all.

## Decision

1. **One backend module, not a plugin or a process.** `backend::discovery`
   lives inside `mib_backend` (Qt-free public headers, C++17 value types, no
   vendor handles). `AppBackend` constructs `DeviceDiscoveryService`,
   registers the compiled-in providers, and owns its shutdown.
2. **Providers keep SDK/protocol knowledge; the service keeps lifecycle.**
   A provider wraps existing enumeration/identity code
   (`CameraControlService::discover*`, `AutofocusService::availableEndpoints`
   + `probeEndpoint` through `nanopositioner::discover`,
   `PulseGeneratorService::scanBus`) behind `std::function` seams and returns
   value candidates with identity strength, identification status and
   structured errors. The service owns job IDs, one worker per job, deadlines,
   cooperative cancellation, retry scheduling, dedup/ambiguity rules, bounded
   snapshots, resource-class serialization, and observers.
3. **Discovery finds; policy decides; services connect.**
   `StartupDiscoveryPolicy` is a pure decision function over a snapshot.
   `StartupDiscoveryCoordinator` (backend application layer) runs the
   camera-then-nanopositioner startup sequence with today's delays/retries and
   calls the existing `AppBackend::set*CameraSelection` /
   `AutofocusService::connect` APIs. It never acts on a partial, cancelled,
   failed or overflowed snapshot and never when more than one device is
   identified.
4. **Frontends are adapters.** `frontend::DeviceInitManager` keeps its public
   API but only marshals coordinator outcomes and job completions onto the
   UI thread through `DiscoverySubscription`; tabs start jobs and render
   snapshots. `BackendFacade` exposes additive `startDeviceDiscovery` /
   `cancelDeviceDiscovery` / `fetchDeviceDiscovery`; the bridge contract
   appends the discovery enums (ABI 14) and the Tauri shell polls the job.
5. **Shutdown order is fixed.** Stop the coordinator (no new startup
   actions), then `shutdownDiscovery()` refuses new jobs, cancels and joins
   workers, then release serial hardware (#417). Workers are never detached;
   an uninterruptible vendor enumeration delays the join and is tracked as
   TD-10.

## Consequences

- Adding a device means adding one provider and (optionally) one policy rule;
  the scan lifecycle, cancellation, timeouts and UI marshalling are inherited.
- Every discovery entry point is headless-testable with fake providers; the
  Qt tests only cover marshalling and widget state.
- Identity is explicit: transient SDK indices are `SessionLocal` and are never
  persisted as identity; adapter VID/PID or a bare Modbus reply never counts
  as identification.
- The camera SDK enumerations remain non-interruptible; cancellation is
  honoured between provider steps and shutdown waits for the current call.
- Bridge consumers poll a job instead of blocking on a synchronous
  enumeration; the old synchronous facade wrapper remains for worker-thread
  C++ callers only and is documented as such.
