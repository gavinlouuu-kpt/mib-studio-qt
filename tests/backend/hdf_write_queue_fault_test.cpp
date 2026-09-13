// Adversarial queue boundaries: throwing error sinks and concurrent shutdown.
#include "backend/recording/HdfWriteQueue.h"
#include "support/assert.h"
#include "support/watchdog.h"

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using backend::recording::HdfWriteQueue;

int main(int argc, char** argv) {
    mib::test::Watchdog watchdog(10);
    const std::string scenario = argc > 1 ? argv[1] : "all";
    if (scenario == "all" || scenario == "throw") {
        watchdog.mark("throwing writer and error callback");
        std::atomic<int> errors{0};
        HdfWriteQueue<int> q(
            2, [](const int&) -> bool { throw std::runtime_error("injected disk failure"); },
            [&](const std::string&) {
                ++errors;
                throw std::runtime_error("injected notification failure");
            });
        MIB_REQUIRE(q.submit(1), "initial work accepted");
        MIB_EXPECT(!q.flushAndStop(), "writer failure survives notification failure");
        MIB_EXPECT(q.hasError() && errors == 1, "fatal error latched and notified once");
        MIB_EXPECT(q.error().find("injected disk failure") != std::string::npos,
                   "original writer failure retained");
        MIB_EXPECT(!q.submit(2), "failed queue stays closed");
    }
    if (scenario == "all" || scenario == "overflow") {
        watchdog.mark("overflow with throwing error callback");
        std::atomic<bool> entered{false}, release{false};
        std::atomic<int> errors{0}, written{0};
        HdfWriteQueue<int> q(
            1,
            [&](const int&) {
                entered = true;
                while (!release.load())
                    std::this_thread::yield();
                ++written;
                return true;
            },
            [&](const std::string&) {
                ++errors;
                throw 42;
            });
        MIB_REQUIRE(q.submit(1), "in-flight write accepted");
        while (!entered.load())
            std::this_thread::yield();
        MIB_REQUIRE(q.submit(2), "queued batch accepted");
        MIB_EXPECT(!q.submit(3), "overflow notification exception stays inside submit");
        MIB_EXPECT(!q.submit(4) && errors == 1, "fatal notification is not retried");
        release = true;
        MIB_EXPECT(!q.flushAndStop() && written == 1,
                   "overflow is explicit failure, not a successful partial drain");
    }
    if (scenario == "all" || scenario == "stop") {
        for (int round = 0; round < 25; ++round) {
            watchdog.mark("concurrent queue stops");
            std::atomic<bool> entered{false}, release{false}, go{false};
            std::atomic<int> ready{0}, stopped{0}, written{0};
            HdfWriteQueue<int> q(
                32,
                [&](const int&) {
                    entered = true;
                    while (!release.load())
                        std::this_thread::yield();
                    ++written;
                    return true;
                },
                [](const std::string&) {});
            for (int i = 0; i < 16; ++i)
                MIB_REQUIRE(q.submit(int(i)), "bounded work accepted");
            while (!entered.load())
                std::this_thread::yield();
            std::vector<std::thread> stoppers;
            for (int i = 0; i < 4; ++i)
                stoppers.emplace_back([&] {
                    ++ready;
                    while (!go.load())
                        std::this_thread::yield();
                    if (q.flushAndStop()) ++stopped;
                });
            while (ready.load() != 4)
                std::this_thread::yield();
            go = true;
            // Widen the concurrent-join window; the watchdog bounds a deadlock.
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            release = true;
            for (auto& t : stoppers)
                t.join();
            MIB_EXPECT(stopped == 4 && written == 16,
                       "all stop callers finish and each accepted batch is written once");
            MIB_EXPECT(!q.submit(17), "queue remains stopped");
        }
    }
    return mib::test::exitCode();
}
