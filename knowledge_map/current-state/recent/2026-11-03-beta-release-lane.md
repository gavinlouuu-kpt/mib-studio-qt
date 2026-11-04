## 2026-11-03 — Merging no longer ships: a `beta` staging branch between `develop` and `main`

Merging to `develop` used to publish a beta to beta users, so integrated but untested work had
nowhere to live. Now `develop` is the dev lane (integration, ships nothing), the new `beta` branch is
staging (promotion PRs from `develop`; every app-changing merge auto-builds the beta channel), and
`main` stays prod (promotion PR from `beta`, then the tag-first stable dispatch). `beta` was created at
`v1.2.0-beta.5d2b76be`, the build beta users already had. `build-windows.yml` now triggers on
`push: [beta]`; the CI workflows also run on `beta`. See `docs/howto/branching-and-releases.md` and
[[build-and-run/Build]].
