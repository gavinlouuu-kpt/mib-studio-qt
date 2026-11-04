## 2026-11-04 — Bench findings of #833: FrameStore memory, stop with no frames, slow write-rate probe (#839, #840, #841)

[[data-model/FrameStore]], [[diagnostics/MemoryBudget]], [[services/CaptureService]]. Found in the bench
test of #833 on win11-agent (MC2066, Coaxlink Quad CXP-12, 27.9 GiB PC); one PR, one commit per finding.

**#839, FrameStore kept ~10 GiB resident after Overview.** `reserveFrameBytes` only grew a slot, so
the 5000 slots filled at the Overview's 1920x1080 (2 MB) stayed that size for the process's life.
The #833 buffer plan sizes from the RAM still available, so the same 1216x208 profile planned
4.7 GiB / 2.00 s in a fresh app and 1.8 GiB / 0.76 s after 100 s on Overview. Now a smaller
reservation (the first frame of the next session) swaps each larger slot for one of the new size
and frees the excess; a stored frame that still fits is kept, a stale larger one is dropped and
reads `Malformed`. `retainedBytes_` / `memoryStats()` follow (the peak stays the high-water mark).
Reproduced and fixed with a 5000-slot store on P5: RSS 9964 MiB after the Overview ring filled
(the bench's "9887.7 MB retained"), 1273 MiB two seconds after the reserve to 1216x208 once the
ring refilled with Experiment frames. Freeing the old buffers took 524 ms (the structural lock
is exclusive, and the caller is the capture thread on its first frame, so the camera's input
buffers would overrun), so the lock is held for the swap only (10 ms) and a release of 16 MiB or
more is freed on its own thread. Test: `backend.frame_store_reserve`.

**#840, the app hung on stop while no frames arrived.** Cause as suspected in the issue:
`EGrabberCamera::grabFrame` holds `stateMutex_` across `ScopedBuffer(*grabber_)`, whose eGrabber
default timeout is `GENTL_INFINITE`; `stop()` takes the same mutex first, so with the camera
delivering nothing (the bench camera's cycle trigger `DEL1_1` was undriven) it never returned
and the Overview to Experiment tab switch froze the app for 4+ minutes. Now the wait is bounded
(`kPopTimeoutMs` = 50 ms, `GC_ERR_TIMEOUT` is "no frame yet"), and `stop()` sets `stopRequested_`
and calls `cancelPop` (under `triggerMutex_`, not `stateMutex_`) before it waits for the mutex.
The `catch` blocks of `start()` called `stop()` with `stateMutex_` held (a self-deadlock); they
release the half-started grabber through the teardown `stop()` shares. The code is under
`#if MIB_HAS_EGRABBER`: syntax-checked against the SDK headers on Linux, not run. **Needs the
win11-agent bench test** (camera with no trigger: switch Overview to Experiment, stop, restart).
