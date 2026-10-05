# 0011. YOFO Studio for the PZ7035: science in the PL, one trunk, contracts shared with the PL

Date: 2026-10-04
Status: accepted (owner decisions 2026-09-30 and 2026-10-04)

Issues: #441 (E1 #445, E2 #446, E3 #447, E4 #448); pz7035-imx426 #3, #7.

Supersedes the draft YOFO ADR, numbered 0008 on `docs/yofo-studio-pz7035-plan`,
which collides with [0008](0008-dot-grid-localization.md). That draft does not
land separately. Amends [0001](0001-react-tauri-migration.md) (frontend
lines), [0007](0007-one-contract-per-shipped-core.md) (what counts as a core)
and the E0 ADR "Standalone Zynq-7035 target and ownership". The E0 ADR is
numbered 0006 on `feat/441-embedded-e0-target-contract`, which collides with
[0006](0006-processing-contract-v2.md); it lands as 0012.

## Context

The PZ7035 instrument runs the IMX426 at 5000 fps on a Zynq-7035. Its dual
Cortex-A9 PS cannot copy every frame: one 512x96 frame copy takes 158 µs of the
200 µs period. Since 2026-09-30, all per-frame science runs in the PL:

- the U-Net mask;
- cell detection and metrics;
- the gates and the target decision;
- the result records.

The PS runs a headless backend, and the operator uses the React UI remotely.

The work has grown on long-lived branches:

- **mib-studio-qt:** `dev/react-tauri`, `feat/yofo-remote-server` and
  `feat/yofo-pl-results` are 78–104 commits ahead of `develop` and 75 behind.
  - 36 files changed on both sides, mostly the science and recording core.
  - The bridge ABI numbers diverged: 14 on `develop`, 19 and 22 on the
    instrument line.
  - The U-Net cell science (Contract 3) and its decoder sit on a separate
    stack off `develop`.
- **pz7035-imx426:** the ABI bundle that mib-studio-qt vendors exists only on a
  feature branch, not on `main`.
- **The instrument image:** it installs a server binary and UI from a local
  path, with no record of their commits.

## Decision

### Carried over from the draft YOFO ADR (decided 2026-09-30)

1. **Product identity.** The instrument build is **YOFO Studio**.
   - User-visible identity changes: product name, window title, bundle and
     package names, update feed, and the HDF5 `software` attribute.
   - Internal names stay: the repository, CMake targets, the
     `mib_processing_*` C ABI, `MIB_*` macros and variables, and packet magic.
2. **Frontend on the instrument.** React + Tauri only, served by the PS and
   used remotely. There is no local display in this version; E4 (touchscreen)
   is deferred. Neither Qt nor Tauri runs on the PS.
3. **Per-frame work runs in the PL.** That covers:
   - the mask, cells and metrics;
   - the E-modulus lookup, gates and target decision;
   - the timed sort output and the recording selection.

   The PS never sees every frame and never synthesises results. Tracking
   (batch/review) stays on the PS.
4. **Two data paths, both consumed on the PS.**
   - **Records at full rate:** FRAME/RESULT/EVENT records (pz7035 ABI) from the
     PS DDR ring. They feed accounting, monitoring, autofocus and the HDF5
     metadata through an execution provider (`IExecutionProvider`, #447).
   - **Pixels only as a preview stream:** through the GenTL producer and
     Aravis. Recorded images come from the PL frame store after the run.
5. **Camera control** goes through GenICam features of the producer. The
   processing profile, LUT, background and channel band go through the
   provider's profile commit.
6. **Background and channel band** are computed on the PS from preview frames,
   off the real-time path, and loaded into the PL.
7. **Two ROIs.**
   - ROI 1 is the sensor window: acquired, previewed and recorded.
   - ROI 2 is the fixed 512x96 processing window inside it.
   - The achievable frame rate and its limit are shown before ROI 1 is
     applied.
8. **Host surfaces removed on the instrument:**
   - the EGrabber/MindVision cameras;
   - the pulse generator and `TriggerService`'s pulse thread (the PL times
     outputs);
   - YOLO;
   - Sentry upload.

The draft's point 6 is replaced. It said the PL must reproduce Contract 1,
with the host kernel as gold. The instrument runs the U-Net cell science
instead; see 10–12.

### Decided 2026-10-04

9. **`develop` is the only trunk.** The instrument is a build configuration,
   not a branch:
   - `MIB_PL_SCIENCE`;
   - the execution provider (`MIB_EXECUTION_PROVIDER`);
   - the `linux-armv7-yocto` preset and the React frontend.

   Instrument work lands on `develop` through PRs. Long-lived branches are
   updated by merging `develop` into them, not by rebasing. They are deleted
   once landed.
10. **Contract 3 is defined by a shared specification owned by
    pz7035-imx426.**
    - **What it is:** `abi/profiles/unet_cells_v2.json` (page, payload and
      reasons) plus its conformance vectors.
    - **How mib-studio-qt takes it:** it vendors the specification like the ABI
      bundle (`third_party/pz7035-abi`), from a **tagged** bundle on `main`.
    - **Host conformance:** the host implementation (`filterUnetCellObjects`,
      `UnetC4`) must reproduce the vectors.
    - **Changing the science:** a change is a new profile version in that
      repository, and then a new contract here.
11. **One contract, encoded twice.** ADR 0007's vocabulary stays.
    - A **contract** is the science: Contract 3 = `unet-cells`.
    - In the ABI it is encoded as `science_profile` 2 / `profile_version` 2.
      The compatibility matrix maps the two 1:1.
    - A MIB **profile** remains the parameter document.
12. **A PL bitstream plus its model weights is a core** in the sense of ADR
    0007: it implements exactly one contract.
    - **New core version, same contract:** a new bitstream or a weights change
      (for example, retraining the U-Net).
    - **Provenance:** recordings and the run snapshot carry the contract, the
      PL build id and the weights sha256.
    - A signed host Contract 3 plugin is optional (desktop reprocessing). It is
      not an instrument dependency.
13. **The Qt frontend is retired after #450 reaches parity.**
    - Until then, Qt takes fixes only; new features go to React/Tauri.
    - The Qt sources are removed once #450 has landed and parity is confirmed.
14. **The instrument image is assembled from CI artifacts.**
    - mib-studio-qt CI builds `yofo-studio-server` and the UI bundle for
      armv7, stamped with the commit and bridge ABI version.
    - `meta-yofo` (pz7035-imx426) fetches them by URL and sha256.
    - The BSP tree goes under git. No image installs from a local path.

## Consequences

- **Order of work:**
  1. land the Contract 3 stack (#unet-cells-contract3 -> pz-results-decoder
     -> unet-c4-host-model) on `develop`;
  2. freeze bridge-ABI bumps on `develop`;
  3. merge `develop` into `dev/react-tauri` and land #450;
  4. land the instrument branches one by one;
  5. renumber the bridge ABI once;
  6. delete the instrument branches and lift the freeze.

  The core-file conflicts (`ProcessingService`, `Hdf5Service`,
  `ProcessingTypes`, config) are resolved with the round-trip, Contract 3
  conformance and PL replay tests on each step.
- **pz7035-imx426** merges its ABI and profile work to `main` and tags the
  bundle (`abi-v1.2.0`). mib-studio-qt re-vendors from the tag.
- **CI** gains an armv7 build job (the `linux-armv7-yocto` preset) and runs the
  PL replay tests on every PR.
- The compatibility matrix gains a Contract 3 row whose reference is the PL
  specification. The provenance fields (PL build id, weights sha256) are added
  when the instrument recording lands.
- The E0 manifest (`deploy/embedded/pz7035-target.json`) is updated when the
  E0 ADR lands as 0012:
  - display "none (remote React client)";
  - PL ownership of gating, target and trigger;
  - React/Tauri allowed for the host client only.
