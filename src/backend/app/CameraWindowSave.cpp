#include "backend/app/CameraWindowSave.h"

#include "backend/app/AppBackend.h"
#include "backend/app/ExperimentCoordinator.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace backend::app {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;

constexpr std::uintmax_t kMaxScriptBytes = 4u * 1024u * 1024u;

std::string lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
}

Json windowJson(const ::camera::common::CameraWindow& w)
{
    return Json{{"x", w.offsetX}, {"y", w.offsetY}, {"width", w.width}, {"height", w.height}};
}

} // namespace

CameraWindowSaveResult saveEgrabberWindow(AppBackend& backend, const std::string& scriptPath,
                                          const ::camera::common::CameraWindow& requested)
{
    CameraWindowSaveResult out;
    out.requested = requested;
    out.scriptPath = scriptPath;
    const auto fail = [&](const std::string& message) {
        out.ok = false;
        out.error = message;
        return out;
    };
    if (requested.width <= 0 || requested.height <= 0 || requested.offsetX < 0 || requested.offsetY < 0)
        return fail("The camera window needs a positive size and offsets of 0 or more");
    if (backend.isFrameRecording()) return fail("Stop recording before editing the camera window");

    const auto sensor = backend.egrabberSensor();
    out.sensorWidth = sensor.width > 0 ? sensor.width : kEgrabberDefaultSensorWidth;
    out.sensorHeight = sensor.height > 0 ? sensor.height : kEgrabberDefaultSensorHeight;
    out.window = snapEgrabberWindow(requested, out.sensorWidth, out.sensorHeight);

    // The script is validated before anything is written.
    fs::path script;
    std::string scriptText;
    if (!scriptPath.empty()) {
        std::error_code ec;
        const fs::path given = fs::u8path(scriptPath);
        if (lower(given.extension().string()) != ".js") return fail("The Experiment camera script must be a .js file");
        script = fs::canonical(given, ec);
        if (ec || !fs::is_regular_file(script, ec)) return fail("Experiment camera script file not found: " + scriptPath);
        if (fs::file_size(script, ec) > kMaxScriptBytes) return fail("The Experiment camera script exceeds 4 MiB");
        std::ifstream input(script, std::ios::binary);
        if (!input) return fail("Cannot read the Experiment camera script: " + scriptPath);
        scriptText.assign(std::istreambuf_iterator<char>(input), {});
    } else if (!backend.acquisitionProfile().window) {
        return fail("No Experiment camera script is selected and the applied profile has no camera.window: "
                    "the window has nowhere to be saved");
    }

    const auto rewrite = scriptPath.empty() ? ScriptWindowRewrite{} : rewriteScriptCameraWindow(scriptText, out.window);
    out.scriptMissing = rewrite.missing;
    out.scriptUpdated = !scriptPath.empty() && !rewrite.rewritten.empty();
    out.scriptChanged = out.scriptUpdated && rewrite.text != scriptText;

    const auto writeScript = [&] {
        if (!out.scriptChanged) return;
        const fs::path staged = script.parent_path() / (script.filename().string() + ".window-tmp");
        struct Cleanup {
            fs::path path;
            ~Cleanup() {
                std::error_code ec;
                fs::remove(path, ec);
            }
        } cleanup{staged};
        {
            std::ofstream file(staged, std::ios::binary | std::ios::trunc);
            file.write(rewrite.text.data(), static_cast<std::streamsize>(rewrite.text.size()));
            file.flush();
            if (!file) throw std::runtime_error("Cannot write the Experiment camera script next to " + script.string());
        }
        std::error_code ec;
        fs::permissions(staged, fs::status(script, ec).permissions(), ec); // best effort: keep the mode
        fs::rename(staged, script);
    };

    bool saved = false;
    std::string failure;
    const bool idle = backend.experiment().withIdleConfiguration([&] {
        try {
            out.profile = profileStoreSaveCameraWindow(backend, out.window, writeScript);
            saved = true;
        } catch (const std::exception& e) {
            failure = e.what();
        }
    });
    if (!idle) return fail("Stop the experiment before editing the camera window");
    if (!saved) return fail("The camera script was not updated: " + failure);
    if (!scriptPath.empty()) backend.setExperimentScriptPath(script.u8string());
    out.ok = true;
    SPDLOG_INFO("Camera window saved: {} (requested {}), script {}, profile {}", describeWindow(out.window),
                describeWindow(requested), scriptPath.empty() ? "none" : (out.scriptChanged ? "rewritten" : "unchanged"),
                out.profile.configUpdated ? "config.json updated" : (out.profile.reason.empty() ? "unchanged" : out.profile.reason));
    return out;
}

std::string toJson(const CameraWindowSaveResult& r)
{
    Json out{{"ok", r.ok}, {"error", r.error}, {"requested", windowJson(r.requested)}, {"window", windowJson(r.window)},
             {"snapped", r.ok && r.requested != r.window}, {"sensor_width", r.sensorWidth}, {"sensor_height", r.sensorHeight}};
    out["script"] = {{"path", r.scriptPath}, {"updated", r.scriptUpdated}, {"changed", r.scriptChanged},
                     {"missing", r.scriptMissing}};
    out["profile"] = {{"config_has_window", r.profile.configHasWindow}, {"local_profile", r.profile.localProfile},
                      {"name", r.profile.profileName}, {"memory_updated", r.profile.memoryUpdated},
                      {"config_updated", r.profile.configUpdated}, {"reason", r.profile.reason}};
    return out.dump();
}

} // namespace backend::app
