#include "backend/app/ProfileStore.h"
#include "backend/app/ConfigDocumentApply.h"
#include "backend/app/StartupConfiguration.h"
#include "backend/app/AppBackend.h"
#include "backend/processing/ProcessingService.h"
#include "backend/processing/ProcessingConfigJson.h"
#include <cmath>
#include <limits>
#include "backend/processing/ProcessingCoreLoader.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#endif
namespace backend::app {
namespace {
namespace fs = std::filesystem;
using J = nlohmann::json;
constexpr size_t limit = 4 * 1024 * 1024;
const char* files[] = {"config.json", "egrabberConfig.js", "profile.meta.json"};
void nameCheck(const std::string& n) {
    if (n.empty() || n.size() > 120 || n == "." || n == ".." || n.front() == '.' ||
        n.back() == '.' || n.back() == ' ' || n.find_first_of("/\\<>:\"|?*") != std::string::npos ||
        std::any_of(n.begin(), n.end(), [](unsigned char c) { return c < 32; }))
        throw std::runtime_error("Invalid profile name");
}
std::string read(const fs::path& p) {
    if (fs::is_symlink(fs::symlink_status(p)))
        throw std::runtime_error("Profile symlinks are not supported");
    if (!fs::is_regular_file(p))
        throw std::runtime_error("Profile file is not regular: " + p.filename().string());
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot read profile file");
    std::string s(limit + 1, '\0');
    f.read(s.data(), s.size());
    s.resize(f.gcount());
    if (f.bad() || s.size() > limit)
        throw std::runtime_error("Profile read failed or exceeds 4 MiB");
    return s;
}
J snapshot(const fs::path& base, const std::string& name) {
    nameCheck(name);
    auto dir = base / name;
    if (fs::is_symlink(fs::symlink_status(dir)) || !fs::is_directory(dir))
        throw std::runtime_error("Invalid profile directory");
    J j = {{"name", name}, {"path", (dir / "config.json").string()}};
    std::string hashed;
    for (auto file : files) {
        const auto path = dir / file;
        const bool exists = fs::exists(path) || fs::is_symlink(fs::symlink_status(path));
        if (!exists && std::string(file) == "config.json")
            throw std::runtime_error("Profile has no config.json");
        const auto bytes = exists ? read(path) : std::string();
        hashed += std::string(file) + ":" + (exists ? "1:" : "0:") + std::to_string(bytes.size()) +
                  ":" + bytes;
        if (std::string(file) == "config.json") {
            if (!J::parse(bytes).is_object())
                throw std::runtime_error("Profile config must be an object");
            j["document_json"] = bytes;
        }
        if (std::string(file) == "profile.meta.json" && exists) j["metadata"] = J::parse(bytes);
        if (std::string(file) == "egrabberConfig.js") j["script"] = exists ? J(bytes) : J(nullptr);
    }
    j["profile_id"] = j.contains("metadata") ? j["metadata"].value("profile_id", name) : name;
    j["revision"] = processing::processingCoreBytesSha256(
        reinterpret_cast<const uint8_t*>(hashed.data()), hashed.size());
    return j;
}
double number(const J& root, const char* key, double fallback, double low, double high) {
    if (!root.contains(key)) return fallback;
    if (!root.at(key).is_number()) throw std::runtime_error(std::string("Expected number: ") + key);
    const double value = root.at(key).get<double>();
    if (!std::isfinite(value) || value < low || value > high)
        throw std::runtime_error(std::string("Invalid range: ") + key);
    return value;
}
int integer(const J& root, const char* key, int fallback, int low, int high) {
    if (root.contains(key) && !root.at(key).is_number_integer())
        throw std::runtime_error(std::string("Expected integer: ") + key);
    return static_cast<int>(number(root, key, fallback, low, high));
}
std::vector<unsigned> version(const std::string& text) {
    std::vector<unsigned> parts;
    std::istringstream in(text);
    std::string part;
    while (std::getline(in, part, '.')) {
        if (part.empty() || part.size() > 9 ||
            !std::all_of(part.begin(), part.end(), [](unsigned char c) { return std::isdigit(c); }))
            throw std::runtime_error("Unsupported application version syntax");
        parts.push_back(static_cast<unsigned>(std::stoul(part)));
    }
    if (parts.empty() || parts.size() > 4)
        throw std::runtime_error("Unsupported application version syntax");
    parts.resize(4);
    return parts;
}
void compatibility(AppBackend& backend, const J& meta) {
    if (!meta.is_object()) throw std::runtime_error("Profile metadata must be an object");
    const int contract =
        !meta.contains("processing_contract_version") ||
                meta.at("processing_contract_version").is_null()
            ? 0
            : integer(meta, "processing_contract_version", 0, 0, (std::numeric_limits<int>::max)());
    if (contract && static_cast<uint32_t>(contract) !=
                        backend.processing().activeProcessingCoreIdentity().contractVersion)
        throw std::runtime_error("Profile processing contract is incompatible with active core");
    const auto current = version(MIB_STUDIO_QT_VERSION);
    for (const auto* key : {"app_min_version", "app_max_version"})
        if (meta.contains(key) && !meta.at(key).is_null()) {
            const auto text = meta.at(key).get<std::string>();
            if (text.empty()) continue;
            const auto bound = version(text);
            if ((std::string(key) == "app_min_version" && current < bound) ||
                (std::string(key) == "app_max_version" && current > bound))
                throw std::runtime_error("Profile is incompatible with this application version");
        }
}
J apply(AppBackend& backend, const J& snapshot) {
    // The one validated applier (ConfigDocumentApply.h) stages and commits;
    // a profile records a provenance document instead of its exact text.
    if (const auto blocker = configApplyBlocker(backend); !blocker.empty())
        throw std::runtime_error(blocker);
    if (snapshot.contains("metadata")) compatibility(backend, snapshot.at("metadata"));
    const std::string bytes = snapshot.at("document_json");
    const auto root = J::parse(bytes);
    const auto staged = stageConfigDocument(backend, bytes);
    // Construct provenance before mutations; malformed old provenance cannot cause
    // a post-apply failure. Only effective known fields claim runtime application.
    auto provenance =
        J::parse(backend.getLastConfigJson().empty() ? "{}" : backend.getLastConfigJson());
    if (!provenance.is_object())
        throw std::runtime_error("Current configuration provenance is not an object");
    provenance["image_processing"] = processing::config_json::toJson(staged.processing);
    provenance["buffer_threshold"] = staged.flushInterval;
    provenance["experiment_buffer_max_mb"] = staged.experimentBufferMb;
    provenance["pixel_to_micron_factor"] = staged.pixelToMicron;
    provenance["realtime_processing"] = {
        {"enabled", staged.realtimeEnabled},
        {"drop_frames", staged.dropFrames},
        {"mode", staged.mode == services::ProcessingService::RealtimeProcessingMode::Inline
                     ? "inline"
                     : "async_batch"},
        {"batch_size", staged.batch.batchSize},
        {"max_queued_frames", staged.batch.maxQueuedFrames},
        {"worker_count", staged.batch.workerCount},
        {"max_batch_delay_ms", staged.batch.maxBatchDelayMs}};
    provenance["roi"] = {{"x", staged.roi.x}, {"y", staged.roi.y}, {"w", staged.roi.w}, {"h", staged.roi.h}};
    if (!provenance.contains("camera") || !provenance.at("camera").is_object())
        provenance["camera"] = J::object();
    provenance["camera"]["frame_delivery_mode"] = camera::common::toString(staged.capture.deliveryMode);
    for (const auto* key :
         {"autofocus_focus_setpoint", "autofocus_focus_range", "autofocus_voltage_step",
          "autofocus_fine_voltage_step", "autofocus_min_voltage", "autofocus_max_voltage",
          "autofocus_initial_voltage", "autofocus_manual_voltage_step",
          "autofocus_min_samples_per_step"})
        if (root.contains(key)) provenance[key] = root.at(key);
    for (const auto* key : {"ring_ratio_stale_ms", "require_new_sample_per_step",
                            "safe_shutdown_voltage", "focus_direction"})
        if (root.contains(key)) provenance[key] = root.at(key);
    if (root.contains("stage")) provenance["stage"] = root.at("stage");
    provenance["profile_selection"] = {{"name", snapshot.at("name")},
                                       {"path", snapshot.at("path")},
                                       {"revision", snapshot.at("revision")},
                                       {"profile_id", snapshot.at("profile_id")},
                                       {"display_fps", staged.displayFps}};
    const auto provenanceBytes = provenance.dump();
    commitStagedConfig(backend, staged);
    backend.setLastConfigJson(provenanceBytes);
    return {
        {"applied", true},
        {"display_fps", staged.displayFps},
        {"profile_id", snapshot.at("profile_id")},
        {"message",
         std::string("Applied processing, buffer, realtime, delivery, calibration, autofocus ") +
             (staged.stage ? "and Z stage " : "") +
             "configuration and validated ROI. No camera script or device connection was executed."}};
}
void write(const fs::path& p, const std::string& bytes) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), bytes.size());
    f.flush();
    if (!f) throw std::runtime_error("Profile write failed");
    f.close();
    if (!f) throw std::runtime_error("Profile close failed");
}
void selectionWrite(const fs::path& base, const J& value) {
    // Selection is a single tiny atomic pointer, never partial profile bytes.
    static std::atomic<unsigned long long> seq{0};
    auto tmp = base / (".selection-tmp-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                       "-" + std::to_string(++seq));
    struct Cleanup {
        fs::path p;
        ~Cleanup() {
            std::error_code e;
            fs::remove(p, e);
        }
    } cleanup{tmp};
    write(tmp, value.dump());
#ifdef _WIN32
    if (!MoveFileExW(tmp.c_str(), (base / ".selection.json").c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Settings applied but startup selection persistence failed");
#else
    fs::rename(tmp, base / ".selection.json");
#endif
}
J selectionRead(const fs::path& base) {
    if (!fs::exists(base / ".selection.json")) return nullptr;
    const auto value = J::parse(read(base / ".selection.json"));
    if (!value.is_object() || !value.contains("name") || !value.contains("revision"))
        throw std::runtime_error("Invalid saved profile selection");
    if (value.at("name") == "") return nullptr;
    return value;
}

} // namespace
std::string profileStoreCommand(AppBackend& backend, const std::string& baseString,
                                const std::string& request) {
    J result = {{"ok", false}};
    try {
        if (baseString.empty()) throw std::runtime_error("Choose a profile directory");
        if (request.size() > 2 * limit + 4096)
            throw std::runtime_error("Profile request too large");
        const auto q = J::parse(request);
        const auto op = q.at("operation").get<std::string>();
        const fs::path base = fs::absolute(baseString).lexically_normal();
        if (fs::is_symlink(fs::symlink_status(base)))
            throw std::runtime_error("Profile root cannot be a symlink");
        if (op == "selection") {
            result["selection"] = selectionRead(base);
            auto runtime =
                J::parse(backend.getLastConfigJson().empty() ? "{}" : backend.getLastConfigJson());
            result["active_profile"] = runtime.value("profile_selection", J(nullptr));
        } else if (op == "restore") {
            const auto selection = selectionRead(base);
            if (selection.is_null()) {
                result["restored"] = false;
            } else {
                const auto profile = snapshot(base, selection.at("name").get<std::string>());
                if (profile.at("revision") != selection.at("revision"))
                    throw std::runtime_error("Startup profile changed; review it before applying");
                result.update(apply(backend, profile));
                result["profile"] = profile;
                result["restored"] = true;
            }
        } else if (op == "list") {
            result["profiles"] = J::array();
            result["warnings"] = J::array();
            if (fs::exists(base))
                for (const auto& e : fs::directory_iterator(base)) {
                    if (result["profiles"].size() + result["warnings"].size() >= 1000) {
                        result["warnings"].push_back(
                            "Listing limited to 1000 entries; use a dedicated profiles folder");
                        break;
                    }
                    const auto name = e.path().filename().string();
                    if (name.empty() || name.front() == '.') continue;
                    try {
                        auto s = snapshot(base, name);
                        s.erase("document_json");
                        s.erase("script");
                        result["profiles"].push_back(s);
                    } catch (const std::exception& e) {
                        result["warnings"].push_back(name + ": " + e.what());
                    }
                }
            std::sort(result["profiles"].begin(), result["profiles"].end(),
                      [](const J& a, const J& b) {
                          return a.at("name").template get<std::string>() <
                                 b.at("name").template get<std::string>();
                      });
        } else {
            auto name = q.at("name").get<std::string>();
            nameCheck(name);
            if (op == "read")
                result["profile"] = snapshot(base, name);
            else if (op == "apply") {
                const auto s = snapshot(base, name);
                if (q.value("baseline", "") != s.at("revision").get<std::string>())
                    throw std::runtime_error("Profile changed; reload before applying");
                result.update(apply(backend, s));
                selectionWrite(
                    base, {{"name", name}, {"revision", s.at("revision")}, {"path", s.at("path")}});
                result["selection_saved"] = true;
                recordStartupProfile(backend, base.string(), name, s.at("revision").get<std::string>());
            } else if (op == "create" || op == "duplicate" || op == "install_remote") {
                fs::create_directories(base);
                std::string document, script;
                bool hasScript = false;
                J metadata;
                fs::path backup;
                if (op == "duplicate") {
                    const auto s = snapshot(base, q.at("source").get<std::string>());
                    if (q.value("baseline", "") != s.at("revision").get<std::string>())
                        throw std::runtime_error("Profile changed; reload before copying");
                    document = s.at("document_json");
                    hasScript = !s.at("script").is_null();
                    if (hasScript) script = s.at("script");
                } else {
                    document = q.at("document_json").get<std::string>();
                    if (document.size() > limit || !J::parse(document).is_object())
                        throw std::runtime_error("Invalid profile JSON or exceeds 4 MiB");
                    if (q.contains("script") && !q.at("script").is_null()) {
                        script = q.at("script").get<std::string>();
                        hasScript = true;
                    }
                }
                if (op == "install_remote") {
                    const auto entry = q.at("entry");
                    compatibility(backend, entry);
                    const auto identity = entry.at("profile_id").get<std::string>();
                    if (identity.empty())
                        throw std::runtime_error("Remote profile has no identity");
                    const auto sha = [](const std::string& bytes) {
                        return processing::processingCoreBytesSha256(
                            reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
                    };
                    auto expected = entry.at("config_sha256").get<std::string>();
                    std::transform(expected.begin(), expected.end(), expected.begin(),
                                   [](unsigned char c) { return std::tolower(c); });
                    if (expected.size() != 64 || expected != sha(document))
                        throw std::runtime_error(
                            "Remote config SHA256 mismatch or missing checksum");
                    if (hasScript) {
                        auto scriptHash = entry.at("camera_script_sha256").get<std::string>();
                        std::transform(scriptHash.begin(), scriptHash.end(), scriptHash.begin(),
                                       [](unsigned char c) { return std::tolower(c); });
                        if (scriptHash.size() != 64 || scriptHash != sha(script))
                            throw std::runtime_error(
                                "Remote script SHA256 mismatch or missing checksum");
                    }
                    metadata = entry;
                    metadata["profile_meta_schema_version"] = 1;
                    metadata["source"] = {{"type", "r2-public-catalog"},
                                          {"channel", q.value("channel", "stable")},
                                          {"catalog_url", q.value("catalog_url", "")}};
                    metadata["config_sha256"] = sha(document);
                    metadata["camera_script_sha256"] = hasScript ? sha(script) : "";
                    if (fs::exists(base / name)) {
                        const auto old = snapshot(base, name);
                        if (q.value("baseline", "") != old.at("revision"))
                            throw std::runtime_error(
                                "Profile changed; reload before remote update");
                        if (!old.contains("metadata") ||
                            old["metadata"].value("profile_id", "") != identity)
                            throw std::runtime_error(
                                "Remote update identity differs; install under a new name");
                        backup = base /
                                 (".backup-" + name + "-" +
                                  std::to_string(
                                      std::chrono::system_clock::now().time_since_epoch().count()));
                    }
                }
                if (script.size() > limit) throw std::runtime_error("Camera script exceeds 4 MiB");
                if (fs::exists(base / name) && backup.empty())
                    throw std::runtime_error("Profile already exists; choose a new name");
                static std::atomic<unsigned long long> seq{0};
                auto staging =
                    base /
                    (".staging-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                     "-" + std::to_string(++seq));
                if (!fs::create_directory(staging))
                    throw std::runtime_error("Cannot stage profile");
                struct Cleanup {
                    fs::path path;
                    ~Cleanup() {
                        std::error_code ec;
                        fs::remove_all(path, ec);
                    }
                } cleanup{staging};
                write(staging / "config.json", document);
                if (hasScript) write(staging / "egrabberConfig.js", script);
                if (!metadata.is_null()) write(staging / "profile.meta.json", metadata.dump(2));
                if (!backup.empty()) fs::rename(base / name, backup);
                try {
                    fs::rename(staging, base / name);
                } catch (...) {
                    if (!backup.empty()) {
                        std::error_code ec;
                        fs::rename(backup, base / name, ec);
                    }
                    throw;
                }
                if (!backup.empty()) result["backup_path"] = backup.string();
                // New copies intentionally have no remote metadata: Qt will derive local identity.
                result["saved"] = true;
                result["profile"] = snapshot(base, name);
            } else if (op == "rename" || op == "archive") {
                const auto s = snapshot(base, name);
                if (q.value("baseline", "") != s.at("revision").get<std::string>())
                    throw std::runtime_error("Profile changed; reload before modifying");
                auto destination = q.value("destination", "");
                if (op == "archive")
                    destination =
                        ".archived-" + name + "-" +
                        std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
                else
                    nameCheck(destination);
                if (fs::exists(base / destination))
                    throw std::runtime_error("Destination profile exists");
                fs::rename(base / name, base / destination);
                const auto selection = selectionRead(base);
                if (!selection.is_null() && selection.at("name") == name) {
                    if (op == "archive")
                        selectionWrite(base, J{{"name", ""}, {"revision", ""}});
                    else
                        selectionWrite(base,
                                       {{"name", destination},
                                        {"revision", s.at("revision")},
                                        {"path", (base / destination / "config.json").string()}});
                }
                result["destination"] = (base / destination).string();
                if (op == "rename") result["profile"] = snapshot(base, destination);
            } else
                throw std::runtime_error("Unknown profile operation");
        }
        result["ok"] = true;
    } catch (const std::exception& e) {
        result["error"] = e.what();
    }
    return result.dump();
}
} // namespace backend::app
