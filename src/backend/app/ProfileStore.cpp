#include "backend/app/ProfileStore.h"
#include "backend/app/AppBackend.h"
#include "backend/app/ProcessingConfigTransaction.h"
#include "backend/processing/ProcessingService.h"
#include "backend/processing/ProcessingConfigJson.h"
#include "backend/services/CaptureService.h"
#include "backend/services/AutofocusService.h"
#include "backend/playback/FrameStore.h"
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
    if (backend.capture().isRunning() || backend.autofocus().isEnabled() ||
        backend.processing().isRealtimeRunning())
        throw std::runtime_error(
            "Stop capture/realtime processing and disable autofocus before applying a profile");
    if (snapshot.contains("metadata")) compatibility(backend, snapshot.at("metadata"));
    const std::string bytes = snapshot.at("document_json");
    const auto root = J::parse(bytes);
    const auto schema = integer(root, "config_schema_version", 1, 1, 1);
    (void)schema;
    auto& processing = backend.processing();
    const auto config = validatedProcessingConfig(bytes, processing.getProcessingConfig());
    const auto flush = integer(root, "buffer_threshold",
                               static_cast<int>(processing.getFlushInterval()), 1, 10000000);
    const double mb = number(root, "experiment_buffer_max_mb",
                             processing.getMaxBufferedBytes() / (1024.0 * 1024.0), 0, 1048576);
    const double factor =
        number(root, "pixel_to_micron_factor", processing.getPixelToMicronFactor(), 1e-12, 1e12);
    bool realtimeEnabled = processing.isRealtimeEnabled(),
         dropFrames = processing.getRealtimeDropFrames();
    auto batch = processing.getRealtimeBatchSettings();
    auto mode = processing.getRealtimeProcessingMode();
    if (root.contains("realtime_processing")) {
        const auto& rp = root.at("realtime_processing");
        if (!rp.is_object()) throw std::runtime_error("realtime_processing must be an object");
        if (rp.contains("enabled")) realtimeEnabled = rp.at("enabled").get<bool>();
        if (rp.contains("drop_frames")) dropFrames = rp.at("drop_frames").get<bool>();
        batch.batchSize = integer(rp, "batch_size", batch.batchSize, 1, 1000000);
        batch.maxQueuedFrames =
            integer(rp, "max_queued_frames", batch.maxQueuedFrames, 1, 10000000);
        batch.workerCount = integer(rp, "worker_count", batch.workerCount, 1, 256);
        batch.maxBatchDelayMs = integer(rp, "max_batch_delay_ms", batch.maxBatchDelayMs, 1, 60000);
        if (rp.contains("mode")) {
            const auto text = rp.at("mode").get<std::string>();
            if (text == "inline")
                mode = services::ProcessingService::RealtimeProcessingMode::Inline;
            else if (text == "async_batch" || text == "batch" || text == "kin6")
                mode = services::ProcessingService::RealtimeProcessingMode::AsyncBatch;
            else
                throw std::runtime_error("Unknown realtime processing mode");
        }
    }
    if (batch.maxQueuedFrames < batch.batchSize)
        throw std::runtime_error("Realtime queue must hold at least one batch");
    services::CaptureService::Config capture{};
    if (root.contains("camera")) {
        if (!root.at("camera").is_object()) throw std::runtime_error("camera must be an object");
        const auto text = root.at("camera").value("frame_delivery_mode", std::string("everyFrame"));
        if (text != "everyFrame" && text != "latestFrame")
            throw std::runtime_error("Unknown frame delivery mode");
        capture.deliveryMode = camera::common::frameDeliveryModeFromString(text);
    }
    auto af = backend.autofocus().getConfig();
    af.focusSetpoint = number(root, "autofocus_focus_setpoint", af.focusSetpoint, 0, 1e9);
    af.focusRange = number(root, "autofocus_focus_range", af.focusRange, 0, 1e9);
    af.voltageStep = number(root, "autofocus_voltage_step", af.voltageStep, 0, 1e6);
    af.fineVoltageStep = number(root, "autofocus_fine_voltage_step", af.fineVoltageStep, 0, 1e6);
    af.minVoltage = number(root, "autofocus_min_voltage", af.minVoltage, -1e6, 1e6);
    af.maxVoltage = number(root, "autofocus_max_voltage", af.maxVoltage, -1e6, 1e6);
    af.initialVoltage =
        number(root, "autofocus_initial_voltage", af.initialVoltage, af.minVoltage, af.maxVoltage);
    af.manualVoltageStep =
        number(root, "autofocus_manual_voltage_step", af.manualVoltageStep, 0, 1e6);
    af.safeShutdownVoltage =
        number(root, "safe_shutdown_voltage", af.safeShutdownVoltage, af.minVoltage, af.maxVoltage);
    af.ringRatioStaleMs = integer(root, "ring_ratio_stale_ms", af.ringRatioStaleMs, 1, 3600000);
    af.minSamplesPerStep =
        integer(root, "autofocus_min_samples_per_step", af.minSamplesPerStep, 1, 10000000);
    if (root.contains("require_new_sample_per_step"))
        af.requireNewSamplePerStep = root.at("require_new_sample_per_step").get<bool>();
    if (root.contains("focus_direction"))
        af.focusDirection = root.at("focus_direction").get<bool>();
    if (af.minVoltage > af.maxVoltage || af.initialVoltage < af.minVoltage ||
        af.initialVoltage > af.maxVoltage || af.safeShutdownVoltage < af.minVoltage ||
        af.safeShutdownVoltage > af.maxVoltage)
        throw std::runtime_error("Invalid autofocus voltage bounds");
    auto roi = processing.getRealtimeRoi();
    if (root.contains("roi")) {
        const auto& r = root.at("roi");
        if (!r.is_object()) throw std::runtime_error("roi must be an object");
        roi.x = integer(r, "x", 0, 0, 1000000);
        roi.y = integer(r, "y", 0, 0, 1000000);
        roi.w = integer(r, "w", 0, 0, 1000000);
        roi.h = integer(r, "h", 0, 0, 1000000);
        if (roi.w || roi.h) {
            playback::Frame frame;
            if (!backend.getFrameStore()->getLatest(frame) || !roi.w || !roi.h ||
                static_cast<uint64_t>(roi.x + roi.w) > frame.width ||
                static_cast<uint64_t>(roi.y + roi.h) > frame.height)
                throw std::runtime_error("ROI cannot be validated: capture a matching preview then "
                                         "stop capture before applying the profile");
        }
    }
    const int fps = integer(root, "display_fps", 60, 1, 240);
    // Construct provenance before mutations; malformed old provenance cannot cause
    // a post-apply failure. Only effective known fields claim runtime application.
    auto provenance =
        J::parse(backend.getLastConfigJson().empty() ? "{}" : backend.getLastConfigJson());
    if (!provenance.is_object())
        throw std::runtime_error("Current configuration provenance is not an object");
    provenance["image_processing"] = processing::config_json::toJson(config);
    provenance["buffer_threshold"] = flush;
    provenance["experiment_buffer_max_mb"] = mb;
    provenance["pixel_to_micron_factor"] = factor;
    provenance["realtime_processing"] = {
        {"enabled", realtimeEnabled},
        {"drop_frames", dropFrames},
        {"mode", mode == services::ProcessingService::RealtimeProcessingMode::Inline
                     ? "inline"
                     : "async_batch"},
        {"batch_size", batch.batchSize},
        {"max_queued_frames", batch.maxQueuedFrames},
        {"worker_count", batch.workerCount},
        {"max_batch_delay_ms", batch.maxBatchDelayMs}};
    provenance["roi"] = {{"x", roi.x}, {"y", roi.y}, {"w", roi.w}, {"h", roi.h}};
    if (!provenance.contains("camera") || !provenance.at("camera").is_object())
        provenance["camera"] = J::object();
    provenance["camera"]["frame_delivery_mode"] = camera::common::toString(capture.deliveryMode);
    for (const auto* key :
         {"autofocus_focus_setpoint", "autofocus_focus_range", "autofocus_voltage_step",
          "autofocus_fine_voltage_step", "autofocus_min_voltage", "autofocus_max_voltage",
          "autofocus_initial_voltage", "autofocus_manual_voltage_step",
          "autofocus_min_samples_per_step"})
        if (root.contains(key)) provenance[key] = root.at(key);
    for (const auto* key : {"ring_ratio_stale_ms", "require_new_sample_per_step",
                            "safe_shutdown_voltage", "focus_direction"})
        if (root.contains(key)) provenance[key] = root.at(key);
    provenance["profile_selection"] = {{"name", snapshot.at("name")},
                                       {"path", snapshot.at("path")},
                                       {"revision", snapshot.at("revision")},
                                       {"profile_id", snapshot.at("profile_id")},
                                       {"display_fps", fps}};
    const auto provenanceBytes = provenance.dump();
    // Every field is parsed/validated before mutation. Running realtime is refused,
    // so setters cannot restart worker threads. No hardware actuation is issued.
    processing.setProcessingConfig(config);
    processing.setFlushInterval(flush);
    processing.setMaxBufferedBytes(static_cast<uint64_t>(mb * 1024.0 * 1024.0));
    processing.setRealtimeDropFrames(dropFrames);
    processing.setRealtimeBatchSettings(batch);
    processing.setRealtimeProcessingMode(mode);
    processing.setRealtimeEnabled(realtimeEnabled);
    processing.setPixelToMicronFactor(factor);
    processing.setRealtimeRoi(roi);
    backend.capture().setConfig(capture);
    backend.autofocus().setConfig(af);
    backend.setLastConfigJson(provenanceBytes);
    return {
        {"applied", true},
        {"display_fps", fps},
        {"profile_id", snapshot.at("profile_id")},
        {"message",
         "Applied processing, buffer, realtime, delivery, calibration, autofocus configuration and "
         "validated ROI. No camera script or device connection was executed."}};
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
