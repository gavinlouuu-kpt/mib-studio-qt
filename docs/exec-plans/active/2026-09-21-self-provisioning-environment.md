# Self-provisioning environment: one manifest per concern, assets on Hugging Face

Status: active

> **For agentic workers:** work one PR at a time in the order below. Each PR has
> its own acceptance box; do not start the next PR until the previous one's box
> is ticked and `python3 scripts/check_docs.py` passes. Steps use checkbox
> (`- [ ]`) syntax for tracking.

Baseline: `7b3243c` on `develop` (post #421 device discovery).
Related debt: TD-5 (no pre-commit), TD-9 (no PR lane for Qt frontend tests).

**Goal:** An agent or human landing in a fresh clone, on a laptop or in a
brand-new container, can run one command that reports exactly which tools,
packages, SDKs, datasets and models are missing, and a second command that
installs them. Every list the setup depends on (apt packages, Conan profiles,
toolchain versions, external assets) exists exactly once, in a machine-readable
file, and every doc, CI workflow and devcontainer reads that file instead of
restating it. Datasets and model weights are hosted on Hugging Face and pinned
by revision; the repo carries no binary weights.

**Non-goals:** changing build targets, presets' semantics, the Windows release
pipeline, or the test taxonomy. Reworking the vault structure.

## Problem statement (2026-09-21 audit)

- No devcontainer, Dockerfile, bootstrap or doctor script exists. A container
  has nothing to execute.
- The apt package list is hand-copied into seven workflows
  (`backend-ci`, `bridge-ci`, `desktop-ci`, `sanitizers`, `soak`,
  `exporter-soak`, `python-wheel`) plus three variants in
  `docs/howto/linux-build.md`. None is authoritative.
- `CMakePresets.json` carries `/home/gavin/miniconda3` and linuxbrew in
  `CMAKE_IGNORE_PREFIX_PATH`. The Windows Conan profile `ci` is written inline
  in YAML and cannot be reproduced locally.
- Toolchain pins are scattered: Python only in workflows (3.12 and 3.13 mixed),
  Node 22 only in `desktop-ci.yml`, no `rust-toolchain.toml`, four Python
  requirements sources plus ad-hoc `pip install` lines in CI.
- `knowledge_map/build-and-run/Build.md` is ~300 lines of dated narrative.
  Operational facts (the `cpuinfo` `[replace_requires]`, the localized
  `cl.exe` `/showIncludes` prefix) live only there. `Dependencies.md` cites a
  `conanfile.txt` that does not exist.
- Test data is undeclared. `gavinlouuu/512x96stream` (public) is fetched three
  different ways (Dataset Viewer REST in the kin10 test, raw `resolve/main`
  URLs in `scripts/fetch_hf_512x96stream.py`, the `datasets` library with
  `HF_HOME` in `synthetic_condition_validation.py`). `gavinlouuu/z_adjustment-data`
  is **private**, needs a token, and no doc says so. `gavinlouuu/dc_ds` is used
  by the kedro pipeline. The gold-standard doc references an HDF5 file by a
  path on one Windows user folder.
- `resources/models/yolo11n-seg.onnx` (11 MB) and `.pt` (6 MB) are tracked in
  git; `MIBWindowsDeployment.cmake` and `AppBackend.cpp` assume that path.
- `.claude/settings.local.json` is tracked in a public repo and contains
  plaintext MLflow and Conan-remote passwords in the permission allowlist.
- Repo root carries six build logs, a debug log,
  `data/logs/symphony-state-last.json`, eight `test_*.py` and a dozen
  `publish-*`/`verify-*` scripts. `scripts/build_mac.sh` and
  `tools/build_mac.sh` are diverged copies.

## Target layout

```
env/
  apt-packages.txt          # one package per line; comments allowed; sections: base, backend, frontend, desktop-shell, sanitizers
  brew-packages.txt         # macOS equivalent
  toolchain.toml            # cmake, ninja, conan, python, node, rust minimums (read by doctor; mirrors mise.toml)
  assets.json               # datasets + models: hub id, type, visibility, revision, files, sha256, consumers, token_required
conan/profiles/
  linux-gcc13               # existing
  windows-msvc194-ninja     # moved out of build-windows.yml / release.yml / python-wheel.yml
  windows-msvc194-vs        # windows-default counterpart
scripts/
  doctor.sh  doctor.ps1     # check only; exit 1 and print exact fix commands
  bootstrap.sh bootstrap.ps1# idempotent install: packages, SDK, assets, conan, preset hint
  provision-assets.py       # reads env/assets.json; huggingface_hub, pinned revisions, sha256 verify
  provision-mindvision-sdk.{sh,ps1}   # existing, unchanged
.devcontainer/
  Dockerfile                # ubuntu:24.04 + env/apt-packages.txt (base+backend[+frontend])
  devcontainer.json         # postCreate: scripts/bootstrap.sh --public-assets-only
.github/actions/setup-linux-env/action.yml   # composite: apt from env/apt-packages.txt, conan, provision SDK + assets
mise.toml                   # optional local pin of cmake/ninja/conan/python/node/rust
rust-toolchain.toml  .nvmrc  .python-version
```

`build/vendor/` remains the provisioned-artifact root (MindVision SDK already
lives there). Assets land in `build/vendor/assets/<kind>/<name>/` and the HF
cache in `.cache/huggingface/` (already gitignored).

## Global constraints

- Every list has exactly one home. A doc may show a command that reads the
  list; it may not restate the list.
- `scripts/doctor.sh` never installs or downloads. `scripts/bootstrap.sh` is
  idempotent and safe to rerun; it never uses `sudo` for anything but package
  managers and prints what it will do before doing it.
- Public assets download with no token. `HF_TOKEN` is required only for
  private assets and the doctor says so by name.
- Assets are pinned by Hub revision (commit SHA) and per-file SHA-256 in
  `env/assets.json`; `provision-assets.py` fails on mismatch. Bumping an asset
  is a reviewed diff to that file.
- No binary weights or datasets in git after PR 1. No Git LFS (decision below).
- CI must not diverge from local: the composite action and the Dockerfile
  consume the same `env/` files as `bootstrap.sh`.
- Network-dependent tests carry the CTest label `network`; the default test
  presets exclude it; a dedicated lane includes it.
- Vault notes update in the same PR as the code they describe.

## Decision log

- 2026-09-21: Baseline branch is `develop` (repository default), not `main`.
- 2026-09-21: Datasets **and** model weights are hosted on Hugging Face
  (user decision). Datasets stay under `gavinlouuu/`; models go to the public
  Hub *model* repo `gavinlouuu/mib-yolo11n-seg` (created 2026-09-21, commit
  `2149ceb`; Ultralytics weights are AGPL so public is fine) holding both `.onnx` and `.pt` so retraining provenance travels with
  the artifact.
- 2026-09-21: No Git LFS. The Hub already gives revision pinning, SHA-256,
  private repos and a Python client; LFS would add a second binary store and
  a quota to manage.
- 2026-09-21: One fetch mechanism: `scripts/provision-assets.py`, stdlib
  `urllib` against `resolve/<revision>` URLs with an optional bearer token,
  chosen over `huggingface_hub` so CI and CMake need no Python package. The kin10 test keeps its Dataset
  Viewer REST path (no Python dependency inside the C++ harness, exit 77
  semantics proven) but reads dataset id, config, split and row list from
  `env/assets.json` via a generated header or a CTest `ENVIRONMENT` property,
  not from string literals. `scripts/fetch_hf_512x96stream.py` is replaced by
  `provision-assets.py --asset 512x96stream-mock-frames`.
- 2026-09-21: Model path contract unchanged at runtime
  (`<exe>/resources/models/yolo11n-seg.onnx`). What changes is the *source*:
  `MIBWindowsDeployment.cmake` and the dev-tree copy step read from
  `build/vendor/assets/models/yolo11n-seg/` and CMake errors at configure with
  the provisioning command if the file is absent and `MIB_HAS_ONNXRUNTIME=ON`.
- 2026-09-21: Devcontainer base is `ubuntu:24.04`. Noble's Qt 6.4.2 is already
  proven to configure, build and pass `linux-backend-only` CTest
  (`docs/howto/linux-build.md`). Conan Qt 6.7.3 stays the release toolchain.
- 2026-09-21: `env/apt-packages.txt` uses `# section:` headers so one file
  serves backend-only (`base`,`backend`), full Qt (`+frontend`), Tauri shell
  (`+desktop-shell`) and sanitizer lanes; the composite action and bootstrap
  take `--sections`.
- 2026-09-21: The private `z_adjustment-data` corpus is an opt-in asset
  (`"required": false`). Conformance against it runs only in the `network`
  lane with `HF_TOKEN` from repository secrets.

## Acceptance criteria (whole plan)

- [ ] `docker build .devcontainer && docker run … bash -lc 'scripts/bootstrap.sh && cmake --preset linux-backend-only && cmake --build --preset linux-backend-only-build && ctest --preset linux-backend-only-test'` is green from a clean image, with `network`-labelled tests excluded by the preset.
- [x] macOS (2026-09-21, bash 3.2): `doctor.sh` exit 1 with four fixes → `bootstrap.sh --public-assets-only` → `doctor.sh` exit 0; second bootstrap a no-op in 5.7 s. Windows: `doctor.ps1`/`bootstrap.ps1` written, parse-checked by `ci.yml`, not yet run on a Windows host.
- [x] `grep -rn 'apt-get install' .github/workflows` matches only the composite action, plus one line inside `python-wheel.yml`'s `docker run` of a slim image (provisioning the test container, not the runner).
- [x] `git ls-files | grep -E '\.(onnx|pt)$'` is empty; the only tracked `.log` files are evidence bundles under `docs/evidence/` (kept on purpose); `git ls-files .claude` lists only `skills/`.
- [x] `env/assets.json` lists every Hub id referenced anywhere in `scripts/`, `tests/`, `tools/`, `docs/howto/`, `.github/`; `scripts/check_docs.py` enforces this (fails on an injected undeclared id).
- [x] `AGENTS.md` "Build and Run" is three commands: doctor, bootstrap, preset (PR 2).
- [x] No path under `/home/<user>` or `C:/Users/<user>` in any tracked build/config file. Remaining hits are history notes (`knowledge_map/task/`, `Recent-Work.md`), evidence bundles, and `deploy/*/README.md` server runbooks that document a specific host's directory layout (kept; not a build input). The tracked build logs go in PR 6.

## PR 0: secrets (do first, smallest possible diff)

- [ ] **Owner action:** rotate the MLflow tracking password and the Conan remote password (both appeared in `.claude/settings.local.json`, tracked since `b0cb0cc`, 2026-03-24). Rotation is the fix; history rewrite is optional and does not replace it.
- [x] `git rm --cached .claude/settings.local.json`; add `.claude/settings.local.json` and `.claude/*.local.*` to `.gitignore`. Keep `.claude/skills/` tracked.
- [x] Grep the tree and full history for the two values and for `PASSWORD=`/`TOKEN=`/`SECRET=`/`API_KEY=` literals: both values occur only in that one file (one commit each); no other hardcoded credentials found.
- [x] Golden principle 11 added to `docs/golden-principles.md`.
- [x] Vault: note in `knowledge_map/current-state/Recent-Work.md`.

Acceptance: `git ls-files .claude` shows only `skills/`; `gitleaks detect` (or an equivalent grep) is clean on the tree.

## PR 1: asset manifest, Hugging Face hosting, provisioner

- [x] Create the Hub model repo and upload `yolo11n-seg.onnx`, `yolo11n-seg.pt`, `convert_yolo11n_seg_to_onnx.py` and a model card stating the source checkpoint, export flags and the ONNX opset. Record the commit SHA.
- [x] Write `env/assets.json`:

  ```json
  {
    "version": 1,
    "assets": [
      {"id": "yolo11n-seg", "kind": "model", "repo": "gavinlouuu/mib-yolo11n-seg", "revision": "<sha>",
       "files": [{"path": "yolo11n-seg.onnx", "sha256": "<hex>"}], "visibility": "public",
       "required": true, "consumers": ["YoloService", "MIBWindowsDeployment.cmake"]},
      {"id": "512x96stream-kin10", "kind": "dataset", "repo": "gavinlouuu/512x96stream", "revision": "<sha>",
       "viewer": {"config": "default", "split": "train", "rows": [0,1,2,2500,4999]},
       "visibility": "public", "required": false, "consumers": ["backend.kin10_hf_dataset_pipeline"]},
      {"id": "512x96stream-mock-frames", "kind": "dataset", "repo": "gavinlouuu/512x96stream", "revision": "<sha>",
       "files": [{"glob": "data/*.png", "limit": 1000}], "visibility": "public", "required": false,
       "consumers": ["MockCamera", "scripts/synthetic_condition_validation.py", "docs/howto/pipeline-latency-diagnosis.md"]},
      {"id": "z-adjustment-50v", "kind": "dataset", "repo": "gavinlouuu/z_adjustment-data",
       "revision": "fc62e3147fb0237e46e6eebd0fb09e669abef12f",
       "files": [{"path": "50V_in_focus/recording_20260511_160006.h5", "sha256": "7ed2a721…"}],
       "visibility": "private", "token_required": true, "required": false,
       "consumers": ["scripts/run_processing_conformance.py", "scripts/z_adjustment_50v_reference.json"]},
      {"id": "dc-ds", "kind": "dataset", "repo": "gavinlouuu/dc_ds", "revision": "<sha>", "visibility": "public",
       "required": false, "consumers": ["scripts/empty_frame_detection.py", "scripts/kedro_frame_detection"]}
    ]
  }
  ```

- [x] Write `scripts/provision-assets.py` (stdlib + `huggingface_hub` only): `--all`, `--required-only`, `--public-only`, `--asset <id>`, `--list`, `--check` (no download, exit 1 on missing/mismatch). Downloads to `build/vendor/assets/<kind>/<id>/`, verifies SHA-256, writes `provisioned.json` with resolved revision. Uses `HF_TOKEN` from env when present; on a 401 for a private asset prints the exact env var name and the Hub URL.
- [x] `git rm` the two weight files. Update `MIBWindowsDeployment.cmake:334-340` and the dev-tree copy to source from `build/vendor/assets/models/yolo11n-seg/`; when `MIB_HAS_ONNXRUNTIME=ON` and the file is absent, `message(FATAL_ERROR "run: python3 scripts/provision-assets.py --asset yolo11n-seg")`. `YoloService.cpp` fallback search unchanged.
- [x] Replace `scripts/fetch_hf_512x96stream.py` with `provision-assets.py --asset 512x96stream-mock-frames`; update `docs/howto/mock-camera-dev-mode.md`, `pipeline-latency-diagnosis.md`, `synthetic-condition-validation.md`, `build-windows.yml:428`.
- [x] `scripts/synthetic_condition_validation.py`, `empty_frame_detection.py`, kedro `parameters.yml`: read repo id and revision from `env/assets.json` (small shared helper `scripts/assets_manifest.py`).
- [x] kin10 test: the Python and shell harnesses read dataset/config/split/rows from the manifest through `scripts/assets_manifest.py` (simpler than a CTest `ENVIRONMENT` property; CMake reads the manifest only for the model path); harness keeps Dataset Viewer + exit 77.
- [x] `backend.kin10_hf_dataset_pipeline` already carried `network` + `SKIP_RETURN_CODE 77`; the Windows fast presets already excluded `integration`. Added `exclude network` to the three Linux test presets and a `linux-network-test` preset (`include network`).
- [x] `docs/gold_standard_metrics.md`: z-adjustment section now provisions via the `z-adjustment-50v` asset; the PANC1 local path is dropped (file was never available; if it surfaces, pin it as an asset).
- [x] Done in PR 2: the three requirements files moved to `env/requirements-{scripts,tools-runtime,tools-build}.txt`; `env/requirements-build.txt` (conan, numpy) added. `provision-assets.py` is stdlib-only, so no `huggingface_hub` pin is needed for provisioning.
- [x] `scripts/check_docs.py`: new check that every `gavinlouuu/<name>` Hub id in `scripts/`, `tests/`, `tools/`, `docs/howto/`, `.github/` appears in `env/assets.json`.
- [x] Vault: new note `knowledge_map/build-and-run/Assets.md` (manifest, provisioner, token policy, how to bump a revision); update `Dependencies.md`, `services/YoloService.md`, `camera/MockCamera.md`, `Run-Modes.md`; `docs/howto/hf-dataset-integration-tests.md` shrinks to "see Assets.md + run the network lane".

Acceptance: fresh clone, `python3 scripts/provision-assets.py --required-only` then `cmake --preset linux-backend-only` configures with ONNX on; `--check` exits 1 before and 0 after; CTest default preset runs zero `network` tests; `git ls-files resources/models` shows only the README and converter.

## PR 2: doctor and bootstrap

- [x] `env/apt-packages.txt` with `# section: base|backend|frontend|desktop-shell|sanitizers` headers, populated by diffing the seven workflow lists and `linux-build.md` (superset, then trim what no lane needs; record removals here). Removed nothing; added `ninja-build`, `python3-venv`, `ca-certificates`, `libfmt-dev`, `libssl-dev` (Linux Ed25519 verify) which only the how-to or CMake mentioned. `env/brew-packages.txt` for macOS (cmake, ninja, conan, sevenzip, hdf5, opencv, spdlog, nlohmann-json, qt@6).
- [x] `env/toolchain.toml`: minimum versions for cmake (3.21), ninja, conan (2.x), python (3.12), node (22), rust (stable, pinned in `rust-toolchain.toml`), MSVC (19.4x). Add `rust-toolchain.toml`, `.nvmrc`, `.python-version`, `engines` in `desktop/package.json`, and a `mise.toml` mirroring the same pins for hosts that use mise.
- [x] `scripts/doctor.sh` and `doctor.ps1`: check OS, each toolchain version against `env/toolchain.toml`, packages from the relevant list, Conan profile presence, MindVision SDK in `build/vendor`, `provision-assets.py --check --required-only`, `HF_TOKEN` only if `--with-private`, VS 2022 x64 shell on Windows, English `cl.exe` `/showIncludes` prefix (from Build.md). Output: one line per item, `OK`/`MISSING`/`OLD`, then a "Run these:" block. Exit 1 if anything is missing. No side effects.
- [x] `scripts/bootstrap.sh` and `bootstrap.ps1`: `--sections`, `--public-assets-only`, `--dry-run`. Steps: package manager install from the list, `pip install -r env/requirements-scripts.txt` into `.venv`, `conan profile detect` if absent and copy repo profiles, `provision-mindvision-sdk`, `provision-assets --required-only`, then print the preset to use for this host. Rerunnable.
- [x] Move the inline Windows Conan profile from `build-windows.yml`, `release.yml`, `python-wheel.yml` into `conan/profiles/windows-msvc194-ninja` (and a VS-generator twin); add the `cpuinfo` `[replace_requires]` line there; workflows pass `-pr:h conan/profiles/…`.
- [x] Vault: `Build.md` gains a "Start here" block pointing at doctor/bootstrap; `AGENTS.md` Build and Run becomes:

  ```bash
  scripts/doctor.sh          # what is missing, with fixes
  scripts/bootstrap.sh       # install it
  cmake --preset linux-backend-only && cmake --build --preset linux-backend-only-build
  ```

Acceptance: on a clean `ubuntu:24.04` container and on a Mac with Homebrew, doctor → bootstrap → doctor gives 1 → 0; `conan install … -pr:h conan/profiles/windows-msvc194-ninja` on the rig PC resolves without a local profile edit.

## PR 3: devcontainer and CI convergence

- [x] `.devcontainer/Dockerfile`: `ubuntu:24.04`, `COPY env/apt-packages.txt`, install sections `base backend frontend`, non-root user, Conan + Python venv. `devcontainer.json`: `postCreateCommand: scripts/bootstrap.sh --public-assets-only`, mounts `.cache/huggingface` and the Conan cache as named volumes, forwards `HF_TOKEN` from the host if set.
- [x] `.github/actions/setup-linux-env/action.yml` (composite): inputs `sections`, `provision-assets` (`required|public|all`), `conan` (bool). Reads `env/apt-packages.txt`. Replace the apt/pip/provision steps in `backend-ci`, `bridge-ci`, `desktop-ci`, `sanitizers`, `soak`, `exporter-soak`, `python-wheel` (Linux jobs) with it.
- [x] Add `network-tests.yml` (nightly + manual): setup with `provision-assets all`, `HF_TOKEN` from secrets, `ctest --preset linux-network-test`.
- [x] Decision: no GHCR image for now. Package installs take ~2 min per job and the composite action keeps lanes independent of an image publish step; revisit if apt time dominates.
- [x] Vault: `Build.md` "CI lanes" table lists the composite action as the single source; `docs/architecture/testing-strategy.md` gains the `network` lane.

Acceptance: whole-plan criterion 1 and 3 pass; all seven workflows green on the PR.

## PR 4: presets and pin hygiene

- [x] Remove `CMAKE_IGNORE_PREFIX_PATH` user paths from `CMakePresets.json`; document in `Build.md` that machine-specific ignores go in `CMakeUserPresets.json` (already ignored). Add a `CMakeUserPresets.example.json`.
- [x] Add `"description"` to every configure preset naming the `env/` sections and the Conan profile it expects, so `cmake --list-presets` is self-explanatory.
- [x] `cmake/MIBCompilerSettings.cmake`: detect the localized `/showIncludes` prefix and `message(FATAL_ERROR)` with the fix from Build.md instead of silently losing header deps.
- [x] Vault: `Build.md`; `Dependencies.md` fixed in PR 2 (`conanfile.py`); the "not vcpkg" residue goes with PR 5's Build.md split.

## PR 5: documentation consolidation

- [x] Split `knowledge_map/build-and-run/Build.md`: keep presets, targets, commands, platform guards (current truth only). Move every dated paragraph (windows-ninja bench numbers, sccache, bridge manifest, Chinese cl.exe, cpuinfo, Authenticode target note) into existing or new `knowledge_map/task/YYYY-MM-DD-*.md` notes and link them from a "History" list.
- [x] (PR 2) `docs/howto/linux-build.md` → prerequisites are "run doctor"; keep only the preset matrix and the `mib_processing`-only fast loop.
- [x] (PR 2) `README.md` "Building": replace the Windows-only Conan walkthrough with the three-command block and a link to `Build.md`.
- [x] `WORKFLOW.md` (Symphony) `before_run`: add `scripts/doctor.sh || scripts/bootstrap.sh --public-assets-only`.
- [x] `knowledge_map/Agent-Onboarding.md` Step 1: insert "run `scripts/doctor.sh`" before reading the architecture notes.
- [x] `docs/golden-principles.md`: add the "every list has one home" rule and the asset-pinning rule.

Acceptance: `scripts/check_docs.py` passes; `grep -rn 'apt install\|apt-get install' docs knowledge_map README.md AGENTS.md` returns only references to `env/apt-packages.txt`.

## PR 6: repository root cleanup

- [x] Untrack `build-ninja-*.log`, `debug-*.log`, `data/logs/symphony-state-last.json`; add `*.log`, `/debug-*.log`, `data/logs/` to `.gitignore` (keep `data/.gitkeep`, `data/mock_frames/`).
- [x] Move `test_*.py` (root) → `tests/release/`; `publish-*.py`, `verify-*.py`, `bump-version.ps1`, `release.ps1`, `publish-*.ps1`, `verify-*.ps1` → `scripts/release/`. Update every path in `python-wheel.yml`, `processing-core-promote.yml`, `build-windows.yml`, `ci.yml`, `release.yml`, `docs/howto/release-workflow.md`, `auto-update-r2.md`, `build-installer.md`, `README.md`, and the `paths:` filters at the top of `python-wheel.yml`.
- [ ] Not done: `docs/howto/hdf5-export-app.md` documents the `scripts/` pair as the standalone export-GUI build with its own output dir, so deleting either pair changes a documented workflow. Logged as TD-14 with an exit criterion.
- [x] Vault: `Recent-Work.md`; `tools/README.md` and how-tos repointed to `scripts/release/`.

Acceptance: `ls` at the repo root shows only directories, `CMakeLists.txt`, `CMakePresets.json`, `conanfile.py`, `mkdocs.yml`, `mise.toml`, `rust-toolchain.toml`, dotfiles, and the four top-level markdown files; the `python-wheel.yml` unittest step still runs all eight release tests.

## Open questions (answer before the PR that needs them)


## Progress

- [x] PR 0 secrets (#424; credentials rotated 2026-09-21)
- [x] PR 1 assets on Hugging Face
- [x] PR 2 doctor and bootstrap
- [x] PR 3 devcontainer and CI convergence
- [x] PR 4 presets and pins
- [x] PR 5 docs consolidation
- [x] PR 6 root cleanup
- [ ] Move this plan to `completed/`; open tech-debt entries for anything skipped
