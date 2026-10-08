## 2026-10-08 — trigger_event_log waits for the record, not only the counter (#595)

`backend.trigger_event_log` failed under ASan on #634 (`ev.size() == 1`, "no-camera drop
recorded"). The TriggerService worker increments the drop counter, logs, and only then records
the event. The test waited on the counter and drained the buffer at once, so slow ASan logging
left the buffer empty. The no-camera and set-failed cases now also wait for
`bufferedEventCount() >= 1`, as the fired case already did. This is a test-only change: drains
are periodic, so the production order is harmless.
Verified by injecting a 50 ms gap before `recordEvent` in both paths: the old test fails exactly as
on CI and the new test passes.
