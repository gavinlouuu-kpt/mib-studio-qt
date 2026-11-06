## 2026-11-07 — Connect B1: a run still saving holds the camera (#863, non-author review of #895)

The non-author review of #895 found that the experiment guards of the camera source and the delivery mode had no
test that fails without them, and that a `Failed` run still flushing its data held nothing. `experimentHoldsCamera`
now takes the `ExperimentStatus` and holds the camera while `flushing` too (Qt `flushInProgress_`);
`cameraSourceChangeRefusal` (C++) and `camera_change_refused` (Rust, `select_hardware_camera`) are the pure rules the
guards call, unit-tested with the capture stopped under a run. `backend_facade_boundary_test` refuses Configure Mock
and the delivery mode while the run Starts, is Active and Stops. The startup auto-select default is #890's
HW-037 line (one implementation). See [[architecture/ExperimentCoordinator]], [[architecture/Desktop-Shell]].

B2 (#896, its non-author review): Stop Camera uses the same status rule (`cameraStopRefusal`): a Failed run still
flushing refuses with "Cannot stop camera while experiment data is being saved. Wait for the save to finish.", and
the shell's `stopCameraBlockReason` reads the status's `flushing` too.
