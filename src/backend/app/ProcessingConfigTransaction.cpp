#include "backend/app/ProcessingConfigTransaction.h"
#include "backend/app/AppBackend.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/processing/ProcessingConfigJson.h"
#include "backend/processing/ProcessingService.h"
#include "backend/processing/ProcessingCoreLoader.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <atomic>
#include <stdexcept>
#include <limits>
#include <cmath>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#include <fcntl.h>
#endif
namespace backend::app {
namespace {
using Json = nlohmann::json;
std::string hash(const std::string& s) {
    return processing::processingCoreBytesSha256(reinterpret_cast<const uint8_t*>(s.data()),
                                                 s.size());
}
void checkPatch(const Json& patch, const Json& schema) {
    if (!patch.is_object() || patch.empty())
        throw std::runtime_error("patch must be a nonempty object");
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        if (!schema.contains(it.key()))
            throw std::runtime_error("unsupported processing key: " + it.key());
        const auto& expected = schema.at(it.key());
        if (it.value().is_number()) {
            const double value = it.value().get<double>();
            if (!std::isfinite(value) ||
                (expected.is_number_integer() && (value < std::numeric_limits<int>::min() ||
                                                  value > std::numeric_limits<int>::max())))
                throw std::runtime_error("processing value outside representable range: " +
                                         it.key());
        }
        if (expected.is_object())
            checkPatch(it.value(), expected);
        else if (expected.is_boolean()          ? !it.value().is_boolean()
                 : expected.is_number_integer() ? !it.value().is_number_integer()
                                                : !it.value().is_number())
            throw std::runtime_error("invalid processing value: " + it.key());
    }
}
// Validate known persisted keys too; unknown additive keys are retained.
void checkDocument(const Json& document, const Json& schema) {
    if (!document.is_object()) throw std::runtime_error("processing section must be an object");
    for (auto it = document.begin(); it != document.end(); ++it) {
        if (!schema.contains(it.key())) continue;
        if (schema.at(it.key()).is_object())
            checkDocument(it.value(), schema.at(it.key()));
        else
            checkPatch(Json{{it.key(), it.value()}}, schema);
    }
}
// Same merged-range rules as the Qt tune draft, plus kernel preconditions.
void validate(const services::ProcessingConfig& c) {
    if (c.gaussian_blur_size < 1 || c.gaussian_blur_size % 2 == 0 || c.morph_kernel_size < 1 ||
        c.morph_iterations < 0 || c.multi_image_count < 1 ||
        (c.enable_area_range_check && c.area_threshold_min > c.area_threshold_max) ||
        (c.enable_deformability_range_check &&
         c.deformability_threshold_min > c.deformability_threshold_max) ||
        (c.enable_ring_ratio_check && c.ring_ratio_min > c.ring_ratio_max) ||
        (c.enable_target_group &&
         (c.target_group_area_min > c.target_group_area_max ||
          c.target_group_deformability_min > c.target_group_deformability_max)))
        throw std::runtime_error("invalid merged processing ranges or kernel settings");
}
void save(const std::filesystem::path& path, const std::string& bytes, bool& saved) {
    static std::atomic<unsigned long long> counter{0};
    auto temp = path;
#ifdef _WIN32
    const auto pid = GetCurrentProcessId();
#else
    const auto pid = getpid();
#endif
    temp += ".tmp." + std::to_string(pid) + "." + std::to_string(++counter);
    struct Cleanup {
        std::filesystem::path p;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove(p, ec);
        }
    } cleanup{};
#ifdef _WIN32
    FILE* f = _wfopen(temp.c_str(), L"wbx");
#else
    FILE* f = fopen(temp.c_str(), "wbx");
#endif
    if (!f) throw std::runtime_error("cannot create temporary config document");
    cleanup.p = temp;
#ifndef _WIN32
    std::error_code permissionError;
    const auto permissions = std::filesystem::status(path, permissionError).permissions();
    if (!permissionError) std::filesystem::permissions(temp, permissions, permissionError);
    if (permissionError) {
        fclose(f);
        throw std::runtime_error("cannot preserve configuration permissions");
    }
#endif
    const bool wrote = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    bool synced = fflush(f) == 0;
#ifdef _WIN32
    synced = synced && _commit(_fileno(f)) == 0;
#else
    synced = synced && fsync(fileno(f)) == 0;
#endif
    const bool closed = fclose(f) == 0;
    if (!wrote || !synced || !closed) throw std::runtime_error("config write/flush failed");
#ifdef _WIN32
    if (!MoveFileExW(temp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("config atomic replacement failed");
    saved = true;
#else
    std::filesystem::rename(temp, path);
    saved = true;
    const auto parent =
        path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
    int fd = open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) throw std::runtime_error("config saved but directory sync unavailable");
    const bool durable = fsync(fd) == 0;
    close(fd);
    if (!durable) throw std::runtime_error("config saved but directory sync failed");
#endif
}
} // namespace
ConfigDocumentSnapshot readConfigDocument(const std::string& path) {
    ConfigDocumentSnapshot r;
    r.path = path;
    try {
        if (path.empty() || !std::filesystem::is_regular_file(path))
            throw std::runtime_error("configuration document is not a regular file");
        std::ifstream f(path, std::ios::binary);
        if (!f) throw std::runtime_error("cannot read configuration document");
        constexpr std::size_t maxDocumentBytes = 4 * 1024 * 1024;
        r.documentJson.resize(maxDocumentBytes + 1);
        f.read(r.documentJson.data(), static_cast<std::streamsize>(r.documentJson.size()));
        r.documentJson.resize(static_cast<std::size_t>(f.gcount()));
        if (r.documentJson.size() > maxDocumentBytes)
            throw std::runtime_error("configuration document exceeds 4 MiB limit");
        if (f.bad()) throw std::runtime_error("configuration read failed");
        if (!Json::parse(r.documentJson).is_object())
            throw std::runtime_error("configuration root must be an object");
        r.revision = hash(r.documentJson);
        r.ok = true;
    } catch (const std::exception& e) {
        r.error = e.what();
    }
    return r;
}
ProcessingConfigTransactionResult applyProcessingConfigTransaction(AppBackend& backend,
                                                                   const std::string& path,
                                                                   const std::string& baseline,
                                                                   const std::string& patchJson) {
    ProcessingConfigTransactionResult r;
    const bool idle = backend.experiment().withIdleConfiguration([&] {
        try {
            const auto doc = readConfigDocument(path);
            if (!doc.ok) throw std::runtime_error(doc.error);
            r.revision = doc.revision;
            if (baseline.empty() || baseline != doc.revision) {
                r.conflict = true;
                throw std::runtime_error(
                    "configuration baseline missing or changed; reload before applying");
            }
            if (patchJson.size() > 64 * 1024)
                throw std::runtime_error("processing patch exceeds 64 KiB limit");
            const auto patch = Json::parse(patchJson);
            if (!patch.is_object() || patch.size() != 1 || !patch.contains("image_processing"))
                throw std::runtime_error("only image_processing patches are supported");
            auto config = backend.processing().getProcessingConfig();
            checkPatch(patch.at("image_processing"), processing::config_json::toJson(config));
            auto root = Json::parse(doc.documentJson);
            if (root.contains("image_processing") && !root.at("image_processing").is_object())
                throw std::runtime_error("existing image_processing section must be an object");
            root.merge_patch(patch);
            checkDocument(root.at("image_processing"), processing::config_json::toJson(config));
            // The persisted document is authoritative for every known field it
            // contains, including fields not changed by this patch.
            std::string error;
            if (!processing::config_json::fromJson(root.at("image_processing"), config, &error))
                throw std::runtime_error(error);
            validate(config);
            const auto bytes = root.dump(2) + "\n";
            if (bytes.size() > 4 * 1024 * 1024)
                throw std::runtime_error("merged configuration exceeds 4 MiB limit");
            const auto check = readConfigDocument(path);
            if (!check.ok) throw std::runtime_error(check.error);
            if (check.revision != baseline) {
                r.conflict = true;
                throw std::runtime_error("configuration changed before save");
            }
            save(path, bytes, r.saved);
            r.revision = hash(bytes);
            backend.setLastConfigJson(bytes);
            backend.processing().setProcessingConfig(config);
            r.applied = true;
            r.verified =
                processing::config_json::toJson(config) ==
                processing::config_json::toJson(backend.processing().getProcessingConfig());
            if (!r.verified) r.error = "saved and applied, but runtime readback differs";
        } catch (const std::exception& e) {
            r.error = e.what();
        }
    });
    if (!idle)
        r.error =
            "configuration is locked while an experiment is starting, active, stopping or failed";
    return r;
}
} // namespace backend::app
