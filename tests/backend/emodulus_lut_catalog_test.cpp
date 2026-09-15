// emodulus_lut_catalog_test
//
// Exercises the Qt-free EModulusLutCatalog (epic #246, ADR 0002): the
// update/verify/cache/fallback state machine, driven entirely through file://
// URLs so no HTTP fetcher is needed. Verifies a remote update is downloaded,
// verified, cached at the managed path, and loadable; then that a broken
// manifest degrades gracefully to the last-known-good local copy.

#include "backend/processing/EModulusLut.h"
#include "backend/processing/EModulusLutCatalog.h"
#include "backend/processing/ProcessingCoreLoader.h" // processingCoreBytesSha256

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

bool writeTextFile(const fs::path& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        std::cerr << "Failed to open " << path.string() << " for write\n";
        return false;
    }
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(f);
}

std::string sha256Hex(const std::string& bytes) {
    return backend::processing::processingCoreBytesSha256(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
}

std::string fileUrl(const fs::path& absPath) {
    return "file://" + absPath.generic_string();
}

fs::path makeTempRoot() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<unsigned long long> dist;
    for (int attempt = 0; attempt < 100; ++attempt) {
        const auto p = fs::temp_directory_path() / ("mib_emodulus_lut_test_" + std::to_string(dist(gen)));
        std::error_code ec;
        if (fs::create_directories(p, ec)) return p;
    }
    return fs::temp_directory_path() / "mib_emodulus_lut_test_fallback";
}

} // namespace

int main() {
    const fs::path root = makeTempRoot();
    const fs::path cacheDir = root / "cache";
    const fs::path bundledDir = root / "bundled" / "isoelastic_curve";
    const fs::path remoteDir = root / "remote";
    std::error_code ec;
    fs::create_directories(cacheDir, ec);
    fs::create_directories(bundledDir, ec);
    fs::create_directories(remoteDir, ec);

    setEnv("MIB_STUDIO_EMODULUS_LUT_CACHE_DIR", cacheDir.string());

    const fs::path bundledPath = bundledDir / "scaled_isoelastic_data_LUT_6.16-4.24.txt";
    // Minimal non-degenerate LUT (2x2 spread, uniform value).
    const std::string bundledBytes =
        "5.0\t0.1\t12.5\n5.0\t0.3\t12.5\n15.0\t0.1\t12.5\n15.0\t0.3\t12.5\n";
    if (!writeTextFile(bundledPath, bundledBytes)) return 1;

    const fs::path remotePath = remoteDir / "scaled_isoelastic_data_LUT_6.16-4.24.txt";
    const std::string remoteBytes =
        "5.0\t0.1\t42.0\n5.0\t0.3\t42.0\n15.0\t0.1\t42.0\n15.0\t0.3\t42.0\n";
    if (!writeTextFile(remotePath, remoteBytes)) return 2;

    const fs::path manifestPath = remoteDir / "latest.json";
    nlohmann::json manifest;
    manifest["manifest_schema_version"] = 1;
    manifest["lut_id"] = "scaled_isoelastic_data_LUT_6.16-4.24";
    manifest["display_name"] = "Scaled Isoelastic LUT";
    manifest["revision"] = "2026.06.11-1";
    manifest["download_url"] = fileUrl(remotePath);
    manifest["sha256"] = sha256Hex(remoteBytes);
    manifest["size_bytes"] = static_cast<std::int64_t>(remoteBytes.size());
    manifest["published_at"] = "2026-06-11T00:00:00.000Z";
    manifest["app_min_version"] = "0.1.0";
    if (!writeTextFile(manifestPath, manifest.dump(2))) return 3;

    setEnv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", fileUrl(manifestPath));

    // appVersion 1.0.0 satisfies app_min_version 0.1.0; no HTTP fetcher needed
    // because every URL is file://.
    backend::EModulusLutCatalog catalog(nullptr, "1.0.0");
    std::string resolvedPath;
    backend::EModulusLutCatalog::ManagedLutInfo info;
    std::string error;
    if (!catalog.ensureManagedLut(bundledPath.string(), &resolvedPath, &info, &error)) {
        std::cerr << "ensureManagedLut failed: " << error << '\n';
        return 4;
    }

    const std::string managedPath = catalog.lutPath();
    if (resolvedPath != managedPath) {
        std::cerr << "Expected managed path " << managedPath << " but got " << resolvedPath << '\n';
        return 5;
    }
    if (!fs::exists(fs::path(managedPath))) {
        std::cerr << "Managed LUT file missing: " << managedPath << '\n';
        return 6;
    }
    if (!info.remoteUpdated) {
        std::cerr << "Expected remoteUpdated to be true\n";
        return 7;
    }
    if (info.checksumStatus != "verified") {
        std::cerr << "Expected checksum_status verified, got " << info.checksumStatus << '\n';
        return 14;
    }

    backend::EModulusLut lut;
    if (!lut.loadFromFile(managedPath)) {
        std::cerr << "Failed to load managed LUT\n";
        return 8;
    }
    const double stiffness = lut.lookup(10.0, 0.2);
    if (std::abs(stiffness - 42.0) > 1e-9) {
        std::cerr << "Unexpected LUT value: " << stiffness << '\n';
        return 9;
    }

    // Break the manifest; the last-known-good local copy must be retained.
    const fs::path badManifestPath = remoteDir / "broken.json";
    if (!writeTextFile(badManifestPath, "{not-json")) return 10;
    setEnv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", fileUrl(badManifestPath));

    std::string fallbackResolvedPath;
    backend::EModulusLutCatalog::ManagedLutInfo fallbackInfo;
    std::string fallbackError;
    if (!catalog.ensureManagedLut(bundledPath.string(), &fallbackResolvedPath, &fallbackInfo, &fallbackError)) {
        std::cerr << "Fallback ensureManagedLut failed: " << fallbackError << '\n';
        return 11;
    }
    if (fallbackResolvedPath != managedPath) {
        std::cerr << "Fallback resolved path changed unexpectedly\n";
        return 12;
    }
    if (!fs::exists(fs::path(managedPath))) {
        std::cerr << "Managed LUT disappeared after fallback\n";
        return 13;
    }

    fs::remove_all(root, ec);
    std::cout << "EModulusLutCatalog Qt-free update/verify/fallback verified\n";
    return 0;
}
