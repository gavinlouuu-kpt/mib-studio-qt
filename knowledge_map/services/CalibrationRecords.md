# CalibrationRecords

> The calibration run (S4) backend (bridge ABI 42, #716 A4a, ADR 0015 item 7): a run of a method
> whose purpose is *calibration* records a reference sample (rigid size beads) through the normal
> one-file-per-run path; the backend derives an instrument-scoped, versioned **calibration
> record** from that run's own file; the operator applies its pixel-to-micron proposal
> explicitly; the `calibration.record` readiness gate checks it per the method's policy.

**Source:** `include/backend/app/CalibrationMethodSpec.h` (the method's declarations),
`include/backend/app/CalibrationRecord.h`, `src/backend/app/CalibrationRecord.cpp`
(method spec parsing, derivation, record JSON, gate), `include/backend/app/CalibrationStore.h`,
`src/backend/app/CalibrationStore.cpp` (the append-only store and apply).
**Related:** [[../architecture/ExperimentCoordinator]] (gate, Start check, derivation at close),
[[../architecture/AppBackend]] (owner), [[../architecture/Rust-Bridge]] (ABI 42 commands, ABI 43
`method_calibration`),
[[../architecture/Desktop-Shell]], [[../data-model/HDF5-Storage]] (`calibration_record_id`),
[[ProfileRegistryService]] (the method document), [[ProcessingService]] (the factor).

## Responsibility

- **Method document.** config.json (the profile registry's method, #398) may declare
  `purpose` (`measurement` | `calibration`), `calibration_policy` (`none` | `warn` |
  `required`), `calibration_max_age_hours` (default 24) and, on a calibration method only (and
  required there), `calibration_reference {nominal_diameter_um, diameter_tolerance_um,
  nominal_e_kpa?, max_cv_percent? (default 10)}`. Absent: a measurement method with policy none,
  so legacy methods are unchanged. `parseCalibrationMethodSpec` is strict;
  `stageConfigDocument` refuses a whole document whose calibration members are malformed.
- **Where the gate reads them.** The coordinator parses the applied config text
  (`AppBackend::getLastConfigJson`). Its calibration members (`kCalibrationDeclarationKeys`)
  always reflect exactly the method that was just applied, on every apply path:
  - the central method (`applyCentralMethod` -> `applyConfigDocument`) stores the method's full
    text verbatim;
  - a local profile (`ProfileStore::apply`) builds a provenance document from the previous one
    plus the keys it applies; the calibration members are replaced, copied verbatim when the
    profile declares them (after the strict parse accepted them) and **removed** when it does
    not, so a declaration never carries over from an earlier method (absent = measurement,
    policy none);
  - applying processing settings only (`ProcessingSettingsCommand`,
    `applyProcessingConfigTransaction`) keeps the current method's declarations unchanged;
  - the retired Qt shell's `AppConfigWatcher` stores the watched file's full text.
- **What the readiness report says about the method** (#776, bridge ABI 43). Every readiness
  evaluation carries the applied method's declarations exactly as its `calibration.record` gate
  read them (`ExperimentReadinessSnapshot::methodCalibration` / `methodCalibrationError`, from
  `CalibrationGateResult::spec` / `specError`), so the workspace reads one document. On the bridge
  it is the report's `method_calibration`: `declared`, `purpose`, `calibration_policy`,
  `calibration_max_age_hours`, `calibration_reference {nominal_diameter_um,
  diameter_tolerance_um, nominal_e_kpa | null, max_cv_percent}` (non-null exactly for a
  calibration method, the CV default filled in) and `error`. With no method applied, or a method
  that declares none of the four members, it says so explicitly: `declared` false, measurement,
  none, 24 h, no reference, never missing members. `declared` is set by
  `parseCalibrationMethodSpec` when any member is present and not null. Because every apply path
  replaces the declarations (above), the report never shows an earlier method's. It is a separate
  member, not the gate's detail: a readiness gate on the bridge is `{id, status, reason,
  remediation}`, with no structured details. The types live in `CalibrationMethodSpec.h` (split
  from `CalibrationRecord.h` so `ExperimentReadiness.h` can hold them).
- **Derivation** (`readCalibrationEvidence` + the pure `deriveCalibration`). The run's valid
  objects (metadata only, as the normal core recorded them). Per object with a finite positive
  area: equivalent diameter `2*sqrt(area/pi)` px. Median, raw MAD and CV (sample SD / mean);
  `proposed_pixel_to_micron = nominal um / median px`; the diameter at the run's own factor
  against nominal ± tolerance (a drifted factor is reported, the proposal does not depend on it);
  the deformation median (report only). Event-rate stability: valid objects per 1 s window over
  the file's **declared** timestamp clock (acquisition provenance), at least three windows, else
  "not recorded" / "too short" / "not monotonic". Focus: the median Laplacian variance only when
  the file records that metric; otherwise "not recorded", never fabricated. E against the
  method's nominal only where both exist (E > 0 in the file), report only. Gates:
  `min_valid_objects` (200), `max_cv_percent` (the method's limit), plus `run_finalized` /
  `file_readable` when those fail. Any failed gate: status `insufficient`, never applicable.
- **Record.** id `cal-<UTC>-<8 hex>` (≤ 128 bytes); source run (run id, file, SHA-256, start
  generation, the background's SHA-256); method source / id / revision / config hash (a central revision: its registry ids and name; a
  local method applied from a local profile, #783: `method_id` = the profile's `profile_id`,
  `revision_id` = its revision, the sha256 of its files (null when settings changed after the
  profile's apply, `localProfileModified`: the profile is the base, not the exact config),
  `display_name` = its name, from `MethodProvenance::localProfile*`; a local text no profile names: those null and
  `config_sha256` identifies it; the profile's path is in the run's snapshot,
  `method.local_profile`); inputs; identity (camera = effective
  source:label, ROI `x,y,w,h` or `full`, chip and objective "not tracked" until a setup declares
  them); results and gates; status `proposed | applied | superseded | invalidated |
  insufficient`; on an applied version the previous and new factor. Unknown values are null.
- **Store** (`CalibrationStore`, `<dataDir>/instrument_calibration.json`, beside
  `instrument_identity.json`: instrument settings, allowed on the PZ7035 eMMC). Append-only: a
  status change is a new version, nothing is removed; at most 2048 versions (then appends are
  refused with the reason). Atomic writes (temp file, flushed, renamed); a failed write changes
  nothing in memory or on disk. A file that cannot be read is left untouched and the store refuses
  every change, so history is never lost silently.
- **Apply** (`BackendFacade::applyCalibrationJson` -> `CalibrationStore::apply`). Inside the
  coordinator's idle transaction (`withIdleConfiguration`, the lock Start takes): read the
  current factor, refuse unless the record is `proposed` (or `applied` but no longer in effect: an
  explicit re-apply), of the current known identity, and `expected_current_factor` matches the
  current factor (relative 1e-9), and refused while raw recording runs; the record's evidence is
  re-checked from its stored results (`evidenceSufficient`: gates, ≥ 200 objects, CV within the
  limit, a positive proposal), never trusted from its status alone; then append the previous applied record's `superseded` version
  and this record's `applied` version in one write, and only then
  `ProcessingService::setPixelToMicronFactor`. Nothing changes a method: the applied config.json
  text (the method's provenance hash) is untouched. Nothing applies on its own, not even at
  startup.
- **Gate** (`evaluateCalibrationGate`, see the table in [[../architecture/ExperimentCoordinator]]).
  An applied record is *in effect* when its identity is the current one and its factor is the
  current factor (`calibrationRecordInEffect`, the one rule the gate, the snapshot and the list
  use); that id goes into the run snapshot. Invalidation is hard: a known identity that
  differs from the applied record's appends its `invalidated` version (once). Age is soft (Warn).
  An unknown identity (no camera source, an unresolved one such as `unavailable`, a pending ROI)
  never invalidates and never confirms. An applied record whose stored evidence fails the re-check
  is never in effect.

**Client.** The workspace's Prepare Calibration card (#716 A4b) lists the records, offers Apply /
Re-apply (also straight from the gate's "re-apply record <id>" Fix after a restart) and the Plan
declares the record in effect at Start. Since ABI 43 it shows the applied method's purpose,
reference and policy from the report's `method_calibration` (an `error` beside the gate), never
parsed from the gate's text; see "Opt-in workspace" in [[../architecture/Desktop-Shell]].

## Key APIs

- The medians are `backend::review::median` (`RunStatistics.h`, since #716 A7a), the one the
  run quick look uses ([[RunQuickLook]]).
- `parseCalibrationMethodSpec`, `deriveCalibration`, `readCalibrationEvidence`,
  `calibrationRecordToJson` / `calibrationRecordFromJson`, `calibrationIdentityOf`,
  `evaluateCalibrationGate`.
- `CalibrationStore::latestRecords / latest / history / applied / addDerived /
  invalidateOnIdentityChange / apply`.
- `AppBackend::calibrationStore()`; `BackendFacade::calibrationRecordsListJson /
  calibrationRecordGetJson / applyCalibrationJson`.

## Gotchas

- A factor changed after the apply (Settings, a method apply) takes the record out of effect:
  the gate says so and offers "re-apply record <id>"; the snapshot then records null, and a Start
  declaring that id is refused. The calibrated factor is not restored automatically after a
  restart either (the method's factor applies); the operator re-applies.
- The derivation runs in the finalization before the terminal status (the file is hashed in full);
  bead runs are short, but a very large calibration file delays the terminal status by its hash.
- The review export / reanalysis lanes may still read files beside a run (the existing #722 risk);
  the derivation adds a reader only while the coordinator is Stopping, never beside a review
  session.
- Chip and objective identity are "not tracked": no setup declares them yet, so they never
  invalidate a record until one does.

## Tests

`backend.calibration_record` (spec parsing, derivation on synthetic bead runs: good, too few,
high CV, drifted factor; focus / E / event rate never fabricated; the gate per policy; the store's
apply compare-and-set and every refusal, supersede, invalidation, persistence, fault injection,
no change from reads), `e2e.calibration_run` (through the facade with the real coordinator: no
actuation from listing / polling, the policy gate, the derivation at close, apply refused during
a run / stale / insufficient, Start identity mismatch, a changed factor, ROI invalidation, an HDF5
bead round trip), `e2e.calibration_profile_apply` (the declarations through the real apply
paths: a profile with `required` blocks Start, a calibration profile derives its record, a
measurement profile without declarations inherits nothing, processing settings keep them, the
central method text is verbatim; since #776 also the readiness report's method calibration after
each apply: no method, a policy, a calibration method's reference, nothing stale after switching,
and unreadable declarations: the defaults, the reason, the gate failing closed, cleared by a
readable text),
and the bridge tests listed in [[../architecture/Rust-Bridge]] (ABI 42, ABI 43).
`backend.calibration_record` also pins `declared` (absent, null and explicit-default members).
