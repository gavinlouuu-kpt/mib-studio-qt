## 2026-10-08 — Browser acceptance of YOFO Studio on the PZ7035

`scripts/yofo/e2e_studio_acceptance.py` runs the #501 acceptance list in headless Chromium against the
board's server (an SSH tunnel to the loopback unit) and writes one pass/fail per item to
`acceptance.json` with screenshots: token/Connect, Preflight with 0 warnings, Align, Run at 5 kHz
(window placed, short experiment), Run↔Align with no shell commands, no MIB-only surfaces, and the
pumps as "pending" until Gavin is present. `--desktop` checks a host server instead (connect, no
PZ7035-only surfaces). It never commands the pumps. See [[architecture/Desktop-Shell]].
