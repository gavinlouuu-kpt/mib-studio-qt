## 2026-10-10 — The workflow stepper follows the PZ7035's camera mode (#651 G15)

`deriveWorkflow` takes an optional `instrumentMode` (align | run | unknown), given only on an instrument with camera modes. There the camera is the instrument: Camera & Alignment
and Experiment no longer wait for the Preflight confirmation or a "camera started" step, and the current stage follows the mode (Run: Experiment, "Start the experiment"; Align:
Camera & Alignment, or Experiment once alignment is confirmed; a running experiment always wins; a finished one moves on to Review). With no mode yet (start-up) the earliest incomplete
stage decides as before, so the Preflight confirmation is still offered there. The desktop stepper is unchanged (no mode: the stages chain as before). React only, no bridge change.
An unconfirmed Preflight is skipped, a failing one is not: with required checks failing (or an invalid core) the banner stays on Preflight and its remedy, because the backend would refuse the start; a running experiment still wins.
