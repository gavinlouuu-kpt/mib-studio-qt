## 2026-10-11 — Stop leads to playback after an SSD run: the ring is held; the ring status carries the epoch (#693, D3, bridge ABI 37)
Until now an SSD run's end called `resumeLiveResults()`, which re-armed the provider: a new ring from sequence 0, so the frames the run had just buffered were gone about 7.9 s after the Stop click
(only the STOPPING window, while the drain empties the ring, could still read them). D3 of the SSD plan ("Stop during a recording closes the run and freezes the ring for review") is the #649 product
requirement, and it was accepted but not implemented. Now `ExperimentCoordinator::finalizeLocked` calls `AppBackend::holdRingAfterRun()` for a run with an SSD run id: if the provider's STOP froze a
readable ring (valid, no fault, no "restore needed", at least one frame; a STOP_STUCK ring counts, flagged) and the instrument is in Run, the Run is held like an operator Stop (`runFrozen`, LED off) and the
live session does not restart; otherwise it resumes as before and the log names the reason.
- **Ends:** Resume Run (`resumeRun`, a new ring), the next experiment Start (`leaveStoppedRun` before the run's own provider start arms; the gate `run.frozen` is now a Warn, "starting the experiment discards
  them", and the UI asks with a confirm that names the number of buffered frames), idle, or Studio stopping/restarting (the ring is RAM and is gone; no persistence).
- **Export:** the download of the run's records works while the ring is held and does not touch it (the SSD bounce buffer sits below the ring base); the unit test checks `fetch_ring_status` and a frame
  are byte-identical before, during and after an export lease. The board proof is the slot.
- **Bridge ABI 37:** `fetch_ring_status` (and the `ring` block) gains `epoch`, the ARM epoch the frame records carry; a frozen ring holds one ARM. `MIBR` is unchanged (version 1, no spare header bytes), so the epoch is per
  ring, not per packet. With it the ring-vs-SSD comparison (#693) matches by (epoch, frame_id).
- **Not changed:** only SSD runs hold the ring (an HDF5-only run resumes the live session as before); mode switches while held are refused ("resume Run first"), as after an operator Stop.
- **Tests:** `backend.instrument_modes` (`testRingHeldAfterSsdRun`: hold, no re-arm, refusals, Resume, leave, export during the hold, epoch in the status), `backend.ssd_experiment` (a replay provider with a ring: an SSD run
  leaves the ring held, Start from it works and discards it, the next run holds again, Resume Run), `ringPlayback.test.ts`.
