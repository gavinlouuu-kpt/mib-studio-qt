#include "backend/app/StartupConfiguration.h"

#include "backend/app/AppBackend.h"
#include "backend/app/ConfigDocumentApply.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/app/MethodApply.h"
#include "backend/app/ProfileStore.h"
#include "backend/processing/ProcessingCoreLoader.h"
#include "backend/profiles/ProfileRegistryWorker.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace backend::app {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;

constexpr const char* kPointerFile = "startup_configuration.json";
constexpr std::size_t kMaxPointerBytes = 64 * 1024;

fs::path pointerPath(AppBackend& backend) { return fs::path(backend.dataDir()) / kPointerFile; }

void writePointer(AppBackend& backend, const Json& value) {
    if (backend.dataDir().empty()) return;
    const auto path = pointerPath(backend);
    const auto tmp = fs::path(path.string() + ".tmp");
    std::error_code ec;
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << value.dump();
        out.flush();
        if (!out) {
            SPDLOG_WARN("StartupConfiguration: cannot write {}", tmp.string());
            fs::remove(tmp, ec);
            return;
        }
    } // closed before the rename (Windows)
    fs::rename(tmp, path, ec);
    if (ec) {
        SPDLOG_WARN("StartupConfiguration: cannot replace {}: {}", path.string(), ec.message());
        fs::remove(tmp, ec);
    }
}

// The recorded pointer, or null when there is none. Throws when unreadable.
Json readPointer(AppBackend& backend) {
    const auto path = pointerPath(backend);
    std::error_code ec;
    if (backend.dataDir().empty() || !fs::exists(path, ec)) return nullptr;
    if (fs::file_size(path, ec) > kMaxPointerBytes) throw std::runtime_error("Startup configuration record is too large");
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    auto value = Json::parse(text.str());
    if (!value.is_object() || !value.contains("kind") || !value.at("kind").is_string())
        throw std::runtime_error("Invalid startup configuration record");
    return value;
}

Json restoreProfile(AppBackend& backend, const std::string& base) {
    std::string reply;
    if (!backend.experiment().withIdleConfiguration(
            [&] { reply = profileStoreCommand(backend, base, R"({"operation":"restore"})"); }))
        return Json{{"ok", false}, {"error", "Profiles cannot change during an experiment"}};
    auto result = Json::parse(reply);
    result["kind"] = "profile";
    return result;
}

Json restoreCentral(AppBackend& backend, const Json& pointer) {
    Json result = {{"ok", false}, {"restored", false}, {"kind", "central"}};
    const auto revisionId = pointer.value("revision_id", std::string{});
    const auto expected = pointer.value("config_sha256", std::string{});
    result["revision_id"] = revisionId;
    if (revisionId.empty() || expected.empty()) {
        result["error"] = "Invalid startup method record";
        return result;
    }
    // The registry reopens the last user's cache offline at startup; wait for
    // that first load (outside the coordinator's idle transaction).
    backend.profileRegistry().waitIdle(std::chrono::seconds(5));
    const auto plan = planMethodApply(backend.profileRegistry().snapshot(), revisionId, backend.getLastConfigJson());
    if (!plan.ok) {
        result["error"] = "Startup method " + revisionId + " cannot be re-applied: " + plan.error;
        return result;
    }
    const auto actual = processing::processingCoreBytesSha256(
        reinterpret_cast<const uint8_t*>(plan.configText.data()), plan.configText.size());
    if (actual != expected) {
        result["error"] = "Startup method changed since it was applied; review it before applying";
        return result;
    }
    const auto report = applyCentralMethod(backend, revisionId);
    if (!report.ok) {
        result["error"] = report.error;
        return result;
    }
    result["ok"] = true;
    result["restored"] = true;
    result["applied"] = report.applied;
    result["not_applied"] = report.notApplied;
    return result;
}

} // namespace

void recordStartupProfile(AppBackend& backend, const std::string& base, const std::string& name,
                          const std::string& revision) {
    writePointer(backend, Json{{"kind", "profile"}, {"base", base}, {"name", name}, {"revision", revision}});
}

void recordStartupCentralMethod(AppBackend& backend, const std::string& revisionId,
                                const std::string& configSha256) {
    writePointer(backend, Json{{"kind", "central"}, {"revision_id", revisionId}, {"config_sha256", configSha256}});
}

std::string restoreStartupConfiguration(AppBackend& backend, const std::string& profileBase) {
    try {
        const auto pointer = readPointer(backend);
        if (pointer.is_null()) {
            if (profileBase.empty()) return Json{{"ok", true}, {"restored", false}, {"kind", nullptr}}.dump();
            return restoreProfile(backend, profileBase).dump(); // legacy: selection only
        }
        const auto kind = pointer.at("kind").get<std::string>();
        if (kind == "central") return restoreCentral(backend, pointer).dump();
        if (kind == "profile") {
            const auto base = pointer.value("base", std::string{});
            if (base.empty()) throw std::runtime_error("Invalid startup profile record");
            return restoreProfile(backend, base).dump();
        }
        throw std::runtime_error("Unknown startup configuration kind: " + kind);
    } catch (const std::exception& e) {
        return Json{{"ok", false}, {"restored", false}, {"error", e.what()}}.dump();
    }
}

} // namespace backend::app
