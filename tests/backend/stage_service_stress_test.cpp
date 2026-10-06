// stage_service_stress_test
//
// Concurrency stress for StageService (#464): random moves from the main
// thread while one thread hammers Stop/cancel and another reads snapshots,
// across several service lifetimes ending in shutdown with work in flight.
// Invariants: every accepted operation reaches exactly one terminal state,
// nothing hangs (watchdog, no naked joins), the axis is stopped after each
// shutdown, and the TSan lane stays clean.

#include "backend/services/StageService.h"

#include "support/assert.h"
#include "support/stage_rig.h"
#include "support/watchdog.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

using namespace backend::services;
using backend::stage::StageError;
using OpState = StageService::OperationState;

int main()
{
    mib::test::Watchdog watchdog(120);
    std::mt19937 rng(464);
    int accepted = 0;
    int terminalStates[6] = {};

    for (int cycle = 0; cycle < 8; ++cycle) {
        watchdog.mark("cycle");
        mib::test::StageRig rig;
        auto svc = rig.service();
        MIB_REQUIRE(svc->startup() == StageError::None, "start-up");
        const auto home = svc->reference();
        MIB_REQUIRE(home.accepted() && svc->waitForOperation(home.id, std::chrono::seconds(10)), "Home");

        std::atomic<bool> done{false};
        std::thread stopper([&, seed = rng()] {
            std::mt19937 local(seed);
            while (!done.load()) {
                // Any Stop during an operation cancels it, so keep the duty low
                // enough that some operations finish and others are stopped.
                std::this_thread::sleep_for(std::chrono::milliseconds(20 + local() % 101));
                if (local() % 3 != 0) continue;
                const auto active = svc->snapshot().activeOperation;
                if (local() % 2) svc->stop();
                else svc->cancel(active);
            }
        });
        std::thread reader([&] {
            while (!done.load()) {
                const auto s = svc->snapshot();
                if (s.referenced && (s.softMaxUm <= 0 || s.softMinUm >= 0)) {
                    std::printf("inconsistent soft limits\n");
                    std::_Exit(98);
                }
            }
        });

        for (int i = 0; i < 25; ++i) {
            const bool absolute = rng() % 3 != 0;
            const double value = absolute ? static_cast<double>(static_cast<int>(rng() % 5001) - 2500)
                                          : static_cast<double>(static_cast<int>(rng() % 401) - 200);
            const auto r = absolute ? svc->moveTo(value) : svc->moveBy(value);
            if (!r.accepted()) continue; // refused (busy, unreferenced after a failure, limits): fine
            ++accepted;
            MIB_EXPECT(svc->waitForOperation(r.id, std::chrono::seconds(10)), "operation terminates");
            const auto op = svc->operation(r.id);
            MIB_REQUIRE(op.has_value(), "operation retained");
            MIB_EXPECT(op->state != OpState::Queued && op->state != OpState::Running, "terminal state");
            ++terminalStates[static_cast<int>(op->state)];
            if (!svc->snapshot().referenced) {
                const auto again = svc->reference();
                if (again.accepted()) svc->waitForOperation(again.id, std::chrono::seconds(10));
            }
        }

        // Shut down with an operation possibly in flight.
        svc->moveTo(static_cast<double>(static_cast<int>(rng() % 4001) - 2000));
        done.store(true);
        stopper.join();
        reader.join();
        svc->shutdown();
        MIB_EXPECT(!rig.device.moving(), "axis stopped after shutdown");
    }

    // Concurrent Disconnects must not leak the pending count: if it stayed above
    // zero, every later Connect, ApplyProfile and move would be refused for good.
    // Review finding on #519: the pending-disconnect state was a bool, so the
    // first of two overlapping disconnect() calls reopened admission early.
    {
        watchdog.mark("disconnect storm");
        mib::test::StageRig rig;
        auto svc = rig.service();
        MIB_REQUIRE(svc->startup() == StageError::None, "start-up");
        std::vector<std::thread> threads;
        std::atomic<int> connected{0};
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < 15; ++i) {
                    if (t % 2 == 0) svc->disconnect();
                    else if (svc->connect() == StageError::None) connected.fetch_add(1);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1 + (i + t) % 3));
                }
            });
        }
        for (auto& th : threads) th.join();
        svc->disconnect();
        MIB_EXPECT(!svc->snapshot().connected, "a final Disconnect leaves the stage disconnected");
        std::string detail;
        MIB_EXPECT(svc->connect(&detail) == StageError::None,
                   "Connect is admitted again once every Disconnect has run (the pending count drained): " + detail);
        MIB_EXPECT(svc->snapshot().connected, "and it connected");
        MIB_EXPECT(connected.load() > 0, "connects raced the disconnects");
        svc->shutdown();
        MIB_EXPECT(!rig.device.moving(), "axis stopped");
    }

    std::printf("stage stress: %d accepted; completed %d, failed %d, cancelled %d, timed out %d\n", accepted,
                terminalStates[static_cast<int>(OpState::Completed)], terminalStates[static_cast<int>(OpState::Failed)],
                terminalStates[static_cast<int>(OpState::Cancelled)],
                terminalStates[static_cast<int>(OpState::TimedOut)]);
    MIB_EXPECT(accepted > 20, "the stress actually exercised operations");
    MIB_EXPECT(terminalStates[static_cast<int>(OpState::Completed)] > 0 &&
                   terminalStates[static_cast<int>(OpState::Cancelled)] > 0,
               "both completion and Stop raced against each other");
    return mib::test::exitCode();
}
