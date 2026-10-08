## 2026-10-08 — Parity spec for PZ7035 stop, playback and save (#649)

`docs/exec-plans/active/2026-10-08-playback-save-parity.md` records what the PC MIB Studio does for the frame
buffer, Stop/scrub/step/play, overlays, and the three save paths (experiment HDF5, Record, Save Buffer), with code
references and the HDF5 layout YOFO Review reads. Findings: the PC review buffer is the raw camera `FrameStore`
(no mask, no results), overlays on a paused frame are recomputed in the host (Qt only; the desktop draws raw), and
there is no range save to HDF5, so record-based playback and a range save are new on the PZ. Also corrects the
attribute names in [[HDF5-Storage]] (snake_case). See [[PZ7035-Records]].
