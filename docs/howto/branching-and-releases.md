# Branching model & release pipeline

Three long-lived branches: **`develop`** (dev: integration, default),
**`beta`** (staging: what beta users run) and **`main`** (prod: stable).
Everything else is a short-lived feature branch. Merging and shipping are
separate steps: merging to `develop` ships nothing; a promotion PR into
`beta` ships to beta users; a promotion PR into `main` plus a release
dispatch ships to everyone.

## Lanes

```
feature/fix ──PR──▶ develop ──promote PR──▶ beta ──promote PR──▶ main ──dispatch──▶ stable
  (checks)        (dev: integrate,      (staging: auto-beta   (prod: review +
                   ships nothing)         when app changed)     manual release)
```

### Feature lane

- Cut work as `feat/*` / `fix/*` / `chore/*` (exploration: `experiment/*`)
  branches off `develop` and open a PR back into `develop`.
- PRs into `develop` run the required checks (`ci.yml`, `backend-ci.yml`,
  `docs-ci.yml`, `sanitizers.yml`).
- Nothing on a feature branch releases anywhere. A manual beta can be built
  from any ref via `build-windows.yml` dispatch (`mode=beta`) when a
  hardware-in-the-loop build of unmerged work is needed.

### Dev lane (`develop`: integration, ships nothing)

- Every reviewed PR lands here, including multi-PR work that is not ready
  for users. Integrated work is tested on `develop` before it is promoted.
- Unfinished user-facing views stay behind a feature flag or an explicit
  "preview" entry point, so a promotion never exposes a half-built screen.
- No workflow publishes from `develop`.

### Staging lane (`develop` → `beta` → beta channel)

- Promote by opening a PR from `develop` into `beta` when a batch has been
  tested (CI green, end-to-end run, rig checks where hardware is involved).
- Every merge into `beta` that **can change the shipped desktop app**
  auto-builds a beta and publishes it as a GitHub prerelease plus the R2
  **beta** channel (`build-windows.yml`, `push: [beta]` → beta mode;
  version `X.Y.Z-beta.<sha>`).
- The trigger is **path-gated**: merges touching only `docs/**`,
  `knowledge_map/**`, `tools/**`, `tests/**`, `bindings/python/**`,
  markdown, `mkdocs.yml`, or other workflows do **not** cut a beta. Changes
  to `build-windows.yml` itself still release, so pipeline edits are
  exercised immediately.
- Beta versions are immutable per-SHA; the beta channel pointer advances
  with each releasing `beta` merge.
- A hotfix for beta users goes `fix/*` → `develop` and is promoted; if it
  cannot wait, PR the fix into `beta` directly and merge `beta` back into
  `develop` right after.

### Prod lane (`main` → stable channel)

- Promote by opening a PR from `beta` into `main`.
  Protected: **1 review + required status checks**.
- A **stable** release is cut by dispatching `build-windows.yml` with
  `mode=release` from `main`. The flow is **tag-first**: the workflow
  computes the next version, builds and validates the installer, then
  pushes only the annotated tag `vX.Y.Z` pointing at the exact `main` SHA
  it built — it never pushes to `main` itself (branch protection rejects
  direct pushes, including the workflow's own token; this failed the first
  v1.0.7 attempt with GH006). Version resolution is tag-based everywhere
  (`scripts/resolve_desktop_release_version.py`, `cmake/MIBVersion.cmake`),
  so the tag alone is authoritative.
- After tagging, the workflow opens an automated **sync PR into `develop`**
  (`chore/release-vX.Y.Z-sync`) carrying the `DEFAULT_VERSION` no-git
  fallback bump and any refreshed manual screenshots. Merge it like any dev
  PR; if its checks did not start (PRs opened by `GITHUB_TOKEN` do not
  trigger CI), close and reopen it. A failed sync PR never blocks the
  release — the workflow warns and the release proceeds.
- The workflow refuses `mode=release` off any other ref.
- The lane runs the correctness suite only (`ctest -LE "soak|performance"`):
  soak and performance tests are timing-sensitive on shared runners and
  are exercised repeatedly by `soak.yml` (nightly / manual) instead, so a
  release cut never waits on or fails because of them.
- The repository **Latest** badge always belongs to the newest stable
  desktop release: `mib-processing-v*` releases are created with
  `--latest=false`, and betas are prereleases.

### Processing-core lane (independent)

The hot-swappable processing core versions and releases independently of the
desktop lanes: `mib-processing-v*` tags run `python-wheel.yml` (wheels +
signed native plugins → GitHub Release + R2 registry), and channel
activation/rollback goes through **Actions → Promote or roll back processing
core**. See [`release-workflow.md`](release-workflow.md).

## CI triggers

| Workflow | Runs on |
|----------|---------|
| `ci.yml` | push + PR to `main`, `beta`, `develop` |
| `backend-ci.yml` | push + PR to `main`, `beta`, `develop` |
| `docs-ci.yml` | push + PR to `main`, `beta`, `develop` |
| `sanitizers.yml` | PR to `main`, `beta`, `develop`; nightly; manual |
| `build-windows.yml` | **push to `beta` (app paths only) → auto beta**; manual dispatch (beta any ref / release from `main`) |
| `release.yml` | `v*.*.*` tags; manual |
| `python-wheel.yml` | `mib-processing-v*` tags; core-path pushes/PRs (build+test only) |

`build-windows.yml` serializes with a `concurrency` group so rapid `beta`
merges queue rather than race on the R2 publish.

## Rules of thumb

- Don't push directly to `main`, `beta` or `develop`: always via PR.
- A stable release is only ever cut from `main` (the workflow refuses
  `mode=release` off any other ref).
- Merging to `develop` ships nothing. Merging to `beta` means "ship to beta
  users" **when app code changed**; promote only tested batches.
- If a stray beta does get published, delete the GitHub prerelease + tag and
  advance/repoint the R2 beta channel with the next good beta.

### User-facing release checklist

Before the release PR to main is approved:

- Curate `docs/release-notes/vX.Y.Z.md` using the release-notes README template.
  Review operator-facing notes in the PR, including hardware compatibility and
  upgrade instructions. Run `python scripts/release_notes.py --version X.Y.Z --check`.
- Update the manual for user-visible changes. Help ▸ User Manual bundles
  `docs/manual/` and images offline; Help ▸ What's New bundles versioned notes.
- Verify notes in Help and the Software Updates list. Stable Windows builds
  gate on curated notes before building and use them for the GitHub Release
  body and update manifest. Betas use recent vault fragments since the last
  reachable stable tag, explicitly labelled uncurated.
