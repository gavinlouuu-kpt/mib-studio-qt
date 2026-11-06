## 2026-11-05 — Leaving the Experiment tab mid-run no longer stops the recording (#899)

On the MC2066 bench (PR #876 rerun, run 4) a click on the Connect tab during a 5000 fps run
stopped realtime processing for 3.3 s, and the run ended `incompleteLoss` with
`storeOverwritten=2,196,816` for a run of about 130,000 frames. Two causes, both fixed:

- **The tab switch stopped the run's consumer.** `MainWindow::onTabChanged` stops the Experiment
  services when the user leaves Overview/Experiment, and that stopped `ProcessingService` realtime
  processing, which admits, processes and saves the run's frames. Only Overview and Review are
  refused during a run. Now `stopExperimentServices()` only pauses the view while a run is starting,
  active or saving (the window's flags or the coordinator's state); the loop kept for the run is
  stopped when the run's end is rendered, if the Experiment tab is still not shown.
  [[frontend/MainWindow]].
- **A restarted realtime loop rewound to write index 0.** `realtimeInlineLoop` and
  `realtimeBatchLoop` stored `rtLastProcessed_ = 0`, so the first pass booked every index below the
  ring's oldest frame as `storeOverwritten` (the bench: the ring's oldest index at the restart was
  2,196,817). They now resume from `realtimeResumePointer()`: after the run's last admitted frame
  in a run, so only the frames evicted while no loop ran are booked; otherwise at the ring's oldest
  frame, with nothing booked. This also fixes the Start of a run with no loop running
  (`startExperiment`, then `startRealtime`), which booked the ring's history into the new run, and
  a restart before the ring wrapped, which admitted the run's frames twice.
  [[services/ProcessingService]].

Tests: `processing.realtime_ring_loss` (restart mid-run with the gap inside and past the ring,
realtime started after the run; inline and async-batch) and `frontend.experiment_tab_switch` (a
real `MainWindow` on a mock camera: the Connect tab mid-run, a run that ends on the Connect tab,
a run started by another client). Each half of the fix fails its own test when reverted.
