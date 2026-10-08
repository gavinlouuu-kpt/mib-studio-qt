## 2026-10-08 — Peristaltic pump rate in rpm and CW/CCW labels in Studio

Item 6 of the board owner's list for retiring the bench viewer (pump parity). A Tushui pump's Rate
unit select now offers "rpm (head)": the UI sends rpm x the connected calibration as a µL/min flow
(`pumpRate.ts`), shows the configured flow as "≈ x rpm", and labels the directions "Infuse (CCW)" and
"Withdraw (CW)". Start, purge and settings keep their Service-mode and arming gates. Not tried on the
pumps (the instrument's pumps run at 0.40 rpm and are not commanded). See
[[architecture/Desktop-Shell]].
