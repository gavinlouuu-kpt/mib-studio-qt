# 0002. E-modulus LUT catalog fetches through an injected HTTP seam

Date: 2026-07-15
Status: accepted

## Context

`EModulusLutCatalog` (`include/backend/processing/EModulusLutCatalog.h`) is the
last significant Qt cluster in the backend and the sole `QtNetwork` user. It
runs a self-contained update/verify/cache/fallback state machine at startup: it
GETs a JSON manifest, GETs the LUT blob, SHA-256-verifies it, atomically caches
it with a `.meta.json` sidecar, and gates on an app-version range — falling back
to the last-known-good local copy or a bundled LUT on any failure.

Every Qt dependency here except the HTTP GET already has a Qt-free replacement
in the repo: JSON → `nlohmann_json`; SHA-256 →
`backend::processing::processingCoreFileSha256` / `…BytesSha256`
(pure C++, no OpenSSL link); paths/atomic-write → `std::filesystem` +
temp-rename; dates → ISO-8601 string helpers over `std::chrono`; app version →
a compiled `MIB_STUDIO_QT_VERSION` string + a small semver compare; env →
`std::getenv`. The one genuinely new capability is the HTTP GET
(`QNetworkAccessManager` + blocking `QEventLoop`/`QTimer`); no C++ HTTP client
is currently a dependency.

This is part of epic #246 (Qt → React/Tauri). ADR
[`0001`](0001-react-tauri-migration.md) requires the backend to become Qt-free
while the Qt shell keeps working, and warns against adding shell-owned
capabilities to the backend that Tauri will own.

## Decision

Keep the update/verify/cache/fallback **state machine in the C++ backend** (it
is tested and valuable) but **delegate the raw HTTP GET to an injected seam**:

```cpp
struct HttpGetResult { bool ok; long status; std::vector<uint8_t> body; std::string error; };
using HttpGetFn = std::function<HttpGetResult(const std::string& url, int timeoutMs)>;
```

- `EModulusLutCatalog` takes an `HttpGetFn`. `file://` URLs are read directly
  with `std::filesystem` (no fetcher needed — preserves the headless/test path);
  `http(s)://` URLs require the injected fetcher, and its absence is a clean
  "remote fetch unavailable" that falls back to cache/bundled.
- The **Qt frontend** supplies a `QtNetwork`-backed `HttpGetFn` (the existing
  blocking GET code, moved out of the backend into
  `src/frontend/system/`), injected via `AppBackend::setLutHttpFetcher()`.
- The **Tauri/Rust shell** will later supply a native fetcher through the same
  seam — no backend change required.

All other Qt usage in the catalog is replaced with the in-repo Qt-free building
blocks listed above.

## Consequences

- **Backend becomes Qt-free of networking** — `Qt6::Network` drops from the
  `mib_backend` link and the backend-only Qt component set (backend now links
  only `Qt6::Core`). `QtNetwork` moves to the frontend, where Qt is expected
  until Phase 5.
- **No new backend dependency** (no libcurl/httplib/OpenSSL-for-HTTP); the Tauri
  era gets networking in Rust for free through the same seam.
- **Backend-only / CI** keep working via `file://` + bundled fallback exactly as
  today (remote fetch was already skipped without a Qt event loop).
- Future agents: do not reintroduce an HTTP client into the backend; feed the
  `HttpGetFn` seam from the shell. The manifest/LUT on-disk format and the
  `.meta.json` schema are unchanged (data compatibility preserved).
