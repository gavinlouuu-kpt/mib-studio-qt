## 2026-11-05 — Autofocus failures reach the bridge as error events (#868, HW-033)

`AutofocusService` now tells a second callback about failures only (`setFailureCallback`): the reason a connect
did not succeed, a voltage write that failed (manual jog or control step), the safe shutdown voltage that could
not be applied. `BackendFacade` installs it at `initialize` and reports each as a `BackendErrorEvent` (source
`Hardware`, command `Autofocus`); a repeat of the same text within 5 s is dropped, because a failing control step
retries every 50 ms. The Qt tab still gets every message through the unchanged status callback. No new event kind,
no ABI step. Before, only the Qt tab registered a callback, so the Tauri app never saw why a connect failed or that
a jog was refused. See [[services/AutofocusService]] and [[architecture/Rust-Bridge]]. Test:
`tests/backend/autofocus_failure_events_test.cpp` (facade over a fake nanopositioner) and a failure-callback block in
`tests/backend/autofocus_backend_safety_test.cpp`.
