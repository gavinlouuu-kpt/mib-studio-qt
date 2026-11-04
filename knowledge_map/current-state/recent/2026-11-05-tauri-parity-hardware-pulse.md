## 2026-11-05 — Tauri pulse generator panel follows Qt (#868, HW-048..057)

The desktop pulse generator panel now behaves like the Qt tab. One **Start** (one arm) writes frequency,
duty and enable and stops at the first failing step, naming it; **Stop** is never arm-gated; baud / data
bits / parity / stop bits are chosen and sent on connect; **Scan** probes addresses 1–16 read-only and fills
the first generator's address; the status label is Qt's (`Verified addr N — ChX: …`, `Disconnected —
<error>`, `Scanning…`); port, serial settings, address, channel, frequency and duty persist in the
preferences file; **Use XGC + R5D preset for Live View** saves the rig preset and the generator link into
the selected MindVision camera file. The channel count and frequency range are one named constant, compared
with the C++ sources by a test. Still open: serial port enumeration (no bridge command, so the port stays a
text field). See [[architecture/Desktop-Shell]] and `docs/howto/desktop-area-ownership.md`.
