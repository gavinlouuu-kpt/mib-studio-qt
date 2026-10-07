// frame_store_resize_under_load_test
//
// Invariant guard: resizing the FrameStore while producers and consumers are
// actively pushing/reading must never yield a torn frame or deadlock. The
// existing concurrency test only resizes after the producer stops; this exercises
// resize concurrent with live IO (the structural exclusive lock vs the hot-path
// shared lock). A watchdog fails fast if resize deadlocks.

#include "backend/playback/FrameStore.h"

#include "support/assert.h"
#include "support/watchdog.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

using backend::playback::FrameStore;
using backend::playback::Frame;

namespace {

constexpr int kW = 64;
constexpr int kH = 64;
constexpr size_t kBytes = static_cast<size_t>(kW * kH);

// Producer writes each frame as a uniform value keyed off its timestamp, so any
// consumer read must observe a single uniform byte == timestamp % 251.
bool frameConsistent(const Frame& f)
{
    if (f.data.size() < kBytes) return false;
    const uint8_t expected = static_cast<uint8_t>(f.timestamp % 251);
    for (uint8_t b : f.data) {
        if (b != expected) return false;
    }
    return true;
}

} // namespace

int main()
{
    mib::test::Watchdog wd(20);
    wd.mark("setup");

    FrameStore store(64);
    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};

    std::thread producer([&] {
        std::vector<uint8_t> buf(kBytes);
        uint64_t ts = 1;
        while (!stop.load(std::memory_order_relaxed)) {
            const uint8_t v = static_cast<uint8_t>(ts % 251);
            std::fill(buf.begin(), buf.end(), v);
            store.pushFrame(buf.data(), buf.size(), kW, kH, kW,
                            /*pixelFormat=*/0x01080001ULL, ts);
            ++ts;
        }
    });

    auto consumer = [&] {
        Frame f;
        while (!stop.load(std::memory_order_relaxed) && !failed.load(std::memory_order_relaxed)) {
            if (store.getLatest(f) && !frameConsistent(f)) {
                failed.store(true);
                break;
            }
        }
    };
    std::vector<std::thread> consumers;
    for (int i = 0; i < 3; ++i) consumers.emplace_back(consumer);

    // Resize repeatedly while IO is live. Run to a resize count, not a fixed
    // time: on a loaded runner (Windows CI) resize() can wait out the producer
    // and consumers long enough that a time window yields only a handful.
    const size_t caps[] = {64, 128, 256, 96, 512};
    constexpr int kTargetResizes = 25;
    int resizes = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    for (int i = 0; resizes < kTargetResizes && std::chrono::steady_clock::now() < deadline; ++i) {
        wd.mark("resize");
        MIB_EXPECT(store.resize(caps[i % 5]), "resize succeeds under load");
        ++resizes;
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }

    wd.mark("teardown");
    stop.store(true);
    producer.join();
    for (auto& t : consumers) t.join();

    MIB_EXPECT(!failed.load(), "no torn frame observed during resize under load");
    MIB_EXPECT(resizes == kTargetResizes, "performed every resize under load");

    // Final sanity: store still works after the churn.
    std::vector<uint8_t> buf(kBytes, 7);
    store.pushFrame(buf.data(), buf.size(), kW, kH, kW, 0x01080001ULL, 7);
    Frame f;
    MIB_EXPECT(store.getLatest(f) && frameConsistent(f), "store usable after resize churn");

    if (mib::test::exitCode() == 0) {
        std::printf("FrameStore survived %d resizes under live producer/consumer load\n", resizes);
    }
    return mib::test::exitCode();
}
