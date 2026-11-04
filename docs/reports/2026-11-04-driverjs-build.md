# Driver.js integrated QA build — 2026-11-04

Project actions: MIBQT-01 (onboarding), MIBQT-16 (evidence navigation).

## Scope and source

Isolated branch `build/mib-driverjs-20261104` combines PR #816 integration
(`49121893`), PR #835 policy/evidence documentation (`e647979d`) and existing
PR #827 tours (`302e654a`). Integration merge: `ad193d3671c1816275ae336d4cdcc8c9ead1dbda`.
No develop promotion, hardware operation, peer-service restart or production deployment.

Six tours: E01 discovery, E02 typed features, E04/E06 run lifecycle, I02 recipes,
F04/F05/F08 populations, I12/F12 workspace persistence. Open via the workspace
**Tours** button or classic **Help → Tours and what's new**. Tours never auto-start.
They include before/after flowcharts, evidence panels, Skip/Restart, missing-target
recovery and feedback. Actions use real app controls and wait for backend facts.

## Build configuration and reproducibility

Linux x86-64 mock-camera QA configuration: MindVision and hardware SDKs off;
Sentry off (local crash reporting). System `fmt` selected to match system spdlog:

```sh
cmake --preset linux-backend-only -DMIB_ENABLE_MINDVISION=OFF \
  -DMIB_ENABLE_HARDWARE_SDKS=OFF -DMIB_USE_SENTRY=OFF \
  -Dfmt_DIR=/usr/lib/x86_64-linux-gnu/cmake/fmt
cmake --build --preset linux-backend-only-build --target mib_backend mib_review_core -j 4
MIB_BRIDGE_NO_CMAKE=1 CARGO_BUILD_JOBS=4 cargo build --locked \
  --manifest-path crates/mib-bridge-server/Cargo.toml
cd desktop
npm ci --no-audit --no-fund
npm run build
npm test
```

The first configuration selected Linuxbrew fmt 12 against system spdlog and
failed compilation; selecting the compatible system package fixed that configuration.
This is a build-local override, not a package/system change.
The browser driver now dates new evidence at execution time instead of hard-coding
2026-11-03. Old screenshots and source evidence retain their historical dates.

## Verification

- Production frontend build: passed; Driver.js 1.3.1 included as lazy JS/CSS assets.
- Focused onboarding tests: 77/77 passed across eight files.
- Full UI suite: 1,663 passed, one skipped (147 files passed, one skipped).
- Documentation and screenshot consistency: passed; nine manual shots in sync.
- Fresh native C++ backend/review archives and Rust server: passed.
- Fresh headless `fold_in_e2e`: 9/9 scenarios passed in 7.92 seconds (one Rust test).
- Exact packaged native server + UI + TIFF fixture: 6/6 light-theme tours and 3/3 extra scenarios passed; 116 screenshots.
- Extra scenarios: entry surfaces/quiet behavior, acquisition/run idle gates, missing-target Skip/Back/Restart.
- Action steps held for at least 2.2 seconds before operator input; success followed backend replies; feedback exported.
- Screenshot inspection: six-tour list is readable with real view behind it; no hardware claim. Dark tour styling was not rerun for this package.

## Limits

Mock camera and synthetic HDF data, not Windows/eGrabber or biological acceptance.
F09 has no production tour. The app has no full dark theme; the existing dark
scenario styles only the tour UI. Workspace preview labels are preserved.
First-frame demonstration uses the nonzero counter, not MIBQT-14's separate event
branch. UI evidence does not mark any of the 37 backend goals newly accepted.

## Packaging validation correction

The first packaged run exposed a CRC-invalid PNG copied from the old mock folder.
Camera startup failed and the E02 tour correctly stayed blocked. The rejected file
was removed from the deliverable and preserved with failed-run logs on P5; the
package now contains only the verified 512×96 Mono8 TIFF. The complete packaged
click-through is rerun rather than accepting the earlier source-matched run.

## Artifacts

P5 archive and raw logs: `/mnt/hdd/developer-data/mib-studio-qt/driverjs-build-20261104/`.
Final evidence is `verified-browser-evidence/manifest.json`; the earlier
`final-browser-evidence/` contains the deliberately retained failed packaging attempt.
The downloadable QA tarball includes a loopback-only mock launcher, native-dependency
list, per-file manifest, synthetic HDF and a verified TIFF. Requires compatible
Ubuntu 24.04 shared libraries; not a self-contained installer or native Tauri/Qt build.

Archive SHA-256: `8f92df0c2b87cedb2b379bab1b7547ec081b9fc6acc487b38124ddb619e44bc3`.

Verified downloadable bundle and curated evidence: [project Drive folder](https://drive.google.com/drive/folders/1AaTUVbUyGTvCrz8oZUdKWUC84tIdz0zH).
Source/report review: [draft PR #836](https://github.com/gavinlouuu-kpt/mib-studio-qt/pull/836).
