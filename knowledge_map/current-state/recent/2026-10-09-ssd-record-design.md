## 2026-10-09 — Design note for Record to the SATA SSD (#667)

`docs/exec-plans/active/2026-10-09-ssd-record-experiments.md` is the Studio side of #667: the register-window needs (R1 to R9) for the
board owner, the experiment list from the SSD run table, the in-box export of a run to the PC experiment HDF5 (staged on the
eMMC with a space check, estimated times, the case that does not fit), the UI states (absent, recovering, drops, closed by
mount-time recovery), how a run coexists with ring playback and Save clip, and the tests that need no board. No code. Findings:
the PC has no experiment list widget, the run table has no times or drop counts and only 29 slots, and the idle rule would end an
unattended run.

Coordinator decisions (same day): one control (the Experiment start/stop with `storage=ssd`); export staged on the eMMC, and a run
bigger than the free space exports automatically in consecutive parts by frame-id range; v1 uncompressed with a follow-up to
measure gzip-1 or LZF on the mask dataset; the unattended opt-in applies to SSD runs only and needs a maximum duration; the
run table of at least 256 entries with no silent overwrite is a hard requirement.

Storage change (Gavin, same day): all recorded data and clips go to the SSD; the eMMC keeps only firmware and internal data (settings,
state, logs). The note now exports a run from the raw record area to an HDF5 file on the SSD's ext4 area (no eMMC staging, no size cap
except that area), keeps the parts export as an option, puts Save clip on the same ext4 area (disabled in #649 v1, enabled with the SSD
path), splits the data dir (settings on the eMMC, user data in a new `--storage-dir`), and states what the UI shows without the SSD:
nothing can be saved, explicitly, with no eMMC fallback. Time estimates are now a function of the unmeasured block throughput.
