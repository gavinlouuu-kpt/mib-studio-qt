#include "backend/app/WallClock.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>

namespace backend::app {

namespace {
constexpr int64_t kMinUnixMs = 1767225600000LL;  // 2026-01-01T00:00:00Z
constexpr int64_t kMaxUnixMs = 4102444800000LL;  // 2100-01-01T00:00:00Z
constexpr int64_t kJitterNs = 2'000'000'000LL;

std::mutex g_mutex;
std::atomic<bool> g_synced{false};
std::atomic<int64_t> g_offsetNs{0};
std::atomic<int64_t> g_lastSyncMs{0};
std::atomic<bool> g_noRtc{false};

int64_t steadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
int64_t systemNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
} // namespace

void WallClock::setBoardWithoutRtc(bool noRtc) { g_noRtc.store(noRtc); }

bool WallClock::sync(int64_t unixMs, bool locked, std::string* why) {
    if (unixMs < kMinUnixMs || unixMs > kMaxUnixMs) {
        if (why) *why = "client time outside 2026..2100: ignored";
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    const int64_t offset = unixMs * 1'000'000LL - steadyNs();
    if (g_synced.load()) {
        if (locked) {
            if (why) *why = "a run is active: the clock is kept until it ends";
            return false;
        }
        if (std::llabs(offset - g_offsetNs.load()) < kJitterNs) {
            g_lastSyncMs.store(unixMs);
            return false;
        }
    }
    g_offsetNs.store(offset);
    g_lastSyncMs.store(unixMs);
    g_synced.store(true);
    return true;
}

uint64_t WallClock::nowNs() {
    if (g_synced.load()) return static_cast<uint64_t>(steadyNs() + g_offsetNs.load());
    return static_cast<uint64_t>(systemNs());
}

WallClockStatus WallClock::status() {
    WallClockStatus s;
    s.synced = g_synced.load();
    s.offsetNs = s.synced ? g_offsetNs.load() : 0;
    s.lastSyncUnixMs = g_lastSyncMs.load();
    s.source = s.synced ? "client_sync" : (g_noRtc.load() ? "board_clock_unsynced" : "system_clock");
    return s;
}

void WallClock::resetForTesting() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_synced.store(false);
    g_offsetNs.store(0);
    g_lastSyncMs.store(0);
    g_noRtc.store(false);
}

uint64_t WallClock::nowNsForTesting(uint64_t steady, uint64_t system) {
    if (g_synced.load()) return static_cast<uint64_t>(static_cast<int64_t>(steady) + g_offsetNs.load());
    return system;
}

} // namespace backend::app
