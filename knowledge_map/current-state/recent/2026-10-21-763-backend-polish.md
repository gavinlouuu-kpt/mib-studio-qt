## 2026-10-21 — Backend and classic-view polish from the browser walk-through (#763)

Develop-side findings of the #763 walk-through of YOFO Studio in a browser. The
experiment-start path (F1/F2), the workspace (`desktop/src/workspace`) and the
review Inspect commands (#756) are not part of this change.

**F7: YOFO Review on a server without the review commands.**
`yofo-studio-server` answers `review_list_dir`, `poll_review_events` and the
other review commands with `UNKNOWN_COMMAND: <name>` (#512 S11 has not landed).
The page showed the backend as ready, listed an empty folder and logged the
same error five times a second. Every `reviewBridge` call now goes through
`gateReviewCommand` (`desktop/src/review/reviewAvailability.ts`): the first
UNKNOWN_COMMAND marks the review commands unavailable on that connection, and
later calls are refused locally (`ReviewUnavailableError`, nothing sent). A
lost or new connection (a server restart or upgrade) or the notice's Retry
probes again; only a reply enables the panel and the poll. `ReviewApp` shows a
plain notice instead of a usable Open, skips the launch file and the Tauri
open-file listener in a browser, and `useReviewJobs` stops polling with one log
line. The Tauri shells only ever reach "available". See
[[frontend/YofoReview]]. The workspace branch's "Backend ready" chip and Open
view take the same state after the next develop sync.

**F17: realtime rates decay to 0.** The inline realtime loop closed its 1 s
window only when a frame arrived, so `getAlgoFps1s` and the valid/invalid
rates held their last value forever once frames stopped. The idle branches now
close the window (publishing 0), the getters report 0 when the last
publication is older than 3 s (`kRealtimeRateStaleUs`), and `stopRealtime`
zeroes them. Pinned by `processing.realtime_rates_decay` (a fake clock for the
staleness rule, plus inline and async-batch runs). See
[[services/ProcessingService]].

**F18: "Reanalysis: completed /".** The classic view's Regenerate feedback
printed the state, phase and `completed/total`, and the backend's terminal
status carries no phase or progress. `reanalysisStatusLine` now gives progress
while running, "completed · 45 records (13 valid, 32 invalid)" when done (the
file stays on the "Output:" line), and says so when a completed job names no
output file; the terminal status JSON carries `valid_objects`. The YOFO Review
completion text and its parse (#761) are unchanged. See
[[services/ReviewSession]].

**F19: the classic view for a viewer.** Mutating buttons stayed enabled for a
client that does not control the instrument, so a press only logged
VIEWER_ONLY. `ViewerGate` (`desktop/src/components/ViewerGate.tsx`, a
`fieldset` with `display: contents`) disables the control-command groups, and
a few single controls carry `viewerOnly` in their own disabled state. It
follows a handover both ways and changes nothing in the desktop shell. A
source test keeps every listed control command behind the gate. See
[[architecture/Desktop-Shell]].

**F20: log noise.**

- Finalize printed 31 HDF5-DIAG "unable to open attribute" blocks. Attribute
  writes probe with `H5Aexists` first, and path probes walk each prefix so
  `H5Lexists` never fails on a missing group ([[services/Hdf5Service]]).
- FRAME_SOURCE_CHANGING when Review opens a file is the client's own discard
  of a pull during the source switch (nothing reaches the backend; the next
  pull works). The frame schedulers no longer log it or FRAME_REPLY_STALE
  (`isFrameReplyRace`); backend refusals are still logged.
- `/favicon.ico` serves the UI's icon from `--dist`, or 204 when there is none.

**F24: the LUT startup log names the file loaded.** It printed the configured
path even when another candidate was loaded; it now prints the loaded path and
the requested one when they differ ([[architecture/AppBackend]]).
