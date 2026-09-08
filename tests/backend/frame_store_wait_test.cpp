// frame_store_wait_test
//
// Coverage for FrameStore::waitForFrame (issue #282 — event-driven realtime
// wake replacing the consumer's fixed sleep-poll):
//   * returns immediately when newer frames already exist
//   * times out (returns unchanged total) when nothing is pushed
//   * a blocked waiter wakes promptly when a frame is pushed
//   * lost-wakeup stress: a paced producer and a wait-loop consumer never
//     stall — every pushed frame is observed well before the poll-equivalent
//     deadline, across many iterations that race registration vs. push

#include "backend/playback/FrameStore.h"

#include "support/watchdog.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

using backend::playback::FrameStore;
using Clock = std::chrono::steady_clock;

namespace {

int failures = 0;

#define CHECK_MSG(cond, msg)                                                                       \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << " " << #cond << " — " << msg    \
                      << "\n";                                                                     \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

#define CHECK(cond) CHECK_MSG(cond, "")

constexpr uint64_t kMono8 = 0x01080001ULL;

void push(FrameStore& store, uint64_t ts) {
    std::vector<uint8_t> buf(64 * 16, 0x40);
    store.pushFrame(buf.data(), buf.size(), 64, 16, 64, kMono8, ts);
}

} // namespace

int main() {
    mib::test::Watchdog watchdog(30);
    FrameStore store(128);

    // --- immediate return when newer frames already exist ---
    watchdog.mark("immediate");
    push(store, 1);
    push(store, 2);
    const auto t0 = Clock::now();
    const uint64_t total = store.waitForFrame(0, std::chrono::milliseconds(500));
    const double immediateMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    CHECK(total == 2);
    CHECK_MSG(immediateMs < 100.0, "took " << immediateMs << " ms");

    // --- timeout with no push returns unchanged total ---
    watchdog.mark("timeout");
    const auto t1 = Clock::now();
    const uint64_t afterTimeout = store.waitForFrame(2, std::chrono::milliseconds(20));
    const double timeoutMs = std::chrono::duration<double, std::milli>(Clock::now() - t1).count();
    CHECK(afterTimeout == 2);
    CHECK_MSG(timeoutMs >= 15.0, "returned after only " << timeoutMs << " ms");

    // --- blocked waiter wakes on push ---
    watchdog.mark("wake-on-push");
    std::atomic<bool> woke{false};
    std::atomic<uint64_t> observed{0};
    std::thread waiter([&] {
        observed.store(store.waitForFrame(2, std::chrono::seconds(5)), std::memory_order_relaxed);
        woke.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // let it block
    CHECK(!woke.load(std::memory_order_acquire));
    const auto pushAt = Clock::now();
    push(store, 3);
    waiter.join();
    const double wakeMs = std::chrono::duration<double, std::milli>(Clock::now() - pushAt).count();
    CHECK(woke.load(std::memory_order_acquire));
    CHECK(observed.load(std::memory_order_relaxed) == 3);
    // Well under the 5 s timeout proves the wake came from the push, not the
    // timeout; generous bound for loaded CI machines.
    CHECK_MSG(wakeMs < 500.0, "wake took " << wakeMs << " ms");

    // --- lost-wakeup stress: registration races the push ---
    watchdog.mark("stress");
    constexpr uint64_t kFrames = 2000;
    std::atomic<bool> producing{true};
    std::thread producer([&] {
        for (uint64_t i = 0; i < kFrames && producing.load(std::memory_order_relaxed); ++i) {
            push(store, 100 + i);
            // Vary pacing so pushes land before, during, and after the
            // consumer's waiter registration.
            if ((i & 7) == 0) std::this_thread::sleep_for(std::chrono::microseconds(200));
            if ((i & 63) == 0) std::this_thread::yield();
        }
    });

    uint64_t seen = store.totalWritten();
    const uint64_t targetTotal = 3 + kFrames;
    const auto stressDeadline = Clock::now() + std::chrono::seconds(20);
    while (seen < targetTotal && Clock::now() < stressDeadline) {
        watchdog.mark("stress-loop");
        // Short timeout mirrors the realtime loop's usage; the wait itself
        // must never be the reason we miss the deadline.
        seen = store.waitForFrame(seen, std::chrono::milliseconds(2));
    }
    producing.store(false, std::memory_order_relaxed);
    producer.join();
    CHECK_MSG(seen >= targetTotal,
              "consumer observed " << seen << " of " << targetTotal << " frames");

    if (failures == 0) {
        std::cout << "frame_store_wait_test: OK\n";
        return 0;
    }
    std::cerr << "frame_store_wait_test: " << failures << " failure(s)\n";
    return 1;
}
