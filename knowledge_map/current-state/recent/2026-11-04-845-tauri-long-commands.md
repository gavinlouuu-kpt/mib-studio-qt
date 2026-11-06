## 2026-11-04 — Tauri: a long backend call no longer freezes status, and Close is bounded (#845)

Found on win11-agent: a Stop with no frames (camera on DEL1_1) held the one bridge mutex for
about six minutes, every other invoke queued behind it, and Close repeated the stop. The
backend stop itself is #840 (PR #842); this is the Tauri side.

- `crates/mib-app-commands/src/bridge_lock.rs`: `BridgeLock` (one mutex, plus the name of the
  long command that holds it) and `StatusCache`. Status reads wait at most 250 ms, then answer
  with the last good reply; control commands still wait.
- `desktop/src-tauri`: every bridge command runs on Tauri's blocking pool
  (`#[tauri::command(async)]`); the status commands go through the cache; `poll_events_exact`
  returns no events while the bridge is busy.
- `desktop/src/closeGuard.ts`: "Stopping camera…", a 10 s wait, then Close now / Keep waiting.

See [[Desktop-Shell]] and [[Rust-Bridge]]. Not yet re-tested on the rig: that waits for #842.
