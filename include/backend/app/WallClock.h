#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>

namespace backend::app {

// The wall clock used to stamp files (experiment start and end, raw recordings, KDE, exports, the SSD run
// table). The PZ7035 has no RTC and no NTP, so its system clock reads whatever the boot left (May 2025
// on the 2026-10-09 board). A connected client sends its own time (`sync`); the offset to the monotonic
// clock is kept, so a later change of the system clock cannot move a run's timestamps. Without a sync
// the system clock is used and the source says so. Header only: the lowest libraries (processing,
// review) stamp files too, and there is one state per process (inline statics).
struct WallClockStatus {
    bool synced{false};
    int64_t offsetNs{0};      // wall clock minus the monotonic clock, as last synced
    int64_t lastSyncUnixMs{0};
    std::string source;       // client_sync | board_clock_unsynced | system_clock
    int holds{0};             // runs and recordings that keep the clock fixed until they have written their end time
};

class WallClock {
    struct State {
        std::mutex mutex;
        std::atomic<bool> synced{false};
        std::atomic<int64_t> offsetNs{0};
        std::atomic<int64_t> lastSyncMs{0};
        std::atomic<bool> noRtc{false};
        std::atomic<int> holds{0};
        std::atomic<int64_t> testSteadyNs{-1};
        std::atomic<int64_t> testSystemNs{-1};
    };
    static State& st() {
        static State s;
        return s;
    }
    static int64_t steadyNs() {
        if (st().testSteadyNs.load() >= 0) return st().testSteadyNs.load();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    static int64_t systemNs() {
        if (st().testSystemNs.load() >= 0) return st().testSystemNs.load();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

public:
    // Keeps the offset fixed while alive: a run or recording takes one before it stamps its start and
    // releases it after it has stamped and persisted its end (also on failure), so its start and end can
    // never come from different offsets and its source label stays true.
    class Hold {
    public:
        Hold() : active_(true) { st().holds.fetch_add(1); }
        Hold(Hold&& other) noexcept : active_(other.active_) { other.active_ = false; }
        Hold& operator=(Hold&& other) noexcept {
            if (this != &other) {
                if (active_) st().holds.fetch_sub(1);
                active_ = other.active_;
                other.active_ = false;
            }
            return *this;
        }
        Hold(const Hold&) = delete;
        Hold& operator=(const Hold&) = delete;
        ~Hold() {
            if (active_) st().holds.fetch_sub(1);
        }
        bool active() const { return active_; }

    private:
        bool active_{false};
    };
    static Hold hold() { return Hold(); }

    // Decided once at startup from the build/platform (PZ7035: no battery-backed clock): only such a
    // platform accepts a client time, and its unsynced source is labelled board_clock_unsynced. A host
    // with a real clock ignores `sync` and always reports system_clock.
    static void setBoardWithoutRtc(bool noRtc) { st().noRtc.store(noRtc); }

    // Accept a client time in ms since the epoch. Rejected: on a platform with a clock, times outside
    // 2026..2100, while a Hold is alive or `locked` (checked first, so not even the first sync can land
    // mid-run), and a step under 2 s on a synced clock (jitter). Returns true when the offset changed.
    static bool sync(int64_t unixMs, bool locked, std::string* why = nullptr) {
        constexpr int64_t kMinUnixMs = 1767225600000LL;  // 2026-01-01T00:00:00Z
        constexpr int64_t kMaxUnixMs = 4102444800000LL;  // 2100-01-01T00:00:00Z
        constexpr int64_t kJitterNs = 2'000'000'000LL;
        auto& s = st();
        if (!s.noRtc.load()) {
            if (why) *why = "this host has its own clock: the client time is ignored";
            return false;
        }
        if (unixMs < kMinUnixMs || unixMs > kMaxUnixMs) {
            if (why) *why = "client time outside 2026..2100: ignored";
            return false;
        }
        std::lock_guard<std::mutex> lock(s.mutex);
        // The run lock comes first, whether or not the clock was ever synced: a first sync in the middle of a
        // run would give it a board-clock start, a client-clock end and the unsynced label.
        if (locked || s.holds.load() > 0) {
            if (why) *why = "a run is active: the clock is kept until it ends";
            return false;
        }
        const int64_t offset = unixMs * 1'000'000LL - steadyNs();
        if (s.synced.load() && std::llabs(offset - s.offsetNs.load()) < kJitterNs) {
            s.lastSyncMs.store(unixMs);
            return false;
        }
        s.offsetNs.store(offset);
        s.lastSyncMs.store(unixMs);
        s.synced.store(true);
        return true;
    }

    static uint64_t nowNs() {
        if (st().synced.load()) return static_cast<uint64_t>(steadyNs() + st().offsetNs.load());
        return static_cast<uint64_t>(systemNs());
    }

    static WallClockStatus status() {
        auto& s = st();
        WallClockStatus out;
        out.synced = s.synced.load();
        out.offsetNs = out.synced ? s.offsetNs.load() : 0;
        out.lastSyncUnixMs = s.lastSyncMs.load();
        out.holds = s.holds.load();
        out.source = out.synced ? "client_sync" : (s.noRtc.load() ? "board_clock_unsynced" : "system_clock");
        return out;
    }

    // Test hooks: fix the monotonic and system clocks (ns) so tests do not depend on timing; -1 restores them.
    static void resetForTesting() {
        auto& s = st();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.synced.store(false);
        s.offsetNs.store(0);
        s.lastSyncMs.store(0);
        s.noRtc.store(false);
        s.testSteadyNs.store(-1);
        s.testSystemNs.store(-1);
    }
    static void setClocksForTesting(int64_t steady, int64_t system) {
        st().testSteadyNs.store(steady);
        st().testSystemNs.store(system);
    }
};

} // namespace backend::app
