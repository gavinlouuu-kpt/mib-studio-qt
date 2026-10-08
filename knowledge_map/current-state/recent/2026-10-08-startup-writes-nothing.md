## 2026-10-08 — A test pins that the UI's load-time polls write no PL register

For the standing YOFO Studio unit on the PZ7035: `instrument_modes_test` now runs the status
fetches the UI makes at load (platform info, instrument status, camera geometry, pump and stage
status) after the control registers are injected and asserts that no PL register was written. The
real control only maps `/dev/mem` at start-up (`DevMemControlRegisters::open`), the execution
provider only reads identity until an experiment starts, and the serial ports are opened only by the
pump, stage and discovery commands the UI sends from buttons; the one automatic serial path (the
startup nanopositioner discovery) is mounted only when `capabilities.autofocus` is set, which the
PZ7035 does not report. See [[architecture/Desktop-Shell]].
