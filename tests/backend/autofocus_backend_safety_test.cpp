#include "backend/services/AutofocusService.h"

#include "support/assert.h"
#include "support/watchdog.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

namespace {

struct FakeState {
    std::atomic<bool> failWrites{false};
    std::atomic<int> attempts{0};
    std::atomic<int> writes{0};
    std::atomic<double> voltage{10.0};
    std::atomic<bool> connected{false};
};

class FakeBackend final : public backend::nanopositioner::INanopositionerBackend {
public:
    explicit FakeBackend(std::shared_ptr<FakeState> state) : state_(std::move(state)) {}

    backend::nanopositioner::BackendKind kind() const override {
        return backend::nanopositioner::BackendKind::Oeabt;
    }
    bool connect(const backend::nanopositioner::Endpoint&, std::string&) override {
        state_->connected.store(true);
        return true;
    }
    void disconnect() override { state_->connected.store(false); }
    bool isConnected() const override { return state_->connected.load(); }
    bool readVoltage(double& volts, std::string&) override {
        volts = state_->voltage.load();
        return true;
    }
    bool setVoltage(double volts, std::string& error) override {
        state_->attempts.fetch_add(1);
        if (state_->failWrites.load()) {
            error = "fake write failure";
            return false;
        }
        state_->voltage.store(volts);
        state_->writes.fetch_add(1);
        return true;
    }
    std::optional<double> maximumVoltage() const override { return 100.0; }
    std::string connectedEndpoint() const override { return "/dev/fake-oeabt"; }

private:
    std::shared_ptr<FakeState> state_;
};

backend::nanopositioner::Endpoint fakeEndpoint() {
    backend::nanopositioner::Endpoint endpoint;
    endpoint.backend = backend::nanopositioner::BackendKind::Oeabt;
    endpoint.persistentId = "fake-oeabt";
    endpoint.systemPath = "/dev/fake-oeabt";
    endpoint.displayName = "Fake OEABT";
    endpoint.knownOeabtCandidate = true;
    return endpoint;
}

} // namespace

int main() {
    // AutofocusService logs from its lifetime stats/control threads. Create
    // spdlog's lazy default logger before either thread starts so the test is
    // measuring service synchronization rather than singleton initialization.
    (void)spdlog::default_logger();
    mib::test::Watchdog watchdog(5);

    {
        const auto legacy =
            backend::nanopositioner::resolvePersistedSelection(std::nullopt, std::nullopt, 6);
        MIB_EXPECT(legacy.backend == backend::nanopositioner::BackendKind::Auto &&
                       legacy.endpointId == "COM6" && legacy.migratedLegacyComPort,
                   "legacy COM-only config preserves port preference without forcing vendor");

        const auto modern = backend::nanopositioner::resolvePersistedSelection(
            std::string("oeabt"), std::string("/dev/serial/by-path/controller"), 6);
        MIB_EXPECT(modern.backend == backend::nanopositioner::BackendKind::Oeabt &&
                       modern.endpointId == "/dev/serial/by-path/controller" &&
                       !modern.migratedLegacyComPort,
                   "modern backend/endpoint selection takes precedence over legacy COM key");
    }

    {
        const auto state = std::make_shared<FakeState>();
        backend::services::AutofocusService service([state](backend::nanopositioner::BackendKind) {
            return std::make_unique<FakeBackend>(state);
        });
        MIB_REQUIRE(service.connect(fakeEndpoint()), "connect through injected OEABT backend");
        MIB_EXPECT(state->writes.load() == 0, "connect is observe-only");
        service.disconnect();
        MIB_EXPECT(state->writes.load() == 0,
                   "read-only session disconnect does not change voltage");
        watchdog.mark("observe-only disconnect");
    }

    {
        const auto state = std::make_shared<FakeState>();
        backend::services::AutofocusService service([state](backend::nanopositioner::BackendKind) {
            return std::make_unique<FakeBackend>(state);
        });
        auto config = service.getConfig();
        config.minVoltage = 0.0;
        config.maxVoltage = 100.0;
        config.manualVoltageStep = 1.0;
        config.safeShutdownVoltage = 3.0;
        service.setConfig(config);

        MIB_REQUIRE(service.connect(fakeEndpoint()), "active-session test connects");
        service.increaseVoltage();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (state->writes.load() == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            watchdog.mark("waiting for manual voltage request");
        }
        MIB_REQUIRE(state->writes.load() == 1, "explicit manual request performs one write");
        MIB_EXPECT(state->voltage.load() == 11.0, "manual request applies configured step");

        service.disconnect();
        MIB_EXPECT(state->writes.load() == 2,
                   "active-session disconnect performs one safe-shutdown write");
        MIB_EXPECT(state->voltage.load() == 3.0,
                   "active-session disconnect applies configured safe voltage");
        watchdog.mark("active disconnect");
    }

    // HW-033 (#868): failures reach the failure callback as well as the status callback; information does not.
    {
        const auto state = std::make_shared<FakeState>();
        backend::services::AutofocusService service([state](backend::nanopositioner::BackendKind) {
            return std::make_unique<FakeBackend>(state);
        });
        std::mutex mutex;
        std::vector<std::string> status;
        std::vector<std::string> failures;
        service.setStatusCallback([&](const std::string& message) {
            std::scoped_lock lock(mutex);
            status.push_back(message);
        });
        service.setFailureCallback([&](const std::string& message) {
            std::scoped_lock lock(mutex);
            failures.push_back(message);
        });
        MIB_REQUIRE(service.connect(fakeEndpoint()), "failure-callback test connects");
        service.setEnabled(true);
        service.setEnabled(false);
        {
            std::scoped_lock lock(mutex);
            MIB_EXPECT(failures.empty(), "connect, enable and disable are information, not failures");
            MIB_EXPECT(status.size() == 3, "the status callback (Qt) still receives every message");
        }
        state->failWrites.store(true);
        service.increaseVoltage();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (state->attempts.load() == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            watchdog.mark("waiting for failing jog");
        }
        MIB_REQUIRE(state->attempts.load() == 1, "the jog reached the device");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        {
            std::scoped_lock lock(mutex);
            MIB_REQUIRE(failures.size() == 1, "one failure delivered");
            MIB_EXPECT(failures[0] == "Voltage change failed: fake write failure", failures[0]);
            MIB_EXPECT(status.back() == failures[0], "the same text went to the status callback");
        }
        service.setFailureCallback({});
        service.increaseVoltage();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        {
            std::scoped_lock lock(mutex);
            MIB_EXPECT(failures.size() == 1, "no failure is delivered after the callback is cleared");
        }
        service.disconnect();
        watchdog.mark("failure callback");
    }

    return mib::test::exitCode();
}
