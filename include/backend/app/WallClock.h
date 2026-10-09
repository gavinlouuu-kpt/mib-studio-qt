#pragma once

#include <cstdint>
#include <string>

namespace backend::app {

// The wall clock used to stamp files (experiment start and end, raw recordings, later the SSD run
// table). The PZ7035 has no RTC and no NTP, so its system clock reads whatever the boot left (May 2025
// on the 2026-10-09 board). A connected client sends its own time (`sync`); the offset to the monotonic
// clock is kept, so a later change of the system clock cannot move a run's timestamps. Without a sync
// the system clock is used and the source says so.
struct WallClockStatus {
    bool synced{false};
    int64_t offsetNs{0};      // wall clock minus the monotonic clock, as last synced
    int64_t lastSyncUnixMs{0};
    std::string source;       // client_sync | board_clock_unsynced | system_clock
};

class WallClock {
public:
    // True when the platform has no battery-backed clock (PZ7035): the unsynced source is then labelled
    // board_clock_unsynced instead of system_clock.
    static void setBoardWithoutRtc(bool noRtc);
    // Accept a client time in ms since the epoch. Rejects times outside 2026..2100 and a client time
    // that moves a synced clock by less than 2 s (jitter) or while `locked` (a run is active: a start and an
    // end stamped from different offsets could invert). Returns true when the offset changed.
    static bool sync(int64_t unixMs, bool locked, std::string* why = nullptr);
    static uint64_t nowNs();
    static WallClockStatus status();
    // Test hooks.
    static void resetForTesting();
    static uint64_t nowNsForTesting(uint64_t steadyNs, uint64_t systemNs);
};

} // namespace backend::app
