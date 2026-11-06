## 2026-11-04 — Runs with Monitoring open no longer fall behind the frame ring (#857)

On the bench (MC2066, 1216x208 at 5000 fps) a run with Experiment › Monitoring open ended
`incompleteLoss`. Cause: on the every-frame inline path each monitoring row kept views into the
full-frame gray image and mask, so the ring and the tab's copies pinned about 1 GB; the allocator
churned (page faults 100k-225k/s instead of ~27k/s), processing fell to 4,100-4,700 fps and the
camera overwrote frames the loop had not read. Rows now keep their own copy of the ROI crop, as the
async-batch path already did ([[services/ProcessingService]]). Bench after the change: two 20 s runs
with Monitoring open, complete, 5000 fps, lag 0-2 frames.

The accounting also booked the loss wrongly: frames evicted while a pass worked through older ones
were skipped silently and showed only as `sequenceGaps`. They are now admitted and counted as
`storeOverwritten`, and the loop logs a warning once per second while a run loses frames. Guard:
`processing.realtime_ring_loss`. A related open question: frames the camera delivered before Stop
but the loop had not reached yet are not part of the run's accounting (a lagging run can read
complete with fewer admitted frames than the camera sent).
