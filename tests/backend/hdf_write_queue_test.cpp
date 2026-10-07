// hdf_write_queue_test
//
// Bounded write queue that decouples slow HDF5 writes from a fast producer.
// Verifies FIFO drain, overflow=fatal, write-failure=fatal+onError-once, clean
// flushAndStop drain, and that no work is accepted after a fatal error.

#include "backend/recording/HdfWriteQueue.h"

#include "support/assert.h"
#include "support/watchdog.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using backend::recording::HdfWriteQueue;

int main()
{
    mib::test::Watchdog watchdog(20);
    // 1) FIFO drain: every submitted batch is written, in order. Slots are
    //    sized above the submit count so this path never overflows (overflow is
    //    fatal by design, so a producer must not retry a rejected submit).
    {
        std::mutex m;
        std::vector<int> written;
        HdfWriteQueue<int> q(
            100,
            [&](const int& v) { std::scoped_lock lk(m); written.push_back(v); return true; },
            [](const std::string&) {});
        for (int i = 0; i < 50; ++i) {
            MIB_REQUIRE(q.submit(int(i)), "submit accepted below capacity");
        }
        MIB_REQUIRE(q.flushAndStop(), "clean drain, no error");
        MIB_REQUIRE(written.size() == 50, "all 50 written");
        bool ordered = true;
        for (int i = 0; i < 50; ++i) {
            if (written[static_cast<size_t>(i)] != i) ordered = false;
        }
        MIB_EXPECT(ordered, "written in FIFO order");
    }

    // 2) Write failure -> fatal; onError fires exactly once; submit rejected after.
    {
        std::atomic<int> errCalls{0};
        HdfWriteQueue<int> q(
            3,
            [](const int&) { return false; },
            [&](const std::string&) { errCalls.fetch_add(1); });
        q.submit(1);
        for (int i = 0; i < 200 && !q.hasError(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        MIB_REQUIRE(q.hasError(), "write failure latches error");
        MIB_EXPECT(!q.submit(2), "submit rejected after error");
        q.flushAndStop();
        MIB_EXPECT(errCalls.load() == 1, "onError fired exactly once");
    }

    // 3) Overflow -> fatal. A blocking writeFn keeps slots occupied so the queue
    //    fills and a further submit is rejected with a latched error.
    {
        std::atomic<bool> release{false};
        std::atomic<int> errCalls{0};
        HdfWriteQueue<int> q(
            3,
            [&](const int&) {
                while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                return true;
            },
            [&](const std::string&) { errCalls.fetch_add(1); });
        bool sawReject = false;
        for (int i = 0; i < 200; ++i) {
            if (!q.submit(int(i))) { sawReject = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        MIB_REQUIRE(sawReject, "submit eventually rejected on overflow");
        MIB_EXPECT(q.hasError(), "overflow latches fatal error");
        MIB_EXPECT(errCalls.load() == 1, "overflow fired onError once");
        release.store(true);
        q.flushAndStop();
    }

    {
        int written = 0;
        HdfWriteQueue<int> q(
            3,
            [&](const int&) {
                ++written;
                return true;
            },
            [](const std::string&) {});
        MIB_REQUIRE(q.submit(1), "first submission");
        MIB_REQUIRE(q.flushAndStop(), "clean stop");
        MIB_EXPECT(!q.submit(2), "stopped queue rejects new work");
        MIB_EXPECT(q.flushAndStop() && written == 1, "repeat stop preserves accounting");
    }
    // Race the producer against Stop: accepted work must be drained exactly once.
    for (int round = 0; round < 10; ++round) {
        std::atomic<int> accepted{0}, written{0};
        HdfWriteQueue<int> q(
            4096,
            [&](const int&) {
                ++written;
                return true;
            },
            [](const std::string&) {});
        std::thread producer([&] {
            for (int i = 0; i < 2000; ++i)
                if (q.submit(int(i))) ++accepted;
        });
        while (accepted.load() == 0)
            std::this_thread::yield();
        const bool ok = q.flushAndStop();
        producer.join();
        MIB_EXPECT(ok && written.load() == accepted.load(),
                   "concurrent stop conserves accepted work");
        MIB_EXPECT(!q.submit(2001), "raced stop remains closed");
    }
    if (mib::test::exitCode() == 0) {
        std::printf("HdfWriteQueue FIFO/overflow/failure verified\n");
    }
    return mib::test::exitCode();
}
