// The SSD store inside AppBackend (#667 S1): it is stood up by initialize() with the data dir, so a status read before initialize() (the instrument
// status of an uninitialised backend) cannot leave the store without its state file. A restart with the same data dir must not announce the same
// recovery again.
#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {
const char* kStatus =
    R"({"state":"READY","state_code":3,"last_error":"OK","raw_sectors":417792,"head_lba":123849,"tail_lba":2048,"free_sectors":293943,"min_run_sectors":131072,"runs":2,"next_run_id":5,"table_entries":256,"open_run":false,"open_run_id":0,"open_start_unix_ms":0,"open_client_tag":0,"open_wall_source":0,"recovered_runs":0,"skipped_bad_entries":0,"recovered_ids":[0,0,0,0,0,0,0,0],"counters":{"seen":0,"empty_filtered":0,"invalid_not_sampled":0,"passed":0,"written":0,"dropped":0,"failed":0,"bytes_written":0,"drain_kbps":0,"first_frame_id":0,"last_frame_id":0}})";
std::string run(int id, int reason, bool unknown) {
    return std::string(R"({"run_id":)") + std::to_string(id) + R"(,"flags":0,"open":0,"deleted":0,"incomplete":0,"start_lba":2048,"end_lba":2200,"start_unix_ms":1791530000000,"wall_source":1,"client_tag":0,"filter":2,"sampler_n":10,"rec_sectors":116,"first_frame_id":0,"last_frame_id":0,"first_ticks":0,"last_ticks":0,"tick_hz":0,"seen":0,"empty_filtered":0,"invalid_not_sampled":0,"passed":0,"written":0,"dropped":0,"failed":0,"recoveries":0,"reason":)" +
           std::to_string(reason) + R"(,"counts_unknown":)" + (unknown ? "true" : "false") + "}";
}

bool waitFor(const std::function<bool()>& ok, int ms) {
    for (int i = 0; i < ms / 20; ++i) {
        if (ok()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return ok();
}
} // namespace

int main() {
    mib::test::Watchdog watchdog;
    mib::test::TempDir temp;
    const std::string script = (temp.path() / "fake_pzrec").string();
    {
        std::ofstream out(script);
        out << "#!/bin/sh\ncase \"$1\" in status) echo '" << kStatus << "';; runs) echo '[" << run(1, 0, false) << "," << run(4, 7, true) << "]';; esac\n";
    }
    ::chmod(script.c_str(), 0755);
    ::setenv("MIB_PZREC", script.c_str(), 1);
    ::setenv("MIB_SSD_IMAGE", "/nonexistent/image", 1);
    const auto data = temp.path() / "data";
    const auto stateFile = data / "ssd-state.json";

    {
        backend::AppBackend app;
        backend::bridge::BackendFacade facade(app);
        // Before initialize(): the status path is reachable (the instrument status of an uninitialised backend) and reads nothing.
        const auto before = nlohmann::json::parse(facade.fetchInstrumentStatusJson());
        MIB_EXPECT(before["ssd"]["configured"] == false && before["ssd"]["state"] == "ABSENT", "before initialize the store is not configured: " + before["ssd"].dump());
        MIB_EXPECT(!app.ssdStore().configured(), "ssdStore() before initialize() is the empty one");
        MIB_REQUIRE(facade.initialize(data.string()), "initialize");
        MIB_EXPECT(app.ssdStore().configured(), "initialize() stands the store up");
        MIB_REQUIRE(waitFor([&] { return app.ssdStore().status().state == backend::pz::SsdState::Ready; }, 8000), "READY from the fake pzrec");
        const auto st = app.ssdStore().status();
        MIB_EXPECT(st.recoveredRuns == 1 && st.recoveredIds == std::vector<uint32_t>({4}), "the reason-7 run is announced on the first start");
        MIB_EXPECT(waitFor([&] { return std::filesystem::exists(stateFile); }, 4000), "the state file lands in the data dir");
        std::ifstream f(stateFile);
        std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        MIB_EXPECT(body.find("\"last_seen_run_id\":4") != std::string::npos, "it holds the highest id seen: " + body);
    }
    {
        backend::AppBackend app;
        backend::bridge::BackendFacade facade(app);
        (void)facade.fetchInstrumentStatusJson();  // the early read again, then initialize
        MIB_REQUIRE(facade.initialize(data.string()), "initialize again");
        MIB_REQUIRE(waitFor([&] { return app.ssdStore().status().state == backend::pz::SsdState::Ready; }, 8000), "READY again");
        std::vector<backend::pz::SsdRun> runs;
        MIB_REQUIRE(waitFor([&] { return app.ssdStore().runs(runs, nullptr); }, 4000), "run table read");
        MIB_EXPECT(app.ssdStore().status().recoveredRuns == 0, "the restart does not announce the same recovery again");
        MIB_EXPECT(runs.size() == 2 && runs[1].reason == 7 && runs[1].countsUnknown, "the run keeps its reason-7 entry");
    }
    ::unsetenv("MIB_PZREC");
    ::unsetenv("MIB_SSD_IMAGE");
    return mib::test::exitCode();
}
