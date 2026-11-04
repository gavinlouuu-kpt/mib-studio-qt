## 2026-11-05 — Tauri: a Latest Frame recording can start, after Qt's question (#858, SHELL-054); Start/run-status fixes from the #877 bench

In the real-camera parity run (S7) a Tauri Start with a Latest Frame capture was always refused
("Latest Frame delivery discards frames; acknowledge the policy or switch to Every Frame"): the
backend requires the operator's acknowledgement and no bridge command could carry it.

- **Bridge ABI 45:** `experiment_start` gains `acknowledge_latest_frame_drops`
  (`acknowledgeLatestFrameDrops` on the Tauri command and the server's dispatch, optional, default
  false). It reaches `ExperimentCommand.acknowledgeLatestFrameDrops`; the coordinator's rule is
  unchanged. See [[architecture/Rust-Bridge]].
- **Start flow** (`desktop/src/experimentStart.ts`, `areas/monitoring/startReadiness.ts`): when the
  preflight readiness's `camera.deliveryMode` gate warns, Qt's question (MainWindow::onStartExperiment)
  opens before the file dialog, word for word, and Continue sends the acknowledgement; Cancel starts
  nothing. A refusal with the backend's Latest Frame message (its rule is wider than the gate: a capture
  switched after the preflight, an instrument with no live-camera gate) asks the same question and
  starts once more. Switch to Every Frame is built in (`switchToEveryFrame`) and is offered once the Connect
  stage's delivery-mode setter (#863) is wired; Switch also acknowledges, because the running capture
  keeps Latest Frame until it restarts, and the start notice says so.
- **Dialog:** `transport/dialogs.choose`, a question with two or three labelled buttons (the dialog
  plugin's custom buttons; a browser asks one confirm per answer); a closed dialog is Cancel, a dialog
  that cannot be shown rejects (the Start flow logs it).
- Tests: `contract.rs` `latest_frame_start_needs_the_acknowledgement`, the dispatch test,
  `experimentStart.test.ts`, `startReadiness.test.ts`, `useMonitoring.dom.test.tsx`,
  `transport/dialogsChoose.test.ts`. See [[architecture/Desktop-Shell]].

Three findings of the #877 bench run on win11-agent (the w1-monitoring stream's BENCH-REPORT B3, B5, B6),
in `areas/monitoring/useMonitoring.ts`:

- **B3:** a Start refusal's alert ("Experiment readiness: ...") stayed until the next Start, beside a
  standing "Ready to start". It now holds while the readiness it was refused under stands (the flow's
  own preflight read refreshes the standing readiness) and clears when that readiness decides
  differently (gate ids and statuses; reason texts with live memory figures do not count). A Dismiss
  button puts away a refusal the standing readiness cannot see (the chosen file, the backend's reply).
- **B5:** the run state flashed the previous run's end state for 7-18 ms between Starting and Running;
  Starting now lasts until the status after the reply is set.
- **B6:** the outcome sentence showed the persisted count as "admitted" for ~20 ms; it is not shown
  until the run's accounting read is back (or failed), at most 5 s. A read cut short by a remount
  (StrictMode) is read again.
