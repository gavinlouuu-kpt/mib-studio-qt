## 2026-10-08 — No dead-end menu items in the browser; px→µm stays on the PZ7035 (#651 G10)

In the browser transport the menu no longer lists "Open Data Folder" (the browser cannot open a folder on the
instrument; it only threw), "Central Methods…" (registry import is a desktop-shell feature) or "Updates…" (the
application updater). On a PL-science instrument the processing quick control now keeps its px→µm field and
Apply button (the calibration of every run: Review multiplies the cell area by it) and hides only the
host-only realtime switch, processing stats and background line; "Pixel to Micron…" leads to it. The acceptance
script checks the menus and that the control is reachable. See [[Desktop-Shell]].
