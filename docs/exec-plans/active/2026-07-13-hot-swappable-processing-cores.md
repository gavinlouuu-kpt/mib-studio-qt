# Hot-swappable processing cores

Status: active

## Goal

Publish immutable, enumerable `mib_processing` releases and let MIB Studio
prepare and explicitly activate an approved native core at a quiescent
between-operation boundary. Biowork resolves the same registry into isolated
per-version Python environments. No application/image redeployment is required
to use a newly published core, and every run records exact provenance.

GitHub tracking:

- [MIB Studio epic #236](https://github.com/KPT1020/mib-studio-qt/issues/236)
- [Biowork companion epic #111](https://github.com/gavinlouuu-kpt/Biowork-monorepo/issues/111)
- [Project #6](https://github.com/users/gavinlouuu-kpt/projects/6)

## Acceptance criteria

- [ ] A signed version is published once as immutable metadata and appears in
      the channel catalog without destroying prior versions.
- [ ] Registry and loader contracts remain OS-neutral: platform-matched shared
      libraries carry a mandatory named trust scheme, while an unimplemented
      platform verifier fails closed rather than falling back to unsigned code.
- [ ] `latest.json` remains consumable by legacy clients; exact pins resolve
      through `versions/<version>.json` and fail closed.
- [ ] Live capture, experiments, replay overlays, offline regeneration,
      tracking/target selection, and empty-frame detection use one selected
      kernel implementation.
- [ ] Native activation verifies SHA-256, the required platform signer, ABI,
      contract, runtime, app compatibility, and self-test before transactional
      swap.
- [ ] A core remains pinned for an operation; activation is rejected while any
      processing lease, experiment, recording, preview executor, or batch job
      is active.
- [ ] Profiles declare contract compatibility but never select a core; HDF5
      output records core version, contract/ABI, artifact/manifest digests,
      release tag, and source.
- [ ] Biowork can run the baked baseline or an exact compatible release wheel
      from a persistent, lock-guarded environment cache.
- [ ] Unit, conformance, lifecycle/stress, HDF5, docs, Windows signing, and live
      hardware acceptance gates pass.

## Delivery slices and dependencies

- [x] [A6 #238](https://github.com/KPT1020/mib-studio-qt/issues/238) — add
      schema-v2 canonical identity, immutable history, short-cache catalog,
      PEP 503 page, GitHub Release asset derivation, safe merge/idempotency,
      and atomic version bump/tag tooling. Unit and sandbox dry-run coverage is
      implemented; real R2 proof remains A12.
- [ ] [A7 #242](https://github.com/KPT1020/mib-studio-qt/issues/242) — route
      every scientific path through `IProcessingKernel`, preserving golden
      behavior before introducing dynamic loading. The C++ seam is now total:
      contours/metrics/LUT/target gating (`analyzeObjects`) and batch track
      matching (`matchTrack`) execute through the selected kernel with the
      shared implementation in `ProcessingScience.cpp`, pinned by a
      pre-migration golden test and a spy-kernel routing proof. Remaining: an
      ABI v2 that marshals object records across the C plugin boundary so a
      dynamic core can replace the science, plus overlay-contour unification.
- [ ] [A8 #239](https://github.com/KPT1020/mib-studio-qt/issues/239) — freeze
      the POD/opaque-handle C ABI, build the Windows plugin and descriptor, and
      validate independently built compatible/incompatible fixtures. Local/CI
      wiring now includes separately configured C/C++ fixture modules,
      import/export audit, a trusted signed-SDK verifier matrix, and isolated Production
      signing; the real signed run remains A12 evidence.
- [x] [A9 #237](https://github.com/KPT1020/mib-studio-qt/issues/237) — extend
      the existing conformance-gated tag workflow through Python 3.13, build
      and sign the native asset, attach one release set, then publish immutable
      manifest/catalog/package page before `latest.json`. The production
      signing/R2 secret path is wired but intentionally not claimed as run.
- [ ] [A10 #241](https://github.com/KPT1020/mib-studio-qt/issues/241) — add the
      trusted content-addressed cache, catalog client, resident module loader,
      core leases, and transactional activation manager. A→B→A, rejection-before-
      reset, stale-state clearing, and watchdog concurrency stress are green;
      the Windows live rehearsal remains.
- [ ] [A11 #243](https://github.com/KPT1020/mib-studio-qt/issues/243) — add the
      Processing Core selector/status, admin pin, compatibility handling, and
      HDF5/run provenance. Profile contract compatibility, explicit downgrade,
      and recorded-vs-active regeneration warnings are now implemented; live
      signed selector/restart evidence remains.
- [ ] [A12 #240](https://github.com/KPT1020/mib-studio-qt/issues/240) — publish
      a real signed release to R2, activate/downgrade it on Windows hardware,
      and capture performance/provenance evidence.
- [ ] [A13 #245](https://github.com/KPT1020/mib-studio-qt/issues/245) — add the
      Linux `.so` release/audit lane and an offline-verifiable detached-signature
      adapter behind the same injected trust boundary. The Ed25519 verifier,
      compiled SPKI pin, manifest signature transport, hidden-visibility `.so`
      + sidecar build, and the Linux CI build/audit/sign-rehearsal lane are
      implemented and tested; the live gate (production keypair, repository
      pin, Production signing job, signed `.so` publication, distro baselines)
      remains open and unsigned Linux production activation stays unavailable.

A6 and A7 proceed in parallel. A8 depends on A7. A9 depends on A6+A8; its
workflow can land before A8 while the new target is developed on the same
branch. A10 depends on A6+A8, A11 on A7+A10, and A12 is the live exit gate.
Biowork B8 starts from A6's contract; its version manager and launcher follow
the resolver and remain independent of desktop activation internals.

## Public contracts

- Registry keys are `{channel}/processing-core/latest.json`, immutable
  `versions/<version>.json`, `index.json`, and
  `simple/mib-processing/index.html`.
- `latest.json` is the canonical channel-active pointer and is written last;
  `index.active_version` is an enumerable mirror, never sufficient by itself
  to select Active during a partially completed publication.
- Schema v2 keeps schema-v1 wheel/contract/profile/LUT fields and adds canonical
  `version`, wheel filename/size, and optional `native_plugins[]`.
- A current native release is
  `mib_processing_core-<version>-windows_x86_64.{dll,json}`. The registry also
  reserves OS-matched `.so`/`.dylib` artifacts without changing schema. The
  sidecar names ABI/contract/runtime/app and mandatory signing-scheme claims;
  the publisher derives URL, digest, and size from actual release bytes.
- The plugin exports `mib_processing_get_api`; no Qt/OpenCV/STL/exceptions or
  allocator ownership crosses its versioned C boundary.
- Environment exact pins outrank user selection. Channel publication is only
  a recommendation and never silently activates a desktop core.

## Verification

- Registry/version tooling: `python3 -m unittest -v
  test_publish_processing_core.py test_bump_mib_processing_version.py
  test_contract_version_consistency.py test_publish_profiles.py`.
- Docs/vault: `python3 scripts/check_docs.py`.
- Backend/core: Linux backend CTest plus installed-wheel and dynamic-plugin
  golden conformance.
- Shared-state changes: watchdog-protected stress, frame-accounting invariants,
  and TSan.
- Save/provenance changes: HDF5 round-trip plus fault injection and legacy-file
  compatibility.
- Realtime boundary: steady-state median/p95 ratio against the bundled adapter;
  no loading, hashing, signature work, or context creation per frame.
- Release: workflow parse, Windows x64 build/import audit, valid Authenticode
  signature, descriptor/manifest equality, exact desktop installer artifacts,
  then live R2/hardware proof in A12. Stable manual CI may publish refs only
  after build/test/package gates; manual tag dispatch must build the exact tag.
  Every desktop publisher must read back the configured numeric/full identity,
  run CTest before external mutation, and preserve full beta identity in R2.

## Decision log

- 2026-07-13: Native desktop cores use in-process C ABI plugins; Biowork keeps
  per-version Python environments because its runtime already speaks Python.
- 2026-07-13: Switching is only between operations. The host owns threads and
  HDF5; the selected kernel owns all version-sensitive scientific behavior.
- 2026-07-13: New publications never auto-activate. Explicit/admin pins fail
  closed; an unpinned fresh install keeps the bundled core.
- 2026-07-13: Native trust requires manifest SHA-256 plus an approved
  Authenticode signer. Sidecar signing metadata is informational, not a trust
  root.
- 2026-07-13: `latest.json` stays a full document for backward compatibility;
  immutable history plus `index.json` provides exact selection and rollback.
- 2026-07-13: rollback copies the existing immutable bytes via
  `--promote-version`; it never reconstructs an old version using current
  serializer/schema behavior.
- 2026-07-13: `bindings/python/pyproject.toml` is the wheel version source of
  truth. Tag creation is a post-commit step so it cannot point at stale files.
- 2026-07-14: Native selection persistence is a locked activation pre-commit,
  not post-swap cleanup. A failed settings sync preserves the prior live core.
  The desktop uses the stable `MIB Studio` settings namespace and performs a
  one-time, non-destructive migration of every legacy `Unknown Organization`
  key before reading any preferences; an unwritable migration stops startup.
- 2026-07-14: Every maintained desktop publisher must opt into CMake's
  non-empty 64-hex signer requirement using the GitHub repository SPKI
  variable. Native release CI independently derives the signed DLL's DER-SPKI
  hash and rejects a mismatch before upload; ordinary development/fork builds
  keep the requirement disabled.
- 2026-07-14: Desktop releases publish only exact, freshly built numeric-version
  installer paths. Manual stable CI defers and atomically publishes its version
  commit/tag after all build gates; tag dispatch validates, checks out, and
  resolves the requested tag rather than inheriting the workflow branch tip.
- 2026-07-14: Desktop version bumps start from the highest reachable release
  tag line, not only the fallback literal. Paired one-configure CMake overrides
  make numeric/full binary identity explicit and reviewable. Beta installer
  bytes retain numeric Inno/GitHub names but use full-version immutable R2 keys;
  equal numeric SHA betas order by publication time.
- 2026-07-14: The C ABI, immutable registry, content-addressed cache, and module
  loader are platform contracts. Authenticode is only the Windows trust
  adapter. Linux `.so` metadata is accepted now, but production activation
  stays fail-closed until A13 implements and audits a detached-signature scheme.
- 2026-07-14: The Linux trust adapter is a detached Ed25519 signature over the
  exact artifact bytes, transported in the immutable manifest's `signing`
  block (44-byte DER SPKI + 64-byte raw signature, both base64) and trusted
  only against the compiled `MIB_PROCESSING_CORE_ED25519_SPKI_SHA256` pin —
  the same pin format as the Windows Authenticode SPKI. The publisher
  re-derives the key hash from the key bytes; manifest fields are transport,
  never a trust root. An empty pin, a non-Ed25519 key, or any non-canonical
  encoding fails closed. Envelope, rotation, and revocation policy:
  `docs/architecture/processing-core-linux-signing.md`.
- 2026-07-14: A7's seam expansion is code motion behind `IProcessingKernel`,
  not a rewrite: `ProcessingScience.cpp` holds the single science
  implementation, `analyzeObjects`/`matchTrack` are kernel virtuals whose
  defaults execute it, and `ProcessingService` only routes. A golden test
  (`processing.science_golden`) was pinned against the pre-migration outputs
  first; a spy kernel (`processing.science_seam`) proves batch/offline
  routing. The native plugin now compiles the science sources so the released
  artifact is self-contained; ABI v1 still transports mask/empty only.

## Residual infrastructure gates

- Production certificate material and the approved signer identity remain
  unavailable. R2 access is verified and stable LUT revision `2026.07.14-1`
  is publicly reachable, but no fake signing identity was invented and no
  unsigned core release was attempted.
- The real GitHub tag/release, conditional R2 reads/writes, and package-page
  installation must be demonstrated in A12.
- PowerShell and the Windows packaging toolchain are unavailable in this Linux
  sandbox. Static regression tests and YAML parsing cover the release wiring,
  but both stable and beta entrypoints still require a real Windows-runner
  rehearsal before production release.
- Windows microscopy hardware and a published v2 do not exist here. A local
  Docker daemon can validate Biowork's image/UI wiring, but cannot prove the
  release-driven version swap without that signed release; unit/conformance
  wiring is not a substitute for live evidence.
- A7 remains a deliberate architectural blocker: ABI v1 owns mask/empty-frame
  decisions, while contours, scientific metrics, tracking, target selection,
  and callbacks remain host-owned. A science-changing core is not fully
  swappable until that boundary is redesigned and re-baselined.

## Worker pass-off — 2026-07-14 15:33 HKT

### Workspace and review state

- Branch in both repositories:
  `claude/biowork-pipeline-portability-j3qpu6`.
- MIB worktree: `/home/gavin/Developer/.worktrees/mib-hot-swap`.
  Implementation head before this documentation-only hand-off commit is
  `9e22e8f`; PR
  [#244](https://github.com/KPT1020/mib-studio-qt/pull/244) remains draft.
- Biowork worktree: `/home/gavin/Developer/.worktrees/biowork-hot-swap`.
  Head `26e9573`; PR
  [#118](https://github.com/gavinlouuu-kpt/Biowork-monorepo/pull/118)
  remains draft and all reported checks are green.
- Both worktrees were clean before writing this pass-off. Do not merge or mark
  either PR ready while the live gates below remain open.

### Completed and locally verified

- `3f55ec1` makes native registry/selector identity OS-neutral: OS-matched
  `.dll`/`.so`/`.dylib`, normalized `x86_64`/`aarch64`, mandatory named trust
  schemes, persisted trust identity, explicit Linux runtime fingerprints, and
  fail-closed non-Windows activation pending A13.
- `9e22e8f` removes hosted-runner certificate generation and trust-store writes
  from PR CI. The Windows verifier matrix copies the embedded-signed Windows
  SDK `signtool.exe`, derives its signer SPKI, and passes unsigned/valid/wrong-
  signer/tamper cases to the watchdog-bounded C++ WinVerifyTrust verifier.
  Production signing of the actual core remains a separate secret-backed job.
- Local verification passed: 42 Python registry/version tests; all 68 Linux
  backend/UI CTests (hardware-only and remote Dataset Viewer cases skip by
  design); PowerShell syntax parsing in the official PowerShell container;
  docs/vault integrity; workflow YAML parse; and `git diff --check`.
- E2E screenshot evidence remains under `/tmp/mib-e2e-screenshots/` and
  `/tmp/biowork-e2e-screenshots/`; it is local evidence, not a committed
  release artifact.

### Active CI proof to pick up first

Implementation run
[29314789785](https://github.com/KPT1020/mib-studio-qt/actions/runs/29314789785)
was active at hand-off. Windows job `87026251950` was in Conan dependency
installation; Docs CI and desktop workflow validation were already green.
Poll it with:

```bash
cd /home/gavin/Developer/.worktrees/mib-hot-swap
gh run view 29314789785 --json status,conclusion,jobs
gh api repos/KPT1020/mib-studio-qt/actions/jobs/87026251950 \
  --jq '{status,conclusion,current:[.steps[]|select(.status=="in_progress")|.name]}'
```

If the Windows job fails, collect only the bounded failure before changing
code:

```bash
gh run view 29314789785 --job 87026251950 --log-failed
```

Do not reintroduce certificate creation or Root-store mutation. Diagnostic
runs `29313197025` and `29313984541` proved `certutil` stalls after
`Signature matches Public Key` for both a self-signed leaf and a proper
CA-root/code-signing-leaf chain. The current signed-SDK sample deliberately
avoids that hosted-image defect.

On a green result, update PR #244 and A8/#239 with the run/job links and exact
WinVerifyTrust matrix evidence. A documentation-only hand-off commit may mean
the run is attached to its immediate parent rather than the new head; record
that relationship explicitly rather than claiming an unrun commit.

### Gates that must stay open

- A7/[#242](https://github.com/KPT1020/mib-studio-qt/issues/242): the C++
  kernel seam now owns contours, metrics, LUT, target gating, and track
  matching (golden-pinned, spy-verified), but the C ABI v1 still transports
  only mask/empty decisions — ABI v1 dynamic cores inherit the host-compiled
  default science. Full swappability of a science-changing core requires the
  ABI v2 object-record marshalling and re-baselining; callbacks and track
  lifecycle state deliberately stay host-owned.
- A12/[#240](https://github.com/KPT1020/mib-studio-qt/issues/240): provision
  the production certificate/password and repository SPKI pin; define branch
  protection and Production-environment approval rules; publish a real tag/R2
  version; and prove signed A→B→A activation, restart, provenance, latency, and
  hardware behavior. The 2026-07-14 API audit found unprotected `main` and no
  Production protection rules.
- A13/[#245](https://github.com/KPT1020/mib-studio-qt/issues/245): the
  envelope/rotation/revocation policy, Linux Ed25519 verifier, and CI
  build/audit/sign-rehearsal lane are implemented
  (`docs/architecture/processing-core-linux-signing.md`). Still open live:
  provision the production Ed25519 keypair and repository SPKI variable, add
  the Production-gated signing job and release-asset allowlist entries, and
  build/sign/publish a real `.so` across supported distro baselines. An
  unpinned production Linux build correctly fails closed.
- Biowork B13/[#116](https://github.com/gavinlouuu-kpt/Biowork-monorepo/issues/116):
  run live v1/v2 concurrent triggers, cold/warm cache checks, MLflow evidence,
  rollback, and the real auto-bump PR against a published core.

Issue #236 now includes A13 and platform-neutral trust wording. A13's landed
versus residual audit is recorded in
[#245](https://github.com/KPT1020/mib-studio-qt/issues/245#issuecomment-4966396755),
and the release-governance evidence is recorded in
[#240](https://github.com/KPT1020/mib-studio-qt/issues/240#issuecomment-4966314630).

## Live-gate operator runbook (A12 / A13-live / B12 / B13)

Everything below needs credentials, repository administration, or hardware
that no sandbox session can hold. Order matters: 1–3 are prerequisites for
4; 5–6 depend on 4.

1. **Provision signing identities.** Windows: install the production
   Authenticode certificate/password as Production-environment secrets and
   set the repository SPKI variable consumed as
   `MIB_PROCESSING_CORE_SIGNER_SPKI_SHA256`. Linux: generate the Ed25519
   keypair offline (`openssl genpkey -algorithm ed25519`), store the private
   key as a Production secret, and set
   `MIB_PROCESSING_CORE_ED25519_SPKI_SHA256` from the DER-SPKI SHA-256 per
   `docs/architecture/processing-core-linux-signing.md`.
2. **Repository governance.** Protect `main` (required reviews + status
   checks) and add Production-environment approval rules; the 2026-07-14 API
   audit found both missing.
3. **Windows-runner release rehearsal.** Run the stable and beta desktop
   release entrypoints once on a real Windows runner; PowerShell/packaging
   have only been statically validated from Linux.
4. **Publish a real signed release (A12 steps 1–2).** Tag
   `mib-processing-v<next>`; the workflow builds, signs, and attaches the
   flat asset set, then publishes immutable manifest/catalog/package page
   before `latest.json`. Verify the R2 documents and package-page install.
   For the Linux lane, first add the Production signing job and extend the
   release-asset allowlists (`python-wheel.yml`) — unsigned `.so` assets must
   never publish.
5. **Windows hardware verification (A12 steps 3–6).** On microscope
   hardware: discover/prepare/activate v2 without reinstall/restart; run
   live/experiment/replay/regeneration and verify HDF5 provenance matches the
   leased core; downgrade A→B→A; promote and roll back the channel without
   implicit desktop activation; capture signature chain, latency
   median/p95 ratios, and run identifiers on #240.
6. **Biowork live rehearsal (B12/B13).** With the published release: start
   the unchanged baked-v1 image, trigger channel-active v2 and a concurrent
   exact-v1 override, verify MLflow identity and cold/warm cache evidence,
   promote/roll back the channel, and let the scheduled bump workflow open
   its first real draft PR. Attach evidence to
   [Biowork #116](https://github.com/gavinlouuu-kpt/Biowork-monorepo/issues/116).

## Worker pass-off — 2026-07-14 (session 2)

- Branch in both repositories: `claude/hot-swap-cores-handoff-14c9ps`,
  based on `claude/biowork-pipeline-portability-j3qpu6` head (`ebed7ee` /
  `26e9573`).
- This session landed: A13's Ed25519 detached-signature verifier, sidecar/
  publisher/catalog transport, Linux CI build/audit/sign-rehearsal lane, and
  spec doc; A7's kernel-seam expansion (science moved behind
  `IProcessingKernel` with golden + spy-kernel proofs); and the Biowork
  clean-toolchain native-wheel gate closure (81 passed).
- Verified here: full Linux backend suite 71/71, TSan lane 40/40 over
  backend/recording/processing/camera labels, Qt-free guard, 44 registry
  tooling unit tests, docs/vault checks, workflow YAML parse, and the local
  nm/readelf/openssl rehearsal of every new CI step.
- Still open: the runbook above (A12, A13-live, B12, B13) and A7's ABI v2
  object-record marshalling.

## Worker pass-off — 2026-07-15 (session 3): live gates executed

This session provisioned the credentials, shipped the first real signed
releases, ran the live desktop and Biowork rehearsals, and verified the
Windows trust chain on real hardware. Both epic PRs
([mib #244](https://github.com/KPT1020/mib-studio-qt/pull/244),
[Biowork #118](https://github.com/gavinlouuu-kpt/Biowork-monorepo/pull/118))
are **merged to `main`**.

### Credentials and governance (provisioned, operator-owned)

- **Linux Ed25519 signing identity** generated offline; private key in the
  `Production` environment secret `LINUX_ED25519_SIGNING_KEY_PEM`; pin
  variable `MIB_PROCESSING_CORE_ED25519_SPKI_SHA256` =
  `94172d4c0651b1e1511b4e243e0acb8e005cd3aacedbac77a3a4609b76668946`.
- **Windows Authenticode identity**: KPT organizational root CA + code-signing
  leaf. Public root committed at `deploy/signing/kpt-mib-studio-root-ca.cer`
  (thumbprint `4E932550441EC14691E4C3388CBF7B4F18A84862`; fleet installs it per
  `deploy/signing/README.md`). PFX + password in `Production` secrets
  `WINDOWS_SIGNING_CERTIFICATE_BASE64` / `_PASSWORD`; pin variable
  `MIB_PROCESSING_CORE_SIGNER_SPKI_SHA256` =
  `0db49e05875c3ad96d52b84876bff1f44b68affede46282b98cfb663e6a8ba5f`. Offline
  key material lives under the operator's `~/.mib-signing/`; **move it to
  durable secure storage** — the GitHub secrets are write-only, so those files
  are the only rotation/recovery copy.
- **Governance**: `main` is branch-protected (5 always-run required checks,
  1 review, no force-push, `enforce_admins` off as the single-maintainer
  emergency path). The `Production` environment requires operator approval and
  only deploys from `main` or `mib-processing-v*` tags. Recorded on #240.

### A13 — Linux Production signing lane (landed)

`python-wheel.yml` gained `sign-native-plugin-linux`: a tag-only,
`Production`-gated job that refuses to sign unless the key's DER-SPKI matches
the repository pin, produces and verifies the detached RFC 8032 signature over
the exact artifact bytes, and replaces the rehearsal envelope in the sidecar
with the production one. The release job now requires the signed `.so`/`.json`
pair in both flat-asset allowlists. The Windows sign job imports the committed
org root into the ephemeral runner via .NET `X509Store` (never `certutil` —
it hangs on hosted images). Spec updated in
`docs/architecture/processing-core-linux-signing.md`.

### A12 — first real signed releases (published, verified)

- **`mib-processing-v0.1.0`** and **`mib-processing-v0.2.0`** published to the
  immutable R2 registry (`https://updates.yofo.bio`), channel-active 0.2.0,
  history `[0.2.0, 0.1.0]`. Each carries a signed Windows DLL and a signed
  Linux `.so`.
- Both offline-verified: manifest SHA-256 and size match; the Linux `.so`
  Ed25519 signature verifies against the compiled pin with stock OpenSSL; the
  Windows DLL signer SPKI matched the repository pin in CI.
- **Promote/rollback proven**: rolled the stable pointer to 0.1.0 then back to
  0.2.0 via `processing-core-promote.yml` (byte-for-byte immutable promote),
  history preserved throughout. Recorded on #240.
- **Beta desktop rehearsal succeeded end-to-end on a Windows runner**
  (`v1.0.6-beta.68e618b` + R2 beta channel). The **stable** desktop entrypoint
  rehearsal remains open: it pushes a version commit to protected `main`, so it
  needs the branch-protection ruleset swap the operator has not yet approved.

### Windows verification on real hardware (2026-07-15)

Verified the Authenticode trust decision on an actual Windows 11 VM — the one
thing neither Linux nor a hosted CI runner can exercise as a clean
first-trust: fail-closed (`UnknownError`) before the org root is trusted,
`Valid` after, chain builds to the committed root, RFC 3161 timestamp intact,
tamper rejected. The beta installer installs silently and the app backend
boots with the processing subsystem enabled. **Finding: the beta desktop
installer `.exe` is itself `NotSigned`** — only the core DLL is signed; confirm
the stable installer is signed. Full evidence on #240. Not done: interactive
Processing Core selector activation (GUI automation over the hypervisor console
was unreliable; its trust primitive is the WinVerifyTrust proven above) and the
true A12 steps 3–6 (live capture/experiment/replay) which need the microscope.

### Biowork B12/B13 (complete)

Ran the full live rehearsal against the real registry, real releases, MLflow,
Docker, and a real 1.5 GB HDF5 dataset from the **unchanged** baked-v1
orchestration image (no redeployment between version selections): concurrent
channel-active v2 and exact-pin v1 with correct provenance and
`drift=False`; two content-addressed cache generations cold, warm reuse with no
repair generation; channel rollback affected only new unpinned runs; the
scheduled bump workflow opened its first real draft PR
([#119](https://github.com/gavinlouuu-kpt/Biowork-monorepo/pull/119)). Full
evidence on [Biowork #116](https://github.com/gavinlouuu-kpt/Biowork-monorepo/issues/116).

### Defects found and fixed by the live rehearsals

Each was invisible to the Linux sandbox and only surfaced under real execution:

1. Publisher CLI tests were coupled to the repository version, so the first
   real version bump broke release CI (registry unharmed — fail-closed
   ordering held). Fixed with a per-fixture pyproject.
2. Desktop entrypoints installed no numpy for the conformance test.
3. Two Conan-Qt offscreen-QPA deadlocks in `QApplication` bring-up on Windows
   runners (the dialog test and the screenshot step) — both moved to the native
   platform, screenshot step bounded + `continue-on-error`.
4. The registry WAF rejects the default `Python-urllib` user-agent; the Biowork
   version-check script was the only consumer missing a product UA.
5. The Biowork bump workflow ran the orchestration image without its deployment
   source mount, and lacked `actions: write` to dispatch the harness.
6. A stray blank line in `mib_processing/__init__.py` made the bump tool treat
   every no-op bump as changed, blocking `--create-tag`.

### Residual (hand-off)

- **Stable desktop rehearsal** — needs the operator's decision on swapping
  `main`'s classic protection for an equivalent ruleset with an admin/Actions
  bypass, so CI can push its version commit.
- **A12 steps 3–6** — live microscope-hardware verification (capture,
  experiment, replay overlays) and interactive selector activation.
- **A13 residual** — distro-baseline validation of the dynamic OpenCV/spdlog
  link surface.
- **A7** — ABI v2 object-record marshalling for a fully swappable
  science-changing core.
- **Infra** — the Cloudflare WAF rule blocking `Python-urllib/*` on
  `updates.yofo.bio`; keep the product-UA convention or add a WAF exception.
