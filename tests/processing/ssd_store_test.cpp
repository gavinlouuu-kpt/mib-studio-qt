// SsdStore (#667 S1): the pzrec JSON contract (pz7035 tools/pzrec, 6c948081), the honest failure modes (a failing, hanging or garbled pzrec is WEDGED,
// never READY), and the child-process runner. Optional argument 1: a real `pzrec` binary, run against a fake disk image it formats itself.
#include "backend/pz/SsdStore.h"

#include "support/assert.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace pz = backend::pz;
using std::chrono::milliseconds;

namespace {

// Captured from `pzrec status` / `pzrec runs` of pzrec 6c948081.
const char* kStatusReady =
    R"({"state":"READY","state_code":3,"last_error":"OK","raw_sectors":417792,"head_lba":123849,"tail_lba":2048,"free_sectors":293943,"min_run_sectors":131072,"runs":1,"next_run_id":2,"table_entries":256,"open_run":false,"open_run_id":0,"open_start_unix_ms":0,"open_client_tag":0,"open_wall_source":0,"recovered_runs":0,"skipped_bad_entries":0,"recovered_ids":[0,0,0,0,0,0,0,0],"counters":{"seen":0,"empty_filtered":0,"invalid_not_sampled":0,"passed":0,"written":0,"dropped":0,"failed":0,"bytes_written":0,"drain_kbps":0,"first_frame_id":0,"last_frame_id":0}})";
const char* kStatusRecording =
    R"({"state":"RECORDING","state_code":4,"last_error":"OK","raw_sectors":417792,"head_lba":2048,"tail_lba":2048,"free_sectors":415744,"min_run_sectors":131072,"runs":1,"next_run_id":2,"table_entries":256,"open_run":true,"open_run_id":1,"open_start_unix_ms":1791530000000,"open_client_tag":7,"open_wall_source":1,"recovered_runs":2,"skipped_bad_entries":1,"recovered_ids":[4,5,0,0,0,0,0,0],"counters":{"seen":25000,"empty_filtered":23575,"invalid_not_sampled":900,"passed":525,"written":525,"dropped":3,"failed":0,"bytes_written":31180800,"drain_kbps":100000,"first_frame_id":1000,"last_frame_id":25999}})";
const char* kRuns =
    R"([
  {"run_id":1,"flags":1081344000,"open":0,"deleted":0,"incomplete":0,"start_lba":2048,"end_lba":123849,"start_unix_ms":1791530000000,"wall_source":1,"client_tag":7,"filter":2,"sampler_n":10,"rec_sectors":116,"first_frame_id":1000,"last_frame_id":50999,"first_ticks":0,"last_ticks":999980000,"tick_hz":100000000,"seen":50000,"empty_filtered":47150,"invalid_not_sampled":1800,"passed":1050,"written":1050,"dropped":0,"failed":0,"recoveries":0,"reason":0,"counts_unknown":false},
  {"run_id":2,"flags":0,"open":0,"deleted":1,"incomplete":1,"start_lba":123849,"end_lba":130000,"start_unix_ms":1791530100000,"wall_source":0,"client_tag":0,"filter":0,"sampler_n":0,"rec_sectors":116,"first_frame_id":0,"last_frame_id":0,"first_ticks":0,"last_ticks":0,"tick_hz":0,"seen":0,"empty_filtered":0,"invalid_not_sampled":0,"passed":0,"written":0,"dropped":0,"failed":0,"recoveries":0,"reason":7,"counts_unknown":true}
])";

struct FakeDevice final : pz::ISsdDevice {
    std::string statusText, runsText, statusError, runsError;
    int statusCalls = 0, runsCalls = 0;
    bool status(std::string& json, std::string* error) override {
        ++statusCalls;
        if (!statusError.empty()) { if (error) *error = statusError; return false; }
        json = statusText;
        return true;
    }
    bool runs(std::string& json, std::string* error) override {
        ++runsCalls;
        if (!runsError.empty()) { if (error) *error = runsError; return false; }
        json = runsText;
        return true;
    }
};

std::string replaceFirst(std::string s, const std::string& from, const std::string& to) {
    const auto at = s.find(from);
    if (at != std::string::npos) s.replace(at, from.size(), to);
    return s;
}

// A shell script standing in for pzrec: prints $1-specific output, exits as told.
std::string writeScript(const std::string& name, const std::string& body) {
    const std::string path = "/tmp/" + name + "." + std::to_string(::getpid());
    std::ofstream(path) << "#!/bin/sh\n" << body << "\n";
    ::chmod(path.c_str(), 0755);
    return path;
}

} // namespace

int main(int argc, char** argv) {
    // --- the real JSON ---------------------------------------------------------------------------------------
    {
        pz::SsdStatus s;
        std::string why;
        MIB_REQUIRE(pz::parseSsdStatus(kStatusReady, s, &why), why);
        MIB_EXPECT(s.state == pz::SsdState::Ready && s.ok(), "READY parses");
        MIB_EXPECT(s.rawSectors == 417792 && s.freeSectors == 293943 && s.tableEntries == 256 && s.runs == 1 && s.nextRunId == 2, "log pointers and table size");
        MIB_EXPECT(!s.openRun && s.recoveredIds.empty(), "zero ids in recovered_ids are padding, not runs");
        MIB_REQUIRE(pz::parseSsdStatus(kStatusRecording, s, &why), why);
        MIB_EXPECT(s.state == pz::SsdState::Recording && s.openRun && s.openRunId == 1 && s.openStartUnixMs == 1791530000000ull && s.openClientTag == 7, "open run");
        MIB_EXPECT(s.counters.seen == 25000 && s.counters.passed == 525 && s.counters.dropped == 3 && s.counters.drainKbps == 100000 && s.counters.lastFrameId == 25999, "counters");
        MIB_EXPECT(s.recoveredRuns == 2 && s.skippedBadEntries == 1 && s.recoveredIds == std::vector<uint32_t>({4, 5}), "R9 recovery fields and skipped_bad_entries");

        std::vector<pz::SsdRun> runs;
        MIB_REQUIRE(pz::parseSsdRuns(kRuns, runs, &why), why);
        MIB_REQUIRE(runs.size() == 2, "two runs");
        MIB_EXPECT(runs[0].id == 1 && runs[0].seen == 50000 && runs[0].written == 1050 && runs[0].emptyFiltered == 47150 && runs[0].tickHz == 100000000u && runs[0].lastTicks == 999980000ull, "run 1 fields");
        MIB_EXPECT(!runs[0].countsUnknown && !runs[0].deleted && runs[0].reason == 0, "a clean run");
        MIB_EXPECT(runs[1].deleted && runs[1].incomplete && runs[1].reason == 7 && runs[1].countsUnknown, "a deleted run recovered at mount: totals unknown");
        MIB_EXPECT(pz::parseSsdRuns("[]", runs, &why) && runs.empty(), "an empty table");
    }

    // --- hostile JSON: nothing parses into a state the UI could trust ---------------------------------------------
    {
        pz::SsdStatus s;
        std::vector<pz::SsdRun> runs;
        std::string why;
        for (const char* bad : {"", "not json", "[]", "null", "{}", R"({"error":"io","code":1,"name":"IO"})"}) {
            MIB_EXPECT(!pz::parseSsdStatus(bad, s, &why) && !why.empty(), std::string("status refuses: ") + bad);
        }
        MIB_EXPECT(!pz::parseSsdStatus(replaceFirst(kStatusReady, "\"READY\"", "\"HAPPY\""), s, &why), "an unknown state name is refused");
        MIB_EXPECT(!pz::parseSsdStatus(replaceFirst(kStatusReady, "\"free_sectors\":293943,", ""), s, &why) && why.find("free_sectors") != std::string::npos, "a missing field is named");
        MIB_EXPECT(!pz::parseSsdStatus(replaceFirst(kStatusReady, "\"runs\":1", "\"runs\":\"1\""), s, &why), "a string where a number belongs is refused");
        MIB_EXPECT(!pz::parseSsdStatus(replaceFirst(kStatusReady, "\"runs\":1", "\"runs\":-1"), s, &why), "a negative count is refused");
        MIB_EXPECT(!pz::parseSsdStatus(replaceFirst(kStatusReady, "\"runs\":1", "\"runs\":4294967296"), s, &why), "a count beyond 32 bits is refused");
        MIB_EXPECT(!pz::parseSsdStatus(replaceFirst(kStatusReady, "\"counters\"", "\"counterz\""), s, &why), "missing counters are refused");
        MIB_EXPECT(!pz::parseSsdRuns("{}", runs, &why) && !pz::parseSsdRuns("[1]", runs, &why) && !pz::parseSsdRuns("[{}]", runs, &why), "runs: wrong shapes");
        MIB_EXPECT(!pz::parseSsdRuns(R"({"error":"not formatted","code":2,"name":"NOT_FORMATTED"})", runs, &why) && why.find("not formatted") != std::string::npos, "runs: an error object is reported");
        MIB_EXPECT(!pz::parseSsdRuns(replaceFirst(kRuns, "\"run_id\":1", "\"run_id\":\"x\""), runs, &why), "runs: a bad run id");
    }

    // --- the store: states and honest failures ---------------------------------------------------------------------
    {
        pz::SsdStore none(nullptr);
        const auto s = none.status();
        MIB_EXPECT(s.state == pz::SsdState::Absent && s.reason == "No SSD: nothing can be saved" && !none.configured(), "no device is ABSENT with the UI text");
        std::vector<pz::SsdRun> runs;
        std::string why;
        MIB_EXPECT(!none.runs(runs, &why) && why == s.reason, "no run list without a device");

        auto dev = std::make_unique<FakeDevice>();
        FakeDevice* d = dev.get();
        d->statusText = kStatusReady;
        d->runsText = kRuns;
        pz::SsdStore store(std::move(dev), milliseconds(0));
        MIB_EXPECT(store.status().state == pz::SsdState::Ready && store.status().reason.empty(), "READY has no reason text");
        MIB_EXPECT(store.runs(runs, &why) && runs.size() == 2, "runs while READY");

        d->statusText = replaceFirst(kStatusReady, "\"READY\"", "\"RAW_FULL\"");
        MIB_EXPECT(store.status().state == pz::SsdState::RawFull && store.status().reason.find("raw area full") != std::string::npos && !store.status().ok(), "RAW_FULL says so and is not usable");
        MIB_EXPECT(store.runs(runs, &why) && runs.size() == 2, "the list stays readable when the raw area is full (the user deletes from it)");
        d->statusText = replaceFirst(kStatusReady, "\"READY\"", "\"RUN_TABLE_FULL\"");
        MIB_EXPECT(store.status().state == pz::SsdState::RunTableFull && store.status().reason.find("run table full") != std::string::npos, "RUN_TABLE_FULL");
        d->statusText = replaceFirst(kStatusReady, "\"READY\"", "\"RECOVERING\"");
        MIB_EXPECT(store.status().state == pz::SsdState::Recovering && store.status().reason == "SSD recovering", "RECOVERING");
        d->statusText = replaceFirst(kStatusReady, "\"READY\"", "\"WEDGED\"");
        MIB_EXPECT(store.status().state == pz::SsdState::Wedged && store.status().reason.find("power-cycle") != std::string::npos, "WEDGED from the device");
        MIB_EXPECT(!store.runs(runs, &why) && runs.empty() && !why.empty(), "no run list while WEDGED");
        d->statusText = replaceFirst(kStatusReady, "\"READY\"", "\"UNFORMATTED\"");
        MIB_EXPECT(store.status().state == pz::SsdState::Unformatted && !store.runs(runs, &why), "UNFORMATTED");

        d->statusText = kStatusReady;
        d->statusError = "pzrec status exited with 1: disk gone";
        const auto w = store.status();
        MIB_EXPECT(w.state == pz::SsdState::Wedged && w.reason.find("disk gone") != std::string::npos && !w.ok(), "a failing pzrec is WEDGED with its reason, never READY");
        d->statusError.clear();
        d->statusText = "garbage";
        MIB_EXPECT(store.status().state == pz::SsdState::Wedged, "unparsable status is WEDGED");
        d->statusText = kStatusReady;
        d->runsError = "pzrec runs exited with 1";
        store.refresh(true);
        MIB_EXPECT(!store.runs(runs, &why) && runs.empty() && why.find("unreadable") != std::string::npos, "a failing run table read is reported, the list is empty");
        d->runsError.clear();
        d->runsText = "[{}]";
        store.refresh(true);
        MIB_EXPECT(!store.runs(runs, &why) && runs.empty(), "a garbled run table is reported, no partial list");
    }

    // --- "recovered at mount" comes from the run table (reason 7), the notice after a restart from the persisted highest id ---------------
    {
        const std::string state = "/tmp/ssd_store_state." + std::to_string(::getpid()) + ".json";
        std::remove(state.c_str());
        auto make = [&](FakeDevice** out, const std::string& runsText, const std::string& stateFile) {
            auto dev = std::make_unique<FakeDevice>();
            *out = dev.get();
            dev->statusText = kStatusReady;  // pzrec's own recovered_runs fields are ignored: the table is the truth
            dev->runsText = runsText;
            return std::make_unique<pz::SsdStore>(std::move(dev), milliseconds(0), false, stateFile);
        };
        FakeDevice* d = nullptr;
        // kRuns: run 1 clean, run 2 deleted + reason 7.
        auto first = make(&d, kRuns, state);
        auto s = first->status();
        MIB_EXPECT(s.recoveredRuns == 1 && s.recoveredIds == std::vector<uint32_t>({2}), "first start: the reason-7 run is announced");
        s = first->status();
        MIB_EXPECT(s.recoveredRuns == 1, "and stays announced for this process (no latch needed: the table says it)");
        std::ifstream f(state);
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        MIB_EXPECT(content.find("\"last_seen_run_id\":2") != std::string::npos, "the highest id seen is persisted: " + content);
        // A restart: Studio has seen run 2 before, so no notice; the run still shows reason 7 in the table.
        auto second = make(&d, kRuns, state);
        s = second->status();
        std::vector<pz::SsdRun> runs;
        MIB_EXPECT(s.recoveredRuns == 0 && s.recoveredIds.empty(), "restart: nothing new to announce");
        MIB_EXPECT(second->runs(runs, nullptr) && runs[1].reason == 7 && runs[1].countsUnknown, "the entry keeps its reason 7 badge");
        // A new recovery at mount while Studio was away: a reason-7 run above the persisted id.
        const std::string withNew = replaceFirst(kRuns, "\n]", ",\n  {\"run_id\":3,\"flags\":0,\"open\":0,\"deleted\":0,\"incomplete\":1,\"start_lba\":130000,\"end_lba\":130100,\"start_unix_ms\":1791530200000,\"wall_source\":1,\"client_tag\":0,\"filter\":0,\"sampler_n\":0,\"rec_sectors\":116,\"first_frame_id\":0,\"last_frame_id\":0,\"first_ticks\":0,\"last_ticks\":0,\"tick_hz\":0,\"seen\":0,\"empty_filtered\":0,\"invalid_not_sampled\":0,\"passed\":0,\"written\":0,\"dropped\":0,\"failed\":0,\"recoveries\":0,\"reason\":7,\"counts_unknown\":true}\n]");
        auto third = make(&d, withNew, state);
        s = third->status();
        MIB_EXPECT(s.recoveredRuns == 1 && s.recoveredIds == std::vector<uint32_t>({3}), "after a restart only the runs recovered since are announced");
        std::ifstream f3(state);
        std::string c3((std::istreambuf_iterator<char>(f3)), std::istreambuf_iterator<char>());
        MIB_EXPECT(c3.find("\"last_seen_run_id\":3") != std::string::npos, "the id moves up");
        // A recovery during the run of this process is announced too (id above the baseline).
        auto fourth = make(&d, kRuns, state);  // baseline 3
        fourth->status();
        d->runsText = withNew.substr(0, withNew.size());  // same table: id 3 is not above 3
        fourth->refresh(true);
        MIB_EXPECT(fourth->status().recoveredRuns == 0, "ids at or below the baseline are not announced");
        // A corrupt or foreign state file: baseline 0, so everything with reason 7 is announced (the safe direction).
        std::ofstream(state) << "{not json";
        auto fifth = make(&d, kRuns, state);
        MIB_EXPECT(fifth->status().recoveredRuns == 1, "an unreadable state file announces rather than hides");
        // No state file configured: same.
        auto sixth = make(&d, kRuns, "");
        MIB_EXPECT(sixth->status().recoveredRuns == 1, "no state file: announced each start");
        std::remove(state.c_str());
        std::remove((state + ".tmp").c_str());
    }

    // --- the cache and the background refresher ----------------------------------------------------------------------
    {
        auto dev = std::make_unique<FakeDevice>();
        FakeDevice* d = dev.get();
        d->statusText = kStatusReady;
        d->runsText = kRuns;
        pz::SsdStore store(std::move(dev), milliseconds(60000));
        for (int i = 0; i < 5; ++i) store.status();
        std::vector<pz::SsdRun> runs;
        for (int i = 0; i < 5; ++i) store.runs(runs, nullptr);
        MIB_EXPECT(d->statusCalls == 1 && d->runsCalls == 1, "the status and the table are read once within the interval (the UI polls)");
        d->runsText = "[]";
        store.refresh(false);
        MIB_EXPECT(d->statusCalls == 1, "refresh within the interval does nothing");
        store.refresh(true);
        MIB_EXPECT(d->statusCalls == 2 && d->runsCalls == 2 && store.runs(runs, nullptr) && runs.empty(), "a forced refresh reads both again");
    }
    {
        // The background thread: callers never wait for pzrec, a hung device only delays the first answer.
        struct Slow final : pz::ISsdDevice {
            bool status(std::string& json, std::string*) override { std::this_thread::sleep_for(milliseconds(600)); json = kStatusReady; return true; }
            bool runs(std::string& json, std::string*) override { json = "[]"; return true; }
        };
        pz::SsdStore store(std::make_unique<Slow>(), milliseconds(50), /*background=*/true);
        const auto t0 = std::chrono::steady_clock::now();
        const auto first = store.status();
        MIB_EXPECT(std::chrono::steady_clock::now() - t0 < milliseconds(300), "status() returns at once while pzrec is slow");
        MIB_EXPECT(first.state == pz::SsdState::Initialising && first.reason == "SSD starting", "before the first answer the state is INITIALISING");
        std::vector<pz::SsdRun> runs;
        std::string why;
        MIB_EXPECT(!store.runs(runs, &why), "no run table before the first answer");
        bool ready = false;
        for (int i = 0; i < 60 && !ready; ++i) {
            std::this_thread::sleep_for(milliseconds(50));
            ready = store.status().state == pz::SsdState::Ready;
        }
        MIB_EXPECT(ready && store.runs(runs, &why), "the refresher delivers the answer");
    }

    // --- the child-process runner --------------------------------------------------------------------------------------
    {
        const auto ok = pz::runBounded({"/bin/sh", "-c", "echo out; echo err >&2; exit 3"}, milliseconds(5000));
        MIB_EXPECT(ok.started && !ok.timedOut && ok.exitCode == 3 && ok.out == "out\n" && ok.err == "err\n", "exit code, stdout and stderr are captured");
        const auto t0 = std::chrono::steady_clock::now();
        const auto hung = pz::runBounded({"/bin/sh", "-c", "sleep 30"}, milliseconds(300));
        const auto took = std::chrono::steady_clock::now() - t0;
        MIB_EXPECT(hung.started && hung.timedOut && took < std::chrono::seconds(5), "a hanging child is killed at the timeout");
        const auto missing = pz::runBounded({"/nonexistent/pzrec", "status"}, milliseconds(1000));
        MIB_EXPECT(!missing.started && !missing.err.empty(), "a missing binary is reported");
        const auto big = pz::runBounded({"/bin/sh", "-c", "head -c 3000000 /dev/zero | tr '\\0' x"}, milliseconds(10000));
        MIB_EXPECT(big.exitCode == 0 && big.out.size() == 3000000, "a large output is read completely");

        const std::string good = writeScript("fake_pzrec_good", std::string("case \"$1\" in status) echo '") + kStatusReady + "';; runs) echo '" + "[]" + "';; esac");
        pz::SsdStore okStore(std::make_unique<pz::PzrecCliDevice>(good, "/tmp/none.img"), milliseconds(0));
        MIB_EXPECT(okStore.status().state == pz::SsdState::Ready, "the CLI device runs `pzrec status IMG`");
        const std::string bad = writeScript("fake_pzrec_bad", "echo 'oops: cannot open image' >&2; echo '{\"error\":\"io\"}'; exit 1");
        pz::SsdStore badStore(std::make_unique<pz::PzrecCliDevice>(bad, "/tmp/none.img"), milliseconds(0));
        const auto b = badStore.status();
        MIB_EXPECT(b.state == pz::SsdState::Wedged && b.reason.find("cannot open image") != std::string::npos && b.reason.find("exited with 1") != std::string::npos,
                   "a non-zero exit is WEDGED with the stderr: " + b.reason);
        const std::string slow = writeScript("fake_pzrec_slow", "sleep 30");
        pz::SsdStore slowStore(std::make_unique<pz::PzrecCliDevice>(slow, "/tmp/none.img", milliseconds(300)), milliseconds(0));
        MIB_EXPECT(slowStore.status().state == pz::SsdState::Wedged && slowStore.status().reason.find("did not answer") != std::string::npos, "a hanging pzrec is WEDGED");
        const std::string junk = writeScript("fake_pzrec_junk", "echo 'READY'");
        pz::SsdStore junkStore(std::make_unique<pz::PzrecCliDevice>(junk, "/tmp/none.img"), milliseconds(0));
        MIB_EXPECT(junkStore.status().state == pz::SsdState::Wedged, "exit 0 with unparsable output is WEDGED, not READY");
        pz::SsdStore missingStore(std::make_unique<pz::PzrecCliDevice>("/nonexistent/pzrec", "/tmp/none.img"), milliseconds(0));
        MIB_EXPECT(missingStore.status().state == pz::SsdState::Wedged, "a missing pzrec is WEDGED");
        for (const auto& p : {good, bad, slow, junk}) std::remove(p.c_str());
    }

    // --- the real pzrec on a fake disk (optional) ------------------------------------------------------------------------
    if (argc > 1) {
        const std::string pzrec = argv[1];
        const std::string img = "/tmp/ssd_store_test." + std::to_string(::getpid()) + ".img";
        auto sh = [&](const std::string& args) { return std::system((pzrec + " " + args + " > /dev/null").c_str()); };
        MIB_REQUIRE(sh("format " + img + " --size-mib 512 --min-run-mib 64") == 0, "format");
        pz::SsdStore store(std::make_unique<pz::PzrecCliDevice>(pzrec, img), milliseconds(0));
        MIB_EXPECT(store.status().state == pz::SsdState::Ready, "real pzrec: READY after format");
        MIB_REQUIRE(sh("start " + img + " --filter valid --sampler 10 --start-ms 1791530000000 --wall-source 1 --tag 7") == 0, "start");  // the simulation runs on the real clock
        MIB_EXPECT(store.status().state == pz::SsdState::Recording && store.status().openRunId == 1 && store.status().openClientTag == 7, "real pzrec: RECORDING");
        MIB_REQUIRE(sh("stop " + img) == 0, "stop");
        std::vector<pz::SsdRun> runs;
        std::string why;
        MIB_REQUIRE(store.runs(runs, &why) && runs.size() == 1, "real pzrec: one run: " + why);
        MIB_EXPECT(runs[0].id == 1 && runs[0].seen >= runs[0].passed && runs[0].passed >= runs[0].written && runs[0].startUnixMs == 1791530000000ull && runs[0].clientTag == 7 && runs[0].wallSource == 1 && !runs[0].countsUnknown && !runs[0].open, "real pzrec: the run entry");
        std::remove(img.c_str());
        std::remove((img + ".fakehw").c_str());
        std::remove((img + ".fakeparams").c_str());
    }

    return mib::test::exitCode();
}
