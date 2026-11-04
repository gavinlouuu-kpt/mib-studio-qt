#include "backend/app/CameraWindowProfile.h"

#include <nlohmann/json.hpp>

#include <cmath>
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
