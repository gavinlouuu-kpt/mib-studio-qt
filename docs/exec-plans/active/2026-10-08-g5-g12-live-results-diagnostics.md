# Live results in Run without a file (G5) and a Diagnostics view (G12)

Status: active

## G5. Monitoring and charts in Run, without starting an experiment

**Today.** The PL results reach the PS only while an experiment runs: `ExperimentCoordinator::start` configures and starts the
execution provider (`provider->configure(profile)`, `provider->start(runId)`, `ExperimentCoordinator.cpp:1053-1076`) and
`finalize` stops it. In Run without an experiment the provider is idle, so the Monitoring tab and the live statistics are empty,
and an operator tuning gates has to record a file to see their effect.

**What is already safe.** `ProcessingService::ingestProviderFrame` is a no-op for accounting and storage when no experiment is
active (`noteRealtime*` check `experimentActive_`; rows are stored only while `experimentAccounting_.wasAdmitted`, `:1176-1178,
2709-2745`), and `appendProviderMonitoringRow` is gated by `monitoringActive_` (the Monitoring tab turns it on, `:2670-2678`).
So "live results" = the provider running with its sink, nothing more on the PS side.

**Design.**
1. `AppBackend` owns a *live results* session: after a successful switch to Run (`setInstrumentMode(Run)`, after the cell path and
   LED are on) it compiles the profile, `configure()`s and `start()`s the provider with a live run id; on leaving Run (any mode
   switch, idle, shutdown) it `stop()`s it first. In Align it never runs (the provider arms the same bridge as the Align preview).
2. Start/stop hand-over with the experiment: `ExperimentCoordinator::start` calls `backend_.stopLiveResults()` before it
   configures the provider, and `finalize` restarts the live session when the instrument is still in Run. The provider's
   `configure()` needs the provider stopped, so there is exactly one owner at a time (an `std::mutex` in `AppBackend`).
3. Apply: when the processing config changes (`apply_config`, profile apply, px→µm apply) while a live session runs, recompile the
   profile and restart the session (stop, configure, start), so a tuned gate takes effect without a file. This is the "recompile
   on apply" part of G5. A compile error leaves the previous profile running and reports the error.
4. Status: `fetch_instrument_status.results.running` already exposes the provider counters; Run shows them live.

**Risks to check on the board (a slot of its own).**
- CPU: decoding up to 5000 FRAME + RESULT records per second on the 2-core A9 next to the server, the tunnel and the run
  preview polls; compare ARM load with and without the session at the dense-scene rate (#615).
- The bridge being armed in Run changes what P[6] "dropped" means (it is counted properly only while the bridge is armed,
  #630); the link-health `bridge_active` flag will read true for the whole Run.
- Run entry order: the live session starts after the cell path is on (step 9 of the Run entry), so it must not move the
  U-Net enable earlier or add receiver resets (see the Run-entry flag-clear question for the board owner).

**Tests.** Fake provider and fake register map: Run starts the session; Align, idle and shutdown stop it; an experiment start
stops it and its finalize restarts it; a config apply restarts it with the new profile; a compile error keeps the old profile.

## G12. A Diagnostics view

**Today.** Logs are reachable only from a shell: `Logger` writes `app.log` under the data dir (`<data>/logs/`, rotated at open, #595),
and the unit's stdout goes to the journal.

**Design.**
1. A read-only bridge command `fetch_diagnostics {log_lines}` returning: versions (`BUILD_INFO` first line of the installed bundle,
   server build, PL identity from `fetch_instrument_status.core`, the producer path and md5), uptime, the data dir and its free space
   (`recordingTarget`), the processing-core and LUT lines from the startup log (G6 check), and the tail of `app.log` (capped at 500
   lines and 256 KiB).
2. A Diagnostics view (Settings menu, browser and desktop): the same fields, the log tail with a level filter and Copy, and a
   Download logs link that uses the G3 route (`/files/download?path=<data>/logs/app.log`), so the whole log can be attached to an issue
   without a shell. The journal stays shell-only; the log file duplicates what matters.
3. Nothing is written; no new write path; the log tail is size-capped so a large file cannot stall the poll.

**Tests.** The command against a temporary data dir (missing log, small log, large log capped, non-UTF-8 bytes replaced); a vitest for the
view model (level filter, copy text).

## Order and dependencies

G3 (file route) first; G12 needs it for the download link. G5 is independent but needs a board slot to measure CPU load, so it follows the
playback work only if the board owner's slots allow; the design above is the request for a go-ahead.

## Open points

1. Should the live session also run in Align once the PL emits results there? (Today the Align stream has no results.)
2. Is a restart of the provider on every config apply acceptable (a gap of the order of 100 ms in results), or should the profile page
   be written while running (the interface says configure while stopped)? Board owner.
