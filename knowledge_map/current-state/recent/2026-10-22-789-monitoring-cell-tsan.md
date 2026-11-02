## 2026-10-22 — Pins own their copy of a strip cell: TSan race in refreshStrip (#789)

[[services/MonitoringCellStore]], [[architecture/Threading-Model]].

**Symptom.** A local TSan run during A7a (#786) failed
`processing.monitoring_cell_concurrency`: a `free` inside OpenCV at the end of
`MonitoringCellStore::refreshStrip` raced a `memcpy` in `cv::Mat::copyTo` from
`image()` on a strip client, taken under the store mutex. Reproduced on develop
8bd40c90: 0 of 30 runs idle, 8 of 60 with 12 copies running in parallel.

**Cause (ours, not OpenCV's).** `pin()` of a cell the strip held took a
refcount share of the strip's crop. A refresh that dropped that strip cell (the
ring moved to another run; pins stay until the next run starts) moved it out
under the mutex and freed it after unlocking. A reader that copied the pin's
pixels in that window, followed by an unpin, left the refresh with the last
share: its free was ordered after the read only by cv::Mat's reference count,
which lives in uninstrumented OpenCV, so TSan cannot see it. That broke the
store's own rule that every free follows its readers by a lock. Real memory was
safe (the refcount is acq_rel), but the rule is what keeps the TSan lane
meaningful, so the fix is ownership, not a suppression.

**Fix.** A pin of a strip cell copies the strip's crop and mask under the store
mutex (at most 2 × 64 KiB, off the hot path) instead of sharing it, so every
store buffer has one owner. The memory owner note no longer says a shared copy
counts twice. New test seam: the read hook also fires as `refresh-dropped`
after the refresh unlocks, while the cells it dropped are still held.

**Tests.** `backend.monitoring_cell_store` "a pinned strip cell owns its copy"
replays the interleaving deterministically (relaxed-atomic handoff, invisible
to TSan): 5/5 TSan failures with the share, 5/5 clean with the fix. After the
fix `processing.monitoring_cell_concurrency` ran 180 TSan runs with 12 in
parallel and the `monitoring_cell` tests ran 20 ctest repeats, all clean.
`performance.monitoring_cell_hot_path` is unchanged (store/off throughput
ratio 0.99–1.01 before and after, strip read 1.4 µs median, pin find
6.5–6.9 µs).

**CI.** The `thread` lane already selected the test (labels `backend;processing`,
not `integration`/`performance`) and nothing in `tsan.supp` covers it. The race
needs a rare interleaving and CI runs on a 4-CPU runner, so the lane passed it.
The deterministic test is in `backend.monitoring_cell_store`, which the lane
also selects.
