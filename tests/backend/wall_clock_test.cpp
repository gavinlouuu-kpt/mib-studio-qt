// wall_clock_test (#651 G14): the PZ7035 has no RTC, so saved files take their wall-clock times from the
// last client sync plus the monotonic clock, and say where the time came from.
#include "backend/app/WallClock.h"
#include "backend/recording/Hdf5Service.h"

#include "support/assert.h"
#include "support/tempdir.h"

#include <chrono>
#include <hdf5.h>
#include <string>
#include <thread>

namespace app = backend::app;

namespace {
int64_t systemMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
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
    // A host (with a clock): the system clock, labelled so.
    app::WallClock::resetForTesting();
    {
        const auto s = app::WallClock::status();
        MIB_EXPECT(!s.synced && s.source == "system_clock", "unsynced host: system_clock");
        const int64_t before = systemMs();
        const int64_t now = static_cast<int64_t>(app::WallClock::nowNs() / 1'000'000ULL);
        MIB_EXPECT(now >= before && now < before + 1000, "unsynced: nowNs is the system clock");
    }
    // A board without an RTC and no client sync: marked.
    app::WallClock::setBoardWithoutRtc(true);
    MIB_EXPECT(app::WallClock::status().source == "board_clock_unsynced", "unsynced board: board_clock_unsynced");

    // The client's time is taken, and follows the monotonic clock.
    constexpr int64_t kClient = 1'790'000'000'000LL; // 2026-09-21
    std::string why;
    MIB_EXPECT(app::WallClock::sync(kClient, false, &why), "first sync is accepted");
    {
        const auto s = app::WallClock::status();
        MIB_EXPECT(s.synced && s.source == "client_sync" && s.lastSyncUnixMs == kClient, "synced: client_sync");
        const int64_t t0 = static_cast<int64_t>(app::WallClock::nowNs() / 1'000'000ULL);
        MIB_EXPECT(t0 >= kClient && t0 < kClient + 1000, "time is the client's, not the board's (May 2025)");
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        const int64_t t1 = static_cast<int64_t>(app::WallClock::nowNs() / 1'000'000ULL);
        MIB_EXPECT(t1 - t0 >= 100 && t1 - t0 < 400, "it keeps running with the monotonic clock");
    }
    // Sanity bounds, jitter and the run lock.
    MIB_EXPECT(!app::WallClock::sync(1'000'000'000'000LL, false, &why) && !why.empty(), "2001 is refused");
    MIB_EXPECT(!app::WallClock::sync(5'000'000'000'000LL, false, &why), "2128 is refused");
    MIB_EXPECT(!app::WallClock::sync(kClient + 1000, false, &why), "a 1 s step is jitter: the offset stays");
    MIB_EXPECT(!app::WallClock::sync(kClient + 3'600'000, true, &why) && !why.empty(), "kept while a run is active");
    MIB_EXPECT(app::WallClock::sync(kClient + 3'600'000, false, &why), "an hour later and idle: accepted");
    {
        const int64_t t = static_cast<int64_t>(app::WallClock::nowNs() / 1'000'000ULL);
        MIB_EXPECT(t >= kClient + 3'600'000 && t < kClient + 3'601'000, "the new offset applies");
    }

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
