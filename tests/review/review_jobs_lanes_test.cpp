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
// racing a job's end, and 200 mixed cycles per policy. Every operation must
// see Started first, Progress only before its one terminal, nothing after.
// `cancelOverridesOutcome`: a cancelled own job that fails reports Cancelled,
// a JSON-style job keeps its Failed. The kind -> lane map. Runs in the TSan
// lane (labels backend / concurrency). Watchdog guarded.

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
        // True when released, false when cancelled.
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

    bool waitUntil(const std::function<bool()> &condition)
    {
        const auto deadline = Clock::now() + std::chrono::seconds(30);
        while (!condition())
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
            MIB_EXPECT(waitUntil([&] { return !jobs->busy(); }), tag + "idle after both");
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
            MIB_EXPECT(waitUntil([&] { return !jobs->busy(ReviewJobLane::Export); }), tag + "export lane free");
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
            MIB_EXPECT(waitUntil([&] { return !jobs->busy(); }), tag + "idle after cancels");
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
            MIB_EXPECT(waitUntil([&] { return !jobs->busy(); }), tag + "idle after ordering");
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
                        while (!cancel->load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        return ReviewJobsTestAccess::Result{ReviewJobState::Failed, "engine failed"};
                    },
                    &err, overrides);
                MIB_REQUIRE(id != 0 && g.waitEntered(), tag + "override job");
                MIB_EXPECT(jobs->cancel(id), tag + "override cancel");
                ReviewJobState s{};
                MIB_REQUIRE(recorder.waitTerminal(id, &s), tag + "override terminal");
                MIB_EXPECT(s == (overrides ? ReviewJobState::Cancelled : ReviewJobState::Failed),
                           tag + (overrides ? "own jobs: a cancelled failure is Cancelled" : "JSON jobs: the engine's Failed stays"));
                MIB_EXPECT(waitUntil([&] { return !jobs->busy(); }), tag + "idle after override");
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
            MIB_EXPECT(waitUntil([&] { return !jobs->busy(); }), tag + "idle after restart");
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
            MIB_EXPECT(waitUntil([&] { return !jobs->busy(); }), tag + "idle after race");
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
                MIB_EXPECT(waitUntil([&] { return !jobs->busy(); }), tag + "cycle idle");
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
    return mib::test::exitCode();
}
