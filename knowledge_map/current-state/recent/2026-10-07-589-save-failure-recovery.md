## 2026-10-07 — Save failure accounting and recovery (#589)

Experiment persistence errors survive write-queue destruction and successful
remainder writes. Finalization persists Failed accounting with the fatal reason
and failed frame counts, instead of reporting declared partial loss. Qt offers
acknowledgement for the Failed lifecycle plus fault gates; its banner uses the
same guarded acknowledgement, restoring Idle and ROI/background editing without
changing the saved failure outcome or replaying completion dialogs.

Regression coverage injects an HDF5 append failure after a successful batch,
reads the accounting back, and exercises offscreen Qt recovery. See
[[architecture/ExperimentCoordinator]], [[services/ProcessingService]],
[[frontend/MainWindow]] and [[frontend/PreviewPage]]. No Rust bridge ABI change.
