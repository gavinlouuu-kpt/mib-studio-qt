#include "backend/app/AppBackend.h"
#include "backend/services/CaptureLifecycle.h"
#include "backend/services/CaptureService.h"
#include "frontend/controllers/CameraController.h"
#include "support/tempdir.h"

#include <QApplication>
#include <QByteArray>

#include <chrono>
#include <cstdio>
#include <thread>

using namespace std::chrono_literals;

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    qputenv("MIB_CAMERA_MODE", QByteArrayLiteral("mindvision"));
    qputenv("MIB_MINDVISION_CAMERA_INDEX", QByteArrayLiteral("0"));
    qputenv("MIB_DISABLED_SERVICES", QByteArrayLiteral("sqlite,hdf5,processing,yolo,autofocus,playback"));
    qputenv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", QByteArrayLiteral("file:///nonexistent/mib-lut-manifest.json"));

    QApplication app(argc, argv);
    mib::test::TempDir td("mindvision_lifecycle_50");
    backend::AppBackend backend;
    if (!backend.initialize((td.path() / "data").string())) {
        std::fprintf(stderr, "FAIL backend.initialize\n");
        return 2;
    }
    if (!backend.isCameraConfigured()) {
        std::fprintf(stderr, "FAIL camera not configured\n");
        return 3;
    }

    frontend::CameraController controller(backend);
    controller.setPollIntervalMs(20);
    std::uint64_t previousGeneration = 0;
    std::uint64_t previousFrames = 0;
    int immediateStops = 0;

    for (int cycle = 1; cycle <= 50; ++cycle) {
        const auto cycleStart = std::chrono::steady_clock::now();
        const auto startResult = controller.requestStart();
        if (!startResult.accepted()) {
            std::fprintf(stderr, "FAIL cycle=%d start outcome=%d message=%s\n", cycle,
                         static_cast<int>(startResult.outcome), startResult.message.toUtf8().constData());
            return 10 + cycle;
        }
        const auto state = backend.capture().waitForState(
            {backend::services::CaptureLifecycleState::Running,
             backend::services::CaptureLifecycleState::Faulted}, 15s);
        auto snap = backend.capture().lifecycleSnapshot();
        if (state != backend::services::CaptureLifecycleState::Running || !snap.cameraReady) {
            std::fprintf(stderr, "FAIL cycle=%d did-not-run state=%s failure=%s\n", cycle,
                         backend::services::toString(state), snap.lastFailureMessage.c_str());
            return 70 + cycle;
        }
        if (snap.generation != previousGeneration + 1) {
            std::fprintf(stderr, "FAIL cycle=%d generation=%llu previous=%llu\n", cycle,
                         static_cast<unsigned long long>(snap.generation),
                         static_cast<unsigned long long>(previousGeneration));
            return 130 + cycle;
        }

        if (cycle <= 5) {
            ++immediateStops;
            std::this_thread::sleep_for(1ms);
        } else {
            const auto frameDeadline = std::chrono::steady_clock::now() + 5s;
            while (std::chrono::steady_clock::now() < frameDeadline &&
                   backend.capture().stats().framesProcessed.load() <= previousFrames) {
                QCoreApplication::processEvents();
                std::this_thread::sleep_for(5ms);
            }
            if (backend.capture().stats().framesProcessed.load() <= previousFrames) {
                std::fprintf(stderr, "FAIL cycle=%d no-new-frame\n", cycle);
                return 190 + cycle;
            }
        }

        const auto stopStart = std::chrono::steady_clock::now();
        const auto stopResult = controller.requestStop();
        const auto stopMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - stopStart).count();
        if (!stopResult.accepted()) {
            std::fprintf(stderr, "FAIL cycle=%d stop outcome=%d message=%s\n", cycle,
                         static_cast<int>(stopResult.outcome), stopResult.message.toUtf8().constData());
            return 250 + cycle;
        }
        snap = backend.capture().lifecycleSnapshot();
        if (snap.state != backend::services::CaptureLifecycleState::Idle || snap.cameraReady) {
            std::fprintf(stderr, "FAIL cycle=%d post-stop state=%s ready=%d\n", cycle,
                         backend::services::toString(snap.state), snap.cameraReady ? 1 : 0);
            return 310 + cycle;
        }
        const auto cycleMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - cycleStart).count();
        previousGeneration = snap.generation;
        previousFrames = backend.capture().stats().framesProcessed.load();
        std::printf("cycle=%d generation=%llu frames=%llu stop_ms=%lld cycle_ms=%lld immediate=%d\n",
                    cycle, static_cast<unsigned long long>(snap.generation),
                    static_cast<unsigned long long>(previousFrames),
                    static_cast<long long>(stopMs), static_cast<long long>(cycleMs), cycle <= 5 ? 1 : 0);
        std::fflush(stdout);
        if (stopMs > 6000) {
            std::fprintf(stderr, "FAIL cycle=%d stop exceeded 6000 ms\n", cycle);
            return 370 + cycle;
        }
    }

    std::printf("PASS cycles=50 generations=%llu immediate_stop_attempts=%d final_state=%s\n",
                static_cast<unsigned long long>(previousGeneration), immediateStops,
                backend::services::toString(backend.capture().lifecycleSnapshot().state));
    return 0;
}
