// Bounded polling helper for lifecycle tests. Every pipeline test used to
// re-roll its own waitFor(); keep one here so the poll interval and the
// "never block forever" rule live in one place (docs/howto/writing-tests.md).
#pragma once

#include <chrono>
#include <thread>

namespace mib::test {

// Polls `pred` every `pollMs` until it returns true or `timeoutMs` elapses.
// Returns whether the predicate became true. Pair with a Watchdog: this
// bounds one step, the watchdog bounds the whole test.
template <typename Pred>
bool waitFor(Pred&& pred, int timeoutMs, int pollMs = 5)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(pollMs));
    }
    return pred();
}

} // namespace mib::test
