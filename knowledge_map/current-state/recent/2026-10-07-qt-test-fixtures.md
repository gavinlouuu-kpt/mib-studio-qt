## 2026-10-07 — Portable Qt monitoring and discovery fixtures (#419)

The Qt monitoring end-to-end test now uses bright Contract 1 synthetic cells
that survive current-background subtraction when the pinned stream asset is
unavailable. The discovery UI test disables capture bootstrap explicitly,
asserts the backend starts unconfigured, and supplies all device results
through scripted providers. Service notes document both fixture paths and the
Windows CTest selection: performance-filtered and fast lanes omit monitoring,
while integration presets and the unfiltered release run include it.

Not a behaviour change: v1.1.2 and develop both default live masks to Contract 1 saturating subtraction (`processing_contract_version` absent → 1; ADR 0006 makes absdiff the opt-in Contract 2). The dark synthetic cells were never valid under Contract 1; the test only passed where the real stream asset was provisioned. Both tests had failed on Linux unnoticed because PR CI does not build Qt.
