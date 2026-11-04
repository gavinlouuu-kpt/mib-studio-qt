## 2026-11-05 — Monitoring bridge: live density, reference contour, E-modulus table, stats telemetry (#866 PR E, bridge ABI 46)

Backend and bridge groundwork so a later frontend PR can close the remaining Tauri Monitoring rows
(MON-008, 043, 058-064, 066, 068, 077, 083-086, 095, 098). No frontend change beyond the TS decoders and
`bridge.ts` functions. See [[architecture/Rust-Bridge]] (ABI 46, exact JSON shapes) and
[[services/MonitoringDensityService]].

**What is new (additive, nothing on the acquisition/processing path).**
- `monitoring_density_set_settings` and `monitoring_density_request_update` (control, VIEWER_ONLY on the
  WebSocket server) and `fetch_monitoring_density(sinceGeneration)` (read): the service that already ran
  for Qt is now reachable from Tauri and the WebSocket. Each point of a result carries its row's whole
  identity `(start_generation, capture_epoch, frame_index, object_id)` (new `MonitoringDensityKey`, filled
  from the ring slot's tag), because a frame index alone restarts with every capture and a frame can hold
  more than one object; the unchanged-input fingerprint includes the first and last key.
- `fetch_kde_reference_from_file(path)`: a reference core contour from an experiment file, read with a
  short-lived reader (never opens the file in Review), the full-run record first, the live one second,
  Qt's reason words, at most 20000 vertices. Refused with plain text while an experiment runs or starts
  (proposed decision: HDF5 is not thread-safe, #722, so the read takes the review lanes' lock and the
  session mutex inside the coordinator's idle transaction; Qt does not refuse).
- `fetch_emodulus_lut(knownIdentity)`: the loaded table once per identity, `unchanged` after.
- `fetch_processing_stats` adds `algo_avg_us`, `algo_avg_age_ms`, `realtime_mode` and a `camera` block
  (running, frame rate and data rate each with a validity state: valid, stale, unavailable, unsupported,
  error; never a 0 for an unknown rate). PR #877 decodes the two `algo_avg_*` names.

**Tests.** C++ `backend.monitoring_density_service` (identities), `e2e.monitoring_density_facade`; Rust
`contract.rs` `monitoring_density_is_described` / `monitoring_density_end_to_end`, `dispatch.rs`
`monitoring_density_through_dispatch`, the server's control-only test; `monitoringDensity.test.ts`.
