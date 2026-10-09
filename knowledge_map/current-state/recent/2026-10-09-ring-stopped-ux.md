## 2026-10-09 — The stopped Run says what it is on the Camera & Alignment tab and hides the live preview

While Run is stopped for playback (#649), the Camera & Alignment tab now shows a banner ("Run is stopped to review buffered frames. Resume Run
to switch to Align.") with a Resume Run button and a link back to the playback, instead of a black canvas with a stale frame line. On the
Experiment tab the live Run preview below the playback is really hidden (`.canvas-wrap[hidden]` had no effect because the class sets
`display: flex`) and its statistics line is replaced by "Live preview paused while Run is stopped." The "Next: Hardware Preflight" banner
is the stepper's, driven by the unconfirmed preflight and independent of Run and Stop: not changed here.
