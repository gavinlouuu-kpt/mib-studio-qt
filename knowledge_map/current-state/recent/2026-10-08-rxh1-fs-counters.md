## 2026-10-08 — FrameStart counters (results12) in the status, run accounting and the flip script

results12 starts a frame only after a FrameStart line (the root-cause fix for #629). RXH1 words 32-34
count FrameStarts seen (`fs_seen`), frames dropped because no FrameStart was seen (`nofs_frames`: a
mid-frame join or a lost FS line) and the lines dropped with them (`nofs_lines`). `link.rx_heal` gains
`fs_present`, `fs_seen`, `nofs_frames`, `nofs_lines` (tear-safe reads; 0 and `fs_present` false on older
builds), `fetch_run_accounting(last_run)` gains `nofs_frames` (null without the counters), accumulated over the
run, not end minus start: the RTL zeroes the counters at a receiver reset (a heal or P[8] bit 6) as well as at a
clear, so `PzPlatformMonitor` adds each sample's delta, or the new value when it fell, and the Run outcome notice says "Frames dropped by the PL for want of a FrameStart: N (lost
data)". The flip script reports the per-flip deltas. About one `nofs_frames` per Align entry is
expected; during a steady Run it should stay 0. See [[Desktop-Shell]].
