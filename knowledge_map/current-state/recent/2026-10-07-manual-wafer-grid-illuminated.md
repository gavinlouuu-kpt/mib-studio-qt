## 2026-10-07 — Qt manual: Wafer Grid and Illuminated Live View (#413)

Added operator sections to `docs/manual/acquire-and-record.md` for the v1.2.0
Qt workflows: Overview-only Wafer Grid, bundled design recognition, pose
overlay and failure messages; MindVision illuminated start/stop, hardware
setup, saved preset timing and troubleshooting. Distinguishes the preset
button's 100 µs exposure from the bundled default's 2 µs and the saved
1000 Hz trigger rate from Overview's 400 Hz override. No new screenshots.

Updated [[frontend/OverviewTab]] and [[frontend/ConfigTabs]] with manual
links and corrected stale preset timing descriptions. Documentation only;
UI and lifecycle claims checked against Qt/backend sources, without hardware.
Validation: `python3 scripts/check_docs.py`.
