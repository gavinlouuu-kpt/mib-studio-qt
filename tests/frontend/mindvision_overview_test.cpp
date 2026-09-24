#include "backend/app/AppBackend.h"
#include "backend/camera/mock/MockCamera.h"
#include "frontend/tabs/OverviewTab.h"
#include "frontend/core/MainWindow.h"
#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"
#include <QApplication>
#include <QSettings>
#include <QSpinBox>
#include "backend/playback/FrameStore.h"
#include "backend/processing/ProcessingService.h"
#include "backend/services/CaptureService.h"
#include "backend/app/ExperimentCoordinator.h"
#include "frontend/utils/SimpleImageCanvas.h"
#include <fstream>
#include <nlohmann/json.hpp>

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("MIB_CAMERA_MODE", "mock");
    qputenv("MIB_DISABLED_SERVICES",
            "auto_update,autofocus,trigger,yolo,syringe_pump,pulse_generator");
    QApplication app(argc, argv);
    mib::test::Watchdog watchdog(90);
    mib::test::TempDir td("mindvision_overview");
    QCoreApplication::setOrganizationName("mib_overview_test");
    QCoreApplication::setApplicationName("mib_overview_test");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       QString::fromStdString(td.path().string()));
    const auto path = td / "camera.json";
    const nlohmann::json original = {{"width", 512},     {"height", 96},
                                     {"offset_x", 32},   {"offset_y", 24},
                                     {"analog_gain", 3}, {"custom_note", "preserve me"}};
    std::ofstream(path) << original.dump(2);
    const auto js = td / "experiment.js";
    std::ofstream(js) << "// untouched eGrabber profile\n";
    QSettings().setValue("Config/ExternalCameraScriptPath", QString::fromStdString(js.string()));
    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize((td / "data").string()), "initialize");
    std::string error;
    MIB_REQUIRE(backend.stageMindVisionConfigFromFile(path.string(), &error), error.c_str());
    backend.setMindVisionCameraSelection(0, "Test camera");
    // Re-stage after selection so the UI-only test also runs in SDK-free builds.
    MIB_REQUIRE(backend.stageMindVisionConfigFromFile(path.string(), &error), error.c_str());
    frontend::OverviewTab overview(backend);
    MIB_REQUIRE(overview.roiPosition() == QPointF(32, 24), "MindVision ROI loads from JSON");
    MIB_REQUIRE(QMetaObject::invokeMethod(&overview, "onRoiPositionChanged", Qt::DirectConnection,
                                          Q_ARG(QPointF, QPointF(64, 48))),
                "drag ROI");
    const auto saved = nlohmann::json::parse(std::ifstream(path));
    MIB_EXPECT(saved.at("offset_x") == 64 && saved.at("offset_y") == 48,
               "ROI persists in MindVision JSON");
    MIB_EXPECT(saved.at("width") == 512 && saved.at("height") == 96,
               "default experiment dimensions");
    MIB_EXPECT(saved.at("custom_note") == original.at("custom_note"), "unrelated fields survive");
    std::ifstream jsFile(js);
    const std::string jsBytes((std::istreambuf_iterator<char>(jsFile)), {});
    MIB_EXPECT(jsBytes == "// untouched eGrabber profile\n", "eGrabber profile unchanged");
    auto* width = overview.findChild<QSpinBox*>("overviewRoiWidth");
    MIB_REQUIRE(width, "editable width control");
    width->setValue(640);
    MIB_EXPECT(nlohmann::json::parse(std::ifstream(path)).at("width") == 640,
               "editable width persists");
    frontend::OverviewTab reopened(backend);
    MIB_EXPECT(reopened.roiWidth() == 640 && reopened.roiPosition() == QPointF(64, 48),
               "ROI round trip");
    const auto before = backend.getFrameStore();
    const uint8_t pixel = 5;
    before->pushFrame(&pixel, 1, 1, 1, 1, 0, 1);
    MIB_REQUIRE(backend.setMindVisionOverview(true, &error), error.c_str());
    MIB_EXPECT(!backend.capture().isRunning(), "idle navigation never starts hardware");
    MIB_EXPECT(backend.getFrameStore()->capacity() == 8 &&
                   backend.getFrameStore()->availableCount() == 0,
               "bounded overview starts with an empty store");
    const auto readiness = backend.experiment().evaluateReadiness();
    bool modeGate = false;
    for (const auto& gate : readiness.gates)
        if (gate.id == "camera.mode") modeGate = true;
    MIB_EXPECT(modeGate, "overview cannot start an experiment");
    MIB_REQUIRE(backend.setMindVisionOverview(false, &error), error.c_str());
    MIB_EXPECT(backend.getFrameStore()->capacity() == before->capacity(),
               "experiment capacity restored");
    const auto roi = backend.processing().getRealtimeRoi();
    MIB_EXPECT(roi.x == 0 && roi.y == 0 && roi.w == 640 && roi.h == 96,
               "processing uses crop-local coordinates");
    MIB_EXPECT(!backend.saveMindVisionRoi(-1, 0, 512, 96, &error), "negative offset rejected");
    MIB_REQUIRE(backend.setMindVisionOverview(true, &error), error.c_str());
    backend.configureMockCamera(camera::mock::MockCameraOptions{});
    MIB_EXPECT(!backend.isMindVisionOverview() &&
                   backend.getFrameStore()->capacity() == before->capacity(),
               "changing provider restores normal buffer capacity");
    backend.setMindVisionCameraSelection(0, "Test camera");
    {
        QSettings().setValue("Config/ExternalMindVisionConfigPath",
                             QString::fromStdString(path.string()));
        MainWindow window(backend);
        MIB_REQUIRE(
            QMetaObject::invokeMethod(&window, "onTabChanged", Qt::DirectConnection, Q_ARG(int, 1)),
            "navigate to Overview");
        MIB_EXPECT(backend.isMindVisionOverview(), "Overview navigation stages MindVision mode");
        MIB_EXPECT(!backend.capture().isRunning() && !backend.processing().isRealtimeEnabled(),
                   "Overview navigation remains idle with processing disabled");
        MIB_REQUIRE(
            QMetaObject::invokeMethod(&window, "onTabChanged", Qt::DirectConnection, Q_ARG(int, 2)),
            "navigate to Experiment");
        MIB_EXPECT(!backend.isMindVisionOverview(), "Experiment navigation restores crop mode");
        MIB_EXPECT(!backend.capture().isRunning() && backend.getFrameStore()->availableCount() == 0,
                   "Experiment navigation remains idle with no stale overview frames");
    }
    std::filesystem::remove(path);
    std::filesystem::create_directory(path);
    MIB_REQUIRE(QMetaObject::invokeMethod(&overview, "onRoiPositionChanged", Qt::DirectConnection,
                                          Q_ARG(QPointF, QPointF(100, 100))),
                "failed save drag");
    MIB_EXPECT(overview.roiPosition() == QPointF(64, 48), "failed save restores previous overlay");
    // A flipped full-sensor image still selects physical sensor coordinates.
    QImage image(1280, 1024, QImage::Format_Grayscale8);
    auto fit = frontend::OverviewTab::FitMode::FitToWindow;
    bool visible = true;
    QPointF sensorPos(64, 48);
    int w = 512, h = 96;
    frontend::SimpleImageCanvas canvas(&image, &fit, &visible, &sensorPos, &w, &h);
    canvas.setRoiTransform(1, 1, true, true);
    MIB_EXPECT(canvas.displayedRoiPosition() == QPointF(704, 880),
               "flipped ROI overlay maps to sensor coordinates");
    return mib::test::exitCode();
}
