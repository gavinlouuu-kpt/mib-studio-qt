## 2026-10-10 — S2 of the SSD record (#667): an experiment records to the SSD through pzrec (no bridge ABI change, stays 35)

With an SSD configured (`MIB_PZREC` + `MIB_SSD_IMAGE`, plus the board's options in `MIB_PZREC_ARGS`, split on white space and passed after the image: `--hw pl --bounce-phys 0x2D000000
--disk-sectors N`, or `--hw pl` with the pzblk device as the image), starting an experiment on the PZ7035 records it to the SSD; without one nothing changes (the transitional
metadata-only experiment). The readiness gate `storage.ssd` is now blocking: SSD READY with no open run, otherwise it fails with the state and pzrec's reason.

Start order (`ExperimentCoordinator::start`, pz7035 #49 and the board owner's answers of 2026-10-10): `SsdStore::prepareRun()` asks `pzrec status` (short timeout, one call, no retry
loop) for READY and takes `next_run_id` N, a refusal names the state; the snapshot gets `ssd_run_id` N; the provider writes N to the bridge RUN_ID and arms the store in
`CONTINUOUS | RING_ONLY | DRAIN` (0x601, `kStoreModeDrain`, ring programmed as for ring-only; needs capability bit 17 SSD_RECORDER and a configured ring); then `pzrec start` runs after
the ARM (ARM under an open run would abort it). `startRun` checks that the run pzrec opened has id N; a mismatch, a failed or timed-out start stops the provider, aborts a run that may be
open and fails the Start with "SSD: ..." (fail closed, file removed).

Stop: `SsdStore::beginStop()` runs `pzrec stop` on its own thread (an attempt is killed at its timeout and retried; pzrec's own status decides whether a killed attempt closed the run),
the bridge STOP follows at once, and the run's end waits up to 60 s for pzrec. The UI never waits: the SSD state reads STOPPING meanwhile. A stop that does not finish is a named fault
(`experiment.ssdStopFailed`, "SSD run N is still stopping / was not confirmed closed") and the next `prepareRun` closes the stuck run before refusing. A user cancel aborts (reason 8);
a save error of the HDF5 file stops gracefully, the SSD data is not bad.

Honesty while recording: pzrec cannot open the disk at 5 kHz (the block path is held back), so a failing `status` during Studio's own run keeps the state RECORDING with "live counters
unavailable: ..." instead of WEDGED, never READY. Every pzrec call is serialised on one mutex; a periodic refresh skips its turn while a start or stop owns the device.
Tests: `ssd_record_test` (scripted pzrec + the real CLI via `MIB_PZREC`), `ssd_experiment_test` (the lifecycle on the real CLI's simulated drain, refusals, mismatch, failing stop),
`pz_frame_ring_test` (0x601). Not tested without the board: the drain bit on the PL, `pzrec start` right after the ARM at 5 kHz (the block path may already be held back), the real stop time.
