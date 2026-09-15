# Qt → React/Tauri migration: backend de-Qt slice 4 (LUT catalog HTTP seam)

Date: 2026-07-15
Epic: #246 · ADRs: `docs/decisions/0001-react-tauri-migration.md`,
`docs/decisions/0002-lut-catalog-http-seam.md` ·
Plan: `docs/exec-plans/active/2026-07-15-qt-decoupling-and-tauri-migration.md`

## Context

`EModulusLutCatalog` was the last significant Qt cluster in the backend and its
sole `QtNetwork` user. ADR 0002 chose to keep the tested update/verify/cache/
fallback state machine in C++ and delegate only the raw HTTP GET to an injected
seam (the shell owns HTTP — Qt now, Rust/Tauri later). Every other Qt piece had
an in-repo Qt-free replacement.

## What shipped

- `include/backend/processing/EModulusLutCatalog.h` + `.cpp` — fully rewritten
  Qt-free. `std::string`/`std::filesystem`; JSON via `nlohmann`; SHA-256 via
  `backend::processing::processingCore{File,Bytes}Sha256`; timestamps carried as
  ISO-8601 strings (`isoNowUtc()`); a small `parseVersion`/`compareVersion`
  replacing `QVersionNumber`; `std::getenv` for env overrides; atomic write via
  temp-file + `std::filesystem::rename`. The `.meta.json` sidecar schema and the
  manifest field names are unchanged (data compatibility preserved).
- **Injected `HttpGetFn` seam** (`backend::HttpGetFn`): `file://` and bare paths
  are read directly (tests/headless); `http(s)://` requires the injected
  fetcher, whose absence is a clean skip → cache/bundled fallback.
- `AppBackend` — `setLutHttpFetcher(HttpGetFn)` and `setLutAppDataDir(string)`;
  the LUT block passes the fetcher, `MIB_STUDIO_QT_VERSION` (added as a backend
  compile def), and the app-data dir into the catalog. The old
  `QCoreApplication::instance()` remote-fetch guard became "has a fetcher or
  file:// URL".
- **Frontend** — `src/frontend/system/LutHttpFetcher.cpp`
  (`makeQtLutHttpGet()`): the blocking QtNetwork GET moved out of the backend;
  `main.cpp` injects it plus
  `QStandardPaths::writableLocation(AppLocalDataLocation)` before
  `backend.initialize()`.
- Build: dropped `Qt6::Network` from the `mib_backend` link and from the
  backend-only Qt component set (`MIBDependencies.cmake` → `Core` only; Network
  moved to the frontend component list).

## Verification

- Full `linux-backend-only` build + `ctest`: **73/73 pass**, incl. the rewritten
  `backend.emodulus_lut_catalog` (Qt-free, file:// state machine: remote update
  downloaded+verified+cached+loadable; broken manifest → last-known-good
  retained) and all integration/e2e tests. `ldd` confirms no `Qt6Network`.
- The frontend `LutHttpFetcher.cpp` was syntax-checked against the Qt6 headers
  (not built here — the frontend is not part of `linux-backend-only`; the
  Windows/frontend lane builds it).

## Not done here

Crash-reporter `QString`/`qtMessageHandler` glue (cluster 6) — the last backend
Qt user — then the final `Qt6::Core` + `AUTOMOC` drop (Phase 1 exit gate:
`linux-backend-only` builds with no Qt SDK).
