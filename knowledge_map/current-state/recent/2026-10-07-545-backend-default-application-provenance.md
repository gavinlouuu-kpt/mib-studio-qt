## 2026-10-07 — Backend default application provenance (#545)

`AppBackend::initialize()` supplies compile-time version, build ID (`MIB_BUILD_ID`
at configure time, otherwise `dev`), and platform/architecture to every shell.
Explicit shell identities still override subsequent runs. The facade mock-run
regression checks defaults, frozen identity, and overrides. No bridge ABI change.
See [[architecture/ExperimentCoordinator]].
