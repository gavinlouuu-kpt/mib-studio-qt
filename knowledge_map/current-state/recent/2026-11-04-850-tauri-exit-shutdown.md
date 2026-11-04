## 2026-11-04 — Tauri: app exit runs the AppBackend shutdown (#850)

tauri's `App::run` exits the process with `std::process::exit`, so the managed `AppState` was
never dropped and the backend shutdown never ran: no pump, pulse-generator or Z-stage
shutdown, and no `AppBackend: shutdown complete` in the log (the Qt app logs all of them).

`desktop/src-tauri/src/lib.rs` now builds the app and, on `RunEvent::Exit`, calls
`mib_app_commands::shutdown`, which waits at most 30 s for the bridge (a forced close during a
stuck stop, #845) and then shuts the backend down once. Tests: `shutdown_tests` (shutdown
through the bridge, once; bounded when a command holds the bridge). See [[Desktop-Shell]].
Not yet checked on the rig.
