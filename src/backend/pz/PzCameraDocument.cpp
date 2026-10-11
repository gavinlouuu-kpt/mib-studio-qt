#include "backend/pz/PzCameraDocument.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

namespace backend::pz {

namespace {

CameraDocumentResult fail(std::string why) {
    CameraDocumentResult r;
    r.error = std::move(why);
    return r;
}

bool looksLikeStrobe(const std::string& key) {
    std::string k = key;
    std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return k.find("strobe") != std::string::npos || k.find("led") != std::string::npos || k.find("delay") != std::string::npos ||
           k.find("width") != std::string::npos || k.find("pulse") != std::string::npos;
}

std::string unknownKey(const std::string& where, const std::string& key) {
    if (looksLikeStrobe(key)) return where + ": \"" + key + "\" is not accepted: the LED strobe is Service-only and not part of the camera document";
    return where + ": unknown key \"" + key + "\"";
}

} // namespace

CameraDocumentResult parseCameraDocument(const std::string& text, double strobeDelayUs, double strobeWidthUs) {
    if (text.size() > kCameraDocumentMaxBytes) return fail("the camera document exceeds 64 KiB");
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(text);
    } catch (const std::exception& e) {
        return fail(std::string("not valid JSON: ") + e.what());
    }
    if (!doc.is_object()) return fail("the camera document must be a JSON object");
    RunSetting setting;
    for (auto it = doc.begin(); it != doc.end(); ++it) {
        if (it.key() == "schema") {
            if (!it.value().is_string() || it.value().get<std::string>() != "pz7035-camera/1")
                return fail("schema: expected \"pz7035-camera/1\"");
        } else if (it.key() == "run") {
            if (!it.value().is_object()) return fail("run: expected an object");
            for (auto r = it.value().begin(); r != it.value().end(); ++r) {
                if (r.key() == "frame_rate_hz") {
                    if (!r.value().is_number()) return fail("run.frame_rate_hz: expected a number of frames per second");
                    setting.fps = r.value().get<double>();
                } else if (r.key() == "exposure_us") {
                    if (r.value().is_string()) {
                        if (r.value().get<std::string>() != "auto") return fail("run.exposure_us: expected \"auto\" or a number of microseconds");
                        setting.autoExposure = true;
                    } else if (r.value().is_number()) {
                        setting.autoExposure = false;
                        setting.exposureUs = r.value().get<double>();
                    } else {
                        return fail("run.exposure_us: expected \"auto\" or a number of microseconds");
                    }
                } else {
                    return fail(unknownKey("run", r.key()));
                }
            }
        } else {
            return fail(unknownKey("document", it.key()));
        }
    }
    if (auto bad = checkRunSetting(setting, strobeDelayUs, strobeWidthUs); !bad.empty()) return fail(bad);
    CameraDocumentResult ok;
    ok.ok = true;
    ok.setting = setting;
    return ok;
}

} // namespace backend::pz
