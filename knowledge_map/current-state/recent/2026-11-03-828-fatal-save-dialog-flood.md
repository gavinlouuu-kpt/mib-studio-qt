## 2026-11-03 — A full experiment buffer no longer crashes the GUI with nested save dialogs (#828)

[[services/ProcessingService]], [[frontend/MainWindow]].

**Symptom.** Users reported a crash when saving images. The MIB rig (Coaxlink
CXP-12, 5000 fps, `1.2.0-beta.6cebe9c3`) crashed 5 times on 2026-11-03, each
1-3 s after Start. Each log shows one `Experiment frame backlog capped`, then
1,100-2,200 `Fatal save error: Experiment buffer capacity exceeded` lines within
about a second, then nothing. The crashpad minidump is a stack overflow
(`0xC00000FD`) on the GUI thread, with the same three app frames nested 60 times
inside Qt Widgets/USER32 dialog frames.

**Cause.**
1. The first HDF5 batch did not land before the 512 MB experiment buffer filled.
   That is the byte cap; the frame cap was not reached.
2. `appendExperimentFrame` then reported the capacity error for every refused
   frame from all 32 workers.
3. The MainWindow fatal-save sink queued one `QMessageBox::critical` per report.
   Each dialog's nested event loop ran the next queued report, so the dialogs
   nested until the stack ran out.

**Fix.**
- `ProcessingService` reports the capacity error once per experiment.
- The MainWindow sink presents only the first error. During a run it holds the
  dialog for the terminal status (the existing deferred-dialog rule). Outside a
  run it opens the dialog with `open()`.

**Tests.** `processing.experiment_capacity_error_latch`.

**Not fixed here.** The save path still cannot sustain 5000 fps of
about 0.5 MB frames on the rig, so such runs still fail; they now fail cleanly
instead of crashing. Tracked in #828.
