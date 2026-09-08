// frame_store_commit_test (issue #367)
//
// FrameStore publication contract: reservation (totalWritten) is separate
// from commit (committedCount); a slot is never exposed before its identity
// and data copy complete; indexed reads return typed outcomes
// (Available / NotYetCommitted / Overwritten / Malformed) so a consumer can
// account for every index it claims; getLatest() follows the committed
// identity. The not-yet-committed window is exercised deterministically via
// the commit hook (a barrier between reservation and commit), not timing.

#include "backend/playback/FrameStore.h"

#include "support/assert.h"
#include "support/watchdog.h"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

using backend::playback::Frame;
using backend::playback::FrameReadOutcome;
using backend::playback::FrameStore;

namespace {
void push(FrameStore& s, uint64_t w, uint64_t h, uint8_t fill, size_t pitch = 0)
{
    const size_t stride = pitch ? pitch : static_cast<size_t>(w);
    std::vector<uint8_t> data(stride * h, fill);
    s.pushFrame(data.data(), data.size(), w, h, stride, 0x01080001, fill);
}
} // namespace

int main()
{
    mib::test::Watchdog wd(20);

    // ---- 1. Reservation vs commit counters and typed outcomes ---------------
    {
        wd.mark("basic");
        FrameStore s(4);
        Frame f;
        MIB_EXPECT(s.readByWriteIndex(0, f) == FrameReadOutcome::OutOfRange, "empty store");
        MIB_EXPECT(!s.getLatest(f), "no latest when empty");
        for (int i = 0; i < 3; ++i) push(s, 8, 4, static_cast<uint8_t>(i + 1));
        MIB_EXPECT(s.totalWritten() == 3 && s.committedCount() == 3, "3 reserved and committed");
        MIB_EXPECT(s.readByWriteIndex(2, f) == FrameReadOutcome::Available && f.data[0] == 3, "index 2 available");
        MIB_EXPECT(s.readByWriteIndex(3, f) == FrameReadOutcome::NotYetCommitted, "index 3 not yet reserved");
        MIB_EXPECT(s.readByWriteIndex(99, f) == FrameReadOutcome::NotYetCommitted, "future index");
        MIB_EXPECT(s.getLatest(f) && f.data[0] == 3, "latest is last committed");
        MIB_EXPECT(s.getByWriteIndex(2, f), "bool wrapper true on Available");
        MIB_EXPECT(!s.getByWriteIndex(3, f), "bool wrapper false otherwise");
    }

    // ---- 2. Wrap: evicted indices are Overwritten, exactly ------------------
    {
        wd.mark("wrap");
        FrameStore s(4);
        Frame f;
        for (int i = 0; i < 10; ++i) push(s, 8, 4, static_cast<uint8_t>(i));
        for (uint64_t i = 0; i < 6; ++i) {
            MIB_EXPECT(s.readByWriteIndex(i, f) == FrameReadOutcome::Overwritten,
                       "index " + std::to_string(i) + " overwritten");
        }
        for (uint64_t i = 6; i < 10; ++i) {
            MIB_EXPECT(s.readByWriteIndex(i, f) == FrameReadOutcome::Available && f.data[0] == i,
                       "index " + std::to_string(i) + " available with right identity");
        }
        MIB_EXPECT(s.earliestAvailableIndex() == 6 && s.latestCommittedIndex() == 9, "window");
    }

    // ---- 3. Malformed slots are reported, not returned as frames -----------
    {
        wd.mark("malformed");
        FrameStore s(4);
        Frame f;
        push(s, 0, 4, 7, /*pitch=*/8); // zero width (payload present)
        MIB_EXPECT(s.readByWriteIndex(0, f) == FrameReadOutcome::Malformed, "zero width malformed");
        push(s, 8, 4, 7, /*pitch=*/4); // pitch < width
        MIB_EXPECT(s.readByWriteIndex(1, f) == FrameReadOutcome::Malformed, "short pitch malformed");
        // Short payload: geometry claims more bytes than supplied.
        std::vector<uint8_t> tiny(4, 1);
        s.pushFrame(tiny.data(), tiny.size(), 8, 4, 8, 0x01080001, 1);
        MIB_EXPECT(s.readByWriteIndex(2, f) == FrameReadOutcome::Malformed, "short payload malformed");
        MIB_EXPECT(!s.getLatest(f), "latest never yields a malformed frame");
        push(s, 8, 4, 9);
        MIB_EXPECT(s.getLatest(f) && f.data[0] == 9, "latest recovers with the next good frame");
    }

    // ---- 4. Deterministic not-yet-committed window ---------------------------
    {
        wd.mark("commit barrier");
        FrameStore s(4);
        push(s, 8, 4, 1); // index 0 committed
        std::mutex m;
        std::condition_variable cv;
        bool inHook = false;
        bool release = false;
        s.setCommitHookForTests([&](uint64_t) {
            std::unique_lock<std::mutex> lk(m);
            inHook = true;
            cv.notify_all();
            cv.wait(lk, [&] { return release; });
        });
        std::thread producer([&] { push(s, 8, 4, 2); }); // index 1 in flight
        {
            std::unique_lock<std::mutex> lk(m);
            cv.wait(lk, [&] { return inHook; });
        }
        // Reservation is visible, commit is not.
        MIB_EXPECT(s.totalWritten() == 2, "index 1 reserved");
        MIB_EXPECT(s.committedCount() == 1, "index 1 not committed");
        Frame f;
        // The slot lock is held by the producer during the hook, so a direct
        // read of index 1 would block until commit — which is exactly the
        // guarantee (no torn read). Check the lock-free contract instead:
        MIB_EXPECT(s.getLatest(f) == true && f.data[0] == 1,
                   "getLatest follows the committed identity, not the reservation");
        MIB_EXPECT(s.latestCommittedIndex() == 0, "latest committed index 0");
        {
            std::lock_guard<std::mutex> lk(m);
            release = true;
        }
        cv.notify_all();
        producer.join();
        s.setCommitHookForTests({});
        MIB_EXPECT(s.committedCount() == 2, "committed after release");
        MIB_EXPECT(s.readByWriteIndex(1, f) == FrameReadOutcome::Available && f.data[0] == 2,
                   "index 1 available after commit");
        MIB_EXPECT(s.getLatest(f) && f.data[0] == 2, "latest advanced after commit");
    }

    // ---- 5. Concurrent producer/consumer: every read outcome is coherent ----
    {
        wd.mark("concurrent");
        FrameStore s(16);
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> available{0}, overwritten{0}, notCommitted{0}, malformed{0}, torn{0};
        std::thread producer([&] {
            for (uint64_t i = 0; i < 20000; ++i) push(s, 32, 8, static_cast<uint8_t>(i % 251));
            stop = true;
        });
        std::thread consumer([&] {
            uint64_t next = 0;
            Frame f;
            while (!stop.load() || next < s.totalWritten()) {
                const auto o = s.readByWriteIndex(next, f);
                switch (o) {
                case FrameReadOutcome::Available:
                    if (f.data[0] != static_cast<uint8_t>(next % 251)) torn++;
                    available++; next++; break;
                case FrameReadOutcome::Overwritten: overwritten++; next++; break;
                case FrameReadOutcome::NotYetCommitted: notCommitted++; std::this_thread::yield(); break;
                case FrameReadOutcome::Malformed: malformed++; next++; break;
                case FrameReadOutcome::OutOfRange: std::this_thread::yield(); break;
                }
                if (stop.load() && next >= s.totalWritten()) break;
            }
        });
        producer.join();
        consumer.join();
        MIB_EXPECT(torn.load() == 0, "no wrong-identity frame ever returned");
        MIB_EXPECT(malformed.load() == 0, "no malformed frames");
        MIB_EXPECT(available.load() + overwritten.load() == 20000, "every index accounted exactly once");
        std::fprintf(stderr, "frame_store_commit: available=%llu overwritten=%llu notCommittedRetries=%llu\n",
                     (unsigned long long)available.load(), (unsigned long long)overwritten.load(),
                     (unsigned long long)notCommitted.load());
    }

    return mib::test::exitCode();
}
