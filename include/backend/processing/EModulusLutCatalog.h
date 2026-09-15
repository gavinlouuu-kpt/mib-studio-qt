#pragma once

// Qt-free E-modulus LUT catalog (epic #246, ADR 0002). The update/verify/cache/
// fallback state machine lives here; the raw HTTP GET is delegated to an
// injected seam so no Qt networking is linked into the backend. JSON is
// nlohmann, hashing is backend::processing::processingCore*Sha256, paths use
// std::filesystem, timestamps are carried as ISO-8601 strings.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace backend {

// Result of an injected HTTP GET. `ok` means the transport succeeded (a non-2xx
// HTTP status with a body is still ok=false with statusCode set).
struct HttpGetResult {
    bool ok = false;
    long statusCode = 0;
    std::vector<uint8_t> body;
    std::string error;
};

// The shell (Qt now, Rust/Tauri later) supplies this. file:// URLs never reach
// it — the catalog reads those directly.
using HttpGetFn = std::function<HttpGetResult(const std::string& url, int timeoutMs)>;

class EModulusLutCatalog final {
public:
    struct Manifest {
        int manifestSchemaVersion = 0;
        std::string lutId;
        std::string displayName;
        std::string revision;
        std::string downloadUrl;
        std::string sha256;
        std::int64_t sizeBytes = -1;
        std::string publishedAt;     // ISO-8601 text (may be empty)
        std::string appMinVersion;
        std::string appMaxVersion;
    };

    struct ManagedLutInfo {
        std::string sourceType;
        std::string lutId;
        std::string displayName;
        std::string revision;
        std::string manifestUrl;
        std::string downloadUrl;
        std::string localPath;
        std::string sha256;
        std::string checksumStatus;
        std::string note;
        std::string publishedAt;
        std::int64_t sizeBytes = -1;
        bool remoteUpdated = false;
        bool usedBundledFallback = false;
    };

    // httpGet: transport for http(s) manifest/LUT URLs (unset -> remote fetch is
    //   skipped and the catalog uses cache/bundled, matching headless today).
    // appVersion: current app version for the manifest min/max gate (empty ->
    //   permissive, as when Qt's version was null).
    // appDataDir: base directory for the default cache location; the shell
    //   passes the platform app-data dir. The env override
    //   MIB_STUDIO_EMODULUS_LUT_CACHE_DIR always takes precedence.
    explicit EModulusLutCatalog(HttpGetFn httpGet = {},
                                std::string appVersion = {},
                                std::string appDataDir = {});

    static std::string defaultManifestUrl(const std::string& channel = "stable");
    std::string manifestUrlFromEnvOrDefault(const std::string& channel = "stable") const;

    std::optional<Manifest> fetchManifest(const std::string& url, std::string* errorOut = nullptr) const;

    bool ensureManagedLut(const std::string& bundledPath,
                          std::string* resolvedPathOut,
                          ManagedLutInfo* infoOut = nullptr,
                          std::string* errorOut = nullptr) const;

    std::string cacheDir() const;
    std::string lutPath() const;
    std::string metadataPath() const;

    // The on-disk `.meta.json` sidecar shape (public so the .cpp JSON helpers
    // can name it; not part of the intended API surface).
    struct LocalMetadata {
        int schemaVersion = 1;
        std::string sourceType;
        std::string lutId;
        std::string displayName;
        std::string revision;
        std::string manifestUrl;
        std::string downloadUrl;
        std::string sha256;
        std::string checksumStatus;
        std::string localPath;
        std::string publishedAt;
        std::string lastCheckedUtc;
        std::string lastUpdatedUtc;
        std::int64_t sizeBytes = -1;
    };

private:
    // http(s) via the injected seam; file:// / bare paths read from disk.
    std::optional<std::vector<uint8_t>> readUrlBytes(const std::string& url, std::string* errorOut) const;
    bool isCompatibleWithCurrentApp(const Manifest& manifest) const;
    std::optional<Manifest> parseManifest(const std::vector<uint8_t>& bytes, std::string* errorOut) const;
    LocalMetadata metadataFromManifest(const Manifest& manifest,
                                       const std::string& sourceType,
                                       const std::string& localPath,
                                       const std::string& checksumStatus) const;

    HttpGetFn httpGet_;
    std::string appVersion_;
    std::string appDataDir_;
};

} // namespace backend
