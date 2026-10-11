#include "backend/pz/PzCameraDocument.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <set>
#include <vector>

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

std::string unknownKey(const std::string& where, const std::string& rawKey) {
    const std::string key = rawKey.size() > 40 ? rawKey.substr(0, 40) + "..." : rawKey;
    if (looksLikeStrobe(rawKey)) return where + ": \"" + key + "\" is not accepted: the LED strobe is Service-only and not part of the camera document";
    return where + ": unknown key \"" + key + "\"";
}

} // namespace

CameraDocumentResult parseCameraDocument(const std::string& text, double strobeDelayUs, double strobeWidthUs) {
    if (text.size() > kCameraDocumentMaxBytes) return fail("the camera document exceeds 64 KiB");
    nlohmann::json doc;
    // Duplicate keys are errors: nlohmann keeps the last one silently, so a repeated frame_rate_hz would apply the one the operator did not look at.
    std::vector<std::set<std::string>> keys;
    std::string duplicate;
    const auto callback = [&](int, nlohmann::json::parse_event_t event, nlohmann::json& parsed) {
        using E = nlohmann::json::parse_event_t;
        if (event == E::object_start) keys.emplace_back();
        else if (event == E::object_end && !keys.empty()) keys.pop_back();
        else if (event == E::key && !keys.empty() && !keys.back().insert(parsed.get<std::string>()).second && duplicate.empty()) duplicate = parsed.get<std::string>();
        return true;
    };
    try {
        doc = nlohmann::json::parse(text, callback);
    } catch (const nlohmann::json::parse_error& e) {
        return fail("not valid JSON (at byte " + std::to_string(e.byte) + ")"); // never echo the file's bytes
    } catch (const std::exception&) {
        return fail("not valid JSON");
    }
    if (!duplicate.empty()) return fail("duplicate key \"" + (duplicate.size() > 40 ? duplicate.substr(0, 40) : duplicate) + "\"");
    if (!doc.is_object()) return fail("the camera document must be a JSON object");
    RunSetting setting;
    bool haveSchema = false, haveRate = false;
    for (auto it = doc.begin(); it != doc.end(); ++it) {
        if (it.key() == "schema") {
            if (!it.value().is_string() || it.value().get<std::string>() != "pz7035-camera/1")
                return fail("schema: expected \"pz7035-camera/1\"");
            haveSchema = true;
        } else if (it.key() == "run") {
            if (!it.value().is_object()) return fail("run: expected an object");
            for (auto r = it.value().begin(); r != it.value().end(); ++r) {
                if (r.key() == "frame_rate_hz") {
                    if (!r.value().is_number()) return fail("run.frame_rate_hz: expected a number of frames per second");
                    setting.fps = r.value().get<double>();
                    haveRate = true;
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
    if (!haveSchema) return fail("schema: required, expected \"pz7035-camera/1\"");
    if (!haveRate) return fail("run.frame_rate_hz: required");
    if (auto bad = checkRunSetting(setting, strobeDelayUs, strobeWidthUs); !bad.empty()) return fail(bad);
    CameraDocumentResult ok;
    ok.ok = true;
    ok.setting = setting;
    return ok;
}

} // namespace backend::pz
