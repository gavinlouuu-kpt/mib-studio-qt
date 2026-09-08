# Agent Onboarding

> Guided reading order for a fresh agent. ~10 minutes to acquire working context.

## Step 1 — Orient

1. Read `AGENTS.md` (top-level agent map: navigation, build commands, hard conventions).
2. Read [[architecture/Overview]] — what this app is and how it's layered.
3. Read [[architecture/Data-Flow]] — the realtime path from camera to HDF5.

## Step 2 — Understand the shared state

4. [[data-model/FrameStore]] — the ring buffer every layer shares.
5. [[architecture/AppBackend]] — how services are wired together
   (and [[architecture/ExperimentCoordinator]] — how an experiment Start is authorized).
6. [[architecture/Threading-Model]] — who runs on which thread.

## Step 3 — Pick the relevant cluster

Jump to the notes that match your task:

| If you're touching... | Start here |
|---|---|
| Frame acquisition / camera | [[services/CaptureService]] + [[camera/_MOC]] (`[[camera/MindVisionCamera]]`, `[[camera/EGrabberCamera]]`, `[[camera/MockCamera]]`) |
| Image analysis / metrics | [[services/ProcessingService]] + [[domain/Microscopy-Pipeline]] |
| Saving/reading experiment files | [[services/Hdf5Service]] + [[data-model/HDF5-Storage]] |
| Exporting CSV/TIFF from HDF5 (native or PySide tool) | [[services/HdfExportService]] + [[frontend/HdfReviewTab]] + [[task/2026-08-24-exporter-stability]] |
| Live preview / ROI / overlays | [[frontend/PreviewPage]], [[frontend/ConfigTabs]] |
| Post-experiment review | [[frontend/HdfReviewTab]] |
| Live charts during a run | [[frontend/ExperimentMonitoringTab]] |
| Processing-core versions / native hot-swap | [[frontend/ProcessingCoreDialog]] + [[services/ProcessingService]] |
| Autofocus / nanopositioner | [[services/AutofocusService]] + [[frontend/NanopositionerTab]] |
| Syringe pumps | [[services/SyringePumpService]] + [[frontend/SyringePumpTab]] |
| Pulse generator / shared RS485 bus | [[services/PulseGeneratorService]] + [[services/SerialBus]] + [[frontend/ConfigTabs]] |
| Crashes / observability | [[services/CrashReporter]] + [[diagnostics/CrashStateMirror]] |
| Pipeline / trigger latency diagnosis | [[diagnostics/PipelineTimingRecorder]] + `docs/howto/pipeline-latency-diagnosis.md` |
| Build / deploy | [[build-and-run/Build]], [[build-and-run/Run-Modes]] |
| User manual / generated screenshots | [[frontend/Screenshot-Tour]] + `docs/manual/README.md` |

## Step 4 — Before you write code

7. Scan [[conventions/Code-Conventions]] — spdlog over `std::cout`, reuse
   `Tools.cpp`, headers mirror src, etc.
8. Check [[current-state/Recent-Work]] and [[current-state/Task-Log-Index]] to
   see what's actively in flight and what pitfalls other agents have hit.

## Step 5 — Domain lookup

9. If microscopy terms are unfamiliar, skim [[domain/Glossary]].

## Ground rules

- Work on the feature branch specified in the task prompt.
- Never `std::cout` in app code — use `spdlog` (see [[conventions/Logging]]).
- Reuse existing utilities in `src/backend/Tools.cpp` before writing new ones.
- Task notes go in `knowledge_map/task/`; user docs go in `docs/`.

## Before you finish — update the vault

The vault is the onboarding path for the **next** agent. If your change
leaves the vault stale, you've broken that path. Every non-trivial commit
MUST include vault updates alongside code changes.

See [[Vault-Maintenance]] for the full source-file → vault-note mapping.
In short:

- **Touched a service?** Update `knowledge_map/services/<Name>Service.md`
  (Responsibility, Key APIs, Gotchas).
- **Touched a tab or dialog?** Update the matching note under
  `knowledge_map/frontend/`.
- **Added or removed anything?** Create/delete the atomic note AND update
  the cluster's `_MOC.md` AND the vault `README.md` AND this file.
- **Shipped a feature or non-trivial fix?** Add a line to
  `knowledge_map/current-state/Recent-Work.md`.
- **Found a stale note while working?** Fix it — don't leave it for next
  time.

Quick sanity check before committing:

```
grep -r '\[\[' knowledge_map/ | <verify each target exists>
```

If your PR has code changes but no vault changes, expect reviewers to push
back.
