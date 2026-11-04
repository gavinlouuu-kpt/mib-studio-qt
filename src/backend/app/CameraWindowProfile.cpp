#include "backend/app/CameraWindowProfile.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace backend::app {
namespace {
using Json = nlohmann::json;

int windowInteger(const Json& section, const char* key, bool required, int low)
{
    const auto it = section.find(key);
    if (it == section.end() || it->is_null()) {
        if (required) throw std::runtime_error(std::string("camera.window.") + key + " is required");
        return 0;
    }
    if (!it->is_number_integer())
        throw std::runtime_error(std::string("Expected integer: camera.window.") + key);
    const auto value = it->get<long long>();
    if (value < low || value > 1000000)
        throw std::runtime_error(std::string("camera.window.") + key + " must be between " + std::to_string(low) +
                                 " and 1000000 (got " + std::to_string(value) + ")");
    return static_cast<int>(value);
}

std::string windowString(const Json& section, const char* key)
{
    const auto it = section.find(key);
    if (it == section.end() || it->is_null()) return {};
    if (!it->is_string()) throw std::runtime_error(std::string("Expected string: camera.window.") + key);
    return it->get<std::string>();
}

} // namespace

AcquisitionProfile parseAcquisitionProfile(const std::string& configJson)
{
    AcquisitionProfile out;
    const auto root = Json::parse(configJson);
    if (!root.is_object()) throw std::runtime_error("config.json root must be an object");
    if (const auto fps = root.find("target_fps"); fps != root.end() && !fps->is_null()) {
        if (!fps->is_number()) throw std::runtime_error("Expected number: target_fps");
        const double value = fps->get<double>();
        if (!std::isfinite(value) || value < 0 || value > 10000000)
            throw std::runtime_error("target_fps must be between 0 and 10000000");
        out.targetFps = value;
    }
    const auto camera = root.find("camera");
    if (camera == root.end() || !camera->is_object()) return out;
    const auto section = camera->find("window");
    if (section == camera->end() || section->is_null()) return out;
    if (!section->is_object()) throw std::runtime_error("camera.window must be an object");
    ProfileCameraWindow w;
    w.window.width = windowInteger(*section, "width", true, 1);
    w.window.height = windowInteger(*section, "height", true, 1);
    w.window.offsetX = windowInteger(*section, "offset_x", false, 0);
    w.window.offsetY = windowInteger(*section, "offset_y", false, 0);
    w.cameraVendor = windowString(*section, "camera_vendor");
    w.cameraModel = windowString(*section, "camera_model");
    out.window = w;
    return out;
}

std::optional<::camera::common::CameraWindow> parseScriptCameraWindow(const std::string& scriptText)
{
    // Same line shape EgrabberConfigParser (Qt) edits: g.RemotePort.set("Width", 512);
    static const std::regex line(R"re(^\s*[A-Za-z_]\w*\.RemotePort\.set\(\s*"(Width|Height|OffsetX|OffsetY)"\s*,\s*"?(-?\d{1,7})"?\s*\))re");
    ::camera::common::CameraWindow w;
    bool width = false, height = false;
    std::istringstream input(scriptText);
    std::string text;
    while (std::getline(input, text)) {
        const auto first = text.find_first_not_of(" \t");
        if (first != std::string::npos && text.compare(first, 2, "//") == 0) continue;
        std::smatch m;
        if (!std::regex_search(text, m, line)) continue;
        const int value = std::stoi(m[2].str());
        const auto feature = m[1].str();
        if (feature == "Width") { w.width = value; width = true; }
        else if (feature == "Height") { w.height = value; height = true; }
        else if (feature == "OffsetX") w.offsetX = value;
        else w.offsetY = value;
    }
    if (!width || !height) return std::nullopt;
    return w;
}

namespace {
// Down to a multiple of `step`; never below 0.
int floorToStep(int value, int step) { return value <= 0 ? 0 : (value / step) * step; }
} // namespace

::camera::common::CameraWindow snapEgrabberWindow(const ::camera::common::CameraWindow& requested, int sensorWidth,
                                                  int sensorHeight)
{
    if (sensorWidth <= 0) sensorWidth = kEgrabberDefaultSensorWidth;
    if (sensorHeight <= 0) sensorHeight = kEgrabberDefaultSensorHeight;
    ::camera::common::CameraWindow out;
    const int maxWidth = std::max(kEgrabberMinWidth, floorToStep(sensorWidth, kEgrabberWidthStep));
    const int maxHeight = std::max(kEgrabberMinHeight, floorToStep(sensorHeight, kEgrabberHeightStep));
    out.width = std::clamp(floorToStep(requested.width, kEgrabberWidthStep), kEgrabberMinWidth, maxWidth);
    out.height = std::clamp(floorToStep(requested.height, kEgrabberHeightStep), kEgrabberMinHeight, maxHeight);
    out.offsetX = floorToStep(requested.offsetX, kEgrabberOffsetXStep);
    out.offsetY = floorToStep(requested.offsetY, kEgrabberOffsetYStep);
    if (out.offsetX + out.width > sensorWidth)
        out.offsetX = floorToStep(sensorWidth - out.width, kEgrabberOffsetXStep);
    if (out.offsetY + out.height > sensorHeight)
        out.offsetY = floorToStep(sensorHeight - out.height, kEgrabberOffsetYStep);
    return out;
}

ScriptWindowRewrite rewriteScriptCameraWindow(const std::string& scriptText, const ::camera::common::CameraWindow& window)
{
    // Same line shape as parseScriptCameraWindow: g.RemotePort.set("Width", 512);  The number may be quoted.
    static const std::regex line(
        R"re(^(\s*[A-Za-z_]\w*\.RemotePort\.set\(\s*"(Width|Height|OffsetX|OffsetY)"\s*,\s*)"?-?\d{1,7}"?(\s*\).*)$)re");
    ScriptWindowRewrite out;
    bool seen[4] = {false, false, false, false};
    static const char* const names[4] = {"Width", "Height", "OffsetX", "OffsetY"};
    const int values[4] = {window.width, window.height, window.offsetX, window.offsetY};
    std::size_t begin = 0;
    while (begin <= scriptText.size()) {
        std::size_t end = scriptText.find('\n', begin);
        const bool last = end == std::string::npos;
        std::string text = scriptText.substr(begin, last ? std::string::npos : end - begin);
        // ECMAScript `.` does not match '\r': keep a CRLF line ending out of the match and put it back.
        const bool carriageReturn = !text.empty() && text.back() == '\r';
        if (carriageReturn) text.pop_back();
        const auto first = text.find_first_not_of(" \t");
        const bool comment = first != std::string::npos && text.compare(first, 2, "//") == 0;
        std::smatch m;
        if (!comment && std::regex_match(text, m, line)) {
            const std::string feature = m[2].str();
            for (int i = 0; i < 4; ++i) {
                if (feature != names[i]) continue;
                text = m[1].str() + std::to_string(values[i]) + m[3].str();
                seen[i] = true;
                break;
            }
        }
        out.text += text;
        if (carriageReturn) out.text += '\r';
        if (last) break;
        out.text += '\n';
        begin = end + 1;
    }
    for (int i = 0; i < 4; ++i) (seen[i] ? out.rewritten : out.missing).emplace_back(names[i]);
    return out;
}

std::string appliedProfileScriptPath(const std::string& appliedConfigJson)
{
    const auto root = Json::parse(appliedConfigJson, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded() || !root.is_object()) return {};
    const auto selection = root.find("profile_selection");
    if (selection == root.end() || !selection->is_object()) return {};
    const auto path = selection->find("path");
    if (path == selection->end() || !path->is_string()) return {};
    const auto folder = std::filesystem::u8path(path->get<std::string>()).parent_path();
    if (folder.empty()) return {};
    return (folder / "egrabberConfig.js").u8string();
}

const char* toString(WindowSource source)
{
    switch (source) {
    case WindowSource::Profile: return "profile";
    case WindowSource::Script: return "script";
    default: return "none";
    }
}

ChosenWindow chooseEgrabberWindow(const std::optional<ProfileCameraWindow>& profile,
                                  const std::optional<::camera::common::CameraWindow>& script)
{
    ChosenWindow out;
    if (profile && profile->window.valid()) {
        out.window = profile->window;
        out.source = WindowSource::Profile;
    } else if (script && script->valid()) {
        out.window = *script;
        out.source = WindowSource::Script;
    }
    return out;
}

std::string cameraModelMismatch(const ProfileCameraWindow& profile, const std::string& connectedVendor,
                                const std::string& connectedModel)
{
    if (!profile.namesCamera() || connectedModel.empty()) return {};
    const bool vendorDiffers =
        !profile.cameraVendor.empty() && !connectedVendor.empty() && profile.cameraVendor != connectedVendor;
    if (profile.cameraModel == connectedModel && !vendorDiffers) return {};
    return "the profile's camera window was made for " + describeCamera(profile.cameraVendor, profile.cameraModel) +
           "; the connected camera is " + describeCamera(connectedVendor, connectedModel);
}

std::string describeWindow(const ::camera::common::CameraWindow& w)
{
    return std::to_string(w.width) + "x" + std::to_string(w.height) + " at (" + std::to_string(w.offsetX) + ", " +
           std::to_string(w.offsetY) + ")";
}

std::string describeCamera(const std::string& vendor, const std::string& model)
{
    if (vendor.empty() && model.empty()) return "an unnamed camera";
    if (vendor.empty()) return model;
    if (model.empty()) return vendor;
    return vendor + " " + model;
}

} // namespace backend::app
