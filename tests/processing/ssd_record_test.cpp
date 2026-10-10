// SsdStore Record (#667 S2): the start sequence (prepare, run-id check, refusals that name the state), the stop that never blocks and survives slow or
// retried pzrec stops, and the honest RECORDING state while the block path is held back. Optional argument 1: a real `pzrec` (pz7035 03ad422e or later) that
// formats a fake disk image and records on its simulated drain through PzrecCliDevice.
#include "backend/pz/SsdStore.h"

#include "support/assert.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

namespace pz = backend::pz;
using std::chrono::milliseconds;

namespace {

std::string statusJson(const std::string& state, bool open, uint32_t openId, uint32_t nextId) {
    char buf[1400];
    std::snprintf(buf, sizeof buf,
                  R"({"state":"%s","state_code":3,"last_error":"OK","raw_sectors":417792,"head_lba":2048,"tail_lba":2048,"free_sectors":415744,"min_run_sectors":131072,"runs":%u,"next_run_id":%u,"table_entries":256,"open_run":%s,"open_run_id":%u,"open_start_unix_ms":1791530000000,"open_client_tag":7,"open_wall_source":1,"recovered_runs":0,"skipped_bad_entries":0,"recovered_ids":[0,0,0,0,0,0,0,0],"counters":{"seen":25000,"empty_filtered":23575,"invalid_not_sampled":900,"passed":525,"written":525,"dropped":0,"failed":0,"bytes_written":31180800,"drain_kbps":100000,"first_frame_id":1000,"last_frame_id":25999}})",
                  state.c_str(), nextId - 1, nextId, open ? "true" : "false", openId);
    return buf;
}

// A recorder with a scripted pzrec: the knobs say how start and stop behave.
struct FakeRecorder final : pz::ISsdDevice {
    std::mutex m;
    std::string state = "READY";
    bool open = false;
    uint32_t openId = 0, next = 2;
    std::string statusError;                  // `status` fails with this
    bool startRefuses = false;                // `start` fails and opens nothing
    bool startTimesOut = false;               // `start` fails with a timeout after opening a run
    uint32_t startOpensId = 0;                // 0: the run takes `next`
    int stopFailures = 0;                     // this many `stop` calls fail before one works
    bool stopClosesWhileFailing = false;      // a failed stop has closed the run after all (killed after it did its work)
    std::function<void()> onStop;             // runs inside `stop` (blocking gates)
    int startCalls = 0, stopCalls = 0, statusCalls = 0, abortStops = 0;
    pz::SsdStartArgs lastStart;
    std::string windowState = "IDLE";
    std::string windowError;                   // the window snapshot fails with this
    std::string runsText = "[]";
    int windowCalls = 0;

    bool status(std::string& json, std::string* error) override {
        std::lock_guard<std::mutex> lock(m);
        ++statusCalls;
        if (!statusError.empty()) { if (error) *error = statusError; return false; }
        json = statusJson(state, open, openId, next);
        return true;
    }
    bool runs(std::string& json, std::string*) override { std::lock_guard<std::mutex> lock(m); json = runsText; return true; }
    bool windowSnapshot(std::string& json, std::string* error) override {
        std::lock_guard<std::mutex> lock(m);
        ++windowCalls;
        if (!windowError.empty()) { if (error) *error = windowError; return false; }
        json = "{\"state\":\"" + windowState + "\",\"state_code\":0}";
        return true;
    }
    bool readArgv(uint32_t run, uint64_t from, uint64_t count, std::vector<std::string>& argv) override {
        argv = {"pzrec", "read", std::to_string(run), "disk", "--hw", "pl", "--summary"};
        if (from) { argv.push_back("--from"); argv.push_back(std::to_string(from)); }
        if (count) { argv.push_back("--count"); argv.push_back(std::to_string(count)); }
        return true;
    }
    bool start(const pz::SsdStartArgs& a, std::string& json, std::string* error) override {
        std::lock_guard<std::mutex> lock(m);
        ++startCalls;
        lastStart = a;
        if (startRefuses) { if (error) *error = "pzrec start exited with 1: start refused"; return false; }
        open = true;
        openId = startOpensId ? startOpensId : next;
        next = openId + 1;
        state = "RECORDING";
        if (startTimesOut) { if (error) *error = "pzrec start did not answer within 15000 ms"; return false; }
        json = statusJson(state, open, openId, next);
        return true;
    }
    bool stop(bool abort, std::string& json, std::string* error) override {
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> lock(m);
            ++stopCalls;
            if (abort) ++abortStops;
            hook = onStop;
        }
        if (hook) hook();
        std::lock_guard<std::mutex> lock(m);
        if (stopFailures > 0) {
            --stopFailures;
            if (stopClosesWhileFailing) { open = false; state = "READY"; }
            if (error) *error = "pzrec stop did not answer within 30000 ms";
            return false;
        }
        open = false;
        state = "READY";
        json = statusJson(state, open, openId, next);
        return true;
    }
};

std::string runRow(uint32_t id, uint64_t written, bool open = false, bool deleted = false, uint32_t reason = 0, bool countsUnknown = false, uint32_t recSectors = 116) {
    char buf[900];
    std::snprintf(buf, sizeof buf,
                  R"([{"run_id":%u,"flags":0,"open":%d,"deleted":%d,"incomplete":0,"start_lba":2048,"end_lba":%llu,"start_unix_ms":1791530000000,"wall_source":1,"client_tag":1,"filter":0,"sampler_n":0,"rec_sectors":%u,"record_sectors":%u,"first_frame_id":1,"last_frame_id":%llu,"first_ticks":0,"last_ticks":1,"tick_hz":100000000,"seen":%llu,"empty_filtered":0,"invalid_not_sampled":0,"passed":%llu,"written":%llu,"dropped":0,"failed":0,"recoveries":0,"reason":%u,"counts_unknown":%s}])",
                  id, open, deleted, (unsigned long long)(2048 + written * recSectors + 1), recSectors, recSectors, (unsigned long long)written, (unsigned long long)written,
                  (unsigned long long)written, (unsigned long long)written, reason, countsUnknown ? "true" : "false");
    return buf;
}

pz::SsdStartArgs args() {
    pz::SsdStartArgs a;
    a.filter = pz::SsdFilter::ValidOnly;
    a.samplerN = 100;
    a.clientTag = 9;
    a.startUnixMs = 1791530000000ull;
    a.clientSynced = true;
    return a;
}

std::unique_ptr<pz::SsdStore> storeOver(FakeRecorder*& raw) {
    auto dev = std::make_unique<FakeRecorder>();
    raw = dev.get();
    auto store = std::make_unique<pz::SsdStore>(std::move(dev), milliseconds(1), /*background=*/false);
    store->setStopRetries(5, milliseconds(5));
    return store;
}

} // namespace

int main(int argc, char** argv) {
    // --- the filter setting (MIB_SSD_FILTER) ------------------------------------------------------------------
    {
        pz::SsdFilter f = pz::SsdFilter::All;
        std::string problem;
        MIB_EXPECT(pz::SsdStore::parseFilter(nullptr, f, &problem) && f == pz::SsdFilter::ValidOnly, "unset: valid");
        MIB_EXPECT(pz::SsdStore::parseFilter("", f, &problem) && f == pz::SsdFilter::ValidOnly, "empty: valid");
        MIB_EXPECT(pz::SsdStore::parseFilter("all", f, &problem) && f == pz::SsdFilter::All, "all");
        MIB_EXPECT(pz::SsdStore::parseFilter("any", f, &problem) && f == pz::SsdFilter::AnyResult, "any");
        MIB_EXPECT(pz::SsdStore::parseFilter("valid", f, &problem) && f == pz::SsdFilter::ValidOnly, "valid");
        MIB_EXPECT(!pz::SsdStore::parseFilter("ALL", f, &problem) && problem.find("MIB_SSD_FILTER=ALL") != std::string::npos, "anything else is a problem: " + problem);
        FakeRecorder* dev = nullptr;
        auto store = storeOver(dev);
        store->setFilterFromEnv("everything");
        MIB_EXPECT(store->filter() == pz::SsdFilter::ValidOnly && !store->configProblem().empty(), "a bad value keeps valid and records the problem");
        const auto p = store->prepareRun();
        MIB_EXPECT(!p.ok && p.why.find("MIB_SSD_FILTER=everything") != std::string::npos && dev->statusCalls == 0, "a bad filter refuses the run before pzrec is asked: " + p.why);
        store->setFilterFromEnv("all");
        MIB_EXPECT(store->configProblem().empty() && store->filter() == pz::SsdFilter::All && store->prepareRun().ok, "a good value clears it");
    }

    // --- prepare: READY and no open run, or a refusal that names the state ----------------------------------------
    {
        FakeRecorder* dev = nullptr;
        auto store = storeOver(dev);
        auto p = store->prepareRun();
        MIB_EXPECT(p.ok && p.runId == 2, "READY: the run id is next_run_id");
        for (const char* st : {"WEDGED", "RAW_FULL", "RUN_TABLE_FULL", "RECOVERING", "INITIALISING", "UNFORMATTED", "ABSENT"}) {
            dev->state = st;
            p = store->prepareRun();
            MIB_EXPECT(!p.ok && !p.why.empty(), std::string("refused in ") + st + ": " + p.why);
            MIB_EXPECT(!p.ok, std::string("no run in ") + st);
        }
        dev->state = "READY";
        dev->open = true;
        dev->openId = 1;
        p = store->prepareRun();
        MIB_EXPECT(!p.ok && p.why.find("open run") != std::string::npos, "an open run refuses: " + p.why);
        dev->open = false;
        dev->statusError = "pzrec status exited with 1: cannot read the disk";
        p = store->prepareRun();
        MIB_EXPECT(!p.ok && p.state == pz::SsdState::Wedged && p.why.find("cannot read the disk") != std::string::npos, "a failing pzrec status refuses, never READY: " + p.why);
        MIB_EXPECT(!pz::SsdStore(nullptr).prepareRun().ok, "no device configured: refused");
        // no retry loop: one prepare is one status call per refresh
        dev->statusError.clear();
        dev->statusCalls = 0;
        (void)store->prepareRun();
        MIB_EXPECT(dev->statusCalls == 1, "prepare reads the status once");
    }

    // --- start: the run pzrec opens must have the id that went into RUN_ID --------------------------------------
    {
        FakeRecorder* dev = nullptr;
        auto store = storeOver(dev);
        auto p = store->prepareRun();
        std::string why;
        bool opened = true;
        MIB_REQUIRE(store->startRun(p.runId, args(), &why, &opened), why);
        MIB_EXPECT(!opened, "a good start reports nothing to abort");
        MIB_EXPECT(dev->lastStart.samplerN == 100 && dev->lastStart.clientTag == 9 && dev->lastStart.clientSynced && dev->lastStart.startUnixMs == 1791530000000ull, "the arguments reach pzrec");
        MIB_EXPECT(store->openedRunId() == 2, "the store remembers the run it opened");
        const auto st = store->status();
        MIB_EXPECT(st.state == pz::SsdState::Recording && st.openRunId == 2, "RECORDING after the start");
        // a second prepare while recording is refused
        MIB_EXPECT(!store->prepareRun().ok, "no second run while one is open");

        // mismatch: pzrec opened another id
        FakeRecorder* dev2 = nullptr;
        auto store2 = storeOver(dev2);
        dev2->startOpensId = 5;
        p = store2->prepareRun();
        MIB_EXPECT(!store2->startRun(p.runId, args(), &why, &opened) && opened && why.find("mismatch") != std::string::npos, "a different id fails closed and asks for an abort: " + why);
        MIB_EXPECT(store2->openedRunId() == 5, "the store knows which run to abort");

        // refused start: nothing opened
        FakeRecorder* dev3 = nullptr;
        auto store3 = storeOver(dev3);
        dev3->startRefuses = true;
        p = store3->prepareRun();
        opened = true;
        MIB_EXPECT(!store3->startRun(p.runId, args(), &why, &opened) && !opened && why.find("start refused") != std::string::npos, "a refusal opens nothing: " + why);
        MIB_EXPECT(store3->openedRunId() == 0, "nothing to stop");

        // a start that timed out after opening the run: the status tells, the caller aborts
        FakeRecorder* dev4 = nullptr;
        auto store4 = storeOver(dev4);
        dev4->startTimesOut = true;
        p = store4->prepareRun();
        MIB_EXPECT(!store4->startRun(p.runId, args(), &why, &opened) && opened, "a timed-out start that opened a run asks for an abort: " + why);

        // a timed-out start whose status cannot be read afterwards is assumed open
        FakeRecorder* dev5 = nullptr;
        auto store5 = storeOver(dev5);
        dev5->startTimesOut = true;
        p = store5->prepareRun();
        dev5->statusError = "pzrec status did not answer within 3000 ms";
        MIB_EXPECT(!store5->startRun(p.runId, args(), &why, &opened) && opened, "unknown after a timeout: assume a run is open");
    }

    // --- stop: never blocks, retries, honest while stopping ----------------------------------------------------
    {
        FakeRecorder* dev = nullptr;
        auto store = storeOver(dev);
        std::string why;
        auto p = store->prepareRun();
        MIB_REQUIRE(store->startRun(p.runId, args(), &why), why);

        // a stop that blocks: beginStop returns at once, the state reads STOPPING, a short wait says "still stopping"
        std::mutex gm;
        std::condition_variable gcv;
        bool release = false;
        dev->onStop = [&] {
            std::unique_lock<std::mutex> lock(gm);
            gcv.wait(lock, [&] { return release; });
        };
        const auto t0 = std::chrono::steady_clock::now();
        store->beginStop();
        MIB_EXPECT(std::chrono::steady_clock::now() - t0 < milliseconds(200), "beginStop does not wait for pzrec");
        MIB_EXPECT(store->stopping() && store->status().state == pz::SsdState::Stopping, "STOPPING while pzrec stops");
        bool still = false;
        MIB_EXPECT(!store->waitStopped(milliseconds(50), &why, &still) && still, "a short wait reports that the stop is still running");
        MIB_EXPECT(store->status().state == pz::SsdState::Stopping, "the status call is not blocked by the stop");
        {
            std::lock_guard<std::mutex> lock(gm);
            release = true;
        }
        gcv.notify_all();
        MIB_EXPECT(store->waitStopped(milliseconds(5000), &why, &still), "the stop completes: " + why);
        MIB_EXPECT(!store->stopping() && store->openedRunId() == 0, "closed");
        MIB_EXPECT(store->status().state == pz::SsdState::Ready, "READY after the stop");

        // retries: two failed attempts, the third works
        FakeRecorder* dev2 = nullptr;
        auto store2 = storeOver(dev2);
        p = store2->prepareRun();
        MIB_REQUIRE(store2->startRun(p.runId, args(), &why), why);
        dev2->stopFailures = 2;
        store2->beginStop();
        MIB_EXPECT(store2->waitStopped(milliseconds(5000), &why), "a stop that fails twice is retried: " + why);
        MIB_EXPECT(dev2->stopCalls == 3, "three attempts");

        // every attempt fails: the run stays open and the reason is given
        FakeRecorder* dev3 = nullptr;
        auto store3 = storeOver(dev3);
        p = store3->prepareRun();
        MIB_REQUIRE(store3->startRun(p.runId, args(), &why), why);
        dev3->stopFailures = 100;
        store3->beginStop();
        MIB_EXPECT(!store3->waitStopped(milliseconds(5000), &why, &still) && !still && why.find("did not answer") != std::string::npos, "all attempts failed: " + why);
        MIB_EXPECT(store3->openedRunId() == p.runId, "the run is still open as far as Studio knows");
        MIB_EXPECT(dev3->stopCalls == 5, "five attempts");

        // after every attempt failed pzrec is read again (the drain was stopped first): the run is open, the reason is the fault text
        dev3->statusCalls = 0;
        store3->refresh(true);
        {
            const auto sf = store3->status();
            MIB_EXPECT(dev3->statusCalls == 1 && sf.reason.find("not confirmed closed") != std::string::npos && sf.reason.find("did not answer") != std::string::npos,
                       "a failed stop is polled and shown with its reason: " + sf.reason);
        }
        // pzrec reports the run closed by itself: Studio forgets it
        {
            FakeRecorder* devF = nullptr;
            auto storeF = storeOver(devF);
            std::string whyF;
            auto pf = storeF->prepareRun();
            MIB_REQUIRE(storeF->startRun(pf.runId, args(), &whyF), whyF);
            devF->stopFailures = 100;
            storeF->beginStop();
            MIB_EXPECT(!storeF->waitStopped(milliseconds(5000), &whyF), "every stop attempt failed");
            {
                std::lock_guard<std::mutex> lock(devF->m);
                devF->open = false;
                devF->state = "READY";
            }
            storeF->refresh(true);
            MIB_EXPECT(storeF->openedRunId() == 0 && storeF->status().state == pz::SsdState::Ready, "a run pzrec reports closed is forgotten");
        }

        // the next prepare retries the stop of a run that was never confirmed closed
        dev3->stopFailures = 0;
        auto again = store3->prepareRun();
        MIB_EXPECT(!again.ok && again.why.find("closing it now") != std::string::npos, "the next prepare closes the stuck run first: " + again.why);
        MIB_EXPECT(store3->waitStopped(milliseconds(5000), &why), "and that stop works: " + why);
        MIB_EXPECT(store3->prepareRun().ok, "then the SSD is usable again");

        // a failed attempt that closed the run anyway (killed after its work) counts as stopped
        FakeRecorder* dev4 = nullptr;
        auto store4 = storeOver(dev4);
        p = store4->prepareRun();
        MIB_REQUIRE(store4->startRun(p.runId, args(), &why), why);
        dev4->stopFailures = 1;
        dev4->stopClosesWhileFailing = true;
        store4->beginStop();
        MIB_EXPECT(store4->waitStopped(milliseconds(5000), &why) && dev4->stopCalls == 1, "pzrec's own answer decides: " + why);

        // an abort is passed through
        FakeRecorder* dev5 = nullptr;
        auto store5 = storeOver(dev5);
        p = store5->prepareRun();
        MIB_REQUIRE(store5->startRun(p.runId, args(), &why), why);
        store5->beginStop(true);
        MIB_EXPECT(store5->waitStopped(milliseconds(5000), &why) && dev5->abortStops == 1, "abort");

        // the destructor does not wait out a long backoff
        FakeRecorder* dev6 = nullptr;
        auto store6 = storeOver(dev6);
        store6->setStopRetries(5, milliseconds(60000));
        p = store6->prepareRun();
        MIB_REQUIRE(store6->startRun(p.runId, args(), &why), why);
        dev6->stopFailures = 100;
        store6->beginStop();
        std::this_thread::sleep_for(milliseconds(50));
        const auto t1 = std::chrono::steady_clock::now();
        store6.reset();
        MIB_EXPECT(std::chrono::steady_clock::now() - t1 < milliseconds(2000), "shutdown cancels the retry backoff");
    }

    // --- while Studio's own run is open pzrec is not called at all (the PL's gate holds the disk back) ----------
    {
        FakeRecorder* dev = nullptr;
        auto store = storeOver(dev);
        std::string why;
        auto p = store->prepareRun();
        MIB_REQUIRE(store->startRun(p.runId, args(), &why), why);
        dev->statusCalls = 0;
        dev->statusError = "pzrec status exited with 1: cannot read the disk: the block path is held back";
        for (int i = 0; i < 5; ++i) store->refresh(true);
        (void)store->status();
        MIB_EXPECT(dev->statusCalls == 0, "no pzrec status during the run");
        const auto st = store->status();
        MIB_EXPECT(st.state == pz::SsdState::Recording && st.openRunId == p.runId && st.reason.find("not read while the run is active") != std::string::npos,
                   "RECORDING with the run Studio opened and the reason there are no counters: " + st.reason);
        MIB_EXPECT(!store->prepareRun().ok && store->openedRunId() == p.runId && dev->stopCalls == 0, "a prepare during a run refuses and does not stop it");
        // after the stop the disk is read again
        dev->statusError.clear();
        store->beginStop();
        MIB_REQUIRE(store->waitStopped(milliseconds(5000), &why), why);
        MIB_EXPECT(store->status().state == pz::SsdState::Ready && dev->statusCalls > 0, "READY after the stop, read from pzrec");
    }

    // --- export lease (#667): one lease for the whole download, two-way exclusion with Start --------------------------------
    {
        FakeRecorder* dev = nullptr;
        auto store = storeOver(dev);
        dev->runsText = runRow(5, 1000);
        auto b = store->exportBegin(5);
        MIB_REQUIRE(b.ok, b.reason);
        MIB_EXPECT(b.lease != 0 && b.runId == 5 && b.records == 1000 && b.bytes == 1000ull * 59392 && b.maxSeconds > 80, "run 5: records, bytes (x 59,392) and a bound");
        MIB_EXPECT(b.argv.size() >= 4 && b.argv[1] == "read" && b.argv[2] == "5" && b.argv[3] == "disk" && b.argv.back() == "--summary", "RUN before IMG, --summary, one argument per element");
        MIB_EXPECT(store->exportActive(), "the lease is held");
        // two-way exclusion
        auto p = store->prepareRun();
        MIB_EXPECT(!p.ok && p.why.find("export in progress") != std::string::npos, "Start is refused while a download is in flight: " + p.why);
        auto second = store->exportBegin(5);
        MIB_EXPECT(!second.ok && second.reason.find("another export") != std::string::npos, "one lease at a time: " + second.reason);
        // the refresh stands down
        dev->statusCalls = 0;
        for (int i = 0; i < 3; ++i) store->refresh(true);
        (void)store->status();
        MIB_EXPECT(dev->statusCalls == 0, "no pzrec status while the lease is held");
        store->exportEnd(b.lease, b.bytes, "complete");
        MIB_EXPECT(!store->exportActive(), "released after the end");
        store->exportEnd(b.lease, b.bytes, "complete again");     // idempotent
        store->exportEnd(999, 0, "unknown lease");
        MIB_EXPECT(!store->exportActive() && store->prepareRun().ok, "Start works again after the end");
        store->cancelPrepare();
        // Start first, then the download: refused (a Start passed prepareRun, or a run is open)
        auto pr = store->prepareRun();
        MIB_REQUIRE(pr.ok, pr.why);
        auto late = store->exportBegin(5);
        MIB_EXPECT(!late.ok && late.reason.find("starting") != std::string::npos, "download after Start's prepare is refused: " + late.reason);
        store->cancelPrepare();
        MIB_EXPECT(store->exportBegin(5).ok, "a Start that did not reach startRun() frees the SSD again (cancelPrepare)");
    }
    {
        FakeRecorder* dev = nullptr;
        auto store = storeOver(dev);
        std::string why;
        dev->runsText = runRow(5, 1000);
        auto p = store->prepareRun();
        MIB_REQUIRE(p.ok, p.why);
        MIB_REQUIRE(store->startRun(p.runId, args(), &why), why);
        auto b = store->exportBegin(5);
        MIB_EXPECT(!b.ok && b.reason.find("recording") != std::string::npos, "download while Studio records: refused: " + b.reason);
        dev->windowCalls = 0;
        store->beginStop();
        MIB_REQUIRE(store->waitStopped(milliseconds(5000), &why), why);
        MIB_EXPECT(store->exportBegin(5).ok, "after the stop the download is possible");
    }
    {
        // refusals, one by one
        FakeRecorder* dev = nullptr;
        auto store = storeOver(dev);
        dev->runsText = runRow(5, 1000);
        for (const char* st : {"ARMED", "DRAINING", "STOPPING", "FAULT", "WEDGED"}) {
            dev->windowState = st;
            auto b = store->exportBegin(5);
            MIB_EXPECT(!b.ok && b.reason.find(st) != std::string::npos && !store->exportActive(), std::string("a window in ") + st + " refuses and leaves no lease: " + b.reason);
        }
        dev->windowState = "IDLE";
        dev->windowError = "window absent or counters torn";
        auto b = store->exportBegin(5);
        MIB_EXPECT(!b.ok && b.reason.find("cannot verify") != std::string::npos && !store->exportActive(), "no window reading: cannot verify idle, refused: " + b.reason);
        dev->windowError.clear();
        MIB_EXPECT(!store->exportBegin(6).ok && store->exportBegin(6).reason.find("no such run") != std::string::npos, "no such run");
        dev->runsText = runRow(5, 1000, false, true);
        MIB_EXPECT(store->exportBegin(5).reason.find("deleted") != std::string::npos, "deleted run");
        dev->runsText = runRow(5, 1000, true);
        MIB_EXPECT(store->exportBegin(5).reason.find("still open") != std::string::npos, "open run");
        dev->runsText = runRow(5, 1000, false, false, 7, true);
        MIB_EXPECT(store->exportBegin(5).reason.find("no reliable record count") != std::string::npos, "a run recovered at mount (count unknown) is not offered");
        dev->runsText = runRow(5, 0);
        MIB_EXPECT(store->exportBegin(5).reason.find("no records") != std::string::npos, "an empty run");
        dev->runsText = runRow(5, 1000, false, false, 0, false, 100);
        MIB_EXPECT(store->exportBegin(5).reason.find("record size") != std::string::npos, "an unexpected record size");
        dev->runsText = runRow(5, 1000);
        auto w = store->exportBegin(5, 10, 100);
        MIB_REQUIRE(w.ok, w.reason);
        MIB_EXPECT(w.records == 100 && w.bytes == 100ull * 59392 && w.argv[w.argv.size() - 4] == "--from" && w.argv[w.argv.size() - 3] == "10" && w.argv[w.argv.size() - 2] == "--count" && w.argv.back() == "100", "a window: count x 59,392 and --from/--count");
        store->exportEnd(w.lease, 0, "test");
        MIB_EXPECT(store->exportBegin(5, 1000).reason.find("outside the run") != std::string::npos, "--from beyond the run is refused");
        MIB_EXPECT(store->exportBegin(5, 900, 200).reason.find("never clipped") != std::string::npos, "--count past the end is refused, not clipped");
        auto tail = store->exportBegin(5, 900);
        MIB_EXPECT(tail.ok && tail.records == 100, "from alone runs to the end");
        store->exportEnd(tail.lease, 0, "test");
        // the lease expires by itself: a caller that never ends it does not hold the SSD for ever
        // the lease outlives the reader's own bound by the kill margin (the reader is terminated at maxSeconds, SIGKILLed later): still held right after the bound
        store->setExportMaxSecondsForTesting(1, 2);
        auto e = store->exportBegin(5);
        MIB_REQUIRE(e.ok && e.maxSeconds == 1, e.reason);
        std::this_thread::sleep_for(milliseconds(1300));
        MIB_EXPECT(store->exportActive() && !store->exportBegin(5).ok, "the lease is still held after the reader's bound, during its kill margin");
        std::this_thread::sleep_for(milliseconds(2000));
        MIB_EXPECT(!store->exportActive() && store->exportBegin(5).ok, "an expired lease is released and a new one can be taken");
    }

    // --- the real pzrec on a fake disk ----------------------------------------------------------------------------
    if (argc > 1) {
        const std::string pzrec = argv[1];
        const std::string img = "/tmp/ssd_record_test." + std::to_string(::getpid()) + ".img";
        const std::string cmd = pzrec + " format " + img + " --size-mib 512 --min-run-mib 64 > /dev/null";
        MIB_REQUIRE(std::system(cmd.c_str()) == 0, "pzrec format");
        pz::SsdStore store(std::make_unique<pz::PzrecCliDevice>(pzrec, img), milliseconds(1), false);
        store.setStopRetries(2, milliseconds(10));
        std::string why;
        auto p = store.prepareRun();
        MIB_REQUIRE(p.ok, p.why);
        MIB_REQUIRE(store.startRun(p.runId, args(), &why), why);
        MIB_EXPECT(store.status().state == pz::SsdState::Recording, "real pzrec: RECORDING");
        std::this_thread::sleep_for(milliseconds(1200));
        store.beginStop();
        MIB_REQUIRE(store.waitStopped(milliseconds(20000), &why), why);
        store.refresh(true);
        MIB_EXPECT(store.status().state == pz::SsdState::Ready, "real pzrec: READY after the stop");
        std::vector<pz::SsdRun> runs;
        MIB_REQUIRE(store.runs(runs, &why) && !runs.empty(), why);
        MIB_EXPECT(runs.back().id == p.runId && !runs.back().open && runs.back().reason == 0 && runs.back().clientTag == 9 && runs.back().startUnixMs == 1791530000000ull &&
                       runs.back().wallSource == 1,
                   "real pzrec: the table holds the run with the arguments of the start");
        MIB_EXPECT(store.prepareRun().runId == p.runId + 1, "real pzrec: ids are monotone");
        std::remove(img.c_str());
        std::remove((img + ".fakehw").c_str());
        std::remove((img + ".fakeparams").c_str());
    }
    return mib::test::exitCode();
}
