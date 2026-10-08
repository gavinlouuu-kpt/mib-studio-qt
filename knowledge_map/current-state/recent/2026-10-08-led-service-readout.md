## 2026-10-08 — Delay steps and a strobe/guard readout for the LED in Service mode

Item 7 of the board owner's list for retiring the bench viewer. In Service mode the LED controls
nudge the delay as well as the width in 0.5 µs steps (clamped to the mode's limits) and show a
read-only line with the strobe control S[0] (on/off), the guard state and its trip count, with an
alert when the guard has tripped. Operators keep the per-mode presets. See
[[architecture/Desktop-Shell]].
