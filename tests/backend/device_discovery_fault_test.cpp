// device_discovery_fault_test (issue #419)
//
// Structured failure semantics of the discovery job service: provider
// exceptions, per-provider structured errors preserved verbatim (busy port,
// incompatible settings, missing SDK, permission/open failure, malformed
// response, unsupported), a job that fails only when every provider failed,
// deadline expiry with partial results retained, cancellation during the
// initial delay / a probe / a retry delay, and the retry policy that re-runs
// providers until an identified candidate appears.
#include "backend/discovery/DeviceDiscoveryService.h"
#include "support/assert.h"
#include "support/fake_discovery_providers.h"
#include "support/watchdog.h"

#include <chrono>
#include <memory>
#include <thread>

using namespace backend::discovery;
using mib::test::makeCandidate;
using mib::test::ScriptedProvider;
using namespace std::chrono_literals;

namespace {

bool hasError(const DiscoverySnapshot& s, ErrorKind kind, const std::string& provider = {})
{
    for (const auto& e : s.errors) {
        if (e.kind == kind && (provider.empty() || e.providerId == provider)) return true;
    }
    return false;
}

DiscoveryRequest nanoRequest()
{
    DiscoveryRequest r;
    r.kinds = {DeviceKind::Nanopositioner};
    r.origin = "fault-test";
    return r;
}

} // namespace

int main()
{
    mib::test::Watchdog watchdog(30);

    // ---- provider exception next to a healthy provider ----------------------
    {
        watchdog.mark("exception");
        DeviceDiscoveryService service;
        auto* bad = new ScriptedProvider("bad", DeviceKind::Nanopositioner);
        auto* good = new ScriptedProvider("good", DeviceKind::Nanopositioner);
        bad->throwOnDiscover = true;
        good->result.candidates = {makeCandidate(DeviceKind::Nanopositioner, "good", "np-1", "COM3")};
        service.registerProvider(std::unique_ptr<ScriptedProvider>(bad));
        service.registerProvider(std::unique_ptr<ScriptedProvider>(good));
        auto job = service.startDiscovery(nanoRequest());
        MIB_REQUIRE(job.accepted && service.waitForTerminal(job.jobId, 5000ms), "job terminates");
        auto snap = service.discoverySnapshot(job.jobId);
        MIB_EXPECT(snap.state == JobState::Completed, "one healthy provider keeps the job Completed");
        MIB_EXPECT(!snap.complete, "a throwing provider makes coverage incomplete");
        MIB_EXPECT(hasError(snap, ErrorKind::ProviderException, "bad"), "exception is structured");
        MIB_EXPECT(snap.candidates.size() == 1, "healthy provider's result survives");

        // Every provider failing is a failed job, distinct from zero matches.
        good->throwOnDiscover = true;
        job = service.startDiscovery(nanoRequest());
        MIB_REQUIRE(service.waitForTerminal(job.jobId, 5000ms), "all-fail job terminates");
        snap = service.discoverySnapshot(job.jobId);
        MIB_EXPECT(snap.state == JobState::Failed && !snap.complete,
                   "all providers failing yields Failed");
    }

    // ---- structured provider errors preserved verbatim ------------------------
    {
        watchdog.mark("structured");
        DeviceDiscoveryService service;
        const ErrorKind kinds[] = {ErrorKind::Busy,          ErrorKind::OpenFailed,
                                   ErrorKind::PermissionDenied, ErrorKind::MalformedResponse,
                                   ErrorKind::MissingSdk,    ErrorKind::Unsupported};
        int i = 0;
        for (auto kind : kinds) {
            auto* p = new ScriptedProvider("p" + std::to_string(i++), DeviceKind::PulseGenerator);
            p->result.complete = false;
            p->result.errors.push_back({"", kind, std::string("scripted ") + toString(kind), "COM9"});
            service.registerProvider(std::unique_ptr<ScriptedProvider>(p));
        }
        DiscoveryRequest req;
        req.kinds = {DeviceKind::PulseGenerator};
        req.serialScope = SerialScanScope{};
        req.serialScope->portName = "COM9";
        auto job = service.startDiscovery(req);
        MIB_REQUIRE(job.accepted && service.waitForTerminal(job.jobId, 5000ms), "job terminates");
        auto snap = service.discoverySnapshot(job.jobId);
        i = 0;
        for (auto kind : kinds) {
            MIB_EXPECT(hasError(snap, kind, "p" + std::to_string(i)),
                       std::string("error kind preserved: ") + toString(kind));
            ++i;
        }
        for (const auto& e : snap.errors) {
            MIB_EXPECT(e.endpoint == "COM9" || e.kind == ErrorKind::Cancelled,
                       "endpoint context preserved on errors");
        }
        MIB_EXPECT(snap.state == JobState::Failed, "only-errors from every provider is Failed");
        MIB_EXPECT(!snap.complete, "errors make coverage incomplete");
    }

    // ---- guard busy ----------------------------------------------------------------
    {
        watchdog.mark("guard");
        DeviceDiscoveryService service;
        auto* cam = new ScriptedProvider("cam", DeviceKind::Camera);
        cam->result.candidates = {makeCandidate(DeviceKind::Camera, "cam", "c1")};
        service.registerProvider(std::unique_ptr<ScriptedProvider>(cam));
        bool capturing = true;
        service.setResourceGuard(DeviceKind::Camera, [&] { return capturing; });
        DiscoveryRequest req;
        req.kinds = {DeviceKind::Camera};
        auto job = service.startDiscovery(req);
        MIB_REQUIRE(job.accepted && service.waitForTerminal(job.jobId, 5000ms), "job terminates");
        auto snap = service.discoverySnapshot(job.jobId);
        MIB_EXPECT(cam->calls.load() == 0, "busy guard keeps the SDK untouched");
        MIB_EXPECT(hasError(snap, ErrorKind::Busy, "cam") && !snap.complete,
                   "busy guard reported as structured Busy, snapshot incomplete");
        MIB_EXPECT(snap.state == JobState::Completed, "busy is not a provider failure");
        capturing = false;
        job = service.startDiscovery(req);
        MIB_REQUIRE(service.waitForTerminal(job.jobId, 5000ms), "job terminates");
        MIB_EXPECT(service.discoverySnapshot(job.jobId).complete, "guard released, coverage complete");
    }

    // ---- deadline ------------------------------------------------------------------
    {
        watchdog.mark("deadline");
        DeviceDiscoveryService service;
        auto* slow = new ScriptedProvider("slow", DeviceKind::Nanopositioner);
        auto* after = new ScriptedProvider("after", DeviceKind::Nanopositioner);
        slow->script = [](const DiscoveryRequest&, const ProviderContext& ctx) {
            // Overrun the job deadline without honouring it (models an SDK
            // call that cannot be bounded), then return partial results.
            while (std::chrono::steady_clock::now() < ctx.deadline + 20ms) {
                std::this_thread::sleep_for(5ms);
            }
            ProviderResult r;
            r.candidates.push_back(makeCandidate(DeviceKind::Nanopositioner, "slow", "np-slow", "COM4"));
            return r;
        };
        service.registerProvider(std::unique_ptr<ScriptedProvider>(slow));
        service.registerProvider(std::unique_ptr<ScriptedProvider>(after));
        auto req = nanoRequest();
        req.deadline = 50ms;
        auto job = service.startDiscovery(req);
        MIB_REQUIRE(job.accepted && service.waitForTerminal(job.jobId, 5000ms), "job terminates");
        auto snap = service.discoverySnapshot(job.jobId);
        MIB_EXPECT(snap.state == JobState::Failed, "deadline expiry fails the job");
        MIB_EXPECT(hasError(snap, ErrorKind::Timeout), "timeout is structured");
        MIB_EXPECT(snap.candidates.size() == 1 && !snap.complete,
                   "partial results stay visible but incomplete");
        MIB_EXPECT(after->calls.load() == 0, "providers after the deadline do not run");
    }

    // ---- cancellation at every phase ----------------------------------------------
    {
        watchdog.mark("cancel-initial-delay");
        DeviceDiscoveryService service;
        auto* p = new ScriptedProvider("p", DeviceKind::Nanopositioner);
        service.registerProvider(std::unique_ptr<ScriptedProvider>(p));
        auto req = nanoRequest();
        req.initialDelay = 30s;
        auto job = service.startDiscovery(req);
        MIB_REQUIRE(job.accepted, "delayed job accepted");
        MIB_EXPECT(service.discoverySnapshot(job.jobId).state == JobState::Queued,
                   "job waits in Queued during the initial delay");
        service.cancelDiscovery(job.jobId);
        MIB_REQUIRE(service.waitForTerminal(job.jobId, 2000ms), "cancel during delay is prompt");
        MIB_EXPECT(service.discoverySnapshot(job.jobId).state == JobState::Cancelled,
                   "cancelled before running");
        MIB_EXPECT(p->calls.load() == 0, "cancelled job never touches the provider");

        watchdog.mark("cancel-probe");
        p->block = true;
        auto req2 = nanoRequest();
        job = service.startDiscovery(req2);
        MIB_REQUIRE(job.accepted && p->waitUntilEntered(2000ms), "probe entered");
        service.cancelDiscovery(job.jobId);
        MIB_REQUIRE(service.waitForTerminal(job.jobId, 2000ms), "cancel during probe is prompt");
        MIB_EXPECT(service.discoverySnapshot(job.jobId).state == JobState::Cancelled,
                   "cancelled during probe");

        watchdog.mark("cancel-retry-delay");
        p->block = false;
        p->reset();
        auto req3 = nanoRequest();
        req3.retry.maxRetries = 3;
        req3.retry.delay = 30s;
        const int callsBefore = p->calls.load();
        job = service.startDiscovery(req3);
        MIB_REQUIRE(job.accepted, "retrying job accepted");
        const auto t0 = std::chrono::steady_clock::now();
        while (p->calls.load() < callsBefore + 1 && std::chrono::steady_clock::now() - t0 < 2s) {
            std::this_thread::sleep_for(2ms);
        }
        // Let the worker enter the retry delay, then cancel.
        std::this_thread::sleep_for(20ms);
        auto mid = service.discoverySnapshot(job.jobId);
        MIB_EXPECT(mid.state == JobState::Running && mid.attempt == 1 && mid.maxAttempts == 4,
                   "attempt counters visible while waiting to retry");
        service.cancelDiscovery(job.jobId);
        MIB_REQUIRE(service.waitForTerminal(job.jobId, 2000ms), "cancel during retry delay is prompt");
        MIB_EXPECT(service.discoverySnapshot(job.jobId).state == JobState::Cancelled,
                   "cancelled during retry delay");
        MIB_EXPECT(p->calls.load() == callsBefore + 1, "no further attempt after cancel");
    }

    // ---- retry until identified --------------------------------------------------
    {
        watchdog.mark("retry");
        DeviceDiscoveryService service;
        auto* p = new ScriptedProvider("p", DeviceKind::Nanopositioner);
        p->script = [p](const DiscoveryRequest&, const ProviderContext& ctx) {
            ProviderResult r;
            // Unidentified adapters do not stop the retry loop; the third
            // attempt identifies the device.
            r.candidates.push_back(makeCandidate(DeviceKind::Nanopositioner, "p", "", "COM5", -1, -1,
                                                 IdentificationStatus::Unidentified,
                                                 IdentityStrength::SessionLocal));
            if (ctx.attempt >= 3) {
                r.candidates.push_back(makeCandidate(DeviceKind::Nanopositioner, "p", "np-5", "COM5"));
            }
            return r;
        };
        service.registerProvider(std::unique_ptr<ScriptedProvider>(p));
        auto req = nanoRequest();
        req.retry.maxRetries = 3;
        req.retry.delay = 10ms;
        auto job = service.startDiscovery(req);
        MIB_REQUIRE(job.accepted && service.waitForTerminal(job.jobId, 5000ms), "retry job terminates");
        auto snap = service.discoverySnapshot(job.jobId);
        MIB_EXPECT(p->calls.load() == 3, "stops retrying once a device is identified");
        MIB_EXPECT(snap.attempt == 3 && snap.maxAttempts == 4, "attempt counters in the snapshot");
        MIB_EXPECT(snap.state == JobState::Completed && snap.complete, "identified attempt completes");
        MIB_EXPECT(snap.candidates.size() == 2, "unidentified endpoint stays in inventory");

        // Exhausted retries with nothing identified: Completed, zero identified.
        auto* never = new ScriptedProvider("never", DeviceKind::Camera);
        service.registerProvider(std::unique_ptr<ScriptedProvider>(never));
        DiscoveryRequest cam;
        cam.kinds = {DeviceKind::Camera};
        cam.retry.maxRetries = 2;
        cam.retry.delay = 5ms;
        job = service.startDiscovery(cam);
        MIB_REQUIRE(service.waitForTerminal(job.jobId, 5000ms), "exhausted job terminates");
        snap = service.discoverySnapshot(job.jobId);
        MIB_EXPECT(never->calls.load() == 3, "all attempts used");
        MIB_EXPECT(snap.state == JobState::Completed && snap.candidates.empty() && snap.complete,
                   "exhausted retries with an empty bus is a complete, empty result");
    }

    return mib::test::exitCode();
}
