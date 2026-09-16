// device_discovery_providers_test (issue #419)
//
// The real provider wrappers over injected seams:
//  - CameraEnumerationProvider maps DiscoveredCamera/DiscoveredFramegrabber
//    verbatim, marks MindVision indices session-local and eGrabber GenTL IDs
//    persistent, reports a compiled-out SDK as MissingSdk;
//  - NanopositionerProvider orders/filters by the preferred endpoint, probes
//    every endpoint once, keeps unidentified adapters in the inventory, maps
//    conflicting vendor claims to Ambiguous, cancels between endpoints, and a
//    cancelled scan never disconnects an established AutofocusService;
//  - PulseGeneratorProvider scans an explicit port/address scope over the
//    shared Modbus bus with FC03 only (zero write commands), maps
//    generator / foreign Modbus device / corrupt responses, busy and missing
//    ports, incompatible settings, and cancels between addresses.
#include "backend/discovery/DeviceDiscoveryService.h"
#include "backend/discovery/providers/CameraEnumerationProvider.h"
#include "backend/discovery/providers/NanopositionerProvider.h"
#include "backend/discovery/providers/PulseGeneratorProvider.h"
#include "backend/services/AutofocusService.h"
#include "backend/services/PulseGeneratorService.h"
#include "backend/services/SerialBus.h"
#include "support/assert.h"
#include "support/fake_modbus_bench.h"
#include "support/watchdog.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace backend::discovery;
using namespace std::chrono_literals;
namespace np = backend::nanopositioner;

namespace {

ProviderContext context(std::function<bool()> cancelled = [] { return false; })
{
    ProviderContext ctx;
    ctx.cancelled = std::move(cancelled);
    ctx.deadline = std::chrono::steady_clock::now() + 60s;
    ctx.jobId = 1;
    return ctx;
}

np::Endpoint endpoint(np::BackendKind backend, const std::string& id, const std::string& path)
{
    np::Endpoint ep;
    ep.backend = backend;
    ep.persistentId = id;
    ep.systemPath = path;
    ep.displayName = path;
    ep.knownOeabtCandidate = backend == np::BackendKind::Oeabt;
    if (backend == np::BackendKind::Coremor) ep.coremorPort = 7;
    return ep;
}

struct FakeNanoState {
    std::atomic<int> writes{0};
    std::atomic<bool> connected{false};
};

class FakeNanoBackend final : public np::INanopositionerBackend {
public:
    explicit FakeNanoBackend(std::shared_ptr<FakeNanoState> s) : state_(std::move(s)) {}
    np::BackendKind kind() const override { return np::BackendKind::Oeabt; }
    bool connect(const np::Endpoint&, std::string&) override { state_->connected = true; return true; }
    void disconnect() override { state_->connected = false; }
    bool isConnected() const override { return state_->connected.load(); }
    bool readVoltage(double& v, std::string&) override { v = 10.0; return true; }
    bool setVoltage(double, std::string&) override { ++state_->writes; return true; }
    std::optional<double> maximumVoltage() const override { return 100.0; }
    std::string connectedEndpoint() const override { return "COM7"; }

private:
    std::shared_ptr<FakeNanoState> state_;
};

bool hasError(const ProviderResult& r, ErrorKind kind)
{
    for (const auto& e : r.errors) {
        if (e.kind == kind) return true;
    }
    return false;
}

} // namespace

int main()
{
    mib::test::Watchdog watchdog(30);

    // ---- camera providers ------------------------------------------------------------
    {
        watchdog.mark("camera");
        backend::services::DiscoveredCamera mv;
        mv.cameraType = backend::services::CameraType::MindVision;
        mv.cameraIndex = 2;
        mv.modelName = "MV-SUA";
        mv.label = "MV-SUA (index 2)";
        auto mindvision = CameraEnumerationProvider::cameras(
            "mindvision", backend::services::CameraType::MindVision, [&] { return std::vector{mv}; });
        auto r = mindvision->discover({}, context());
        MIB_REQUIRE(r.candidates.size() == 1 && r.errors.empty() && r.complete, "one MindVision camera");
        const auto& c = r.candidates.front();
        MIB_EXPECT(c.kind == DeviceKind::Camera && c.providerId == "mindvision", "kind/provider");
        MIB_EXPECT(c.camera && c.camera->cameraIndex == 2 && c.camera->label == mv.label,
                   "legacy camera payload preserved verbatim");
        MIB_EXPECT(c.identityStrength == IdentityStrength::SessionLocal && c.endpoint.sdkIndex == 2,
                   "MindVision index is session-local, never a persistent identity");
        MIB_EXPECT(c.identification == IdentificationStatus::Identified, "SDK-enumerated camera is identified");
        MIB_EXPECT(mindvision->resourceClass() == "camera-sdk" && !mindvision->cancellable(),
                   "camera SDK enumeration is serialized and documented non-cancellable");

        backend::services::DiscoveredCamera eg;
        eg.cameraType = backend::services::CameraType::EGrabber;
        eg.interfaceIndex = 0;
        eg.deviceIndex = 1;
        eg.interfaceID = "PC1633";
        eg.deviceID = "Device0";
        eg.label = "PC1633/Device0 (Cam)";
        auto egrabber = CameraEnumerationProvider::cameras(
            "egrabber", backend::services::CameraType::EGrabber, [&] { return std::vector{eg}; });
        r = egrabber->discover({}, context());
        MIB_REQUIRE(r.candidates.size() == 1, "one eGrabber camera");
        MIB_EXPECT(r.candidates[0].identityStrength == IdentityStrength::Persistent &&
                       r.candidates[0].stableIdentity == "PC1633/Device0",
                   "GenTL interface/device IDs form a persistent identity");
        MIB_EXPECT(r.candidates[0].endpoint.interfaceIndex == 0 && r.candidates[0].endpoint.deviceIndex == 1,
                   "indices kept in the structured endpoint");

        // A provider only owns its camera type: foreign entries are dropped.
        auto mixed = CameraEnumerationProvider::cameras(
            "egrabber", backend::services::CameraType::EGrabber, [&] { return std::vector{eg, mv}; });
        MIB_EXPECT(mixed->discover({}, context()).candidates.size() == 1, "foreign camera types filtered");

        backend::services::DiscoveredFramegrabber fg;
        fg.interfaceIndex = 0;
        fg.deviceIndex = 0;
        fg.streamIndex = 0;
        fg.interfaceID = "PC1633";
        fg.deviceID = "Device0";
        fg.streamID = "Stream0";
        fg.label = "PC1633/Device0/Stream0";
        auto grabbers = CameraEnumerationProvider::framegrabbers("egrabber-framegrabber",
                                                                 [&] { return std::vector{fg}; });
        r = grabbers->discover({}, context());
        MIB_REQUIRE(r.candidates.size() == 1, "one framegrabber");
        MIB_EXPECT(r.candidates[0].kind == DeviceKind::Framegrabber && r.candidates[0].framegrabber &&
                       r.candidates[0].framegrabber->streamID == "Stream0",
                   "framegrabbers are their own kind with the legacy payload");

        auto throwing = CameraEnumerationProvider::cameras(
            "mindvision", backend::services::CameraType::MindVision,
            [] { throw std::runtime_error("SDK exploded"); return std::vector<backend::services::DiscoveredCamera>{}; });
        bool threw = false;
        try {
            (void)throwing->discover({}, context());
        } catch (const std::exception&) {
            threw = true;
        }
        MIB_EXPECT(threw, "enumerator exceptions propagate for the service to structure");

        auto missing = CameraEnumerationProvider::cameras(
            "egrabber", backend::services::CameraType::EGrabber, [&] { return std::vector{eg}; },
            /*sdkAvailable=*/false);
        r = missing->discover({}, context());
        MIB_EXPECT(r.candidates.empty() && hasError(r, ErrorKind::MissingSdk) && !r.complete,
                   "compiled-out SDK reports MissingSdk without calling the enumerator");
    }

    // ---- nanopositioner provider -------------------------------------------------------
    {
        watchdog.mark("nanopositioner");
        const auto oeabt7 = endpoint(np::BackendKind::Oeabt, "usb-oeabt-7", "COM7");
        const auto oeabt8 = endpoint(np::BackendKind::Oeabt, "usb-oeabt-8", "COM8");
        const auto coremor6 = endpoint(np::BackendKind::Coremor, "COM6", "COM6");
        std::vector<np::Endpoint> probed;
        NanopositionerProvider provider(
            [&] { return std::vector{oeabt8, oeabt7, coremor6, coremor6}; },
            [&](const np::Endpoint& ep) {
                probed.push_back(ep);
                return ep.systemPath == "COM7";
            });
        MIB_EXPECT(provider.id() == "nanopositioner" && provider.kind() == DeviceKind::Nanopositioner &&
                       provider.cancellable(),
                   "nanopositioner provider identity");

        DiscoveryRequest req;
        req.kinds = {DeviceKind::Nanopositioner};
        auto preferred = endpoint(np::BackendKind::Auto, "usb-oeabt-7", "");
        preferred.coremorBaudRate = 57600;
        preferred.coremorAddress = 3;
        req.preferredNanopositioner = preferred;
        auto r = provider.discover(req, context());
        MIB_REQUIRE(r.complete && r.errors.empty(), "clean probe");
        MIB_EXPECT(probed.size() == 3, "duplicates deduplicated before probing; every endpoint probed once");
        MIB_REQUIRE(!probed.empty(), "probed");
        MIB_EXPECT(probed.front().persistentId == "usb-oeabt-7", "preferred endpoint probed first");
        for (const auto& ep : probed) {
            MIB_EXPECT(ep.coremorBaudRate == 57600 && ep.coremorAddress == 3,
                       "saved serial settings applied to every endpoint");
        }
        MIB_EXPECT(r.candidates.size() == 3, "unidentified endpoints stay in the inventory");
        int identified = 0;
        for (const auto& c : r.candidates) {
            if (c.identification == IdentificationStatus::Identified) {
                ++identified;
                MIB_EXPECT(c.endpoint.systemPath == "COM7" && c.nanopositioner &&
                               c.nanopositioner->persistentId == "usb-oeabt-7" &&
                               c.identityStrength == IdentityStrength::Persistent &&
                               c.stableIdentity == "usb-oeabt-7",
                           "identified candidate carries the endpoint and persistent identity");
                MIB_EXPECT(c.claimedBy.size() == 1 && c.claimedBy[0] == "OEABT", "vendor claim recorded");
            } else {
                MIB_EXPECT(c.identification == IdentificationStatus::Unidentified, "others unidentified");
            }
        }
        MIB_EXPECT(identified == 1, "exactly one identified");
        for (const auto& c : r.candidates) {
            if (c.endpoint.systemPath == "COM6") {
                MIB_EXPECT(c.endpoint.busAddress == 3, "CoreMorrow address exposed as bus address");
            }
        }

        // Vendor filter: only Coremor endpoints are probed when requested.
        probed.clear();
        auto coremorOnly = endpoint(np::BackendKind::Coremor, "", "");
        req.preferredNanopositioner = coremorOnly;
        r = provider.discover(req, context());
        MIB_EXPECT(probed.size() == 1 && probed[0].systemPath == "COM6", "vendor filter honoured");

        // Conflicting vendor claims on one port are ambiguous.
        NanopositionerProvider conflicting(
            [&] {
                auto both = oeabt7;
                auto asCoremor = oeabt7;
                asCoremor.backend = np::BackendKind::Coremor;
                asCoremor.coremorPort = 7;
                return std::vector{both, asCoremor};
            },
            [](const np::Endpoint&) { return true; });
        req.preferredNanopositioner.reset();
        r = conflicting.discover(req, context());
        MIB_REQUIRE(r.candidates.size() == 2, "both vendor views retained");
        // Two vendor protocols answered on the same OS path: the service's
        // cross-claim rule marks both ambiguous once they share the endpoint.
        DeviceDiscoveryService service;
        service.registerProvider(std::make_unique<NanopositionerProvider>(
            [&] {
                auto asCoremor = oeabt7;
                asCoremor.backend = np::BackendKind::Coremor;
                asCoremor.coremorPort = 7;
                return std::vector{oeabt7, asCoremor};
            },
            [](const np::Endpoint&) { return true; }));
        auto job = service.startDiscovery(req);
        MIB_REQUIRE(job.accepted && service.waitForTerminal(job.jobId, 5000ms), "conflict job terminates");
        auto snap = service.discoverySnapshot(job.jobId);
        // One persistent adapter identity claimed by two vendor protocols is
        // one physical device: merged into a single Ambiguous candidate that
        // lists both claimants, never silently attributed to either.
        MIB_REQUIRE(snap.candidates.size() == 1, "one adapter identity yields one candidate");
        MIB_EXPECT(snap.candidates[0].identification == IdentificationStatus::Ambiguous,
                   "conflicting vendor claims for one port are ambiguous, never silently chosen");
        bool oeabt = false, coremor = false;
        for (const auto& claim : snap.candidates[0].claimedBy) {
            oeabt |= claim == "OEABT";
            coremor |= claim == "CoreMorrow / XMT";
        }
        MIB_EXPECT(oeabt && coremor, "both vendor claims listed on the ambiguous candidate");

        // Cancellation between endpoints; an established connection survives.
        auto state = std::make_shared<FakeNanoState>();
        backend::services::AutofocusService autofocus(
            [state](np::BackendKind) -> std::unique_ptr<np::INanopositionerBackend> {
                return std::make_unique<FakeNanoBackend>(state);
            });
        MIB_REQUIRE(autofocus.connect(oeabt7), "fake nanopositioner connected");
        std::atomic<int> probes{0};
        std::atomic<bool> cancel{false};
        NanopositionerProvider slow(
            [&] { return std::vector{oeabt7, oeabt8, coremor6}; },
            [&](const np::Endpoint&) {
                ++probes;
                cancel = true; // cancel after the first probe
                return false;
            });
        r = slow.discover(req, context([&] { return cancel.load(); }));
        MIB_EXPECT(probes.load() == 1, "cancellation checked between endpoints");
        MIB_EXPECT(!r.complete, "cancelled probe is incomplete");
        MIB_EXPECT(autofocus.isConnected() && state->writes.load() == 0,
                   "cancelled discovery never disconnects or moves the established device");
        autofocus.disconnect();
    }

    // ---- pulse-generator provider ------------------------------------------------------
    {
        watchdog.mark("pulse-generator");
        mib::test::FakeModbusBench bench;
        mib::test::FakeModbusWire generator;
        generator.address = 3;
        mib::test::makeGeneratorLike(generator);
        mib::test::FakeModbusWire pump;
        pump.address = 5;
        pump.exceptionOnRead = true; // a foreign device that rejects the generator register map
        bench.attach("fake-bus", &generator);
        bench.attach("fake-bus", &pump);
        mib::test::FakeModbusWire garbage;
        garbage.address = 9;
        garbage.corruptRead = true;
        bench.attach("fake-bus", &garbage);
        bench.busy.insert("held-elsewhere");

        backend::services::serialbus::SerialBusManager bus;
        bus.setSerialPortFactory([&] { return std::make_unique<mib::test::FakeModbusPort>(bench); });
        backend::services::PulseGeneratorService generatorService(bus);
        PulseGeneratorProvider provider(generatorService);
        MIB_EXPECT(provider.id() == "pulse-generator" && provider.kind() == DeviceKind::PulseGenerator &&
                       provider.cancellable(),
                   "pulse-generator provider identity");

        DiscoveryRequest req;
        req.kinds = {DeviceKind::PulseGenerator};
        auto r = provider.discover(req, context());
        MIB_EXPECT(r.candidates.empty() && hasError(r, ErrorKind::InvalidRequest) && !r.complete,
                   "a scan without an explicit serial scope is refused, never a broad sweep");

        SerialScanScope scope;
        scope.portName = "fake-bus";
        scope.settings.baudRate = 9600;
        scope.addressFrom = 1;
        scope.addressTo = 10;
        scope.perAddressTimeoutMs = 20;
        req.serialScope = scope;
        r = provider.discover(req, context());
        MIB_REQUIRE(r.errors.empty() && r.complete, "clean scan");
        MIB_EXPECT(generator.writeCommands.load() == 0 && pump.writeCommands.load() == 0,
                   "discovery issues no write function codes");
        MIB_EXPECT(generator.reads.load() >= 1, "identity read reached the generator");
        MIB_REQUIRE(r.candidates.size() == 3, "generator, foreign device and corrupt responder listed");
        for (const auto& c : r.candidates) {
            MIB_EXPECT(c.endpoint.systemPath == "fake-bus" && c.pulseGenerator, "endpoint + legacy hit");
            MIB_EXPECT(c.identityStrength == IdentityStrength::SessionLocal, "Modbus address is session-local");
            if (c.endpoint.busAddress == 3) {
                MIB_EXPECT(c.identification == IdentificationStatus::Identified &&
                               c.pulseGenerator->kind ==
                                   backend::services::PulseGeneratorService::ScanHit::Kind::PulseGenerator,
                           "generator identified");
            } else if (c.endpoint.busAddress == 5) {
                MIB_EXPECT(c.identification == IdentificationStatus::Unidentified,
                           "foreign Modbus device reported, not identified");
            } else if (c.endpoint.busAddress == 9) {
                MIB_EXPECT(c.identification == IdentificationStatus::Unidentified &&
                               !c.diagnostics.empty() &&
                               c.diagnostics.front().kind == ErrorKind::MalformedResponse,
                           "corrupt response is a malformed-response diagnostic");
            } else {
                MIB_EXPECT(false, "unexpected address in scan result");
            }
        }
        MIB_EXPECT(bench.openPorts.load() == 0, "scan releases the port when done");

        // Busy port (held by another program).
        req.serialScope->portName = "held-elsewhere";
        r = provider.discover(req, context());
        MIB_EXPECT(r.candidates.empty() && hasError(r, ErrorKind::Busy) && !r.complete,
                   "port held elsewhere is Busy");
        // Missing port.
        req.serialScope->portName = "unplugged";
        r = provider.discover(req, context());
        MIB_EXPECT(hasError(r, ErrorKind::OpenFailed) && !r.complete, "unplugged adapter is OpenFailed");
        // Incompatible settings: the bus is already held at other settings.
        req.serialScope->portName = "fake-bus";
        backend::services::SerialSettings other;
        other.baudRate = 19200;
        auto held = bus.acquire("fake-bus", other);
        MIB_REQUIRE(held, "bus held at 19200");
        r = provider.discover(req, context());
        MIB_EXPECT(hasError(r, ErrorKind::Busy) && !r.complete,
                   "port held with different settings reports Busy instead of changing the baud rate");
        held.reset();

        // Cancellation between addresses.
        std::atomic<bool> cancel{false};
        int polls = 0;
        r = provider.discover(req, context([&] {
            if (++polls >= 2) cancel = true;
            return cancel.load();
        }));
        MIB_EXPECT(!r.complete && hasError(r, ErrorKind::Cancelled), "cancelled scan is incomplete");
        MIB_EXPECT(r.candidates.size() < 3, "cancel stops before the whole range is probed");
        MIB_EXPECT(generator.writeCommands.load() == 0, "still no writes after cancel");
    }

    return mib::test::exitCode();
}
