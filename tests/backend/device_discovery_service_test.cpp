// device_discovery_service_test (issue #419)
//
// Job core of backend::discovery::DeviceDiscoveryService over fake providers:
// request validation bounds, zero/one/multiple candidates, provider-aware
// dedup (persistent identity survives a changed SDK index / OS path; separate
// Modbus addresses stay separate), cross-provider ambiguity, overflow that can
// never look like a unique match, coalescing, job limits, retained-job
// eviction, non-blocking snapshots, cooperative cancellation, shutdown with a
// job in flight, and exactly one terminal observer notification per job.
#include "backend/discovery/DeviceDiscoveryService.h"
#include "support/assert.h"
#include "support/fake_discovery_providers.h"
#include "support/watchdog.h"

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace backend::discovery;
using mib::test::makeCandidate;
using mib::test::ScriptedProvider;
using namespace std::chrono_literals;

namespace {

DiscoveryRequest cameraRequest()
{
    DiscoveryRequest r;
    r.kinds = {DeviceKind::Camera};
    r.origin = "test";
    return r;
}

} // namespace

int main()
{
    mib::test::Watchdog watchdog(30);

    // ---- request validation ----------------------------------------------
    {
        watchdog.mark("validation");
        DeviceDiscoveryService service;
        service.registerProvider(std::make_unique<ScriptedProvider>("mv", DeviceKind::Camera));
        DiscoveryRequest empty;
        auto r = service.startDiscovery(empty);
        MIB_EXPECT(!r.accepted && r.rejection == ErrorKind::InvalidRequest, "empty kinds rejected");
        auto tooManyRetries = cameraRequest();
        tooManyRetries.retry.maxRetries = kMaxRetries + 1;
        MIB_EXPECT(service.startDiscovery(tooManyRetries).rejection == ErrorKind::InvalidRequest,
                   "retry bound enforced");
        auto longDelay = cameraRequest();
        longDelay.retry.maxRetries = 1;
        longDelay.retry.delay = 61s;
        MIB_EXPECT(service.startDiscovery(longDelay).rejection == ErrorKind::InvalidRequest,
                   "retry delay bound enforced");
        auto longDeadline = cameraRequest();
        longDeadline.deadline = 11min;
        MIB_EXPECT(service.startDiscovery(longDeadline).rejection == ErrorKind::InvalidRequest,
                   "deadline bound enforced");
        auto unknownProvider = cameraRequest();
        unknownProvider.providers = {"does-not-exist"};
        MIB_EXPECT(service.startDiscovery(unknownProvider).rejection == ErrorKind::InvalidRequest,
                   "unknown provider filter rejected");
        auto badScope = cameraRequest();
        badScope.kinds = {DeviceKind::PulseGenerator};
        badScope.serialScope = SerialScanScope{};
        badScope.serialScope->addressFrom = 10;
        badScope.serialScope->addressTo = 2;
        MIB_EXPECT(service.startDiscovery(badScope).rejection == ErrorKind::InvalidRequest,
                   "inverted address range rejected");
        MIB_EXPECT(service.discoverySnapshot(9999).jobId == 0 &&
                       service.discoverySnapshot(9999).state == JobState::Failed,
                   "unknown job snapshot is a Failed placeholder with jobId 0");
    }

    // ---- zero / one / multiple, dedup, ambiguity ---------------------------
    {
        watchdog.mark("results");
        DeviceDiscoveryService service;
        auto* mv = new ScriptedProvider("mindvision", DeviceKind::Camera, "camera-sdk");
        auto* eg = new ScriptedProvider("egrabber", DeviceKind::Camera, "camera-sdk");
        service.registerProvider(std::unique_ptr<ScriptedProvider>(mv));
        service.registerProvider(std::unique_ptr<ScriptedProvider>(eg));

        auto zero = service.startDiscovery(cameraRequest());
        MIB_REQUIRE(zero.accepted && zero.jobId != 0, "zero-match job accepted");
        MIB_REQUIRE(service.waitForTerminal(zero.jobId, 5000ms), "zero-match job terminates");
        auto snap = service.discoverySnapshot(zero.jobId);
        MIB_EXPECT(snap.state == JobState::Completed && snap.candidates.empty() && snap.complete,
                   "zero matches is Completed+complete, not Failed");
        MIB_EXPECT(snap.providersRun.size() == 2, "both camera providers ran");
        MIB_EXPECT(snap.generation == zero.jobId, "generation tracks the job");

        // Same physical camera reported twice by one provider with a changed
        // transient index/path collapses to one candidate.
        mv->result.candidates = {
            makeCandidate(DeviceKind::Camera, "mindvision", "SN-100", "", 0),
            makeCandidate(DeviceKind::Camera, "mindvision", "SN-100", "", 1)};
        auto one = service.startDiscovery(cameraRequest());
        MIB_REQUIRE(service.waitForTerminal(one.jobId, 5000ms), "one-match job terminates");
        snap = service.discoverySnapshot(one.jobId);
        MIB_EXPECT(snap.candidates.size() == 1, "persistent identity dedups changed SDK index");
        MIB_EXPECT(snap.complete && snap.state == JobState::Completed, "single match complete");

        // Session-local identities dedup on endpoint, so two distinct
        // indices are two devices.
        mv->result.candidates = {
            makeCandidate(DeviceKind::Camera, "mindvision", "", "", 0, -1,
                          IdentificationStatus::Identified, IdentityStrength::SessionLocal),
            makeCandidate(DeviceKind::Camera, "mindvision", "", "", 1, -1,
                          IdentificationStatus::Identified, IdentityStrength::SessionLocal)};
        auto two = service.startDiscovery(cameraRequest());
        MIB_REQUIRE(service.waitForTerminal(two.jobId, 5000ms), "two-match job terminates");
        MIB_EXPECT(service.discoverySnapshot(two.jobId).candidates.size() == 2,
                   "session-local identities on different indices stay separate");

        // Two providers claiming the same serial endpoint are ambiguous.
        DeviceDiscoveryService serial;
        auto* a = new ScriptedProvider("vendor-a", DeviceKind::Nanopositioner, "serial:COM7");
        auto* b = new ScriptedProvider("vendor-b", DeviceKind::Nanopositioner, "serial:COM7");
        serial.registerProvider(std::unique_ptr<ScriptedProvider>(a));
        serial.registerProvider(std::unique_ptr<ScriptedProvider>(b));
        a->result.candidates = {makeCandidate(DeviceKind::Nanopositioner, "vendor-a", "", "COM7", -1,
                                              -1, IdentificationStatus::Identified,
                                              IdentityStrength::SessionLocal)};
        b->result.candidates = {makeCandidate(DeviceKind::Nanopositioner, "vendor-b", "", "COM7", -1,
                                              -1, IdentificationStatus::Identified,
                                              IdentityStrength::SessionLocal)};
        DiscoveryRequest nano;
        nano.kinds = {DeviceKind::Nanopositioner};
        auto amb = serial.startDiscovery(nano);
        MIB_REQUIRE(serial.waitForTerminal(amb.jobId, 5000ms), "ambiguous job terminates");
        snap = serial.discoverySnapshot(amb.jobId);
        MIB_EXPECT(snap.candidates.size() == 2, "conflicting claims are kept, not merged");
        bool allAmbiguous = !snap.candidates.empty();
        for (const auto& c : snap.candidates) {
            if (c.identification != IdentificationStatus::Ambiguous || c.claimedBy.size() != 2)
                allAmbiguous = false;
        }
        MIB_EXPECT(allAmbiguous, "same endpoint claimed by two providers is Ambiguous for both");

        // Separate Modbus addresses on one port are separate devices.
        a->result.candidates = {
            makeCandidate(DeviceKind::Nanopositioner, "vendor-a", "", "COM7", -1, 1,
                          IdentificationStatus::Identified, IdentityStrength::SessionLocal),
            makeCandidate(DeviceKind::Nanopositioner, "vendor-a", "", "COM7", -1, 2,
                          IdentificationStatus::Identified, IdentityStrength::SessionLocal)};
        b->result.candidates.clear();
        auto addrs = serial.startDiscovery(nano);
        MIB_REQUIRE(serial.waitForTerminal(addrs.jobId, 5000ms), "address job terminates");
        snap = serial.discoverySnapshot(addrs.jobId);
        MIB_EXPECT(snap.candidates.size() == 2, "two addresses on one bus are two devices");
        for (const auto& c : snap.candidates) {
            MIB_EXPECT(c.identification == IdentificationStatus::Identified,
                       "one provider on two addresses is not ambiguous");
        }
    }

    // ---- overflow ------------------------------------------------------------
    {
        watchdog.mark("overflow");
        DeviceDiscoveryService service;
        auto* big = new ScriptedProvider("big", DeviceKind::Camera);
        service.registerProvider(std::unique_ptr<ScriptedProvider>(big));
        for (int i = 0; i < 300; ++i) {
            big->result.candidates.push_back(makeCandidate(
                DeviceKind::Camera, "big", "cam-" + std::to_string(i)));
        }
        auto job = service.startDiscovery(cameraRequest());
        MIB_REQUIRE(service.waitForTerminal(job.jobId, 5000ms), "overflow job terminates");
        auto snap = service.discoverySnapshot(job.jobId);
        MIB_EXPECT(snap.overflow && !snap.complete, "overflow is explicit and incomplete");
        MIB_EXPECT(snap.candidates.size() == kMaxCandidates, "candidates bounded");
        bool overflowError = false;
        for (const auto& e : snap.errors) overflowError |= e.kind == ErrorKind::Overflow;
        MIB_EXPECT(overflowError, "overflow reported as a structured error");
        // Truncation to a single survivor must never look like a unique match.
        big->result.candidates.clear();
        for (int i = 0; i < 2; ++i) {
            big->result.candidates.push_back(makeCandidate(DeviceKind::Camera, "big", "x" + std::to_string(i)));
        }
        for (std::size_t i = 0; i < kMaxErrors + 10; ++i) {
            big->result.errors.push_back({"big", ErrorKind::MalformedResponse, "noise", ""});
        }
        job = service.startDiscovery(cameraRequest());
        MIB_REQUIRE(service.waitForTerminal(job.jobId, 5000ms), "error overflow terminates");
        snap = service.discoverySnapshot(job.jobId);
        MIB_EXPECT(snap.errors.size() <= kMaxErrors + 1, "errors bounded (plus overflow marker)");
        MIB_EXPECT(!snap.complete, "provider errors make the snapshot incomplete");
    }

    // ---- coalescing, job limit, retention -----------------------------------
    {
        watchdog.mark("coalesce");
        DeviceDiscoveryService service;
        auto* blocker = new ScriptedProvider("blocker", DeviceKind::Camera);
        blocker->block = true;
        service.registerProvider(std::unique_ptr<ScriptedProvider>(blocker));
        auto first = service.startDiscovery(cameraRequest());
        MIB_REQUIRE(first.accepted, "first accepted");
        MIB_REQUIRE(blocker->waitUntilEntered(2000ms), "provider entered");
        auto again = service.startDiscovery(cameraRequest());
        MIB_EXPECT(again.accepted && again.coalesced && again.jobId == first.jobId,
                   "identical running request is coalesced to the same job");
        MIB_EXPECT(blocker->calls.load() == 1, "coalesced request does not re-enter the provider");
        // A snapshot must not wait for the blocked worker.
        const auto t0 = std::chrono::steady_clock::now();
        auto running = service.discoverySnapshot(first.jobId);
        const auto snapshotMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - t0)
                                    .count();
        MIB_EXPECT(running.state == JobState::Running, "blocked job reports Running");
        MIB_EXPECT(snapshotMs < 1000, "snapshot returns without waiting for the probe");

        // Different requests fill the concurrency budget; the next is rejected.
        std::vector<std::uint64_t> extra;
        for (std::size_t i = 1; i < kMaxConcurrentJobs; ++i) {
            auto req = cameraRequest();
            req.deadline = std::chrono::milliseconds(60000 - static_cast<int>(i));
            auto r = service.startDiscovery(req);
            MIB_EXPECT(r.accepted && !r.coalesced, "distinct request accepted");
            extra.push_back(r.jobId);
        }
        auto tooMany = cameraRequest();
        tooMany.deadline = 1234ms;
        auto rejected = service.startDiscovery(tooMany);
        MIB_EXPECT(!rejected.accepted && rejected.rejection == ErrorKind::TooManyJobs,
                   "concurrency budget rejects with TooManyJobs");
        blocker->release();
        MIB_REQUIRE(service.waitForTerminal(first.jobId, 5000ms), "first terminates");
        for (auto id : extra) MIB_REQUIRE(service.waitForTerminal(id, 5000ms), "extra terminates");
        MIB_EXPECT(service.discoverySnapshot(first.jobId).state == JobState::Completed,
                   "released job completes");
        MIB_EXPECT(service.activeWorkerCount() == 0, "no worker left after completion");

        // Retention: only the newest kMaxRetainedJobs terminal jobs are kept.
        blocker->block = false;
        std::vector<std::uint64_t> ids;
        for (std::size_t i = 0; i < kMaxRetainedJobs + 4; ++i) {
            auto req = cameraRequest();
            req.origin = "retain-" + std::to_string(i);
            auto r = service.startDiscovery(req);
            MIB_REQUIRE(r.accepted, "retention job accepted");
            MIB_REQUIRE(service.waitForTerminal(r.jobId, 5000ms), "retention job terminates");
            ids.push_back(r.jobId);
        }
        MIB_EXPECT(service.discoverySnapshot(ids.front()).jobId == 0,
                   "oldest terminal job evicted");
        MIB_EXPECT(service.discoverySnapshot(ids.back()).jobId == ids.back(),
                   "newest terminal job retained");
    }

    // ---- cancellation and shutdown ------------------------------------------
    {
        watchdog.mark("cancel");
        DeviceDiscoveryService service;
        auto* blocker = new ScriptedProvider("blocker", DeviceKind::Nanopositioner);
        blocker->block = true;
        service.registerProvider(std::unique_ptr<ScriptedProvider>(blocker));

        std::mutex m;
        std::map<std::uint64_t, int> terminalCount;
        std::atomic<int> notifications{0};
        const auto observerId = service.addObserver([&](const DiscoverySnapshot& s) {
            if (!isTerminal(s.state)) return;
            std::lock_guard<std::mutex> lk(m);
            ++terminalCount[s.jobId];
            ++notifications;
        });

        DiscoveryRequest nano;
        nano.kinds = {DeviceKind::Nanopositioner};
        auto job = service.startDiscovery(nano);
        MIB_REQUIRE(job.accepted, "cancel job accepted");
        MIB_REQUIRE(blocker->waitUntilEntered(2000ms), "provider blocked");
        service.cancelDiscovery(job.jobId);
        service.cancelDiscovery(job.jobId); // idempotent
        MIB_REQUIRE(service.waitForTerminal(job.jobId, 5000ms), "cancelled job terminates");
        auto snap = service.discoverySnapshot(job.jobId);
        MIB_EXPECT(snap.state == JobState::Cancelled && !snap.complete,
                   "cancel yields Cancelled and incomplete");
        MIB_EXPECT(blocker->sawCancel.load(), "provider observed the cancellation token");
        service.cancelDiscovery(424242); // unknown id is a no-op

        // Shutdown with a job in flight.
        blocker->reset();
        auto inflight = service.startDiscovery(nano);
        MIB_REQUIRE(inflight.accepted, "in-flight job accepted");
        MIB_REQUIRE(blocker->waitUntilEntered(2000ms), "second probe blocked");
        service.shutdownDiscovery();
        MIB_EXPECT(service.activeWorkerCount() == 0, "shutdown drained every worker");
        snap = service.discoverySnapshot(inflight.jobId);
        MIB_EXPECT(snap.state == JobState::Cancelled, "in-flight job cancelled by shutdown");
        auto late = service.startDiscovery(nano);
        MIB_EXPECT(!late.accepted && late.rejection == ErrorKind::ShuttingDown,
                   "no jobs accepted after shutdown");
        {
            std::lock_guard<std::mutex> lk(m);
            MIB_EXPECT(terminalCount[job.jobId] == 1 && terminalCount[inflight.jobId] == 1,
                       "exactly one terminal notification per accepted job");
            MIB_EXPECT(notifications.load() == 2, "no notifications for rejected jobs");
        }
        service.removeObserver(observerId);
        service.shutdownDiscovery(); // idempotent
    }

    return mib::test::exitCode();
}
