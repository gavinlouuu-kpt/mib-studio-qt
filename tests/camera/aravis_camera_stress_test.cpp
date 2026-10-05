#include "backend/camera/aravis/AravisCamera.h"

#include "support/assert.h"
#include "support/watchdog.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

int main()
{
    mib::test::Watchdog watchdog(60);
    camera::aravis::AravisCameraOptions options;
    options.deviceId = "Fake_1";
    options.useFake = true;
    options.streamBuffers = 4;
    options.popTimeoutMs = 200;
    camera::aravis::AravisCamera camera(options);
    camera::common::CameraConfig config;

    // Exercise the actual overlapping lifecycle entry points, including the
    // case where stop publishes cancellation while start is opening the
    // Fake device. The gate makes the calls concurrent instead of merely
    // doing a stop followed by a start in one thread.
    for (int round = 0; round < 12; ++round) {
        watchdog.mark("overlapping start stop");
        struct StartGate {
            std::mutex mutex;
            std::condition_variable cv;
            bool entered{false};
            bool release{false};
        };
        auto gate = std::make_shared<StartGate>();
        camera::aravis::AravisCameraOptions overlapOptions = options;
        overlapOptions.beforeStartHook = [gate] {
            std::unique_lock<std::mutex> lock(gate->mutex);
            gate->entered = true;
            gate->cv.notify_all();
            gate->cv.wait(lock, [&] { return gate->release; });
        };
        camera::aravis::AravisCamera overlap(overlapOptions);
        overlap.applyConfig(config);
        bool started = false;
        std::thread starter([&] {
            started = overlap.start();
        });
        {
            std::unique_lock<std::mutex> lock(gate->mutex);
            gate->cv.wait(lock, [&] { return gate->entered; });
        }
        // Do not start the stopper until the starter has definitely loaded
        // its generation and entered the barrier; otherwise the scheduler can
        // legitimately run a complete stop before start has begun.
        std::thread stopper([&] { overlap.stop(); });
        // start is now parked before SDK ownership. stop must complete while
        // start is still in progress; releasing the hook then proves the
        // generation-cancellation path deterministically.
        stopper.join();
        {
            std::lock_guard<std::mutex> lock(gate->mutex);
            gate->release = true;
        }
        gate->cv.notify_all();
        starter.join();
        // Assert before cleanup so a stale-running result cannot be masked by
        // a corrective stop(). If it is running, probe it while resources are
        // still live, then clean it up for the next round.
        const bool runningAfterOverlap = overlap.isRunning();
        MIB_EXPECT(!runningAfterOverlap, "overlapping start/stop leaves camera stopped");
        if (runningAfterOverlap) {
            camera::common::Frame probe;
            MIB_EXPECT(overlap.grabFrame(probe),
                       "a camera left running after overlap remains usable before cleanup");
        }
        overlap.stop();
        if (!started)
            MIB_EXPECT(overlap.lastFailure().code.empty(), "a losing start does not leave a spurious failure");
    }

    for (int round = 0; round < 3; ++round) {
        watchdog.mark("stress start");
        camera.applyConfig(config);
        MIB_REQUIRE(camera.start(), "stress camera starts");
        std::atomic<bool> grabbing{true};
        std::thread grabber([&] {
            while (grabbing.load(std::memory_order_acquire)) {
                camera::common::Frame frame;
                camera.grabFrame(frame);
            }
        });

        std::vector<std::thread> stoppers;
        for (int i = 0; i < 3; ++i) {
            stoppers.emplace_back([&] {
                watchdog.mark("concurrent stop");
                camera.stop();
            });
        }
        for (auto& stopper : stoppers)
            stopper.join();
        grabbing.store(false, std::memory_order_release);
        grabber.join();
        MIB_EXPECT(!camera.isRunning(), "concurrent stop leaves camera stopped");

        watchdog.mark("stress restart");
        MIB_REQUIRE(camera.start(), "stress camera restarts");
        camera::common::Frame frame;
        bool delivered = false;
        for (int attempt = 0; attempt < 5 && !delivered; ++attempt)
            delivered = camera.grabFrame(frame);
        MIB_EXPECT(delivered, "restarted stress camera delivers");
        camera.stop();

    }
    return mib::test::exitCode();
}
