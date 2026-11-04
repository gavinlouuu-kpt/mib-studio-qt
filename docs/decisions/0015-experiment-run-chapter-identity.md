# 0015. Experiments group runs; one file per sample; chapters are recorded, never inferred

Date: 2026-10-14
Status: accepted (owner decisions 2026-10-14; not yet implemented; item 3 deferred 2026-10-15 — see Amendment 1)

Plan: [eGrabber workspace](../exec-plans/active/2026-10-14-egrabber-workspace.md) ·
Design note: [Forecyt fold-in](../design/egrabber-workspace/FORECYT.md) ·
Related: [#314](https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/314) (run segments, conditions, comparison),
[#393](https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/393) (run identity, journal),
[#397](https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/397) (annotations),
[#462](https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/462) (experiment intent),
[#523](https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/523) (Cytoflow analysis)

## Context

Operators measure several samples in one sitting: a vehicle and two drug
concentrations for two donors, or one sample whose treatment changes partway
through the recording. Review then has to answer per-sample questions:
- n per sample;
- median deformability per condition;
- which sample lost frames;
- how a dose series compares across donors.

At the base commit of this decision none of that can be answered reliably:

- **One run is one HDF5 file**, chosen in a save dialog. `ExperimentStartRequest` carries only
  `outputPath`, `readinessGeneration`, `profileId` and two acknowledgements
  (`include/backend/app/ExperimentCoordinator.h`). The file is created with `H5F_ACC_TRUNC`, and
  Start is refused while any file is open.
- **Per-record identity is local to the file.** `ProcessedFrameMetadataRecord` has `index`,
  `timestampNs`, `objectId` and track fields, but no run, sample or segment identity. On the
  PZ7035 the PL `runId` and `epoch` (`PzRecords.h`) are dropped at ingest.
- **Accounting is run-level counts only.** Empty frames are never persisted, so per-sample
  dropped or failed counts **cannot be reconstructed afterwards** from the stored records.
- **Provenance has gaps that block cross-run comparison.** It does not record the E-modulus LUT
  identity, pump rates, LED/stage readbacks, operator, medium or temperature.

High-throughput plate cytometry software assigns events to wells by *inferring* boundaries in one
continuous stream. That is its most fragile step. MIB controls start, stop and sample changes, so
it does not need to infer anything.

## Decision

1. **An experiment is a set of runs.**
   - Each run's snapshot records an `experiment_id` (UUID), a `run_index` within the experiment,
     and a `sample_ref` into the experiment's sample sheet.
   - The plan itself (sample sheet, order, stop conditions) lives in the workspace store from
     #393/#395, behind the injected adapter. The core gains no SQLite.

2. **One file per sample.**
   - In a manual-swap queue, each sample is recorded as its own run, in its own HDF5 file.
   - Samples are never appended to an existing file.
   - The TRUNC, single-open-file and settings-frozen-per-run rules are unchanged.
   - "Next sample" means: stop → drain → finalize → confirmed close, then a new Start for the
     next queue entry after its readiness passes.

3. **Chapters mark sample changes inside one run.** This is the #314 "run segment". They are used
   only when the sample changes without stopping, e.g. a drug added mid-run or a flow-rate step.
   - A coordinator operation, `mark_chapter`, opens a new chapter at the current authoritative
     index: FrameStore write index on the host, PL frame id on the PZ7035. It does not touch
     configuration.
   - At each boundary the coordinator writes a row to a new `/chapters` compound dataset (with its
     own `chapters_schema_version`): chapter id, `sample_ref`, label, reason, start/end index,
     start/end wall time, accounting and telemetry snapshots, and pump state.
   - Every record gets an appended `chapterId` (u32) member, stamped at ingest:
     - in `ProcessedFrameMetadataRecord`;
     - in `/recorded_frames/metadata`;
     - in `/trigger_events`.

     `0` means unchaptered. Legacy files read as unchaptered through the existing field-aware reader.
   - Boundaries are **never inferred from event rate or image content**. Review may *flag* a
     chapter (loss inside it, low n), but it cannot move a boundary in the recording.

4. **Planned annotations are written once into the file, as planned.**
   - At Start the run writes an immutable, versioned `/experiment_annotation` block. It holds the
     sample's planned condition, experimental unit (e.g. donor), and concentration with unit.
   - Later corrections and reported or measured values are versioned annotations in the workspace
     store (#397, #523 FJ01). They never rewrite the recording.

5. **Run snapshot v3 completes provenance.** It is additive to snapshot v2 and adds:
   - E-modulus LUT id and sha256;
   - pump configuration and readbacks;
   - LED and stage/piezo readbacks where the device reports them;
   - the *selected* operator, per #393 (not authentication);
   - the applied calibration record id (item 7);
   - declared channel, medium and temperature, recorded as unknown when not declared.

   The PZ7035 ingest stops dropping `runId` and `epoch`.

6. **Auto-stop is an option on each run.**
   - The operator chooses one of:
     - manual (default);
     - a target count of valid objects (#462);
     - a duration;
     - both, whichever comes first.
   - The backend owns the condition. The UI never times the stop itself.
   - The stop reason is recorded in the run. A queue advances only after the stopped run is
     confirmed closed.

7. **Calibration runs use the same run model.**
   - A calibration run is a run whose method has the purpose *calibration*. It records a reference
     sample through the normal one-file-per-sample path; there is no separate acquisition path.
   - Its results become an instrument-scoped **calibration record** (#393). The record is a
     proposal until an operator explicitly applies it.
   - Later runs reference the applied record's id in snapshot v3.
   - Rigid size beads are the standard reference. Only pixel-to-micron may be applied from a
     record; deformation offset and E-vs-nominal are checked and reported, never corrected.
     Derivation runs in the backend. Details and expiry policy are in the execution plan (P2).

8. **Out of scope:**
   - sample identity in the processing-core C ABI or the PL record format;
   - plate, rack and carousel formats until an autosampler exists. It would be a separate device
     class behind an interface, like `IMotionStage` ([0013](0013-motion-stage-device-class.md)),
     never actuated by discovery;
   - electronic signatures and 21 CFR Part 11;
   - pass/fail-only QC runs that produce nothing the instrument uses (item 7 covers calibration).

## Amendment 1 (2026-10-15): chapters deferred

Item 3 stays decided but is not scheduled. The sample queue ships as one file per sample
only; `mark_chapter`, `/chapters` and `chapterId` are implemented when a mid-run
sample-change experiment is actually planned. Until then a sample change means a new run.
Rationale: the chapter path touches the HDF5 compound, ingest, accounting, the bridge and
the review contract for a secondary use case.

## Consequences

- **Bridge.** `experiment_start` grows a request carrying experiment, run, sample and stop-condition
  fields, and `mark_chapter` is new. Both are additive under [0004](0004-bridge-contract-and-operation-state.md).
  The ABI number is reserved with the merge coordinator.
- **Review.** The review contract gains chapter columns and a chapter list. Review ABI 2 stays
  separate from the main contract, per [0014](0014-yofo-review-on-react-tauri.md).
- **HDF5.** The new blocks are additive and versioned. Writers need round-trip, legacy-read and
  fault-injection tests. The chapter path also needs lifecycle e2e, concurrency stress and TSan,
  because boundaries are taken while the writer runs.
- **Analysis.** The #523 Cytoflow worker groups by experiment, run, chapter and annotation for:
  - per-sample statistics;
  - small multiples;
  - multi-run comparison;
  - dose-response fits.

  Fits and tests still require a declared experimental unit, pairing and weighting (#314). Cells
  are not replicates.
- **Sample queue, the first bounded worklist.** Every new mutating command joins the bridge
  server's `CONTROL_COMMANDS` allowlist. Losing the controlling client must leave fluidics safe
  before unattended queues are offered.
- **Out-of-scope features need their own ADR** if they are ever wanted (see item 8).
