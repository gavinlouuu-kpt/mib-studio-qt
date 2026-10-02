// discovery_facade_test (issue #419)
//
// Headless facade surface for device discovery (no widgets):
//  - startDeviceDiscovery / fetchDeviceDiscovery / cancelDeviceDiscovery over
//    the backend job service with frontend-neutral DTOs and contract-pinned
//    integer enums;
//  - a running probe never blocks other facade pulls (fetchCameraSelection
//    returns while a fake provider blocks);
//  - camera snapshots carry the synthetic mock entry (camera_type 2) so the
//    shell can always offer the mock source, flagged synthetic;
//  - the legacy synchronous fetchCameraDiscovery (worker-thread wrapper) is
//    equivalent to the asynchronous result plus the mock entry;
//  - facade shutdown drains an in-flight job.
#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "backend/discovery/DeviceDiscoveryService.h"
#include "backend/services/CameraControlService.h"
#include "support/assert.h"
#include "support/fake_discovery_providers.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <chrono>
#include <cstdlib>
#include <memory>
#include <thread>

using namespace backend::discovery;
using backend::bridge::BackendFacade;
using mib::test::makeCandidate;
using mib::test::ScriptedProvider;
using namespace std::chrono_literals;

namespace {

void setEnv(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

DiscoveredDevice mindVisionCandidate(int index, const std::string& label)
{
    auto d = makeCandidate(DeviceKind::Camera, "cam", "", "", index, -1,
                           IdentificationStatus::Identified, IdentityStrength::SessionLocal);
    d.displayName = label;
    backend::services::DiscoveredCamera cam;
    cam.cameraType = backend::services::CameraType::MindVision;
    cam.cameraIndex = index;
    cam.modelName = "Fake MV";
    cam.firmwareVersion = "1.2";
    cam.label = label;
    d.camera = cam;
    return d;
}

template <typename Pred>
bool pollUntil(Pred pred, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

bool terminal(int state)
{
    return state == static_cast<int>(JobState::Completed) || state == static_cast<int>(JobState::Cancelled) ||
           state == static_cast<int>(JobState::Failed);
}

} // namespace

int main()
{
    setEnv("MIB_CAMERA_MODE", "mock");
    setEnv("MIB_DISABLED_SERVICES", "yolo,auto_update");
    mib::test::Watchdog watchdog(40);
    mib::test::TempDir dir("discovery_facade");
    backend::AppBackend backend;
    BackendFacade facade(backend);
    MIB_REQUIRE(facade.initialize(dir.path().string()), "facade initializes");
    auto& service = backend.deviceDiscovery();
    for (const auto& id : service.providerIds()) {
        MIB_REQUIRE(service.unregisterProvider(id), "production providers swapped for fakes");
    }
    auto* cam = new ScriptedProvider("cam", DeviceKind::Camera, "camera-sdk");
    cam->result.candidates = {mindVisionCandidate(1, "Fake MV (index 1)")};
    cam->block = true;
    service.registerProvider(std::unique_ptr<ScriptedProvider>(cam));

    // ---- async start / non-blocking status / snapshot -------------------------------
    {
        watchdog.mark("start");
        backend::bridge::BackendDiscoveryRequest request;
        request.kinds = {static_cast<int>(DeviceKind::Camera), static_cast<int>(DeviceKind::Framegrabber)};
        request.origin = "facade-test";
        const auto start = facade.startDeviceDiscovery(request);
        MIB_REQUIRE(start.accepted && start.jobId != 0, "job accepted through the facade");
        MIB_REQUIRE(cam->waitUntilEntered(2000ms), "probe blocked");

        const auto t0 = std::chrono::steady_clock::now();
        backend::bridge::BackendCameraSelection selection;
        MIB_EXPECT(facade.fetchCameraSelection(selection), "status pull works during a scan");
        backend::bridge::BackendDiscoverySnapshot running;
        MIB_EXPECT(facade.fetchDeviceDiscovery(start.jobId, running), "snapshot pull works during a scan");
        const auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        MIB_EXPECT(elapsedMs < 1000, "pulls do not wait for the blocked probe");
        MIB_EXPECT(running.valid && running.state == static_cast<int>(JobState::Running),
                   "snapshot reports Running while the probe blocks");
        MIB_EXPECT(running.jobId == start.jobId && running.generation == start.jobId, "ids round-trip");

        cam->release();
        backend::bridge::BackendDiscoverySnapshot done;
        MIB_REQUIRE(pollUntil([&] { return facade.fetchDeviceDiscovery(start.jobId, done) && terminal(done.state); }, 5000ms),
                    "job reaches a terminal state");
        MIB_EXPECT(done.state == static_cast<int>(JobState::Completed) && done.complete, "completed and complete");
        MIB_REQUIRE(done.candidates.size() == 2, "camera plus the synthetic mock entry");
        const auto* fake = &done.candidates[0];
        const auto* mock = &done.candidates[1];
        if (fake->synthetic) std::swap(fake, mock);
        MIB_EXPECT(fake->kind == static_cast<int>(DeviceKind::Camera) && fake->cameraType == 1 &&
                       fake->label == "Fake MV (index 1)" && fake->modelName == "Fake MV" &&
                       fake->firmwareVersion == "1.2" && fake->sdkIndex == 1 &&
                       fake->identityStrength == static_cast<int>(IdentityStrength::SessionLocal) &&
                       fake->identification == static_cast<int>(IdentificationStatus::Identified),
                   "legacy camera fields and typed identity survive the DTO mapping");
        MIB_EXPECT(mock->synthetic && mock->cameraType == 2 && mock->kind == static_cast<int>(DeviceKind::Camera) &&
                       mock->label == "Mock camera (folder frame stream)",
                   "mock entry is explicit, synthetic and contract-typed (2)");
        MIB_EXPECT(done.kinds.size() == 2, "requested kinds echoed in the snapshot");
        MIB_EXPECT(!facade.fetchDeviceDiscovery(987654, done) || !done.valid, "unknown job is invalid");
    }

    // ---- cancel and validation -------------------------------------------------------
    {
        watchdog.mark("cancel");
        cam->reset();
        backend::bridge::BackendDiscoveryRequest request;
        request.kinds = {static_cast<int>(DeviceKind::Camera)};
        request.deadlineMs = 30000;
        const auto start = facade.startDeviceDiscovery(request);
        MIB_REQUIRE(start.accepted && cam->waitUntilEntered(2000ms), "second job blocked");
        MIB_EXPECT(facade.cancelDeviceDiscovery(start.jobId), "cancel accepted for a running job");
        backend::bridge::BackendDiscoverySnapshot snap;
        MIB_REQUIRE(pollUntil([&] { return facade.fetchDeviceDiscovery(start.jobId, snap) && terminal(snap.state); }, 5000ms),
                    "cancelled job terminates");
        MIB_EXPECT(snap.state == static_cast<int>(JobState::Cancelled) && !snap.complete, "Cancelled + incomplete");
        MIB_EXPECT(!facade.cancelDeviceDiscovery(424242), "cancel of an unknown job is refused");

        backend::bridge::BackendDiscoveryRequest bad;
        const auto rejected = facade.startDeviceDiscovery(bad);
        MIB_EXPECT(!rejected.accepted && rejected.rejection == static_cast<int>(ErrorKind::InvalidRequest),
                   "empty request rejected with a contract-pinned reason");
        backend::bridge::BackendDiscoveryRequest scope;
        scope.kinds = {static_cast<int>(DeviceKind::PulseGenerator)};
        scope.hasSerialScope = true;
        scope.serialPortName = "COM9";
        scope.addressFrom = 5;
        scope.addressTo = 2;
        MIB_EXPECT(facade.startDeviceDiscovery(scope).rejection == static_cast<int>(ErrorKind::InvalidRequest),
                   "serial scope validated before any work");
    }

    // ---- legacy synchronous wrapper (worker thread) ------------------------------------
    {
        watchdog.mark("legacy");
        cam->reset();
        cam->block = false;
        backend::bridge::BackendCameraDiscovery legacy;
        bool ok = false;
        std::thread worker([&] { ok = facade.fetchCameraDiscovery(legacy); });
        worker.join();
        MIB_EXPECT(ok, "legacy wrapper succeeds");
        bool sawFake = false, sawMock = false;
        for (const auto& c : legacy.cameras) {
            sawFake |= c.type == 1 && c.label == "Fake MV (index 1)" && c.cameraIndex == 1;
            sawMock |= c.type == 2;
        }
        MIB_EXPECT(sawFake && sawMock && legacy.cameras.size() == 2 && legacy.framegrabbers.empty(),
                   "legacy result equals the asynchronous result plus the mock entry");
    }

    // ---- shutdown drains an in-flight job ----------------------------------------------
    {
        watchdog.mark("shutdown");
        cam->reset();
        cam->block = true;
        backend::bridge::BackendDiscoveryRequest request;
        request.kinds = {static_cast<int>(DeviceKind::Camera)};
        const auto start = facade.startDeviceDiscovery(request);
        MIB_REQUIRE(start.accepted && cam->waitUntilEntered(2000ms), "job blocked before shutdown");
        facade.shutdown();
        MIB_EXPECT(service.isShutdown(), "facade shutdown shuts discovery down");
        MIB_EXPECT(service.activeWorkerCount() == 0, "no worker after facade shutdown");
        MIB_EXPECT(service.discoverySnapshot(start.jobId).state == JobState::Cancelled, "in-flight job cancelled");
        backend::bridge::BackendDiscoveryRequest late;
        late.kinds = {static_cast<int>(DeviceKind::Camera)};
        MIB_EXPECT(!facade.startDeviceDiscovery(late).accepted, "no jobs after shutdown");
    }

    return mib::test::exitCode();
}
