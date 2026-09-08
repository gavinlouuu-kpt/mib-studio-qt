# Branching model & release pipeline

Two long-lived branches: **`develop`** (integration, default) and **`main`**
(stable / released). Everything else is a short-lived feature branch.

## Lanes

```
feature/fix branch ──PR──▶ develop ──PR──▶ main ──dispatch──▶ stable release
   (checks)          (dev lane: auto-beta   (prod lane: review +
                      when app code changed)  checks, manual release)
```

### Feature lane

- Cut work as `feat/*` / `fix/*` / `chore/*` (exploration: `experiment/*`)
  branches off `develop` and open a PR back into `develop`.
- PRs into `develop` run the required checks (`ci.yml`, `backend-ci.yml`,
  `docs-ci.yml`, `sanitizers.yml`) but need **no review** — fast iteration
  while staying green.
- Nothing on a feature branch releases anywhere. A manual beta can be built
  from any ref via `build-windows.yml` dispatch (`mode=beta`) when a
  hardware-in-the-loop build of unmerged work is needed.

### Dev lane (`develop` → beta channel)

- Every merge to `develop` that **can change the shipped desktop app**
  auto-builds a beta and publishes it as a GitHub prerelease plus the R2
  **beta** channel (`build-windows.yml`, `push: [develop]` → beta mode;
  version `X.Y.Z-beta.<sha>`).
- The trigger is **path-gated**: merges touching only `docs/**`,
  `knowledge_map/**`, `tools/**`, `tests/**`, `bindings/python/**`,
  markdown, `mkdocs.yml`, or other workflows do **not** cut a beta. This
  keeps docs work and explorations (e.g. `tools/fpga_exploration/`) out of
  the release stream. Changes to `build-windows.yml` itself still release,
  so pipeline edits are exercised immediately.
- Beta versions are immutable per-SHA; the beta channel pointer advances
  with each releasing `develop` merge.

### Prod lane (`main` → stable channel)

- Promote by opening a PR from `develop` (or a release branch) into `main`.
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
| `ci.yml` | push + PR to `main`, `develop` |
| `backend-ci.yml` | push + PR to `main`, `develop` |
| `docs-ci.yml` | push + PR to `main`, `develop` |
| `sanitizers.yml` | PR to `main`, `develop`; nightly; manual |
| `build-windows.yml` | **push to `develop` (app paths only) → auto beta**; manual dispatch (beta any ref / release from `main`) |
| `release.yml` | `v*.*.*` tags; manual |
| `python-wheel.yml` | `mib-processing-v*` tags; core-path pushes/PRs (build+test only) |

`build-windows.yml` serializes with a `concurrency` group so rapid `develop`
merges queue rather than race on the R2 publish.

## Rules of thumb

- Don't push directly to `main` or `develop` — always via PR.
- A stable release is only ever cut from `main` (the workflow refuses
  `mode=release` off any other ref).
- Merging to `develop` means "ship to beta users" **when app code changed**;
  keep pure explorations under `tools/` (or on an `experiment/*` branch) and
  they will merge without releasing.
- If a stray beta does get published, delete the GitHub prerelease + tag and
  advance/repoint the R2 beta channel with the next good beta.
