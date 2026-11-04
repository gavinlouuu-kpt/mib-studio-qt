// camera_window_profile_test (#833): the profile's camera window section
// (config.json camera.window) and its precedence inputs.
//  - camera.window parses width/height/offset_x/offset_y and the camera it was
//    made for; a malformed section is refused with the reason (the appliers
//    then change nothing); absent = no window, the script decides;
//  - target_fps is read (it had no reader before #833);
//  - the camera script's Width/Height/OffsetX/OffsetY lines are read the way
//    the Qt editor writes them (comments skipped, last assignment wins);
//  - a window made for another camera model is refused by name;
//  - (#863) the eGrabber window the Overview edits: snapped as the Qt Overview snaps it, written back
//    into the script's lines without touching the rest of the text, and chosen between the profile
//    and the script;
//  - (#863 A2) the MindVision experiment profile the Overview shows: its saved ROI, whether it is illuminated and
//    its ISP mirroring (the geometry carries flip_horizontal / flip_vertical), and why it cannot be read.

#include "backend/app/CameraWindowProfile.h"

#include "support/assert.h"
#include "support/tempdir.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

using backend::app::cameraModelMismatch;
using backend::app::chooseEgrabberWindow;
using backend::app::rewriteScriptCameraWindow;
using backend::app::snapEgrabberWindow;
using backend::app::WindowSource;
using ::camera::common::CameraWindow;
using backend::app::parseAcquisitionProfile;
using backend::app::parseScriptCameraWindow;

namespace {
// Braced initializers do not survive a macro argument list: build the values in a call.
CameraWindow win(int width, int height, int offsetX, int offsetY) { return CameraWindow{width, height, offsetX, offsetY}; }
std::vector<std::string> strs(std::initializer_list<std::string> items) { return std::vector<std::string>(items); }

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

    // ---- the eGrabber window the Overview edits (#863) ------------------------------------------------
    {
        // Sizes snap DOWN to width 16 / height 4 and clamp to 64..1920 x 16..1080 (OverviewTab::onRoiSizeChanged).
        auto snapped = snapEgrabberWindow({1000, 130, 368, 200});
        MIB_EXPECT(snapped.width == 992 && snapped.height == 128, "1000x130 snaps down to 992x128");
        snapped = snapEgrabberWindow({10, 3, 0, 0});
        MIB_EXPECT(snapped.width == 64 && snapped.height == 16, "sizes are at least 64x16");
        snapped = snapEgrabberWindow({4000, 4000, 0, 0});
        MIB_EXPECT(snapped.width == 1920 && snapped.height == 1080, "sizes are at most the sensor");
        // Offsets snap down (EgrabberConfigParser::snapToStep) to X 16 / Y 4, never below 0.
        snapped = snapEgrabberWindow({512, 96, 719, 503});
        MIB_EXPECT(snapped.offsetX == 704 && snapped.offsetY == 500, "719,503 snaps down to 704,500");
        snapped = snapEgrabberWindow({512, 96, 15, 3});
        MIB_EXPECT(snapped.offsetX == 0 && snapped.offsetY == 0, "15,3 snaps down to 0,0");
        snapped = snapEgrabberWindow({512, 96, -40, -9});
        MIB_EXPECT(snapped.offsetX == 0 && snapped.offsetY == 0, "negative offsets give 0");
        // An offset that would push the window off the sensor is pulled back to the last step that fits.
        snapped = snapEgrabberWindow({512, 96, 1900, 1070});
        MIB_EXPECT(snapped.offsetX == 1408 && snapped.offsetX + snapped.width <= 1920 &&
                       snapped.offsetY == 984 && snapped.offsetY + snapped.height <= 1080,
                   "1900,1070 with 512x96 is pulled back to 1408,984");
        // The size change clamps the offset (Qt: ox + w > 1920 -> ox = ((1920 - w) / 16) * 16).
        snapped = snapEgrabberWindow({1216, 208, 1000, 900});
        MIB_EXPECT(snapped.offsetX == 704 && snapped.offsetY == 872, "1216x208 at 1000,900 fits as 704,872");
        // The rig's profile window is already on the steps and unchanged.
        snapped = snapEgrabberWindow({1216, 208, 368, 616});
        MIB_EXPECT(snapped == win(1216, 208, 368, 616), "the bench window is a fixed point");
        // The camera's own sensor, when the backend knows it.
        snapped = snapEgrabberWindow({4000, 4000, 4000, 4000}, 1936, 1216);
        MIB_EXPECT(snapped.width == 1936 && snapped.height == 1216 && snapped.offsetX == 0 && snapped.offsetY == 0,
                   "a 1936x1216 sensor allows a 1936x1216 window");
        snapped = snapEgrabberWindow({100, 100, 0, 0}, 0, 0);
        MIB_EXPECT(snapped.width == 96 && snapped.height == 100, "an unknown sensor is Qt's 1920x1080");
    }
    {
        // Only the numbers change: indentation, the receiver name, trailing text, comments and line endings stay.
        const std::string script =
            "var g = grabbers[0];\r\n"
            "// g.RemotePort.set(\"Width\", 1920);\r\n"
            "  g.RemotePort.set(\"Width\", 512);   // keep\r\n"
            "g.RemotePort.set(\"Height\", \"96\");\r\n"
            "grab.RemotePort.set( \"OffsetY\" , 500 );\r\n"
            "g.RemotePort.set(\"OffsetX\", 704);\r\n"
            "g.RemotePort.set(\"Width\", 640);\r\n"
            "g.DevicePort.set(\"CycleMinimumPeriod\", \"200\");";
        const auto r = rewriteScriptCameraWindow(script, {1216, 208, 368, 616});
        MIB_EXPECT(r.missing.empty() && r.rewritten.size() == 4, "all four features have an active line");
        const std::string expected =
            "var g = grabbers[0];\r\n"
            "// g.RemotePort.set(\"Width\", 1920);\r\n"
            "  g.RemotePort.set(\"Width\", 1216);   // keep\r\n"
            "g.RemotePort.set(\"Height\", 208);\r\n"
            "grab.RemotePort.set( \"OffsetY\" , 616 );\r\n"
            "g.RemotePort.set(\"OffsetX\", 368);\r\n"
            "g.RemotePort.set(\"Width\", 1216);\r\n"
            "g.DevicePort.set(\"CycleMinimumPeriod\", \"200\");";
        MIB_EXPECT(r.text == expected, "rewritten text:\n" + r.text);
        const auto again = parseScriptCameraWindow(r.text);
        MIB_EXPECT(again && again->width == 1216 && again->height == 208 && again->offsetX == 368 && again->offsetY == 616,
                   "the parser reads back what the rewrite wrote");
        const auto same = rewriteScriptCameraWindow(r.text, {1216, 208, 368, 616});
        MIB_EXPECT(same.text == r.text, "rewriting the same window changes nothing");

        // Qt only rewrites lines that exist; a missing feature is named, nothing is added.
        const auto partial = rewriteScriptCameraWindow("g.RemotePort.set(\"Width\", 512);\n// g.RemotePort.set(\"OffsetX\", 1);\n",
                                                       {64, 16, 16, 4});
        MIB_EXPECT(partial.rewritten == strs({"Width"}) &&
                       partial.missing == strs({"Height", "OffsetX", "OffsetY"}) &&
                       partial.text == "g.RemotePort.set(\"Width\", 64);\n// g.RemotePort.set(\"OffsetX\", 1);\n",
                   "a partial script: only its existing line changed");
        const auto none = rewriteScriptCameraWindow("", {64, 16, 0, 0});
        MIB_EXPECT(none.text.empty() && none.missing.size() == 4, "an empty script stays empty");
    }
    {
        backend::app::ProfileCameraWindow profile;
        profile.window = {1216, 208, 368, 616};
        const CameraWindow script{512, 96, 704, 500};
        auto chosen = chooseEgrabberWindow(profile, script);
        MIB_EXPECT(chosen.source == WindowSource::Profile && chosen.window == profile.window, "the profile wins");
        chosen = chooseEgrabberWindow(std::nullopt, script);
        MIB_EXPECT(chosen.source == WindowSource::Script && chosen.window == script, "else the script's window");
        chosen = chooseEgrabberWindow(std::nullopt, std::nullopt);
        MIB_EXPECT(chosen.source == WindowSource::None, "else none");
        profile.window = {0, 0, 0, 0};
        chosen = chooseEgrabberWindow(profile, script);
        MIB_EXPECT(chosen.source == WindowSource::Script, "an invalid profile window does not win");
        MIB_EXPECT(std::string(toString(WindowSource::Profile)) == "profile" &&
                       std::string(toString(WindowSource::Script)) == "script" &&
                       std::string(toString(WindowSource::None)) == "none",
                   "the names the geometry JSON carries");
    }

    // ---- the applied profile's own Experiment script ------------------------------------------------------
    {
        const std::string applied = R"({"target_fps":5000,"profile_selection":{"name":"lab","path":"/p/profiles/lab/config.json","revision":"r"}})";
        MIB_EXPECT(backend::app::appliedProfileScriptPath(applied) == "/p/profiles/lab/egrabberConfig.js",
                   "the script sits beside the applied profile's config.json");
        MIB_EXPECT(backend::app::appliedProfileScriptPath(R"({"target_fps":5000})").empty(), "a configuration that is not a local profile");
        MIB_EXPECT(backend::app::appliedProfileScriptPath(R"({"profile_selection":{"name":"lab"}})").empty(), "no recorded path");
        MIB_EXPECT(backend::app::appliedProfileScriptPath(R"({"profile_selection":{"path":"config.json"}})").empty(), "a bare file name has no folder");
        MIB_EXPECT(backend::app::appliedProfileScriptPath("not json").empty() && backend::app::appliedProfileScriptPath("").empty() &&
                       backend::app::appliedProfileScriptPath("[1]").empty(),
                   "text that is not a configuration object");
    }

    // ---- the MindVision profile the Overview shows (#863 A2) ----------------------------------------------
    {
        mib::test::TempDir td("camera_window_profile_mv");
        const auto write = [&](const char* name, const std::string& text) {
            const auto path = td / name;
            std::ofstream(path, std::ios::binary) << text;
            return path.string();
        };
        const auto mirrored = backend::app::readMindVisionOverviewProfile(write("mirrored.json",
            R"({"width": 640, "height": 128, "offset_x": 16, "offset_y": 8, "flip_horizontal": true, "flip_vertical": false})"));
        MIB_REQUIRE(mirrored.ok, "a valid profile is read: " + mirrored.error);
        MIB_EXPECT(mirrored.width == 640 && mirrored.height == 128 && mirrored.offsetX == 16 && mirrored.offsetY == 8,
                   "its saved ROI in sensor coordinates");
        MIB_EXPECT(mirrored.flipHorizontal && !mirrored.flipVertical && !mirrored.illuminatedLive,
                   "horizontal mirroring only, not illuminated");
        const auto both = backend::app::readMindVisionOverviewProfile(write("both.json",
            R"({"width": 512, "height": 96, "flip_horizontal": true, "flip_vertical": true})"));
        MIB_EXPECT(both.ok && both.flipHorizontal && both.flipVertical, "both mirrorings");
        const auto plain = backend::app::readMindVisionOverviewProfile(write("plain.json", R"({"width": 512, "height": 96})"));
        MIB_EXPECT(plain.ok && !plain.flipHorizontal && !plain.flipVertical && plain.offsetX == 0 && plain.offsetY == 0,
                   "no flips and offset 0 by default");
        const auto illuminated = backend::app::readMindVisionOverviewProfile(write("lit.json",
            R"({"width":512,"height":96,"exposure_time_us":100,"trigger_mode":2,"ext_trig_signal_type":2,
                "strobe_mode":1,"strobe_pulse_width_us":100,"strobe_polarity":1,"flip_vertical":true,
                "live_view":{"enabled":true,"port":"COM1"}})"));
        MIB_REQUIRE(illuminated.ok, "an illuminated-live profile is read: " + illuminated.error);
        MIB_EXPECT(illuminated.illuminatedLive && illuminated.flipVertical && !illuminated.flipHorizontal,
                   "it says it is illuminated and keeps its vertical mirroring");
        const auto missing = backend::app::readMindVisionOverviewProfile((td / "absent.json").string());
        MIB_EXPECT(!missing.ok && missing.error.find("Cannot read MindVision experiment profile") != std::string::npos &&
                       missing.error.find("absent.json") != std::string::npos,
                   "a missing file is named: " + missing.error);
        const auto broken = backend::app::readMindVisionOverviewProfile(write("broken.json", "{ not json"));
        MIB_EXPECT(!broken.ok && !broken.error.empty() && broken.error.find("Cannot read") == std::string::npos,
                   "an unparseable profile gives the parser's own reason: " + broken.error);
        const auto empty = backend::app::readMindVisionOverviewProfile("");
        MIB_EXPECT(!empty.ok && !empty.error.empty(), "no profile path (none staged yet)");
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
