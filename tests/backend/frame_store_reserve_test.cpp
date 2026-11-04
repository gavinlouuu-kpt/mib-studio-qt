// frame_store_reserve_test
//
// reserveFrameBytes sizes ring slots so the pushFrame hot path is
// allocation-free for frames of that size or smaller. Verifies reservation +
// round-trip correctness, that oversize frames still grow correctly, and (#839)
// that the reservation follows the frame size down: after an Overview-sized
// session (large frames) a smaller reservation releases the excess, so the
// retained bytes are about slots x the small size, not slots x the large one.

#include "backend/playback/FrameStore.h"

#include "support/assert.h"
#include "support/watchdog.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using backend::playback::Frame;
using backend::playback::FrameReadOutcome;
using backend::playback::FrameStore;

namespace {

constexpr uint64_t kFmt = 0x01080001u;

// The fill byte of frame `index`: a read can tell a frame is whole and is the
// one it asked for.
uint8_t fillFor(uint64_t index)
{
    return static_cast<uint8_t>(index % 251 + 1);
}

// Pushes a `bytes`-byte frame whose timestamp is index + 1 (width x height
// must equal bytes; the line pitch is the width).
void pushFilled(FrameStore& fs, size_t bytes, uint64_t index, uint64_t width, uint64_t height)
{
    std::vector<uint8_t> src(bytes, fillFor(index));
    fs.pushFrame(src.data(), src.size(), width, height, width, kFmt, index + 1);
}

bool wholeFrame(const Frame& f, size_t bytes, uint64_t index)
{
    if (f.data.size() != bytes) return false;
    const uint8_t v = fillFor(index);
    return std::all_of(f.data.begin(), f.data.end(), [v](uint8_t b) { return b == v; });
}

} // namespace

int main()
{
    mib::test::Watchdog wd(60);
    wd.mark("basic");

    const size_t cap = 8;
    const size_t bytes = 64 * 64; // 4096

    FrameStore fs(cap);
    fs.reserveFrameBytes(bytes);

    std::vector<uint8_t> src(bytes, 7);
    // Push two full rings worth; data of size == reserved must round-trip and
    // the ring caps at capacity.
    for (size_t i = 0; i < cap * 2; ++i) {
        fs.pushFrame(src.data(), src.size(), 64, 64, 64, kFmt, i + 1);
    }
    MIB_REQUIRE(fs.totalWritten() == cap * 2, "all frames pushed");
    MIB_REQUIRE(fs.availableCount() == cap, "ring capped at capacity");

    Frame out;
    MIB_REQUIRE(fs.getByWriteIndex(cap * 2 - 1, out), "latest frame retrievable");
    MIB_EXPECT(out.data.size() == bytes && out.data[0] == 7, "frame data intact");

    // A larger-than-reserved frame must still work (grows that slot).
    std::vector<uint8_t> big(bytes * 2, 9);
    fs.pushFrame(big.data(), big.size(), 128, 64, 128, kFmt, 999);
    MIB_REQUIRE(fs.getByWriteIndex(cap * 2, out), "oversize frame retrievable");
    MIB_EXPECT(out.data.size() == bytes * 2 && out.data[0] == 9, "oversize frame intact");

    // Reserving the same size twice is idempotent: the second call changes
    // nothing and keeps the stored frames.
    fs.reserveFrameBytes(bytes * 2);
    {
        const auto before = fs.memoryStats();
        fs.reserveFrameBytes(bytes * 2);
        MIB_EXPECT(fs.memoryStats().currentBytes == before.currentBytes, "same-size reserve changes nothing");
        MIB_REQUIRE(fs.getByWriteIndex(cap * 2, out) && out.data.size() == bytes * 2, "frame kept");
    }

    // A smaller reservation re-sizes the slots (#839); pushes of the old size
    // still round-trip afterwards (the slot grows again, as for any oversize frame).
    fs.reserveFrameBytes(16);
    fs.pushFrame(src.data(), src.size(), 64, 64, 64, kFmt, 1000);
    MIB_REQUIRE(fs.getByWriteIndex(cap * 2 + 1, out), "post-reserve push retrievable");
    MIB_EXPECT(out.data.size() == bytes && out.data[0] == 7, "push after a smaller reserve is intact");

    // ---- #839: an Overview-sized session, then a small experiment window ----------
    wd.mark("shrink");
    {
        constexpr size_t slots = 64;
        constexpr size_t large = 256 * 1024; // the Overview frame (512 x 512 here)
        constexpr size_t small = 16 * 1024;  // the experiment window (128 x 128)
        FrameStore store(slots);
        store.reserveFrameBytes(large);
        for (uint64_t i = 0; i < slots * 2; ++i) pushFilled(store, large, i, 512, 512);
        auto ms = store.memoryStats();
        MIB_EXPECT(ms.currentBytes >= slots * large && ms.capacityBytes == slots * large,
                   "the Overview session retains slots x the large frame");
        const uint64_t largePeak = ms.peakBytes;
        MIB_EXPECT(largePeak >= slots * large, "the peak covers the large ring");

        // A new session with a smaller frame: the excess is released.
        store.reserveFrameBytes(small);
        ms = store.memoryStats();
        MIB_EXPECT(ms.currentBytes >= slots * small && ms.currentBytes <= slots * small * 5 / 4,
                   "retained bytes drop to about slots x the small frame, not slots x the large one");
        MIB_EXPECT(ms.capacityBytes == slots * small, "the declared bound follows the reservation down");
        MIB_EXPECT(ms.peakBytes == largePeak, "the peak stays the store's high-water mark");
        MIB_EXPECT(ms.currentCount == slots, "the ring still counts its slots");

        // The stale large frames are gone: a read says so instead of returning them.
        Frame f;
        MIB_EXPECT(store.readByWriteIndex(slots * 2 - 1, f) == FrameReadOutcome::Malformed,
                   "a stale larger frame reads Malformed");
        MIB_EXPECT(!store.getLatest(f), "the stale latest is not served as a frame");

        // New frames store and read correctly, with no regrowth of the slots.
        const uint64_t afterShrink = ms.currentBytes;
        for (uint64_t i = slots * 2; i < slots * 4; ++i) pushFilled(store, small, i, 128, 128);
        ms = store.memoryStats();
        MIB_EXPECT(ms.currentBytes == afterShrink, "pushing frames of the new size does not regrow the slots");
        bool allWhole = true;
        for (uint64_t i = slots * 3; i < slots * 4; ++i)
            allWhole = allWhole && store.getByWriteIndex(i, f) && wholeFrame(f, small, i);
        MIB_EXPECT(allWhole, "frames of the new size read back whole");
        MIB_REQUIRE(store.getLatest(f), "latest readable");
        MIB_EXPECT(wholeFrame(f, small, slots * 4 - 1) && f.width == 128 && f.height == 128,
                   "the latest is the newest frame");

        // The same size again is a no-op; a larger frame grows the slots back.
        store.reserveFrameBytes(small);
        MIB_EXPECT(store.memoryStats().currentBytes == afterShrink, "same-size reserve is a no-op");
        store.reserveFrameBytes(large);
        MIB_EXPECT(store.memoryStats().currentBytes >= slots * large, "a larger frame grows the slots back");
        MIB_REQUIRE(store.getByWriteIndex(slots * 4 - 1, f), "frames survive growth");
        MIB_EXPECT(wholeFrame(f, small, slots * 4 - 1), "and are intact");
    }

    // ---- stored frames that still fit survive the shrink --------------------------
    wd.mark("keep fitting frames");
    {
        constexpr size_t slots = 16;
        FrameStore store(slots);
        store.reserveFrameBytes(256 * 1024);
        for (uint64_t i = 0; i < slots; ++i) pushFilled(store, 1000, i, 100, 10);
        store.reserveFrameBytes(8 * 1024); // the slots hold 256 KiB each, the frames are 1000 bytes
        const auto ms = store.memoryStats();
        MIB_EXPECT(ms.currentBytes >= slots * 8 * 1024 && ms.currentBytes <= slots * 8 * 1024 * 5 / 4,
                   "the excess capacity is released");
        Frame f;
        bool kept = true;
        for (uint64_t i = 0; i < slots; ++i) kept = kept && store.getByWriteIndex(i, f) && wholeFrame(f, 1000, i);
        MIB_EXPECT(kept, "frames within the new size are carried over intact");
    }

    // ---- a large release is freed off the calling thread ------------------------
    // 8 slots of 16 MiB shrunk to 64 KiB release ~127 MiB, above the size that is freed on its
    // own thread: the call returns with the accounting final, the store usable at once.
    wd.mark("large release");
    {
        constexpr size_t slots = 8;
        constexpr size_t large = 16 * 1024 * 1024; // 4096 x 4096
        constexpr size_t small = 64 * 1024;        // 256 x 256
        FrameStore store(slots);
        store.reserveFrameBytes(large);
        for (uint64_t i = 0; i < slots; ++i) pushFilled(store, large, i, 4096, 4096);
        MIB_EXPECT(store.memoryStats().currentBytes >= slots * large, "the large ring is retained");
        store.reserveFrameBytes(small);
        const auto ms = store.memoryStats();
        MIB_EXPECT(ms.currentBytes >= slots * small && ms.currentBytes <= slots * small * 5 / 4,
                   "the accounting is final when the call returns");
        for (uint64_t i = slots; i < slots * 2; ++i) pushFilled(store, small, i, 256, 256);
        Frame f;
        bool whole = true;
        for (uint64_t i = slots; i < slots * 2; ++i) whole = whole && store.getByWriteIndex(i, f) && wholeFrame(f, small, i);
        MIB_EXPECT(whole, "the store works at once, whatever the release thread is still doing");
    }

    // ---- under load: the reservation flipping between sizes while IO is live ------
    // A producer and consumers run against a store whose reservation flips
    // between a large and a small size. Every frame a consumer gets must be
    // whole (never torn by the re-allocation), nothing deadlocks (watchdog),
    // and the retained accounting ends consistent with the slots.
    wd.mark("under load");
    {
        constexpr size_t slots = 64;
        constexpr size_t large = 128 * 1024;
        constexpr size_t small = 8 * 1024;
        FrameStore store(slots);
        store.reserveFrameBytes(large);
        std::atomic<bool> stop{false};
        std::atomic<bool> torn{false};
        std::thread producer([&] {
            uint64_t i = 0;
            std::vector<uint8_t> buf(small);
            while (!stop.load(std::memory_order_relaxed)) {
                std::fill(buf.begin(), buf.end(), fillFor(i));
                store.pushFrame(buf.data(), buf.size(), 128, 64, 128, kFmt, i + 1);
                ++i;
            }
        });
        auto consumer = [&] {
            Frame f;
            while (!stop.load(std::memory_order_relaxed) && !torn.load(std::memory_order_relaxed)) {
                // The write index of a frame is its timestamp - 1; the fill must match it.
                if (store.getLatest(f) && !wholeFrame(f, small, f.timestamp - 1)) torn.store(true);
            }
        };
        std::vector<std::thread> consumers;
        for (int i = 0; i < 3; ++i) consumers.emplace_back(consumer);
        for (int round = 0; round < 200; ++round) {
            store.reserveFrameBytes(round % 2 == 0 ? small : large);
            std::this_thread::yield();
        }
        stop.store(true);
        producer.join();
        for (auto& t : consumers) t.join();
        MIB_EXPECT(!torn.load(), "no consumer saw a torn or short frame while the reservation flipped");
        store.reserveFrameBytes(small);
        const auto ms = store.memoryStats();
        MIB_EXPECT(ms.currentBytes >= slots * small && ms.currentBytes <= slots * small * 5 / 4,
                   "after the flips the retained bytes match slots x the final reservation");
        MIB_EXPECT(ms.capacityBytes == slots * small, "and the bound is the final reservation");
    }

    if (mib::test::exitCode() == 0) {
        std::printf("FrameStore reserveFrameBytes verified\n");
    }
    return mib::test::exitCode();
}
