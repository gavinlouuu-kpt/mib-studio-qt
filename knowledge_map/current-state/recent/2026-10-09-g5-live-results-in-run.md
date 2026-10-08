## 2026-10-09 — PL results in Run without an experiment (#651 G5)

The execution provider used to run only inside an experiment, so Monitoring and the live statistics were empty in Run
until a file was recorded. `AppBackend` now runs a **live results session**: after a successful switch to Run it
compiles the profile (`AppBackend::compilePlProfile`, moved out of the coordinator), configures and starts the provider
(run id 0; ingest does no accounting or storage while no experiment is active, and the Monitoring rows are gated by the
tab as before). Align, idle and shutdown stop it; an experiment start stops it first and its finalize resumes it when the
instrument is still in Run (and a refused start resumes it). A watcher thread follows the processing config version: when
a setting changes it recompiles, and if the PL page or E-modulus table differ it restarts the provider with the new
profile (a compile error keeps the previous one). `liveMutex_` is a leaf lock. Board check (a slot): CPU load of the ARM
with the session at 5 kHz, dense scene, against without it; `bridge_active` reads true for the whole Run. See
[[Desktop-Shell]] and `docs/exec-plans/active/2026-10-08-g5-g12-live-results-diagnostics.md`.
