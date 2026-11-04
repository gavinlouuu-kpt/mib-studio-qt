// camera_window_profile_test (#833): the profile's camera window section
// (config.json camera.window) and its precedence inputs.
//  - camera.window parses width/height/offset_x/offset_y and the camera it was
//    made for; a malformed section is refused with the reason (the appliers
//    then change nothing); absent = no window, the script decides;
//  - target_fps is read (it had no reader before #833);
//  - the camera script's Width/Height/OffsetX/OffsetY lines are read the way
//    the Qt editor writes them (comments skipped, last assignment wins);
//  - a window made for another camera model is refused by name.

#include "backend/app/CameraWindowProfile.h"

#include "support/assert.h"

#include <stdexcept>
#include <string>

using backend::app::cameraModelMismatch;
using backend::app::parseAcquisitionProfile;
using backend::app::parseScriptCameraWindow;

namespace {
bool refused(const std::string& json, const char* part)
{
    try {
        (void)parseAcquisitionProfile(json);
    } catch (const std::exception& e) {
        return std::string(e.what()).find(part) != std::string::npos;
    }
    return false;
}
} // namespace

int main()
{
    // ---- the section ------------------------------------------------------------------------------
    {
        const auto p = parseAcquisitionProfile(R"({"target_fps": 5000, "camera": {"frame_delivery_mode": "everyFrame",
            "window": {"width": 1216, "height": 208, "offset_x": 368, "offset_y": 616,
                       "camera_vendor": "SVS-Vistek GmbH", "camera_model": "MC2066"}}})");
        MIB_REQUIRE(p.window.has_value(), "window parsed");
        MIB_EXPECT(p.window->window.width == 1216 && p.window->window.height == 208 &&
                       p.window->window.offsetX == 368 && p.window->window.offsetY == 616,
                   "the rig's window");
        MIB_EXPECT(p.window->cameraVendor == "SVS-Vistek GmbH" && p.window->cameraModel == "MC2066", "its camera");
        MIB_EXPECT(p.targetFps == 5000.0, "target_fps has a reader");
        MIB_EXPECT(backend::app::describeWindow(p.window->window) == "1216x208 at (368, 616)", "described");
    }
    {
        const auto p = parseAcquisitionProfile(R"({"camera": {"frame_delivery_mode": "everyFrame"}})");
        MIB_EXPECT(!p.window && p.targetFps == 0.0, "absent: no window, no rate");
        const auto bare = parseAcquisitionProfile(R"({"camera": {"window": {"width": 512, "height": 96}}})");
        MIB_EXPECT(bare.window && bare.window->window.offsetX == 0 && !bare.window->namesCamera(),
                   "offsets default to 0; a window without its camera parses");
    }
    MIB_EXPECT(refused(R"({"camera": {"window": {"width": 512}}})", "height is required"), "missing height");
    MIB_EXPECT(refused(R"({"camera": {"window": {"width": 512, "height": 0}}})", "height must be"), "zero height");
    MIB_EXPECT(refused(R"({"camera": {"window": {"width": 512, "height": 96, "offset_x": -16}}})", "offset_x"),
               "negative offset");
    MIB_EXPECT(refused(R"({"camera": {"window": {"width": "512", "height": 96}}})", "Expected integer"), "string width");
    MIB_EXPECT(refused(R"({"camera": {"window": [512, 96]}})", "must be an object"), "array section");
    MIB_EXPECT(refused(R"({"target_fps": "fast"})", "target_fps"), "non-numeric target_fps");

    // ---- the camera script's window (precedence input) --------------------------------------------
    {
        const std::string script = R"(var g = grabbers[0];
g.RemotePort.execute("AcquisitionStop");
// g.RemotePort.set("Width", 1920);
g.RemotePort.set("Width", 512);
g.RemotePort.set("Height", 96);
g.RemotePort.set("OffsetY", 500);
g.RemotePort.set("OffsetX", 704);
g.DevicePort.set("CycleMinimumPeriod", "200");
)";
        const auto w = parseScriptCameraWindow(script);
        MIB_REQUIRE(w.has_value(), "default script window");
        MIB_EXPECT(w->width == 512 && w->height == 96 && w->offsetX == 704 && w->offsetY == 500,
                   "512x96 at (704, 500), comment skipped");
        MIB_EXPECT(!parseScriptCameraWindow("g.RemotePort.set(\"Width\", 512);\n"), "no Height: no window");
        const auto last = parseScriptCameraWindow("g.RemotePort.set(\"Width\", 512);\ng.RemotePort.set(\"Height\", 96);\n"
                                                  "g.RemotePort.set(\"Width\", \"1216\");\n");
        MIB_EXPECT(last && last->width == 1216, "the last assignment wins, quoted numbers accepted");
    }

    // ---- camera compatibility ------------------------------------------------------------------------
    {
        backend::app::ProfileCameraWindow p;
        p.window = {1216, 208, 368, 616};
        p.cameraVendor = "SVS-Vistek GmbH";
        p.cameraModel = "MC2066";
        MIB_EXPECT(cameraModelMismatch(p, "SVS-Vistek GmbH", "MC2066").empty(), "same camera");
        MIB_EXPECT(cameraModelMismatch(p, "", "MC2066").empty(), "vendor unknown: the model decides");
        const auto other = cameraModelMismatch(p, "Basler", "acA1920-155um");
        MIB_EXPECT(other.find("MC2066") != std::string::npos && other.find("acA1920-155um") != std::string::npos,
                   "another model is refused by name: " + other);
        MIB_EXPECT(!cameraModelMismatch(p, "Other GmbH", "MC2066").empty(), "same model, other vendor: refused");
        MIB_EXPECT(cameraModelMismatch(p, "", "").empty(), "connected model unknown: not refused");
        p.cameraModel.clear();
        MIB_EXPECT(cameraModelMismatch(p, "Basler", "acA1920-155um").empty(), "a window naming no camera is not refused");
    }
    return mib::test::exitCode();
}
