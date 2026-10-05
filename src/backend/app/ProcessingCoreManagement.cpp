#include "backend/app/ProcessingCoreManagement.h"
#include "backend/app/ProcessingCoreTrust.h"
#include "backend/app/AppBackend.h"
#include "backend/processing/ProcessingCoreCache.h"
#include "backend/processing/ProcessingCoreAbi.h"
#include "backend/processing/ProcessingService.h"
#include "backend/services/CaptureService.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <atomic>
#ifdef _WIN32
#include <windows.h>
#endif
namespace backend::app {
namespace {
using J = nlohmann::json;
namespace fs = std::filesystem;
std::string os() {
#ifdef _WIN32
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}
std::string arch() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#else
    return "unsupported";
#endif
}
std::vector<unsigned> version(const std::string& t) {
    std::vector<unsigned> v;
    std::istringstream stream(t);
    std::string p;
    while (std::getline(stream, p, '.')) {
        if (p.empty() || p.size() > 9 ||
            !std::all_of(p.begin(), p.end(), [](unsigned char c) { return std::isdigit(c); }))
            throw std::runtime_error("Invalid app compatibility version");
        v.push_back(std::stoul(p));
    }
    if (v.empty() || v.size() > 4) throw std::runtime_error("Invalid app compatibility version");
    v.resize(4);
    return v;
}
std::string optionalString(const J& j, const char* k) {
    return !j.contains(k) || j.at(k).is_null() ? "" : j.at(k).get<std::string>();
}
J native(const J& v) {
    J found;
    for (const auto& p : v.at("native_plugins")) {
        auto a = p.at("arch").get<std::string>();
        if (a == "amd64" || a == "x64") a = "x86_64";
        if (a == "arm64") a = "aarch64";
        if (p.at("os") == os() && a == arch()) {
            if (!found.is_null()) throw std::runtime_error("Duplicate native platform in manifest");
            found = p;
        }
    }
    if (found.is_null()) throw std::runtime_error("No native artifact for this platform");
    return found;
}
void persist(const fs::path& path, const J& value, std::string& error) {
    static std::atomic<unsigned> n{0};
    auto tmp = path;
    tmp += ".tmp." + std::to_string(++n);
    struct Cleanup {
        fs::path p;
        ~Cleanup() {
            std::error_code ec;
            fs::remove(p, ec);
        }
    } cleanup{tmp};
    try {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        auto bytes = value.dump(2);
        f.write(bytes.data(), bytes.size());
        f.flush();
        if (!f) throw std::runtime_error("Cannot write core selection");
        f.close();
        if (!f) throw std::runtime_error("Cannot close core selection");
#ifdef _WIN32
        if (!MoveFileExW(tmp.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot commit core selection");
#else
        fs::rename(tmp, path);
#endif
    } catch (const std::exception& e) {
        error = e.what();
    }
}
} // namespace
std::string processingCoreCommand(AppBackend& backend, const std::string& rootString,
                                  const std::string& request) {
    J result = {{"ok", false}};
    bool restoring = false;
    try {
        if (request.size() > 4 * 1024 * 1024) throw std::runtime_error("Core request too large");
        auto q = J::parse(request);
        auto action = q.at("operation").get<std::string>();
        auto& processing = backend.processing();
        const auto host = processing::bundledProcessingCoreIdentity();
        const auto cache = fs::absolute(rootString);
        const auto selection = cache / "selection.json";
        if (action == "info") {
            result.update({{"ok", true},
                           {"os", os()},
                           {"arch", arch()},
                           {"app_version", MIB_STUDIO_QT_VERSION},
                           {"runtime_fingerprint", host.runtimeFingerprint},
                           {"bundled_version", host.version},
                           {"active_version", processing.activeProcessingCoreIdentity().version},
                           {"required_version", processing.requiredProcessingCoreVersion()},
                           {"pin_satisfied", processing.isProcessingCorePinSatisfied()}});
            return result.dump();
        }
        if (backend.capture().isRunning() || backend.isFrameRecording() ||
            processing.isRealtimeRunning())
            throw std::runtime_error("Stop capture, realtime and recording before core changes");
        if (action == "restore") {
            restoring = true;
            if (!fs::exists(selection)) {
                result["ok"] = true;
                result["restored"] = false;
                return result.dump();
            }
            if (fs::file_size(selection) > 4 * 1024 * 1024)
                throw std::runtime_error("Saved core selection exceeds size limit");
            std::ifstream f(selection);
            if (!f) throw std::runtime_error("Cannot read saved core selection");
            f >> q;
            action = q.at("operation").get<std::string>();
        }
        fs::create_directories(cache);
        if (action == "bundled") {
            std::string error;
            auto kernel = processing::makeBundledProcessingKernel();
            const bool activated =
                processing.activateProcessingKernel(kernel, &error, [&](std::string& e) {
                    persist(selection, J{{"operation", "bundled"}}, e);
                    return e.empty();
                });
            if (!activated) throw std::runtime_error(error);
            result["ok"] = true;
            result["active_version"] = kernel->identity().version;
            return result.dump();
        }
        if (action != "activate_local") throw std::runtime_error("Unknown core operation");
        const auto manifestBytes = q.at("manifest_json").get<std::string>();
        const auto manifest = J::parse(manifestBytes);
        const auto entry = q.at("entry");
        if (manifest.at("processing_core_manifest_schema_version") != 2 ||
            manifest.at("version") != entry.at("version") ||
            manifest.at("contract_version") != entry.at("contract_version") ||
            manifest.at("channel") != q.at("channel") ||
            manifest.at("wheel").at("version") != manifest.at("version") ||
            manifest.at("wheel").at("release_tag") != entry.at("release_tag"))
            throw std::runtime_error("Index and immutable core manifest disagree");
        const auto plugin = native(manifest);
        if (plugin != native(entry))
            throw std::runtime_error("Index and manifest artifact metadata disagree");
        if (plugin.at("entrypoint") != "mib_processing_get_api" ||
            plugin.at("engine_abi_version") != MIB_PROCESSING_ENGINE_ABI_VERSION ||
            plugin.at("contract_version") != MIB_PROCESSING_CONTRACT_VERSION ||
            plugin.at("runtime_fingerprint") != host.runtimeFingerprint)
            throw std::runtime_error(
                "Core ABI, processing contract or runtime fingerprint mismatch");
        const auto current = version(MIB_STUDIO_QT_VERSION),
                   minimum = version(plugin.at("app_min_version").get<std::string>());
        const auto maximum = optionalString(plugin, "app_max_version");
        if (current < minimum || (!maximum.empty() && current > version(maximum)))
            throw std::runtime_error("Core app compatibility bounds exclude this app");
        const auto url = plugin.at("url").get<std::string>();
        if (url.rfind("https://", 0) != 0)
            throw std::runtime_error("Core artifact URL must use HTTPS");
        const auto source = fs::absolute(q.at("source_path").get<std::string>());
        const auto expectedSize = plugin.at("size_bytes").get<uint64_t>();
        if (!expectedSize || expectedSize > 512 * 1024 * 1024 || !fs::is_regular_file(source) ||
            fs::file_size(source) != expectedSize)
            throw std::runtime_error("Core artifact size mismatch");
        processing::ProcessingCoreCacheRequest cr{
            source, cache, manifest.at("version").get<std::string>(),
            plugin.at("sha256").get<std::string>(), plugin.at("filename").get<std::string>()};
        const auto prepared = processing::prepareProcessingCoreArtifact(cr);
        if (!prepared) throw std::runtime_error(prepared.error);
        const auto signing = plugin.at("signing");
        ProcessingCoreSignaturePolicy policy{
            signing.at("required").get<bool>(),
            signing.value("scheme", signing.value("format", std::string())),
            optionalString(signing, "public_key_spki_base64"),
            optionalString(signing, "signature_base64")};
        processing::ProcessingCoreLoadRequirements r;
        r.expectedVersion = cr.version;
        r.expectedContractVersion = MIB_PROCESSING_CONTRACT_VERSION;
        r.expectedEngineAbiVersion = MIB_PROCESSING_ENGINE_ABI_VERSION;
        r.expectedRuntimeFingerprint = host.runtimeFingerprint;
        r.artifactSha256 = cr.sha256;
        r.releaseTag = entry.at("release_tag").get<std::string>();
        r.manifestSha256 = processing::processingCoreBytesSha256(
            reinterpret_cast<const uint8_t*>(manifestBytes.data()), manifestBytes.size());
        r.trustVerifier = processingCoreTrustVerifier(policy);
        const auto loaded = processing::loadProcessingCorePlugin(prepared.pluginPath, r);
        if (!loaded) throw std::runtime_error(loaded.error);
        q["source_path"] = prepared.pluginPath.string();
        std::string error;
        if (!processing.activateProcessingKernel(loaded.kernel, &error, [&](std::string& e) {
                persist(selection, q, e);
                return e.empty();
            }))
            throw std::runtime_error(error);
        result.update({{"ok", true},
                       {"active_version", loaded.kernel->identity().version},
                       {"restored", restoring}});
    } catch (const std::exception& e) {
        if (restoring) backend.processing().markProcessingCoreSelectionUnavailable();
        result["error"] = e.what();
    }
    return result.dump();
}
} // namespace backend::app
