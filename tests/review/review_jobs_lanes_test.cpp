// review_jobs_lanes_test (#512 S7, plan
// docs/exec-plans/active/2026-10-18-one-review-implementation.md, decision D7
// as amended 2026-10-19)
//
// ReviewJobs' lanes and host hooks, driven with job bodies supplied through
// ReviewJobsTestAccess, so every interleaving is set by handshakes (a body
// waits until the test releases it or its cancel flag is set), never by
// timing. Both shipped policies are checked through their named presets:
//  - ReviewJobsOptions::forReviewBridge() (YOFO Review): one job of any kind
//    at a time ("Another review job is still running" for either lane, #722),
//    ReviewJobs' own ids from 1 with Started through the sink, the lane free
//    before the terminal event (busy() is false when it arrives), and a start
//    from inside the terminal event works (no self-join);
//  - ReviewJobsOptions::forFacade(host) (MIB Studio): one export-lane and one
//    processing-lane job at the same time, a second start on a busy lane
//    refused with that job's message, the host's ids, Started and cancel
//    flags, the lane free only after the terminal event (a start from inside
//    it is refused).
// Then lanes x cancel x shutdown: cancel one lane while the other runs,
// cancel by id and through the host's flag, unknown / finished ids, shutdown
// with both lanes held (each job ends Cancelled, shutdown returns after both
// terminals, nothing reported after it, starts work again), concurrent starts
// racing for the lanes (exactly one winner per lane, or one overall), a cancel
// racing a job's end, and 200 mixed cycles per policy; a shutdown from inside
// a terminal event followed by destroying ReviewJobs while that event's
// callback still runs (the detached worker keeps the runner state it uses
// alive; #742 review). #742 review round 2: a shutdown() from inside a job's
// own Started event (the sink; the facade preset: the host's beginOperation)
// does not let the job run: one Cancelled terminal from the start itself,
// the JSON jobs' terminal and status snapshot byte for byte with their
// engine run with the flag set on entry; and a shutdown() from a terminal
// event while another thread's start is joining that event's worker returns
// (it used to wait for that start: deadlock). #742 review round 3: a
// shutdown() from outside the runner (the owner's teardown) does wait for a
// start in progress on another thread, here held inside its Started event:
// it returns only after that start reported its Cancelled. Lane release is waited for on ReviewJobs' own
// condition (ReviewJobsTestAccess::waitIdle). Every operation must
// see Started first, Progress only before its one terminal, nothing after.
// `cancelOverridesOutcome`: a cancelled own job that fails reports Cancelled,
// a JSON-style job keeps its Failed. The kind -> lane map. Runs in the TSan
// lane (labels backend / concurrency). Watchdog guarded.

#include "backend/processing/ProcessingService.h"
#include "backend/review/ExportEngine.h"
#include "backend/review/Reanalysis.h"
#include "backend/review/ReviewJobs.h"
#include "backend/review/ReviewSession.h"

#include "support/assert.h"
#include "support/watchdog.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace backend::review;

namespace
{
    using Clock = std::chrono::steady_clock;
    using CancelFlag = std::shared_ptr<std::atomic<bool>>;

    bool isTerminal(ReviewJobState s)
    {
        return s == ReviewJobState::Completed || s == ReviewJobState::Failed || s == ReviewJobState::Cancelled ||
               s == ReviewJobState::TimedOut;
    }

    // Every event per operation, with the ADR 0004 invariants checked as they
    // arrive. Started comes from the sink (own ids) or the fake host.
    struct Recorder
    {
        std::mutex m;
        std::condition_variable cv;
        std::map<std::uint64_t, std::vector<ReviewJobState>> byId;
        std::map<std::uint64_t, std::string> terminalMessage;
        std::size_t total{0};
        int violations{0};
        // Called on the reporting thread for terminal events, outside the lock.
        std::mutex hookMutex;
        std::function<void(const ReviewJobEvent &)> onTerminal;
        void setOnTerminal(std::function<void(const ReviewJobEvent &)> hook)
        {
            std::scoped_lock lock(hookMutex);
            onTerminal = std::move(hook);
        }

        void record(std::uint64_t id, ReviewJobState state, const std::string &message)
        {
            std::scoped_lock lock(m);
            auto &seq = byId[id];
            if (state == ReviewJobState::Started && !seq.empty()) ++violations;
            if (state != ReviewJobState::Started && (seq.empty() || seq.front() != ReviewJobState::Started)) ++violations;
            if (!seq.empty() && isTerminal(seq.back())) ++violations; // nothing after the terminal
            seq.push_back(state);
            if (isTerminal(state)) terminalMessage[id] = message;
            ++total;
            cv.notify_all();
        }
        void operator()(const ReviewJobEvent &e)
        {
            if (isTerminal(e.state))
            {
                std::function<void(const ReviewJobEvent &)> hook;
                {
                    std::scoped_lock lock(hookMutex);
                    hook = onTerminal;
                }
                if (hook) hook(e);
            }
            record(e.operationId, e.state, e.message);
        }
        bool waitTerminal(std::uint64_t id, ReviewJobState *state = nullptr)
        {
            std::unique_lock lock(m);
            const bool ok = cv.wait_for(lock, std::chrono::seconds(30), [&] {
                const auto it = byId.find(id);
                return it != byId.end() && !it->second.empty() && isTerminal(it->second.back());
            });
            if (ok && state) *state = byId[id].back();
            return ok;
        }
        std::string messageOf(std::uint64_t id)
        {
            std::scoped_lock lock(m);
            const auto it = terminalMessage.find(id);
            return it == terminalMessage.end() ? std::string{} : it->second;
        }
        int terminals(std::uint64_t id)
        {
            std::scoped_lock lock(m);
            int n = 0;
            for (auto s : byId[id]) n += isTerminal(s) ? 1 : 0;
            return n;
        }
        bool hasProgress(std::uint64_t id)
        {
            std::scoped_lock lock(m);
            for (auto s : byId[id])
                if (s == ReviewJobState::Progress) return true;
            return false;
        }
        std::size_t count()
        {
            std::scoped_lock lock(m);
            return total;
        }
        int violationCount()
        {
            std::scoped_lock lock(m);
            return violations;
        }
    };

    // A fake host in the facade's shape: its own ids (from 1000), a cancel
    // flag per operation, Started recorded when the operation begins.
    struct FakeHost
    {
        Recorder &recorder;
        std::mutex m;
        std::uint64_t next{1000};
        std::map<std::uint64_t, CancelFlag> flags;
        std::map<std::uint64_t, std::pair<ReviewJobKind, std::string>> begun;

        ReviewJobHost hook()
        {
            return ReviewJobHost{[this](ReviewJobKind kind, const std::string &message, CancelFlag &cancel) {
                std::uint64_t id = 0;
                {
                    std::scoped_lock lock(m);
                    id = next++;
                    cancel = std::make_shared<std::atomic<bool>>(false);
                    flags[id] = cancel;
                    begun[id] = {kind, message};
                }
                recorder.record(id, ReviewJobState::Started, message);
                return id;
            }};
        }
        CancelFlag flag(std::uint64_t id)
        {
            std::scoped_lock lock(m);
            return flags[id];
        }
    };

    // A held job: reports one Progress, then waits until released or its
    // cancel flag is set (polled: the flag has no condition variable).
    struct Gate
    {
        std::mutex m;
        std::condition_variable cv;
        bool entered{false};
        bool released{false};
        void enter()
        {
            std::scoped_lock lock(m);
            entered = true;
            cv.notify_all();
        }
        bool waitEntered()
        {
            std::unique_lock lock(m);
            return cv.wait_for(lock, std::chrono::seconds(30), [&] { return entered; });
        }
        void release()
        {
            std::scoped_lock lock(m);
            released = true;
            cv.notify_all();
        }
        // True when released, false when cancelled (the flag is polled: it
        // has no condition variable).
        bool hold(const CancelFlag &cancel)
        {
            std::unique_lock lock(m);
            const auto deadline = Clock::now() + std::chrono::seconds(60);
            while (!released)
            {
                if (cancel->load()) return false;
                if (Clock::now() > deadline) return false;
                cv.wait_for(lock, std::chrono::milliseconds(1));
            }
            return true;
        }
    };

    // Concurrency bookkeeping: the bodies that run at once, per lane and in all.
    struct Occupancy
    {
        std::atomic<int> lane[2]{};
        std::atomic<int> all{0};
        std::atomic<int> maxLane[2]{};
        std::atomic<int> maxAll{0};
        static void raise(std::atomic<int> &m, int v)
        {
            int cur = m.load();
            while (v > cur && !m.compare_exchange_weak(cur, v)) {}
        }
        void enter(ReviewJobLane l)
        {
            const auto i = static_cast<std::size_t>(l);
            raise(maxLane[i], ++lane[i]);
            raise(maxAll, ++all);
        }
        void leave(ReviewJobLane l)
        {
            --lane[static_cast<std::size_t>(l)];
            --all;
        }
    };

    ReviewJobsTestAccess::Body heldBody(Gate &gate, Occupancy &occupancy, ReviewJobLane lane,
                                        ReviewJobState onRelease = ReviewJobState::Completed)
    {
        return [&gate, &occupancy, lane, onRelease](std::uint64_t, const CancelFlag &cancel,
                                                    const ReviewJobsTestAccess::Progress &progress) {
            occupancy.enter(lane);
            progress(1, 2);
            gate.enter();
            const bool released = gate.hold(cancel);
            occupancy.leave(lane);
            if (!released) return ReviewJobsTestAccess::Result{ReviewJobState::Cancelled, "cancelled"};
            return ReviewJobsTestAccess::Result{onRelease, onRelease == ReviewJobState::Completed ? "done" : "failed"};
        };
    }

    // Lane release is waited for on ReviewJobs' own condition (no sleeps).
    constexpr std::chrono::seconds kWait{30};
    bool idle(const ReviewJobs &jobs) { return ReviewJobsTestAccess::waitIdle(jobs, kWait); }
    bool idle(const ReviewJobs &jobs, ReviewJobLane lane) { return ReviewJobsTestAccess::waitIdle(jobs, lane, kWait); }

    // The cancel flag has no condition variable: a body observes it by
    // polling, as ReviewJobs' own jobs do.
    bool waitForCancel(const CancelFlag &cancel)
    {
        const auto deadline = Clock::now() + std::chrono::seconds(60);
        while (!cancel->load())
        {
            if (Clock::now() > deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    // Kinds used for the custom jobs: one per lane.
    constexpr ReviewJobKind kExportKind = ReviewJobKind::Export;
    constexpr ReviewJobKind kProcessingKind = ReviewJobKind::Reanalysis;
    const std::string kExportBusy = "An export is already active";
    const std::string kProcessingBusy = "Reanalysis already active";
    const std::string kAnother = "Another review job is still running";

    std::uint64_t startHeld(ReviewJobs &jobs, ReviewJobKind kind, Gate &gate, Occupancy &occupancy,
                            std::string *error = nullptr)
    {
        const auto lane = laneOf(kind);
        return ReviewJobsTestAccess::start(jobs, kind, heldBody(gate, occupancy, lane), error, true,
                                           lane == ReviewJobLane::Export ? kExportBusy : kProcessingBusy);
    }

    // ---- one policy ---------------------------------------------------------
    struct Policy
    {
        const char *name;
        bool facade; // forFacade (concurrent lanes, host, AfterTerminal) vs forReviewBridge
    };

    void runPolicy(const Policy &policy, mib::test::Watchdog &wd)
    {
        const std::string tag = std::string("[") + policy.name + "] ";
        ReviewSession session; // never opened: the custom jobs read no file
        Recorder recorder;
        FakeHost host{recorder};
        const ReviewJobsOptions options =
            policy.facade ? ReviewJobsOptions::forFacade(host.hook()) : ReviewJobsOptions::forReviewBridge();
        MIB_REQUIRE(options.concurrentLanes == policy.facade, tag + "preset lane policy");
        MIB_REQUIRE((options.laneRelease == ReviewLaneRelease::AfterTerminal) == policy.facade, tag + "preset release");
        MIB_REQUIRE(static_cast<bool>(options.host.beginOperation) == policy.facade, tag + "preset host");
        auto jobs = std::make_unique<ReviewJobs>(session, nullptr, std::ref(recorder), options);
        std::string err;

        // ---- lanes: refusals while held ---------------------------------
        {
            Occupancy occ;
            Gate a, b;
            const auto ida = startHeld(*jobs, kExportKind, a, occ, &err);
            MIB_REQUIRE(ida != 0, tag + "first export-lane job: " + err);
            MIB_REQUIRE(a.waitEntered(), tag + "export-lane job running");
            if (policy.facade)
            {
                MIB_EXPECT(ida >= 1000, tag + "the host's id");
                MIB_EXPECT(host.begun[ida].first == kExportKind, tag + "host told the kind");
            }
            else
            {
                MIB_EXPECT(ida == 1, tag + "own ids from 1");
            }
            MIB_EXPECT(jobs->busy() && jobs->busy(ReviewJobLane::Export) && !jobs->busy(ReviewJobLane::Processing),
                       tag + "busy() per lane");
            Gate unused;
            err.clear();
            MIB_EXPECT(startHeld(*jobs, kExportKind, unused, occ, &err) == 0 && err == kExportBusy,
                       tag + "same lane refused with the job's message: " + err);
            err.clear();
            MIB_EXPECT(ReviewJobsTestAccess::start(*jobs, ReviewJobKind::ExportCharts, heldBody(unused, occ, ReviewJobLane::Export), &err) == 0 &&
                           err == kAnother,
                       tag + "own export-lane kind refused with the shared message: " + err);
            err.clear();
            const auto idb = startHeld(*jobs, kProcessingKind, b, occ, &err);
            if (policy.facade)
            {
                MIB_REQUIRE(idb != 0, tag + "processing lane runs beside the export lane: " + err);
                MIB_REQUIRE(b.waitEntered(), tag + "processing-lane job running");
                MIB_EXPECT(occ.maxAll.load() == 2, tag + "both lanes inside their bodies at once");
                err.clear();
                MIB_EXPECT(startHeld(*jobs, kProcessingKind, unused, occ, &err) == 0 && err == kProcessingBusy,
                           tag + "busy processing lane refused: " + err);
                err.clear();
                MIB_EXPECT(ReviewJobsTestAccess::start(*jobs, ReviewJobKind::Density, heldBody(unused, occ, ReviewJobLane::Processing), &err) == 0 &&
                               err == kAnother,
                           tag + "own processing kind refused: " + err);
            }
            else
            {
                MIB_EXPECT(idb == 0 && err == kAnother, tag + "single job: the other lane is refused too: " + err);
                err.clear();
                MIB_EXPECT(ReviewJobsTestAccess::start(*jobs, ReviewJobKind::Density, heldBody(unused, occ, ReviewJobLane::Processing), &err) == 0 &&
                               err == kAnother,
                           tag + "single job: density refused: " + err);
            }
            a.release();
            ReviewJobState sa{};
            MIB_REQUIRE(recorder.waitTerminal(ida, &sa), tag + "export-lane terminal");
            MIB_EXPECT(sa == ReviewJobState::Completed, tag + "released job completes");
            MIB_EXPECT(recorder.hasProgress(ida), tag + "progress reported");
            if (policy.facade)
            {
                MIB_EXPECT(jobs->busy(ReviewJobLane::Processing), tag + "processing lane still running");
                b.release();
                MIB_REQUIRE(recorder.waitTerminal(idb, &sa), tag + "processing terminal");
                MIB_EXPECT(sa == ReviewJobState::Completed, tag + "processing completes");
            }
            MIB_EXPECT(idle(*jobs), tag + "idle after both");
            MIB_EXPECT(occ.maxLane[0].load() == 1 && occ.maxLane[1].load() <= 1, tag + "single flight per lane");
            MIB_EXPECT(occ.maxAll.load() == (policy.facade ? 2 : 1), tag + "lane policy held");
        }
        wd.mark("lanes");

        // ---- cancel x lanes ------------------------------------------------
        {
            Occupancy occ;
            Gate a, b;
            const auto ida = startHeld(*jobs, kExportKind, a, occ, &err);
            MIB_REQUIRE(ida != 0 && a.waitEntered(), tag + "cancel: export-lane job");
            std::uint64_t idb = 0;
            if (policy.facade)
            {
                idb = startHeld(*jobs, kProcessingKind, b, occ, &err);
                MIB_REQUIRE(idb != 0 && b.waitEntered(), tag + "cancel: processing-lane job");
            }
            MIB_EXPECT(!jobs->cancel(ida + 7777), tag + "unknown id");
            MIB_EXPECT(jobs->cancel(ida), tag + "cancel the export lane by id");
            ReviewJobState s{};
            MIB_REQUIRE(recorder.waitTerminal(ida, &s), tag + "cancelled terminal");
            MIB_EXPECT(s == ReviewJobState::Cancelled, tag + "cancelled, not completed");
            MIB_EXPECT(idle(*jobs, ReviewJobLane::Export), tag + "export lane free");
            MIB_EXPECT(!jobs->cancel(ida), tag + "finished id");
            if (policy.facade)
            {
                MIB_EXPECT(jobs->busy(ReviewJobLane::Processing) && recorder.terminals(idb) == 0,
                           tag + "the other lane keeps running");
                // The host's own cancel (BackendFacade::requestOperationCancel) sets the flag it handed out.
                host.flag(idb)->store(true);
                MIB_REQUIRE(recorder.waitTerminal(idb, &s), tag + "host-cancelled terminal");
                MIB_EXPECT(s == ReviewJobState::Cancelled, tag + "the host's flag cancels the job");
            }
            MIB_EXPECT(idle(*jobs), tag + "idle after cancels");
        }
        wd.mark("cancel");

        // ---- terminal ordering (lane release) and a start from inside it ----
        {
            Occupancy occ;
            Gate a, next;
            std::atomic<int> laneBusyAtTerminal{-1};
            std::atomic<std::uint64_t> startedFromTerminal{0};
            std::string fromTerminalError;
            std::atomic<std::uint64_t> watched{0};
            recorder.setOnTerminal([&](const ReviewJobEvent &e) {
                if (e.operationId != watched.load()) return;
                laneBusyAtTerminal = jobs->busy(ReviewJobLane::Export) ? 1 : 0;
                // On the job's own worker thread: a start of the same lane.
                startedFromTerminal = startHeld(*jobs, kExportKind, next, occ, &fromTerminalError);
            });
            // Arm before the start: the released job may report its terminal
            // before the start call returns.
            const auto ida = startHeld(*jobs, kExportKind, a, occ, &err);
            MIB_REQUIRE(ida != 0 && a.waitEntered(), tag + "ordering job");
            watched = ida;
            a.release();
            MIB_REQUIRE(recorder.waitTerminal(ida), tag + "ordering terminal");
            recorder.setOnTerminal(nullptr);
            if (policy.facade)
            {
                MIB_EXPECT(laneBusyAtTerminal.load() == 1, tag + "AfterTerminal: lane busy during the terminal event");
                MIB_EXPECT(startedFromTerminal.load() == 0 && fromTerminalError == kExportBusy,
                           tag + "a start from the terminal event is refused: " + fromTerminalError);
            }
            else
            {
                MIB_EXPECT(laneBusyAtTerminal.load() == 0, tag + "BeforeTerminal: lane free when the terminal arrives");
                MIB_REQUIRE(startedFromTerminal.load() != 0,
                            tag + "a start from the terminal event runs (no self-join): " + fromTerminalError);
                MIB_REQUIRE(next.waitEntered(), tag + "the job started from the terminal event runs");
                next.release();
                MIB_REQUIRE(recorder.waitTerminal(startedFromTerminal.load()), tag + "its terminal");
            }
            MIB_EXPECT(idle(*jobs), tag + "idle after ordering");
        }
        wd.mark("ordering");

        // ---- cancelOverridesOutcome ----------------------------------------
        {
            for (const bool overrides : {true, false})
            {
                Occupancy occ;
                Gate g;
                const auto id = ReviewJobsTestAccess::start(
                    *jobs, kExportKind,
                    [&](std::uint64_t, const CancelFlag &cancel, const ReviewJobsTestAccess::Progress &) {
                        g.enter();
                        waitForCancel(cancel);
                        return ReviewJobsTestAccess::Result{ReviewJobState::Failed, "engine failed"};
                    },
                    &err, overrides);
                MIB_REQUIRE(id != 0 && g.waitEntered(), tag + "override job");
                MIB_EXPECT(jobs->cancel(id), tag + "override cancel");
                ReviewJobState s{};
                MIB_REQUIRE(recorder.waitTerminal(id, &s), tag + "override terminal");
                MIB_EXPECT(s == (overrides ? ReviewJobState::Cancelled : ReviewJobState::Failed),
                           tag + (overrides ? "own jobs: a cancelled failure is Cancelled" : "JSON jobs: the engine's Failed stays"));
                MIB_EXPECT(idle(*jobs), tag + "idle after override");
            }
        }

        // ---- shutdown with the lanes held -----------------------------------
        {
            Occupancy occ;
            Gate a, b;
            const auto ida = startHeld(*jobs, kExportKind, a, occ, &err);
            MIB_REQUIRE(ida != 0 && a.waitEntered(), tag + "shutdown: export lane held");
            std::uint64_t idb = 0;
            if (policy.facade)
            {
                idb = startHeld(*jobs, kProcessingKind, b, occ, &err);
                MIB_REQUIRE(idb != 0 && b.waitEntered(), tag + "shutdown: processing lane held");
            }
            jobs->shutdown(); // sets every lane's flag, joins: the held bodies see their flags
            const std::size_t after = recorder.count();
            MIB_EXPECT(recorder.terminals(ida) == 1, tag + "shutdown: one terminal for the export lane");
            ReviewJobState s{};
            MIB_EXPECT(recorder.waitTerminal(ida, &s) && s == ReviewJobState::Cancelled, tag + "shutdown cancels the export lane");
            if (policy.facade)
            {
                MIB_EXPECT(recorder.terminals(idb) == 1, tag + "shutdown: one terminal for the processing lane");
                MIB_EXPECT(recorder.waitTerminal(idb, &s) && s == ReviewJobState::Cancelled,
                           tag + "shutdown cancels the processing lane");
                MIB_EXPECT(host.flag(idb)->load(), tag + "shutdown set the host's flag");
            }
            MIB_EXPECT(!jobs->busy(), tag + "shutdown returns with both lanes free");
            MIB_EXPECT(recorder.count() == after, tag + "nothing reported after shutdown returned");
            // Starts work again after a shutdown (as before #512 S7).
            Gate c;
            const auto idc = startHeld(*jobs, kExportKind, c, occ, &err);
            MIB_REQUIRE(idc != 0 && c.waitEntered(), tag + "start after shutdown: " + err);
            c.release();
            MIB_REQUIRE(recorder.waitTerminal(idc, &s) && s == ReviewJobState::Completed, tag + "completes after shutdown");
            MIB_EXPECT(idle(*jobs), tag + "idle after restart");
        }
        wd.mark("shutdown");

        // ---- racing starts --------------------------------------------------
        for (int round = 0; round < 25; ++round)
        {
            Occupancy occ;
            constexpr int kThreads = 8;
            std::vector<std::unique_ptr<Gate>> gates;
            for (int i = 0; i < 2 * kThreads; ++i) gates.push_back(std::make_unique<Gate>());
            std::atomic<int> ready{0};
            std::atomic<bool> go{false};
            std::vector<std::uint64_t> ids(2 * kThreads, 0);
            std::vector<std::thread> threads;
            for (int i = 0; i < 2 * kThreads; ++i)
            {
                threads.emplace_back([&, i] {
                    ++ready;
                    while (!go.load()) std::this_thread::yield();
                    ids[i] = startHeld(*jobs, i < kThreads ? kExportKind : kProcessingKind, *gates[i], occ);
                });
            }
            while (ready.load() < 2 * kThreads) std::this_thread::yield();
            go = true;
            for (auto &t : threads) t.join();
            int winnersExport = 0, winnersProcessing = 0;
            for (int i = 0; i < 2 * kThreads; ++i)
                if (ids[i] != 0) (i < kThreads ? winnersExport : winnersProcessing)++;
            if (policy.facade)
                MIB_EXPECT(winnersExport == 1 && winnersProcessing == 1, tag + "one winner per lane");
            else
                MIB_EXPECT(winnersExport + winnersProcessing == 1, tag + "one winner overall");
            for (int i = 0; i < 2 * kThreads; ++i)
            {
                if (ids[i] == 0) continue;
                MIB_REQUIRE(gates[i]->waitEntered(), tag + "winner runs");
                gates[i]->release();
                MIB_REQUIRE(recorder.waitTerminal(ids[i]), tag + "winner terminal");
            }
            MIB_EXPECT(idle(*jobs), tag + "idle after race");
            MIB_EXPECT(occ.maxLane[0].load() <= 1 && occ.maxLane[1].load() <= 1, tag + "race: single flight per lane");
            MIB_EXPECT(occ.maxAll.load() <= (policy.facade ? 2 : 1), tag + "race: lane policy");
        }
        wd.mark("race");

        // ---- mixed cycles: complete / cancel one / cancel all / shutdown / racing cancel ----
        {
            Occupancy occ;
            for (int cycle = 0; cycle < 200; ++cycle)
            {
                Gate a, b;
                const auto ida = startHeld(*jobs, kExportKind, a, occ, &err);
                MIB_REQUIRE(ida != 0 && a.waitEntered(), tag + "cycle export lane: " + err);
                std::uint64_t idb = 0;
                if (policy.facade)
                {
                    idb = startHeld(*jobs, kProcessingKind, b, occ, &err);
                    MIB_REQUIRE(idb != 0 && b.waitEntered(), tag + "cycle processing lane: " + err);
                }
                ReviewJobState expectA = ReviewJobState::Completed, expectB = ReviewJobState::Completed;
                bool eitherA = false;
                switch (cycle % 5)
                {
                case 0: // both complete
                    a.release();
                    b.release();
                    break;
                case 1: // cancel the export lane, the processing lane completes
                    jobs->cancel(ida);
                    expectA = ReviewJobState::Cancelled;
                    b.release();
                    break;
                case 2: // cancel both (by id; the processing lane through the host's flag)
                    jobs->cancel(ida);
                    expectA = ReviewJobState::Cancelled;
                    if (idb)
                    {
                        host.flag(idb)->store(true);
                        expectB = ReviewJobState::Cancelled;
                    }
                    break;
                case 3: // shutdown in the middle
                    jobs->shutdown();
                    expectA = expectB = ReviewJobState::Cancelled;
                    break;
                case 4: // a cancel racing the job's end: either outcome, one terminal
                {
                    std::thread canceller([&] { jobs->cancel(ida); });
                    a.release();
                    canceller.join();
                    eitherA = true;
                    b.release();
                    break;
                }
                }
                ReviewJobState s{};
                MIB_REQUIRE(recorder.waitTerminal(ida, &s), tag + "cycle terminal A");
                MIB_EXPECT(eitherA ? (s == ReviewJobState::Completed || s == ReviewJobState::Cancelled) : s == expectA,
                           tag + "cycle " + std::to_string(cycle) + " export-lane outcome");
                if (idb)
                {
                    MIB_REQUIRE(recorder.waitTerminal(idb, &s), tag + "cycle terminal B");
                    MIB_EXPECT(s == expectB, tag + "cycle " + std::to_string(cycle) + " processing-lane outcome");
                }
                MIB_EXPECT(idle(*jobs), tag + "cycle idle");
                MIB_EXPECT(recorder.terminals(ida) == 1 && (!idb || recorder.terminals(idb) == 1), tag + "one terminal each");
            }
            MIB_EXPECT(occ.maxLane[0].load() <= 1 && occ.maxLane[1].load() <= 1, tag + "cycles: single flight per lane");
            MIB_EXPECT(occ.maxAll.load() <= (policy.facade ? 2 : 1), tag + "cycles: lane policy");
        }
        wd.mark("cycles");

        jobs.reset(); // destructor = shutdown, idle here
        MIB_EXPECT(recorder.violationCount() == 0,
                   tag + "Started first, nothing after a terminal: " + std::to_string(recorder.violationCount()) + " violations");
        // Every operation that started reached exactly one terminal.
        {
            std::scoped_lock lock(recorder.m);
            for (const auto &[id, seq] : recorder.byId)
            {
                int terminals = 0;
                for (auto st : seq) terminals += isTerminal(st) ? 1 : 0;
                MIB_EXPECT(terminals == 1, tag + "operation " + std::to_string(id) + " terminals: " + std::to_string(terminals));
            }
        }
    }

    // ---- a shutdown from inside a terminal event, then the runner destroyed ----
    struct Signal
    {
        std::mutex m;
        std::condition_variable cv;
        bool set{false};
        void raise()
        {
            std::scoped_lock lock(m);
            set = true;
            cv.notify_all();
        }
        bool isSet()
        {
            std::scoped_lock lock(m);
            return set;
        }
        bool wait()
        {
            std::unique_lock lock(m);
            return cv.wait_for(lock, std::chrono::seconds(30), [&] { return set; });
        }
    };

    // Carried by the sink closure: raises `gone` when the last copy of that
    // closure (the one inside the runner) is destroyed.
    struct SinkToken
    {
        explicit SinkToken(Signal *g) : gone(g) {}
        SinkToken(const SinkToken &) = delete;
        SinkToken &operator=(const SinkToken &) = delete;
        ~SinkToken() { gone->raise(); }
        Signal *gone;
    };

    // #742 review (P2): a terminal-event callback calls shutdown() on the
    // job's own worker, which cannot join itself and is detached; the owner
    // then destroys ReviewJobs while that callback still runs. The worker must
    // still find the runner state it uses after the callback (the sink it is
    // executing, its lane and mutex: the facade preset frees the lane after
    // the event) alive, and the state must go once the worker ends. Clean
    // under ASan; before the fix the destructor freed the sink under the
    // running callback and the worker then released its lane in freed memory.
    void reentrantShutdown(const Policy &policy)
    {
        const std::string tag = std::string("[") + policy.name + ", shutdown inside the terminal event] ";
        ReviewSession session;
        Recorder recorder;
        FakeHost host{recorder};
        Signal shutdownReturned, destroyed, sinkGone;
        std::atomic<bool> sinkAliveAfterDestroy{false};
        std::atomic<bool> callbackDone{false};
        std::atomic<ReviewJobs *> self{nullptr};
        auto token = std::make_shared<SinkToken>(&sinkGone);
        ReviewJobSink sink = [token, &recorder, &self, &shutdownReturned, &destroyed, &sinkGone, &sinkAliveAfterDestroy,
                              &callbackDone](const ReviewJobEvent &e) {
            recorder(e);
            if (!isTerminal(e.state)) return;
            self.load()->shutdown(); // re-entrant, on this job's own worker
            shutdownReturned.raise();
            destroyed.wait(); // the owner destroys ReviewJobs meanwhile
            sinkAliveAfterDestroy = !sinkGone.isSet();
            callbackDone = true;
        };
        const ReviewJobsOptions options =
            policy.facade ? ReviewJobsOptions::forFacade(host.hook()) : ReviewJobsOptions::forReviewBridge();
        auto jobs = std::make_unique<ReviewJobs>(session, nullptr, std::move(sink), options);
        token.reset(); // the runner's sink holds the only copy now
        self = jobs.get();
        std::string err;
        const auto id = ReviewJobsTestAccess::start(
            *jobs, kExportKind,
            [](std::uint64_t, const CancelFlag &, const ReviewJobsTestAccess::Progress &progress) {
                progress(1, 1);
                return ReviewJobsTestAccess::Result{ReviewJobState::Completed, "done"};
            },
            &err);
        MIB_REQUIRE(id != 0, tag + "start: " + err);
        MIB_REQUIRE(shutdownReturned.wait(), tag + "shutdown() from inside the terminal event returns (no self-join)");
        jobs.reset(); // destroyed while the callback still runs on the detached worker
        destroyed.raise();
        MIB_REQUIRE(sinkGone.wait(), tag + "the runner state is released once the detached worker ends");
        MIB_EXPECT(callbackDone.load(), tag + "the callback finished");
        MIB_EXPECT(sinkAliveAfterDestroy.load(),
                   tag + "the sink the worker was running outlived ReviewJobs until the worker ended");
        MIB_EXPECT(recorder.terminals(id) == 1, tag + "one terminal");
        MIB_EXPECT(recorder.violationCount() == 0, tag + "Started first, nothing after the terminal");
    }

    // #742 review round 2: shutdown() from inside a job's own Started event
    // (the sink: ReviewJobs' own ids; the facade preset: the host's
    // beginOperation, which reports Started). The start must not launch a
    // worker that runs the body: it reports exactly one terminal, Cancelled,
    // itself before it returns (the JSON jobs: their engine's outcome for a
    // run cancelled before its first step, byte for byte with the engine run
    // with the flag set on entry, and that status snapshot), and the lane is
    // free when it returns. Starts after it run normally. Before this round
    // the job ran after the shutdown and completed.
    void shutdownInsideStarted(const Policy &policy)
    {
        const std::string tag = std::string("[") + policy.name + ", shutdown inside Started] ";
        ReviewSession session;
        Recorder recorder;
        FakeHost host{recorder};
        backend::services::ProcessingService kernel; // owns no threads until start(); outlives the jobs
        std::atomic<ReviewJobs *> self{nullptr};
        std::atomic<bool> armed{false};
        std::atomic<int> shutdownsInStarted{0};
        const std::function<void()> shutdownIfArmed = [&] {
            if (!armed.exchange(false)) return;
            self.load()->shutdown(); // on the starting thread, inside Started
            ++shutdownsInStarted;
        };
        ReviewJobSink sink = [&recorder, &shutdownIfArmed](const ReviewJobEvent &e) {
            recorder(e);
            if (e.state == ReviewJobState::Started) shutdownIfArmed();
        };
        ReviewJobsOptions options = ReviewJobsOptions::forReviewBridge();
        if (policy.facade)
        {
            auto begin = host.hook().beginOperation;
            options = ReviewJobsOptions::forFacade(ReviewJobHost{
                [begin, &shutdownIfArmed](ReviewJobKind kind, const std::string &message, CancelFlag &cancel) {
                    const auto id = begin(kind, message, cancel); // records Started
                    shutdownIfArmed();
                    return id;
                }});
        }
        auto jobs = std::make_unique<ReviewJobs>(session, nullptr, std::move(sink), options);
        self = jobs.get();
        const auto setFlag = std::make_shared<std::atomic<bool>>(true);

        // ReviewJobs' own job (one of the custom bodies).
        std::atomic<bool> ran{false};
        std::uint64_t own = 0;
        {
            std::string err;
            armed = true;
            own = ReviewJobsTestAccess::start(
                *jobs, kExportKind,
                [&ran](std::uint64_t, const CancelFlag &, const ReviewJobsTestAccess::Progress &) {
                    ran = true;
                    return ReviewJobsTestAccess::Result{ReviewJobState::Completed, "done"};
                },
                &err);
            MIB_REQUIRE(own != 0, tag + "own job: the start returns its operation: " + err);
            MIB_EXPECT(shutdownsInStarted.load() == 1, tag + "own job: shutdown() ran inside Started and returned");
            // Reported by the start itself: no wait.
            MIB_EXPECT(recorder.terminals(own) == 1, tag + "own job: its one terminal was reported before the start returned");
            ReviewJobState s{};
            MIB_EXPECT(recorder.waitTerminal(own, &s) && s == ReviewJobState::Cancelled, tag + "own job: Cancelled");
            MIB_EXPECT(recorder.messageOf(own) == "Review jobs shut down before the job started",
                       tag + "own job: message '" + recorder.messageOf(own) + "'");
            MIB_EXPECT(!jobs->busy(), tag + "own job: the lane is free when the start returns");
            if (policy.facade) MIB_EXPECT(host.flag(own)->load(), tag + "own job: the host's cancel flag is set");
        }

        // The export JSON job: nothing touches its (missing) sources.
        {
            ExportBatchRequest batch;
            batch.sources = {"/nonexistent/review-jobs-lanes/a.h5", "/nonexistent/review-jobs-lanes/b.h5"};
            batch.request.sourcePath = batch.sources.front();
            batch.request.outputRoot = "/nonexistent/review-jobs-lanes/out";
            armed = true;
            const auto r = jobs->startExportJson(batch);
            MIB_REQUIRE(r.operationId != 0 && r.started, tag + "export: started, its terminal already reported: " + r.error);
            MIB_EXPECT(shutdownsInStarted.load() == 2, tag + "export: shutdown() ran inside Started");
            const auto engine = runExportBatch(batch.request, batch.sources, setFlag, {});
            MIB_REQUIRE(engine.cancelled && engine.files.empty(), tag + "export: the engine run cancelled on entry reaches no source");
            MIB_EXPECT(recorder.terminals(r.operationId) == 1, tag + "export: one terminal, reported by the start");
            ReviewJobState s{};
            MIB_EXPECT(recorder.waitTerminal(r.operationId, &s) && s == ReviewJobState::Cancelled, tag + "export: Cancelled");
            MIB_EXPECT(recorder.messageOf(r.operationId) == engine.error,
                       tag + "export: the engine's message: '" + recorder.messageOf(r.operationId) + "'");
            MIB_EXPECT(jobs->exportStatusJson() == exportTerminalStatusJson(r.operationId, engine),
                       tag + "export: the engine's cancelled snapshot, byte for byte: " + jobs->exportStatusJson());
            MIB_EXPECT(!jobs->busy(), tag + "export: the lane is free when the start returns");
            if (policy.facade) MIB_EXPECT(host.flag(r.operationId)->load(), tag + "export: the host's cancel flag is set");
        }

        // The reanalysis JSON job: its (missing) source is never opened.
        {
            ReanalysisRequest request;
            request.sourcePath = "/nonexistent/review-jobs-lanes/source.h5";
            request.outputPath = "/nonexistent/review-jobs-lanes/reanalysed.h5";
            armed = true;
            const auto r = jobs->startReanalysisJson(request, kernel);
            MIB_REQUIRE(r.operationId != 0 && r.started, tag + "reanalysis: started, its terminal already reported: " + r.error);
            MIB_EXPECT(shutdownsInStarted.load() == 3, tag + "reanalysis: shutdown() ran inside Started");
            const auto engine = runReanalysis(request, kernel, setFlag, {});
            MIB_REQUIRE(engine.cancelled && !engine.published, tag + "reanalysis: the engine run cancelled on entry");
            MIB_EXPECT(recorder.terminals(r.operationId) == 1, tag + "reanalysis: one terminal, reported by the start");
            ReviewJobState s{};
            MIB_EXPECT(recorder.waitTerminal(r.operationId, &s) && s == ReviewJobState::Cancelled,
                       tag + "reanalysis: Cancelled");
            MIB_EXPECT(recorder.messageOf(r.operationId) == engine.error,
                       tag + "reanalysis: the engine's message: '" + recorder.messageOf(r.operationId) + "'");
            MIB_EXPECT(jobs->reanalysisStatusJson() == reanalysisTerminalStatusJson(r.operationId, request, engine),
                       tag + "reanalysis: the engine's cancelled snapshot, byte for byte: " + jobs->reanalysisStatusJson());
            MIB_EXPECT(!jobs->busy(), tag + "reanalysis: the lane is free when the start returns");
        }

        // A start after it runs normally, on each lane.
        for (const auto kind : {kExportKind, kProcessingKind})
        {
            std::string err;
            const auto id = ReviewJobsTestAccess::start(
                *jobs, kind,
                [](std::uint64_t, const CancelFlag &, const ReviewJobsTestAccess::Progress &progress) {
                    progress(1, 1);
                    return ReviewJobsTestAccess::Result{ReviewJobState::Completed, "done"};
                },
                &err);
            MIB_REQUIRE(id != 0, tag + "a start afterwards: " + err);
            ReviewJobState s{};
            MIB_EXPECT(recorder.waitTerminal(id, &s) && s == ReviewJobState::Completed, tag + "a start afterwards completes");
            MIB_EXPECT(idle(*jobs), tag + "idle afterwards");
        }
        MIB_EXPECT(shutdownsInStarted.load() == 3, tag + "no other shutdown");
        jobs.reset(); // joins every worker: a body that had been launched has run by now
        MIB_EXPECT(!ran.load(), tag + "the overtaken job's body never ran");
        MIB_EXPECT(recorder.violationCount() == 0, tag + "Started first, nothing after a terminal");
        std::scoped_lock lock(recorder.m);
        for (const auto &[id, seq] : recorder.byId)
        {
            int terminals = 0;
            for (auto st : seq) terminals += isTerminal(st) ? 1 : 0;
            MIB_EXPECT(terminals == 1, tag + "operation " + std::to_string(id) + " terminals: " + std::to_string(terminals));
        }
    }

    // #742 review round 2: under the review bridge's policy a job's lane is
    // free before its terminal event, so another thread's start can claim it
    // and join the previous worker while that worker is still in the event.
    // If the event's callback then calls shutdown(), shutdown() must not wait
    // for that start (which waits for the worker): it returns, the worker
    // ends, and the start, overtaken by the shutdown, reports Cancelled
    // without running. Round 1's shutdown() waited for a start in progress
    // and this deadlocked. Since round 3 a shutdown() from outside waits
    // again, but this one runs on a lane worker (the inside case) and does
    // not, which is why the outside wait cannot deadlock here. (The facade
    // preset frees a lane only after the event, so no start can join a
    // worker still in it.)
    void shutdownFromTerminalWhileStarting()
    {
        const std::string tag = "[review bridge, shutdown from a terminal event while a start joins its worker] ";
        ReviewSession session;
        Recorder recorder;
        auto jobs = std::make_unique<ReviewJobs>(session, nullptr, std::ref(recorder), ReviewJobsOptions::forReviewBridge());
        Signal inTerminal, shutdownReturned;
        std::atomic<bool> fired{false};
        std::atomic<bool> claimSeen{false};
        recorder.setOnTerminal([&](const ReviewJobEvent &) {
            if (fired.exchange(true)) return; // the first job's terminal only
            inTerminal.raise();
            // The next start has claimed the lane; it joins this worker next.
            claimSeen = ReviewJobsTestAccess::waitBusy(*jobs, ReviewJobLane::Export, kWait);
            jobs->shutdown();
            shutdownReturned.raise();
        });
        std::string err;
        const auto first = ReviewJobsTestAccess::start(
            *jobs, kExportKind,
            [](std::uint64_t, const CancelFlag &, const ReviewJobsTestAccess::Progress &) {
                return ReviewJobsTestAccess::Result{ReviewJobState::Completed, "done"};
            },
            &err);
        MIB_REQUIRE(first != 0, tag + "first job: " + err);
        MIB_REQUIRE(inTerminal.wait(), tag + "the first job is in its terminal event");
        std::atomic<bool> ran{false};
        std::uint64_t second = 0;
        std::string secondErr;
        std::thread starter([&] {
            second = ReviewJobsTestAccess::start(
                *jobs, kExportKind,
                [&ran](std::uint64_t, const CancelFlag &, const ReviewJobsTestAccess::Progress &) {
                    ran = true;
                    return ReviewJobsTestAccess::Result{ReviewJobState::Completed, "done"};
                },
                &secondErr);
        });
        MIB_REQUIRE(shutdownReturned.wait(), tag + "shutdown() returns while the start joins its worker (no deadlock)");
        starter.join();
        MIB_EXPECT(claimSeen.load(), tag + "the second start claimed the lane before the shutdown");
        MIB_REQUIRE(second != 0, tag + "the second start returns its operation: " + secondErr);
        ReviewJobState s{};
        MIB_EXPECT(recorder.waitTerminal(first, &s) && s == ReviewJobState::Completed, tag + "the first job completed");
        MIB_EXPECT(recorder.waitTerminal(second, &s) && s == ReviewJobState::Cancelled, tag + "the overtaken start: Cancelled");
        MIB_EXPECT(recorder.messageOf(second) == "Review jobs shut down before the job started",
                   tag + "the overtaken start's message: '" + recorder.messageOf(second) + "'");
        MIB_EXPECT(recorder.terminals(first) == 1 && recorder.terminals(second) == 1, tag + "one terminal each");
        MIB_EXPECT(idle(*jobs), tag + "idle");
        recorder.setOnTerminal({});
        jobs.reset();
        MIB_EXPECT(!ran.load(), tag + "the overtaken job's body never ran");
        MIB_EXPECT(recorder.violationCount() == 0, tag + "Started first, nothing after a terminal");
    }

    // #742 review round 3: shutdown() from outside the runner (a thread that
    // is neither a lane worker nor inside a ReviewJobs callback: the owner's
    // teardown) waits until a start in progress on another thread has
    // returned, its Cancelled reported, so none is inside ReviewJobs, the
    // host or the sink when shutdown() returns. The start is held inside its
    // Started event (the sink; the facade preset: the host's beginOperation)
    // while the shutdown waits; released, it reports Cancelled without
    // running, and only then does shutdown() return.
    void externalShutdownWaitsForStart(const Policy &policy)
    {
        const std::string tag = std::string("[") + policy.name + ", shutdown from outside while a start is in Started] ";
        ReviewSession session;
        Recorder recorder;
        FakeHost host{recorder};
        Signal inStarted, release;
        std::atomic<bool> armed{true};
        const std::function<void()> holdIfArmed = [&] {
            if (!armed.exchange(false)) return;
            inStarted.raise();
            release.wait();
        };
        // Order of the two events that matter, on a shared counter.
        std::atomic<int> sequence{0};
        std::atomic<int> cancelledAt{0}, shutdownReturnedAt{0};
        ReviewJobSink sink = [&](const ReviewJobEvent &e) {
            recorder(e);
            if (e.state == ReviewJobState::Started) holdIfArmed();
            if (e.state == ReviewJobState::Cancelled) cancelledAt = ++sequence;
        };
        ReviewJobsOptions options = ReviewJobsOptions::forReviewBridge();
        if (policy.facade)
        {
            auto begin = host.hook().beginOperation;
            options = ReviewJobsOptions::forFacade(ReviewJobHost{
                [begin, &holdIfArmed](ReviewJobKind kind, const std::string &message, CancelFlag &cancel) {
                    const auto id = begin(kind, message, cancel); // records Started
                    holdIfArmed();
                    return id;
                }});
        }
        auto jobs = std::make_unique<ReviewJobs>(session, nullptr, std::move(sink), options);
        std::atomic<bool> ran{false};
        std::uint64_t id = 0;
        std::string err;
        std::thread starter([&] {
            id = ReviewJobsTestAccess::start(
                *jobs, kExportKind,
                [&ran](std::uint64_t, const CancelFlag &, const ReviewJobsTestAccess::Progress &) {
                    ran = true;
                    return ReviewJobsTestAccess::Result{ReviewJobState::Completed, "done"};
                },
                &err);
        });
        MIB_REQUIRE(inStarted.wait(), tag + "the start is held inside its Started event");
        Signal shutdownReturned;
        std::thread stopper([&] {
            jobs->shutdown(); // from outside: neither a lane worker nor a callback
            shutdownReturnedAt = ++sequence;
            shutdownReturned.raise();
        });
        // The shutdown has begun and is waiting for the start (it would
        // otherwise have returned: a mutation that skips the wait fails here).
        MIB_REQUIRE(ReviewJobsTestAccess::waitShutdownWaiting(*jobs, kWait),
                    tag + "shutdown() waits for the start in progress");
        MIB_EXPECT(!shutdownReturned.isSet(), tag + "shutdown() has not returned while the start is in Started");
        release.raise();
        MIB_REQUIRE(shutdownReturned.wait(), tag + "shutdown() returns once the start has returned");
        stopper.join();
        starter.join();
        MIB_REQUIRE(id != 0, tag + "the start returns its operation: " + err);
        MIB_EXPECT(recorder.terminals(id) == 1, tag + "one terminal");
        ReviewJobState s{};
        MIB_EXPECT(recorder.waitTerminal(id, &s) && s == ReviewJobState::Cancelled, tag + "the overtaken start: Cancelled");
        MIB_EXPECT(recorder.messageOf(id) == "Review jobs shut down before the job started",
                   tag + "the overtaken start's message: '" + recorder.messageOf(id) + "'");
        MIB_EXPECT(cancelledAt.load() != 0 && cancelledAt.load() < shutdownReturnedAt.load(),
                   tag + "the Cancelled was reported before shutdown() returned (" + std::to_string(cancelledAt.load()) +
                       " < " + std::to_string(shutdownReturnedAt.load()) + ")");
        MIB_EXPECT(!jobs->busy(), tag + "the lane is free when shutdown() returns");
        if (policy.facade) MIB_EXPECT(host.flag(id)->load(), tag + "the host's cancel flag is set");
        jobs.reset();
        MIB_EXPECT(!ran.load(), tag + "the overtaken job's body never ran");
        MIB_EXPECT(recorder.violationCount() == 0, tag + "Started first, nothing after a terminal");
    }
} // namespace

int main()
{
    mib::test::Watchdog wd(60);

    // ---- the kind -> lane map (D7) ----------------------------------------------
    for (auto k : {ReviewJobKind::ExportMetrics, ReviewJobKind::ExportAll, ReviewJobKind::BatchExport,
                   ReviewJobKind::ExportCharts, ReviewJobKind::Export})
        MIB_EXPECT(laneOf(k) == ReviewJobLane::Export, "export lane kind " + std::to_string(static_cast<int>(k)));
    for (auto k : {ReviewJobKind::RegenerateMasks, ReviewJobKind::ComputeCore, ReviewJobKind::Density,
                   ReviewJobKind::Reanalysis})
        MIB_EXPECT(laneOf(k) == ReviewJobLane::Processing, "processing lane kind " + std::to_string(static_cast<int>(k)));
    // The default construction is the review bridge's policy.
    {
        const ReviewJobsOptions d;
        const ReviewJobsOptions b = ReviewJobsOptions::forReviewBridge();
        MIB_EXPECT(!d.concurrentLanes && !b.concurrentLanes && d.laneRelease == b.laneRelease && !b.host.beginOperation,
                   "default options = the review bridge's");
    }

    runPolicy(Policy{"review bridge", false}, wd);
    runPolicy(Policy{"facade", true}, wd);
    reentrantShutdown(Policy{"review bridge", false});
    reentrantShutdown(Policy{"facade", true});
    wd.mark("re-entrant shutdown");
    shutdownInsideStarted(Policy{"review bridge", false});
    shutdownInsideStarted(Policy{"facade", true});
    wd.mark("shutdown inside Started");
    shutdownFromTerminalWhileStarting();
    wd.mark("shutdown from a terminal event while a start joins its worker");
    externalShutdownWaitsForStart(Policy{"review bridge", false});
    externalShutdownWaitsForStart(Policy{"facade", true});
    wd.mark("shutdown from outside waits for a start in progress");
    return mib::test::exitCode();
}
