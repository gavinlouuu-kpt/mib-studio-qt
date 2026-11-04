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

**Tests.**
- `processing.experiment_capacity_error_latch`: 8 producers saturate the buffer; one report per run.
- `integration.fatal_save_dialog_e2e`: the real MainWindow runs an experiment at the rig's load.
  - **Load:** the 512x96 real-cell mock stream centred on a flat 1216x208 background, a 5000 fps target,
    every detected cell accepted, and the default 512 MB buffer.
  - **Writer delay:** each HDF5 append is delayed 1.5 s by default, the latency the rig's D: HDD showed
    per ~530-frame batch.
  - **Expected result:**
    - one Save Error box, opened after the run is terminal;
    - never more than one box open at once.
  - **Results on the rig, 2026-11-03:**

    | Run | Code | Outcome |
    |---|---|---|
    | Default settings | Fixed | Overflow at about 8 s; passes 2 of 2. |
    | Natural (`_SLOW_MS=0`, `_DIR` on the D: HDD) | Fixed | Overflow at about 16 s, with the same `buffered=1061` backlog line as the users' logs; passes. |
    | Natural (`_SLOW_MS=0`, `_DIR` on the D: HDD) | Unfixed | Crashes at 10.8 s after 1,238 `Fatal save error` lines. |

  - **Throughput ceiling:** the mock pipeline admits about 1.4-2.6k frames/s, not the full 5000 fps.
  - **Fast disk:** with the file on NVMe and no delay the writer kept up for 60 s, so no overflow.
- `MIB_MOCK_CAMERA_INTERVAL_MS` now accepts fractional milliseconds (`0.2` = 5000 fps). The how-to already
  documented that value, but `std::stoi` truncated it to 0 and the mock silently stayed at about 30 fps.
- The test honours `QT_QPA_PLATFORM` (default offscreen). On the Windows rig run it with
  `QT_QPA_PLATFORM=windows`, because the offscreen plugin stalls in the QApplication constructor there,
  as `integration.monitoring_scatter_e2e` does too.

**Not fixed here.** The save path still cannot sustain 5000 fps of
about 0.5 MB frames on the rig, so such runs still fail; they now fail cleanly
instead of crashing. Tracked in #828.
