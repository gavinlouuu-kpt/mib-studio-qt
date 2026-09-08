# Knowledge Vault — mib-studio-qt

Obsidian-style agent knowledge base. Each note is short and atomic; navigate by
following `[[WikiLinks]]`. If you are a new agent, start here:
[[Agent-Onboarding]].

## Map of Content

### Architecture
- [[architecture/_MOC|Architecture MOC]]
- [[architecture/Overview]] — layered design (frontend → backend → services)
- [[architecture/AppBackend]] — composition root
- [[architecture/ExperimentCoordinator]] — experiment readiness gates + frozen run snapshot
- [[architecture/Threading-Model]]
- [[architecture/Data-Flow]]

### Services (`src/backend/services/`)
- [[services/_MOC|Services MOC]]
- Realtime path: [[services/CaptureService]] → [[services/ProcessingService]]
- Persistence: [[services/Hdf5Service]], [[services/SqliteService]]; export: [[services/HdfExportService]]
- Playback: [[services/PlaybackService]]
- Hardware I/O: [[services/CameraControlService]], [[services/AutofocusService]],
  [[services/TriggerService]], [[services/SerialBus]],
  [[services/SyringePumpService]], [[services/PulseGeneratorService]]
- Optional: [[services/YoloService]], [[services/RecorderService]],
  [[services/BatchMaskSources]]

### Frontend (`src/frontend/`)
- [[frontend/_MOC|Frontend MOC]]
- [[frontend/MainWindow]], [[frontend/Controllers]]
- Native core selection: [[frontend/ProcessingCoreDialog]]
- Tabs: [[frontend/ConnectTab]], [[frontend/PreviewPage]],
  [[frontend/ConfigTabs]], [[frontend/ExperimentMonitoringTab]],
  [[frontend/HdfReviewTab]], [[frontend/NanopositionerTab]],
  [[frontend/SyringePumpTab]]
- Support: [[frontend/Dialogs]], [[frontend/System-Utilities]],
  [[frontend/Screenshot-Tour]]

### Camera (`src/camera/`)
- [[camera/_MOC|Camera MOC]]
- [[camera/ICamera]], [[camera/EGrabberCamera]], [[camera/MindVisionCamera]],
  [[camera/MockCamera]]

### Data model
- [[data-model/FrameStore]]
- [[data-model/HDF5-Storage]]

### Diagnostics
- [[diagnostics/_MOC|Diagnostics MOC]]
- [[diagnostics/CrashStateMirror]] — lock-free service-state snapshot
- [[diagnostics/PipelineTimingRecorder]] — per-frame pipeline latency recorder
- [[services/CrashReporter]] — process-level crash handler + Sentry

### Domain
- [[domain/Glossary]]
- [[domain/Microscopy-Pipeline]]

### Build & Run
- [[build-and-run/Build]]
- [[build-and-run/Run-Modes]]
- [[build-and-run/Dependencies]]

### Conventions
- [[conventions/Code-Conventions]]
- [[conventions/Logging]]

### Current state
- [[current-state/Recent-Work]]
- [[current-state/Task-Log-Index]]

## Maintenance

This vault is required reading *and* required writing. Every code change
must land with matching vault updates in the same commit. See
[[Vault-Maintenance]] for the source-file → vault-note mapping, and
[[Agent-Onboarding]] for the pre-commit checklist. `scripts/check_docs.py`
verifies wikilink integrity (enforced in CI).

## Related

- `AGENTS.md` — top-level agent map (read first)
- `docs/` — user-facing how-tos (`docs/howto/*.md`) and integration notes
- `docs/golden-principles.md` — mechanical rules for this repo
- `knowledge_map/task/` — dated task records (historical design/debug notes)
