## 2026-11-05 — Tauri trigger tests follow the Qt Monitoring tab (#868, HW-043..045)

The Sort Trigger / pulse duration / Periodic Test controls take the Qt tooltips, ranges and defaults.
The pulse duration (1–1,000,000 µs, default 1) is applied as it is typed (300 ms pause, Enter or leaving the
field) instead of through a Set Pulse button; Sort Trigger and the periodic start apply a pending value first,
so they fire with the duration the backend reports, and the log shows that value. The periodic interval
(10–60,000 ms, default 1000) is validated and locked while running. Blank, 0 and out-of-range values are refused
beside the field, never replaced by 1 or 1000. A running periodic test stops, and is logged, when the Monitoring
tab is left. Service mode and the one-shot arm stay, because these controls actuate the sorter. See
[[architecture/Desktop-Shell]] and [[task/2026-11-05-tauri-parity-hardware-pulse]].
