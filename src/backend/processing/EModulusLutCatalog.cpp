#include "backend/processing/EModulusLutCatalog.h"

#include "backend/processing/ProcessingCoreLoader.h" // processingCore*Sha256

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
constexpr int kNetworkTimeoutMs = 8000;
constexpr char kLutFileName[] = "scaled_isoelastic_data_LUT_6.16-4.24.txt";
constexpr char kLutManifestEnv[] = "MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL";
constexpr char kLutCacheDirEnv[] = "MIB_STUDIO_EMODULUS_LUT_CACHE_DIR";
constexpr char kDefaultChannel[] = "stable";

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string toLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string getenvStr(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

// Scheme prefix of a URL ("https", "http", "file", or "" if none).
std::string urlScheme(const std::string& url) {
    const auto pos = url.find("://");
    if (pos == std::string::npos) return {};
    return toLower(url.substr(0, pos));
}

// "file:///abs/path" -> "/abs/path" (host component, if any, is ignored).
std::string fileUrlToPath(const std::string& url) {
    const std::string prefix = "file://";
    if (url.rfind(prefix, 0) != 0) return url; // already a path
    std::string rest = url.substr(prefix.size());
    // Windows drive paths: "file://C:/x" and "file:///C:/x" -> "C:/x".
    const auto isDrive = [](const std::string& s, std::size_t at) {
        return s.size() > at + 1 && std::isalpha(static_cast<unsigned char>(s[at])) && s[at + 1] == ':';
    };
    if (isDrive(rest, 0)) return rest;
    if (rest.size() > 1 && rest[0] == '/' && isDrive(rest, 1)) return rest.substr(1);
    // file://host/path -> drop host; file:///path -> rest starts with '/'.
    if (!rest.empty() && rest[0] != '/') {
        const auto slash = rest.find('/');
        rest = (slash == std::string::npos) ? std::string("/") : rest.substr(slash);
    }
    return rest;
}

// Current UTC timestamp as ISO-8601 with milliseconds and a trailing 'Z',
// matching Qt::ISODateWithMs on a UTC QDateTime closely enough for the sidecar.
std::string isoNowUtc() {
    const auto now = std::chrono::system_clock::now();
    const auto secs = std::chrono::time_point_cast<std::chrono::seconds>(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - secs).count();
    const std::time_t t = std::chrono::system_clock::to_time_t(secs);
    std::tm tmv{};
#if defined(_WIN32)
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03lld",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec, static_cast<long long>(ms));
    return std::string(buf) + "Z";
}

// --- version compare (replaces QVersionNumber) ------------------------------
// Parse leading dot-separated numeric segments; stop at the first segment that
// does not start with a digit. Empty result == "null" (permissive).
std::vector<long> parseVersion(const std::string& s) {
    std::vector<long> out;
    const std::string t = trim(s);
    size_t i = 0;
    while (i < t.size()) {
        if (!std::isdigit(static_cast<unsigned char>(t[i]))) break;
        long v = 0;
        while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) {
            v = v * 10 + (t[i] - '0');
            ++i;
        }
        out.push_back(v);
        if (i < t.size() && t[i] == '.') { ++i; continue; }
        break;
    }
    return out;
}

int compareVersion(const std::vector<long>& a, const std::vector<long>& b) {
    const size_t n = std::max(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const long av = i < a.size() ? a[i] : 0;
        const long bv = i < b.size() ? b[i] : 0;
        if (av < bv) return -1;
        if (av > bv) return 1;
    }
    return 0;
}

// --- lenient JSON accessors (mirror QJsonValue::to*(default)) ----------------
std::string jStr(const json& obj, const char* key) {
    auto it = obj.find(key);
    if (it != obj.end() && it->is_string()) return it->get<std::string>();
    return {};
}
int jInt(const json& obj, const char* key, int def) {
    auto it = obj.find(key);
    if (it != obj.end() && it->is_number()) return it->get<int>();
    return def;
}
std::int64_t jInt64(const json& obj, const char* key, std::int64_t def) {
    auto it = obj.find(key);
    if (it == obj.end()) return def;
    if (it->is_number_integer() || it->is_number_unsigned()) return it->get<std::int64_t>();
    if (it->is_number_float()) return static_cast<std::int64_t>(it->get<double>());
    if (it->is_string()) {
        try { return std::stoll(it->get<std::string>()); } catch (...) { return def; }
    }
    return def;
}

std::optional<std::vector<uint8_t>> readFileBytes(const std::string& path, std::string* errorOut) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (errorOut) *errorOut = "Failed to open " + path;
        return std::nullopt;
    }
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool writeFileBytesAtomic(const std::string& path, const std::vector<uint8_t>& bytes, std::string* errorOut) {
    std::error_code ec;
    const fs::path p(path);
    if (p.has_parent_path()) {
        fs::create_directories(p.parent_path(), ec);
    }
    const fs::path tmp = fs::path(path + ".tmp");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            if (errorOut) *errorOut = "Failed to open " + tmp.string() + " for write";
            return false;
        }
        if (!bytes.empty()) {
            f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        if (!f) {
            if (errorOut) *errorOut = "Failed to write " + tmp.string();
            return false;
        }
    }
    fs::rename(tmp, p, ec);
    if (ec) {
        // Cross-device or other rename failure: fall back to copy+remove.
        fs::copy_file(tmp, p, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp, ec);
        if (ec) {
            if (errorOut) *errorOut = "Failed to commit " + path + ": " + ec.message();
            return false;
        }
    }
    return true;
}

std::string fileSha256(const std::string& path, std::string* errorOut) {
    std::string err;
    const std::string hex = backend::processing::processingCoreFileSha256(fs::path(path), &err);
    if (hex.empty() && errorOut) *errorOut = err.empty() ? ("Failed to hash " + path) : err;
    return hex;
}

std::string bytesSha256(const std::vector<uint8_t>& bytes) {
    return backend::processing::processingCoreBytesSha256(bytes.data(), bytes.size());
}
} // namespace

namespace backend {

namespace {
json toCacheJson(const EModulusLutCatalog::LocalMetadata& m) {
    json cache;
    cache["schema_version"] = m.schemaVersion;
    cache["source_type"] = m.sourceType;
    cache["lut_id"] = m.lutId;
    cache["display_name"] = m.displayName;
    cache["revision"] = m.revision;
    cache["manifest_url"] = m.manifestUrl;
    cache["download_url"] = m.downloadUrl;
    cache["sha256"] = m.sha256;
    cache["checksum_status"] = m.checksumStatus;
    cache["local_path"] = m.localPath;
    cache["size_bytes"] = m.sizeBytes;
    cache["published_at"] = m.publishedAt.empty() ? json(nullptr) : json(m.publishedAt);
    cache["last_checked_utc"] = m.lastCheckedUtc.empty() ? json(nullptr) : json(m.lastCheckedUtc);
    cache["last_updated_utc"] = m.lastUpdatedUtc.empty() ? json(nullptr) : json(m.lastUpdatedUtc);
    return cache;
}

std::optional<EModulusLutCatalog::LocalMetadata> metadataFromCacheJson(const json& obj) {
    EModulusLutCatalog::LocalMetadata m;
    m.schemaVersion = jInt(obj, "schema_version", 0);
    m.sourceType = jStr(obj, "source_type");
    m.lutId = jStr(obj, "lut_id");
    m.displayName = jStr(obj, "display_name");
    m.revision = jStr(obj, "revision");
    m.manifestUrl = jStr(obj, "manifest_url");
    m.downloadUrl = jStr(obj, "download_url");
    m.sha256 = toLower(trim(jStr(obj, "sha256")));
    m.checksumStatus = jStr(obj, "checksum_status");
    m.localPath = jStr(obj, "local_path");
    m.sizeBytes = jInt64(obj, "size_bytes", 0);
    m.publishedAt = trim(jStr(obj, "published_at"));
    m.lastCheckedUtc = trim(jStr(obj, "last_checked_utc"));
    m.lastUpdatedUtc = trim(jStr(obj, "last_updated_utc"));
    if (m.schemaVersion <= 0) return std::nullopt;
    return m;
}

bool writeMetadataFile(const std::string& path, const EModulusLutCatalog::LocalMetadata& m, std::string* errorOut) {
    const std::string payload = toCacheJson(m).dump(2);
    const std::vector<uint8_t> bytes(payload.begin(), payload.end());
    return writeFileBytesAtomic(path, bytes, errorOut);
}

std::optional<EModulusLutCatalog::LocalMetadata> readMetadataFile(const std::string& path, std::string* errorOut) {
    const auto bytes = readFileBytes(path, errorOut);
    if (!bytes.has_value()) return std::nullopt;
    json doc = json::parse(std::string(bytes->begin(), bytes->end()), nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        if (errorOut) *errorOut = "Invalid LUT metadata JSON in " + path;
        return std::nullopt;
    }
    return metadataFromCacheJson(doc);
}
} // namespace

EModulusLutCatalog::EModulusLutCatalog(HttpGetFn httpGet, std::string appVersion, std::string appDataDir)
    : httpGet_(std::move(httpGet)), appVersion_(std::move(appVersion)), appDataDir_(std::move(appDataDir)) {}

std::string EModulusLutCatalog::defaultManifestUrl(const std::string& channel) {
    const std::string trimmed = trim(channel).empty() ? std::string(kDefaultChannel) : trim(channel);
    return "https://updates.yofo.bio/" + trimmed + "/emodulus-lut/latest.json";
}

std::string EModulusLutCatalog::manifestUrlFromEnvOrDefault(const std::string& channel) const {
    const std::string override = trim(getenvStr(kLutManifestEnv));
    if (!override.empty()) {
        const std::string scheme = urlScheme(override);
        if (scheme == "https" || scheme == "http" || scheme == "file") {
            return override;
        }
        SPDLOG_WARN("EModulusLutCatalog: ignoring invalid {}='{}'", kLutManifestEnv, override);
    }
    return defaultManifestUrl(channel);
}

std::string EModulusLutCatalog::cacheDir() const {
    const std::string override = trim(getenvStr(kLutCacheDirEnv));
    if (!override.empty()) {
        return fs::absolute(fs::path(override)).string();
    }
    std::string base = trim(appDataDir_);
    if (base.empty()) {
        // Fallback when no shell-provided app-data dir (backend-only/headless):
        // best-effort platform location. Production always injects appDataDir.
#if defined(_WIN32)
        base = getenvStr("LOCALAPPDATA");
#elif defined(__APPLE__)
        base = getenvStr("HOME");
        if (!base.empty()) base += "/Library/Application Support";
#else
        base = getenvStr("XDG_DATA_HOME");
        if (base.empty()) {
            const std::string home = getenvStr("HOME");
            if (!home.empty()) base = home + "/.local/share";
        }
#endif
        if (base.empty()) base = fs::current_path().string();
    }
    return (fs::path(base) / "isoelastic_curve").string();
}

std::string EModulusLutCatalog::lutPath() const {
    return (fs::path(cacheDir()) / kLutFileName).string();
}

std::string EModulusLutCatalog::metadataPath() const {
    return (fs::path(cacheDir()) / (std::string(kLutFileName) + ".meta.json")).string();
}

bool EModulusLutCatalog::isCompatibleWithCurrentApp(const Manifest& manifest) const {
    const std::vector<long> current = parseVersion(appVersion_);
    if (current.empty()) return true; // permissive (matches null QVersionNumber)

    if (!trim(manifest.appMinVersion).empty()) {
        const auto minV = parseVersion(manifest.appMinVersion);
        if (!minV.empty() && compareVersion(current, minV) < 0) return false;
    }
    if (!trim(manifest.appMaxVersion).empty()) {
        const auto maxV = parseVersion(manifest.appMaxVersion);
        if (!maxV.empty() && compareVersion(current, maxV) > 0) return false;
    }
    return true;
}

std::optional<std::vector<uint8_t>> EModulusLutCatalog::readUrlBytes(const std::string& url, std::string* errorOut) const {
    if (url.empty()) {
        if (errorOut) *errorOut = "Invalid URL: (empty)";
        return std::nullopt;
    }
    const std::string scheme = urlScheme(url);
    if (scheme == "file") {
        return readFileBytes(fileUrlToPath(url), errorOut);
    }
    if (scheme.empty()) {
        // Treat as a local path.
        return readFileBytes(url, errorOut);
    }
    if (!httpGet_) {
        if (errorOut) *errorOut = "No HTTP fetcher available for " + url;
        return std::nullopt;
    }
    const HttpGetResult res = httpGet_(url, kNetworkTimeoutMs);
    if (!res.ok) {
        if (errorOut) {
            std::string detail = res.error;
            if (res.statusCode > 0) detail += " (HTTP " + std::to_string(res.statusCode) + ")";
            if (!res.body.empty()) {
                const size_t n = std::min<size_t>(res.body.size(), 500);
                detail += "\n" + std::string(res.body.begin(), res.body.begin() + n);
            }
            *errorOut = detail;
        }
        return std::nullopt;
    }
    return res.body;
}

std::optional<EModulusLutCatalog::Manifest> EModulusLutCatalog::parseManifest(const std::vector<uint8_t>& bytes, std::string* errorOut) const {
    json doc = json::parse(std::string(bytes.begin(), bytes.end()), nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        if (errorOut) *errorOut = "Invalid LUT manifest JSON.";
        return std::nullopt;
    }

    Manifest m;
    m.manifestSchemaVersion = jInt(doc, "manifest_schema_version", 0);
    m.lutId = trim(jStr(doc, "lut_id"));
    m.displayName = jStr(doc, "display_name");
    m.revision = trim(jStr(doc, "revision"));
    m.downloadUrl = jStr(doc, "download_url");
    m.sha256 = toLower(trim(jStr(doc, "sha256")));
    m.sizeBytes = jInt64(doc, "size_bytes", 0);
    m.publishedAt = trim(jStr(doc, "published_at"));
    m.appMinVersion = jStr(doc, "app_min_version");
    m.appMaxVersion = jStr(doc, "app_max_version");

    if (m.manifestSchemaVersion <= 0) {
        if (errorOut) *errorOut = "Manifest is missing manifest_schema_version.";
        return std::nullopt;
    }
    if (m.lutId.empty()) {
        if (errorOut) *errorOut = "Manifest is missing lut_id.";
        return std::nullopt;
    }
    const std::string dlScheme = urlScheme(m.downloadUrl);
    if (dlScheme != "https" && dlScheme != "http" && dlScheme != "file") {
        if (errorOut) *errorOut = "Manifest download_url must be HTTP(S) or file://.";
        return std::nullopt;
    }
    if (m.revision.empty()) {
        if (errorOut) *errorOut = "Manifest is missing revision.";
        return std::nullopt;
    }
    if (m.sha256.size() != 64) {
        if (errorOut) *errorOut = "Manifest sha256 must be a 64-character hex string.";
        return std::nullopt;
    }
    if (!isCompatibleWithCurrentApp(m)) {
        if (errorOut) *errorOut = "Manifest is incompatible with this app version (" + appVersion_ + ").";
        return std::nullopt;
    }
    return m;
}

std::optional<EModulusLutCatalog::Manifest> EModulusLutCatalog::fetchManifest(const std::string& url, std::string* errorOut) const {
    const auto bytes = readUrlBytes(url, errorOut);
    if (!bytes.has_value()) return std::nullopt;
    return parseManifest(*bytes, errorOut);
}

EModulusLutCatalog::LocalMetadata EModulusLutCatalog::metadataFromManifest(const Manifest& manifest,
                                                                           const std::string& sourceType,
                                                                           const std::string& localPath,
                                                                           const std::string& checksumStatus) const {
    LocalMetadata m;
    m.schemaVersion = 1;
    m.sourceType = sourceType;
    m.lutId = manifest.lutId;
    m.displayName = manifest.displayName;
    m.revision = manifest.revision;
    m.manifestUrl.clear();
    m.downloadUrl = manifest.downloadUrl;
    m.sha256 = manifest.sha256;
    m.checksumStatus = checksumStatus;
    m.localPath = localPath;
    m.publishedAt = manifest.publishedAt;
    m.lastCheckedUtc = isoNowUtc();
    m.lastUpdatedUtc = isoNowUtc();
    m.sizeBytes = manifest.sizeBytes;
    return m;
}

bool EModulusLutCatalog::ensureManagedLut(const std::string& bundledPath,
                                          std::string* resolvedPathOut,
                                          ManagedLutInfo* infoOut,
                                          std::string* errorOut) const {
    const std::string cache = cacheDir();
    std::error_code ec;
    fs::create_directories(fs::path(cache), ec);
    if (ec && !fs::exists(fs::path(cache))) {
        if (errorOut) *errorOut = "Failed to create LUT cache directory: " + cache;
        return false;
    }

    const std::string managedPath = lutPath();
    const std::string metaPath = metadataPath();
    ManagedLutInfo info;
    info.sourceType = "bundled-fallback";
    info.localPath = managedPath;

    std::string localSha;
    if (fs::exists(fs::path(managedPath))) {
        std::string localReadError;
        const std::string sha = fileSha256(managedPath, &localReadError);
        if (!sha.empty()) {
            localSha = sha;
            info.checksumStatus = "verified";
        } else {
            info.checksumStatus = "unknown";
            if (!localReadError.empty()) {
                SPDLOG_WARN("EModulusLutCatalog: could not hash local cache {}: {}", managedPath, localReadError);
            }
        }
    }

    std::optional<LocalMetadata> cachedMetadata = readMetadataFile(metaPath, nullptr);
    if (cachedMetadata.has_value()) {
        info.sourceType = cachedMetadata->sourceType.empty() ? info.sourceType : cachedMetadata->sourceType;
        info.lutId = cachedMetadata->lutId;
        info.displayName = cachedMetadata->displayName;
        info.revision = cachedMetadata->revision;
        info.manifestUrl = cachedMetadata->manifestUrl;
        info.downloadUrl = cachedMetadata->downloadUrl;
        info.sha256 = cachedMetadata->sha256;
        info.checksumStatus = cachedMetadata->checksumStatus.empty() ? info.checksumStatus : cachedMetadata->checksumStatus;
        info.publishedAt = cachedMetadata->publishedAt;
        info.sizeBytes = cachedMetadata->sizeBytes;
    }

    const std::string manifestUrl = manifestUrlFromEnvOrDefault();
    info.manifestUrl = manifestUrl;

    std::optional<Manifest> manifest;
    // Remote fetch needs either an injected HTTP fetcher or a file:// URL (used
    // by tests and headless). Without a fetcher, http(s) URLs are skipped and
    // the catalog falls back to cache/bundled.
    const bool allowRemoteFetch = static_cast<bool>(httpGet_) || urlScheme(manifestUrl) == "file";
    if (allowRemoteFetch) {
        manifest = fetchManifest(manifestUrl, nullptr);
    } else {
        SPDLOG_INFO("EModulusLutCatalog: skipping remote LUT fetch because no HTTP fetcher is set");
    }

    if (manifest.has_value()) {
        info.lutId = manifest->lutId;
        info.displayName = manifest->displayName;
        info.revision = manifest->revision;
        info.downloadUrl = manifest->downloadUrl;
        info.sha256 = manifest->sha256;
        info.sizeBytes = manifest->sizeBytes;
        info.publishedAt = manifest->publishedAt;

        const bool localMatchesRemote = !localSha.empty() && toLower(localSha) == toLower(manifest->sha256);
        const bool needsRefresh = !fs::exists(fs::path(managedPath)) || !localMatchesRemote;
        if (needsRefresh) {
            std::string downloadError;
            const auto remoteBytes = readUrlBytes(manifest->downloadUrl, &downloadError);
            if (!remoteBytes.has_value()) {
                info.note = "remote download failed: " + downloadError;
                SPDLOG_WARN("EModulusLutCatalog: {}", info.note);
            } else {
                const std::string remoteSha = bytesSha256(*remoteBytes);
                const bool shaOk = toLower(remoteSha) == toLower(manifest->sha256);
                const bool sizeOk = manifest->sizeBytes < 0 ||
                                    manifest->sizeBytes == static_cast<std::int64_t>(remoteBytes->size());
                if (shaOk && sizeOk) {
                    if (!writeFileBytesAtomic(managedPath, *remoteBytes, errorOut)) {
                        return false;
                    }
                    const LocalMetadata metadata = metadataFromManifest(*manifest, "r2-public-catalog", managedPath, "verified");
                    if (!writeMetadataFile(metaPath, metadata, errorOut)) {
                        return false;
                    }
                    info.sourceType = "r2-public-catalog";
                    info.remoteUpdated = true;
                    info.checksumStatus = "verified";
                    info.localPath = managedPath;
                    info.note = "remote LUT updated";
                    localSha = remoteSha;
                    SPDLOG_INFO("EModulusLutCatalog: updated LUT '{}' revision '{}' from {}",
                                manifest->lutId, manifest->revision, manifestUrl);
                } else {
                    info.note = "remote checksum/size mismatch";
                    SPDLOG_WARN("EModulusLutCatalog: remote LUT checksum/size mismatch for '{}' (sha_ok={}, size_ok={})",
                                manifest->lutId, shaOk, sizeOk);
                }
            }
        }
    } else if (fs::exists(fs::path(managedPath))) {
        info.note = "manifest fetch failed; using local cache";
        SPDLOG_WARN("EModulusLutCatalog: {}: {}", manifestUrl, info.note);
    } else {
        info.note = "manifest fetch failed and no local cache exists";
        SPDLOG_WARN("EModulusLutCatalog: {}", info.note);
    }

    if (fs::exists(fs::path(managedPath))) {
        if (info.lutId.empty()) info.lutId = kLutFileName;
        if (info.displayName.empty()) info.displayName = "Young's modulus LUT";
        if (info.revision.empty()) info.revision = "local-cache";
        if (info.checksumStatus.empty()) info.checksumStatus = localSha.empty() ? "unknown" : "verified";
        info.sourceType = info.remoteUpdated ? std::string("r2-public-catalog")
                          : (cachedMetadata.has_value() ? cachedMetadata->sourceType : std::string("local-cache"));
        info.localPath = managedPath;
        if (resolvedPathOut) *resolvedPathOut = managedPath;
        if (infoOut) *infoOut = info;
        return true;
    }

    if (!fs::exists(fs::path(bundledPath))) {
        if (errorOut) *errorOut = "Bundled LUT not found: " + bundledPath;
        return false;
    }

    std::string copyError;
    const auto bundledBytes = readFileBytes(bundledPath, &copyError);
    if (!bundledBytes.has_value()) {
        if (errorOut) *errorOut = copyError;
        return false;
    }
    if (!writeFileBytesAtomic(managedPath, *bundledBytes, errorOut)) {
        if (resolvedPathOut) *resolvedPathOut = bundledPath;
        info.sourceType = "bundled-fallback";
        info.usedBundledFallback = true;
        info.note = "using bundled LUT directly because local cache could not be written";
        if (infoOut) *infoOut = info;
        return true;
    }

    const std::string bundledSha = bytesSha256(*bundledBytes);
    Manifest bundledManifest;
    bundledManifest.manifestSchemaVersion = 1;
    bundledManifest.lutId = kLutFileName;
    bundledManifest.displayName = "Bundled LUT";
    bundledManifest.revision = "bundled";
    bundledManifest.downloadUrl = "file://" + bundledPath;
    bundledManifest.sha256 = bundledSha;
    bundledManifest.sizeBytes = static_cast<std::int64_t>(bundledBytes->size());
    const LocalMetadata metadata = metadataFromManifest(bundledManifest, "bundled-fallback", managedPath, "verified");
    if (!writeMetadataFile(metaPath, metadata, nullptr)) {
        SPDLOG_WARN("EModulusLutCatalog: failed to write LUT metadata at {}", metaPath);
    }

    info.sourceType = "bundled-fallback";
    info.usedBundledFallback = true;
    info.lutId = kLutFileName;
    info.displayName = "Young's modulus LUT";
    info.revision = "bundled";
    info.localPath = managedPath;
    info.sha256 = bundledSha;
    info.checksumStatus = "verified";
    info.note = "seeded local cache from bundled LUT";
    if (resolvedPathOut) *resolvedPathOut = managedPath;
    if (infoOut) *infoOut = info;
    SPDLOG_INFO("EModulusLutCatalog: seeded local LUT cache from bundled copy at {}", managedPath);
    return true;
}

} // namespace backend
