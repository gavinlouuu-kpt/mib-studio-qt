# Native processing-core selection

**Date:** 2026-07-13

**Issues:** [A7 #242](https://github.com/KPT1020/mib-studio-qt/issues/242),
[A8 #239](https://github.com/KPT1020/mib-studio-qt/issues/239),
[A10 #241](https://github.com/KPT1020/mib-studio-qt/issues/241),
[A11 #243](https://github.com/KPT1020/mib-studio-qt/issues/243),
[A12 #240](https://github.com/KPT1020/mib-studio-qt/issues/240)

**Plan:** `docs/exec-plans/active/2026-07-13-hot-swappable-processing-cores.md`

## Implemented slice

- Added a C-compatible engine ABI, bundled adapter, independently built native
  module, strict loader, pooled single-owner contexts, and process-lifetime
  module residency.
- Added content-addressed cache preparation with digest validation, atomic
  staging, concurrent lock protection, and stale-lock recovery.
- Added a stable/beta Settings selector that validates the history index and
  the canonical `latest.json` active pointer independently, then validates the
  selected immutable manifest → downloaded bytes → Authenticode signer →
  ABI/self-test before a between-operation activation. A partially published
  `index.active_version` cannot lead `latest.json`. Persisted explicit
  selections are revalidated on startup; administrator hard pins fail closed.
- Routed live/offline/playback mask generation and raw/buffer empty detection
  through the selected kernel. Operation leases keep one identity selected
  through realtime, batch, recording, and buffer-export lifecycles. Added exact
  core identity to both experiment and raw-recording HDF5 provenance.
- Added C ABI, dynamic parity, loader trust, cache concurrency/recovery,
  catalog hardening, activation lifecycle, and HDF5 round-trip tests. The
  pure-C ABI compile guard requires C11 explicitly so MSVC enables the
  `_Static_assert` checks instead of using its legacy default C mode.
- Kept the local active-core identity independent of registry availability:
  the dialog renders it before the first fetch, while preserving any registry
  failure in the separate status label. An offscreen Qt regression covers the
  original blank-label failure without network access.
- Closed the settings-write ordering defect found by desktop E2E: the desktop
  now has a stable organization/application identity, migrates every legacy
  `Unknown Organization` preference once without overwriting current values,
  and commits an exact core selection under the backend activation lock before
  swapping kernels. Deterministic `QSettings` and injected pre-commit failures
  preserve both the prior persisted selection and prior usable kernel.
- Added the production signer gate to every maintained desktop publisher.
  Stable/beta Actions builds require a normalized 64-hex repository SPKI before
  CMake, the manual workflow checks before its version mutation, and the local
  publisher reads the same GitHub variable before every non-skipped build and
  before bumping. Native release CI derives DER-SPKI SHA-256 from the signed
  DLL and rejects a certificate/pin mismatch before upload; development and
  fork builds remain default-off.
- Hardened those desktop publishers against release/source/artifact mismatch.
  Tag dispatch validates and checks out the exact requested tag; manual stable
  CI changes no refs until build, CTest, installer, exact-artifact, and upload
  gates pass, then pushes main/tag atomically. All paths clear stale installer
  outputs and hash/publish only exact numeric-version Setup/Update files. A
  shared resolver bumps beyond reachable stable/beta tags even when the CMake
  fallback is stale; paired CMake overrides plus identity-file readback prove
  the binary's numeric/full version matches the release. All entrypoints build
  the full target set and run CTest before external publication. Local pushes
  require a clean tree (`main` for stable), are atomic, and GitHub/R2 failures
  are fatal. Beta R2 object keys retain the
  full prerelease identity and same-line SHA betas sort by publication time.
  Local dry-run calculates the prospective version, installer failures are
  fatal, and existing release tags are immutable.
- Closed the local A8/A10/A11 release-quality gaps: repaired manylinux wheels
  import in the slim Biowork runtime base after installing its allowlisted
  `libgl1`/`libglib2.0-0` prerequisites, with EPEL explicitly enabled in the
  pinned AlmaLinux builder for HDF5/spdlog packages; Windows uses independently configured
  good/truncated/incompatible/malformed/throwing modules, export/import audits,
  a `dumpbin` parser that permits its trailing alias text while counting exact
  export rows, a trusted signed-SDK Authenticode matrix, flat release assets, and a separate
  Production signing job. Activation rejects leases before reset, clears all
  stale state after a committed swap, and passes concurrent A→B→A stress.
  Profiles carry optional processing-contract compatibility, downgrades require
  explicit confirmation, and HDF regeneration warns on recorded/active drift.
- Made the activation stress assertion scheduler-independent by holding an
  explicit synchronized operation lease for the mandatory rejection check;
  the concurrent 300-swap phase still verifies frame accounting and accepts
  either quiescent success or lease-guard rejection for every attempt.
- Bounded the Windows Authenticode regression helper with a 60-second child
  watchdog and a version-directory SDK lookup, avoiding an unbounded recursive
  SDK scan or process wait while retaining the real WinVerifyTrust/SPKI checks.
- After bounded diagnostics proved both certificate generation and current-
  user Root-store mutation can stall on the hosted image, removed both from PR
  CI. The verifier matrix now copies the already trusted, Microsoft-signed SDK
  `signtool.exe`, derives its embedded signer SPKI, and checks unsigned/valid/
  wrong-signer/tamper outcomes under one watchdog. Production signing remains
  secret-backed, signs the actual core, and stays isolated in the release job.
- Capped the complete Windows native job at 30 minutes and moved artifact
  uploads to the Node 24-based action, so a provider regression cannot consume
  a six-hour runner and the release lane has no Node 20 deprecation warnings.
- Kept the engine portable instead of baking Windows into the contract:
  registry publication accepts OS-matched `.dll`/`.so`/`.dylib` artifacts,
  catalog and persisted identity include a mandatory signing scheme, Linux
  runtime fingerprints name OS/architecture, and non-Windows production trust
  remains explicitly fail-closed pending A13/#245.
- 2026-07-14 (this session): implemented A13's Linux trust adapter. A
  detached Ed25519 signature and 44-byte DER SPKI travel in the immutable
  manifest's `signing` block; `verifyProcessingCoreEd25519`
  (`src/backend/processing/ProcessingCoreEd25519.cpp`, OpenSSL libcrypto)
  enforces the compiled `MIB_PROCESSING_CORE_ED25519_SPKI_SHA256` pin and
  fails closed without one. The Linux plugin builds with hidden symbol
  visibility, a release-named `.so` plus generated sidecar, and CI gained a
  `build-native-plugin-linux` lane (build, focused CTests including the new
  `processing.core_ed25519` matrix and cache→`dlopen` load, `nm`/`readelf`
  audits, ephemeral-key signing rehearsal through the real publisher).
  Envelope/rotation/revocation policy:
  `docs/architecture/processing-core-linux-signing.md`. The live gate — real
  keypair, repository pin, Production signing job, and signed `.so`
  publication — deliberately stays open.
- Added a Production promotion/rollback action, stable/beta tag-derived
  channels, and explicit app compatibility bounds. Published and publicly
  verified stable LUT revision `2026.07.14-1`, removing the registry's missing
  cross-link prerequisite.

## Deliberate residual scope

The C++ kernel seam now owns the full science (2026-07-14): contours,
metrics, LUT/target gating, and track matching execute through the selected
`IProcessingKernel` (`analyzeObjects`/`matchTrack`) with the single shared
implementation in `ProcessingScience.cpp`, pinned by
`processing.science_golden` and spy-verified by `processing.science_seam`.
The C ABI, however, is still v1 and transports only mask/empty decisions, so
ABI v1 dynamic cores inherit host-compiled default science. A core that
changes contour/metric/tracking semantics is not fully replaceable until an
ABI v2 marshals object records across the plugin boundary; that is the
remaining A7/#242 scope. Callbacks and track lifecycle state deliberately
stay host-owned.

A8/#239 now has independent-source fixtures, import/export audit, a secret-free
signature matrix, and isolated production signing wiring; a real signed runner
execution still belongs to A12. A10/#241 now has A→B→A/reset and concurrent
stress coverage; the remaining live proof is the Windows production path.
A11/#243 now includes profile-contract coupling, downgrade confirmation, and
recorded-vs-active regeneration warnings; live signed Windows selector and
restart rehearsal remain.

Production Authenticode signing and Windows hardware proof need the real
certificate/infrastructure and remain A12 gates. R2 access is present, the
stable LUT is live, and promotion/rollback is wired but cannot run until a
first immutable core exists. The selector's production trust root is the signer public key's DER
SPKI SHA-256 compiled into the application; production environment variables
cannot replace it. The release gate is wired, but the real certificate secrets
and repository pin still need provisioning and remain an A12 release
configuration gate. The Linux sandbox also has no PowerShell/Windows packaging
toolchain, so the static release regressions and YAML checks still require a
real stable+beta Windows rehearsal. No live release was loaded in the sandbox.

**Related:** [[../frontend/ProcessingCoreDialog]] ·
[[../services/ProcessingService]] · [[../services/Hdf5Service]] ·
[[2026-07-13-processing-core-registry]]

## Operator provisioning (2026-07-14, local session)

The credential/governance gates were closed from the operator's machine:
`main` is branch-protected (five always-run required checks, one review,
admin bypass as the emergency path) and the `Production` environment
requires operator approval and only deploys from `main` or
`mib-processing-v*` tags. The Linux Ed25519 identity is provisioned
(`LINUX_ED25519_SIGNING_KEY_PEM` + repository pin variable), and
`sign-native-plugin-linux` now performs the tag-only Production signing with
key-vs-pin verification before any signature. The Windows identity is the
KPT organizational root/leaf chain in `deploy/signing/`; the Production sign
job imports the committed public root into the ephemeral runner store via
.NET `X509Store` (never `certutil`) so `Get-AuthenticodeSignature` can
report `Valid`, while the DER-SPKI pin stays the trust decision. Evidence:
issues #240 and #245.

The first real Windows-runner beta rehearsal (run 29330458231) failed in
`Run tests` without mutating any refs, catching two Windows-only defects the
Linux sandbox could not: the desktop release entrypoints installed no numpy
for `scripts.run_processing_conformance_input`, and
`frontend.processing_core_dialog` was the only backend-booting test not
stubbing `MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL`, so `AppBackend::initialize`
performed a real synchronous LUT-manifest fetch whose Windows proxy
resolution exceeded the 30 s CTest timeout.

## Live gates executed (2026-07-15, session 3)

Both epic PRs (mib #244, Biowork #118) are merged to `main`. The first real
signed releases `mib-processing-v0.1.0` and `v0.2.0` are published to the
immutable R2 registry (channel-active 0.2.0), each with a signed Windows DLL
and Linux Ed25519 `.so`, all offline-verified; promote/rollback was proven on
the stable channel. The beta desktop rehearsal succeeded end-to-end on a
Windows runner (`v1.0.6-beta.68e618b`). The Authenticode trust chain was
verified on a real Windows 11 VM (fail-closed before the org root is trusted,
`Valid` after, chain to the committed root, tamper rejected) — with the finding
that the beta desktop installer `.exe` is itself unsigned. Biowork B12/B13 ran
fully against real infrastructure. The full narrative, evidence links, the six
rehearsal-surfaced defects, and the residual gates are in the runbook's
**Worker pass-off — 2026-07-15 (session 3)** section.

## Pass-off pointer (2026-07-14)

The exact branch/worktrees, implementation commits, active Windows CI run and
job IDs, diagnostic history, pickup commands, and gates that must remain open
are recorded in
`docs/exec-plans/active/2026-07-13-hot-swappable-processing-cores.md` under
**Worker pass-off — 2026-07-14 15:33 HKT**. Start there; do not repeat the
hosted-runner certificate-generation or Root-store experiments.
