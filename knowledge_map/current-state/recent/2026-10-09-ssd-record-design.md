## 2026-10-09 — Design note for Record to the SATA SSD (#667)

`docs/exec-plans/active/2026-10-09-ssd-record-experiments.md` is the Studio side of #667: the register-window needs (R1 to R9) for the
board owner, the experiment list from the SSD run table, the in-box export of a run to the PC experiment HDF5 (staged on the
eMMC with a space check, estimated times, the case that does not fit), the UI states (absent, recovering, drops, closed by
mount-time recovery), how a run coexists with ring playback and Save clip, and the tests that need no board. No code. Findings:
the PC has no experiment list widget, the run table has no times or drop counts and only 29 slots, and the idle rule would end an
unattended run.
