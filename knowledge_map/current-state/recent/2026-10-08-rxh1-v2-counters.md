## 2026-10-08 — Heal v2 counters in the status and the flip script

results11's heal v2 (CTRL2 at RXH1 word 24 non-zero) counts flag clears (word 5), episodes (6) and
episodes that reached persistent failure (7) apart from receiver resets (word 3); a flag clear during
Run means frames were corrupted before it. `link.rx_heal` gains `v2`, `flag_clears`, `episodes`,
`failed_episodes` (read until two reads match; 0 on v1), and the flip script reports their per-flip
deltas and the latency readout. `bundle_assemble.sh` marks a bundle built from a bare commit instead of
a tag "PRE-QUALIFICATION, untagged pz7035 <commit>" in the first BUILD_INFO line. See [[Desktop-Shell]].
