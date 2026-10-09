// wall_clock_test (#651 G14): the PZ7035 has no RTC, so saved files take their wall-clock times from the
// last client sync plus the monotonic clock, and say where the time came from.
#include "backend/app/WallClock.h"
#include "backend/recording/Hdf5Service.h"

#include "support/assert.h"
#include "support/tempdir.h"

#include <hdf5.h>
#include <string>
#include <utility>

namespace app = backend::app;

namespace {
std::string readStringAttr(hid_t file, const char* group, const char* name) {
    std::string out;
    hid_t g = H5Gopen2(file, group, H5P_DEFAULT);
    if (g < 0) return out;
    hid_t a = H5Aopen(g, name, H5P_DEFAULT);
    if (a >= 0) {
        hid_t type = H5Tcopy(H5T_C_S1);
        H5Tset_size(type, H5T_VARIABLE);
        H5Tset_cset(type, H5T_CSET_UTF8);
        char* ptr = nullptr;
        if (H5Aread(a, type, &ptr) >= 0 && ptr) {
            out = ptr;
            H5free_memory(ptr);
        }
        H5Tclose(type);
        H5Aclose(a);
    }
    H5Gclose(g);
    return out;
}
} // namespace

int main() {
    constexpr int64_t kClient = 1'790'000'000'000LL; // 2026-09-21, ms
    constexpr int64_t kBoot = 1'748'544'000'000LL;   // May 2025: what the board booted with, ms
    std::string why;

    // A host with a clock (the PC): the system clock, labelled so, and a client time never replaces it.
    app::WallClock::resetForTesting();
    app::WallClock::setClocksForTesting(5'000'000'000LL, kBoot * 1'000'000LL);
    {
        const auto s = app::WallClock::status();
        MIB_EXPECT(!s.synced && s.source == "system_clock", "unsynced host: system_clock");
        MIB_EXPECT(app::WallClock::nowNs() == static_cast<uint64_t>(kBoot) * 1'000'000ULL, "host: nowNs is the system clock");
        MIB_EXPECT(!app::WallClock::sync(kClient, false, &why) && !why.empty(), "a host with a clock ignores a client time");
        const auto after = app::WallClock::status();
        MIB_EXPECT(!after.synced && after.source == "system_clock" && after.offsetNs == 0,
                   "the host's source and timestamps are unchanged by a controller browser's sync");
        MIB_EXPECT(app::WallClock::nowNs() == static_cast<uint64_t>(kBoot) * 1'000'000ULL, "host: still the system clock");
    }

    // A board without an RTC (decided at startup): marked until a client syncs.
    app::WallClock::resetForTesting();
    app::WallClock::setClocksForTesting(5'000'000'000LL, kBoot * 1'000'000LL);
    app::WallClock::setBoardWithoutRtc(true);
    MIB_EXPECT(app::WallClock::status().source == "board_clock_unsynced", "unsynced board: board_clock_unsynced");

    // The run lock comes first: not even the first sync lands in the middle of a run.
    MIB_EXPECT(!app::WallClock::sync(kClient, true, &why) && !app::WallClock::status().synced, "first sync with locked=true is refused");
    {
        auto hold = app::WallClock::hold();
        MIB_EXPECT(app::WallClock::status().holds == 1, "a run holds the clock");
        MIB_EXPECT(!app::WallClock::sync(kClient, false, &why) && !app::WallClock::status().synced,
                   "first sync while a run holds the clock is refused: board-clock start and end, unsynced label, consistently");
        MIB_EXPECT(app::WallClock::nowNs() == static_cast<uint64_t>(kBoot) * 1'000'000ULL, "the run stamps from the board clock");
        auto moved = std::move(hold); // the run's hold moves into its record
        MIB_EXPECT(app::WallClock::status().holds == 1 && !hold.active(), "a moved hold still counts once");
    }
    MIB_EXPECT(app::WallClock::status().holds == 0, "the hold is released with the run");

    // The client's time is taken, and follows the monotonic clock (deterministic: no sleeping).
    MIB_EXPECT(app::WallClock::sync(kClient, false, &why), "first sync is accepted");
    {
        const auto s = app::WallClock::status();
        MIB_EXPECT(s.synced && s.source == "client_sync" && s.lastSyncUnixMs == kClient, "synced: client_sync");
        MIB_EXPECT(app::WallClock::nowNs() == static_cast<uint64_t>(kClient) * 1'000'000ULL, "time is the client's, not the board's (May 2025)");
        app::WallClock::setClocksForTesting(5'000'000'000LL + 120'000'000LL, kBoot * 1'000'000LL);
        MIB_EXPECT(app::WallClock::nowNs() == static_cast<uint64_t>(kClient) * 1'000'000ULL + 120'000'000ULL,
                   "it keeps running with the monotonic clock, whatever the system clock does");
        app::WallClock::setClocksForTesting(5'000'000'000LL, kBoot * 1'000'000LL);
    }
    // Sanity bounds, jitter, and the lock during and until the end of a run (a failed run keeps its hold
    // while its writers drain; here the hold stands in for the coordinator's).
    MIB_EXPECT(!app::WallClock::sync(1'000'000'000'000LL, false, &why) && !why.empty(), "2001 is refused");
    MIB_EXPECT(!app::WallClock::sync(5'000'000'000'000LL, false, &why), "2128 is refused");
    MIB_EXPECT(!app::WallClock::sync(kClient + 1000, false, &why), "a 1 s step is jitter: the offset stays");
    MIB_EXPECT(!app::WallClock::sync(kClient + 3'600'000, true, &why) && !why.empty(), "kept while a run is active");
    {
        auto hold = app::WallClock::hold(); // a failed run is draining its writers: the end is not stamped yet
        MIB_EXPECT(!app::WallClock::sync(kClient + 3'600'000, false, &why), "a resync during the drain is refused");
        MIB_EXPECT(app::WallClock::nowNs() == static_cast<uint64_t>(kClient) * 1'000'000ULL, "the end is stamped with the start's offset");
    }
    MIB_EXPECT(app::WallClock::sync(kClient + 3'600'000, false, &why), "an hour later and idle: accepted");
    MIB_EXPECT(app::WallClock::nowNs() == static_cast<uint64_t>(kClient + 3'600'000) * 1'000'000ULL, "the new offset applies");

    // The file says where the time came from.
    {
        mib::test::TempDir dir("wall_clock");
        const auto path = (dir.path() / "run.h5").string();
        {
            backend::services::Hdf5Service w;
            MIB_REQUIRE(w.openFile(path), "open");
            MIB_REQUIRE(w.initializeRecordingDatasets(), "init");
            MIB_REQUIRE(w.writeRecordingInfo(1, 2, 0, 0), "info");
            const auto s = app::WallClock::status();
            MIB_REQUIRE(w.writeWallClockProvenance(s.source, s.offsetNs), "write wall-clock provenance");
            MIB_REQUIRE(w.writeWallClockProvenance("board_clock_unsynced", 0), "a second write replaces the first");
            w.closeFile();
        }
        hid_t file = H5Fopen(path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
        MIB_REQUIRE(file >= 0, "reopen");
        MIB_EXPECT(readStringAttr(file, "/recording_info", "timestamp_wall_source") == "board_clock_unsynced",
                   "timestamp_wall_source is in the file");
        hid_t g = H5Gopen2(file, "/recording_info", H5P_DEFAULT);
        int64_t offset = 1;
        hid_t a = H5Aopen(g, "timestamp_wall_offset_ns", H5P_DEFAULT);
        MIB_REQUIRE(a >= 0, "offset attribute exists");
        H5Aread(a, H5T_NATIVE_INT64, &offset);
        MIB_EXPECT(offset == 0, "timestamp_wall_offset_ns is in the file");
        H5Aclose(a);
        H5Gclose(g);
        H5Fclose(file);
    }
    app::WallClock::resetForTesting();
    return mib::test::exitCode();
}
