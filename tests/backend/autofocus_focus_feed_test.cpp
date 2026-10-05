// Autofocus feeds by contract. Contract 1 drives the ring-width setpoint
// controller from onRingRatio; Contracts 2 and 3 drive the focus-score
// peak-seeker (AutofocusFocusScore.h) from per-object Laplacian samples
// (onFocusSample). Regressions:
//  - a NaN ring ratio (every Contract 2/3 object) passed the `<= 0.0` guards
//    and entered the ring-ratio statistics;
//  - after the first ring-mode step the sample counter restarted at 0 while
//    the applied sequence kept its old value, so the unsigned difference
//    wrapped and minSamplesPerStep was no longer enforced.
#include "backend/services/AutofocusService.h"

#include "support/assert.h"
#include "support/watchdog.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace af = backend::services::autofocus;
using backend::services::AutofocusService;

namespace {

struct FakeState {
    std::atomic<int> writes{0};
    std::atomic<double> voltage{40.0};
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
    bool setVoltage(double volts, std::string&) override {
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

AutofocusService::Config controlConfig(int minSamples) {
    AutofocusService::Config cfg;
    cfg.minVoltage = 0.0;
    cfg.maxVoltage = 100.0;
    cfg.voltageStep = 4.0;
    cfg.fineVoltageStep = 1.0;
    cfg.minSamplesPerStep = minSamples;
    cfg.ringRatioStaleMs = 1500;
    cfg.focusScoreHoldTolerance = 1.0;
    cfg.focusSetpoint = 20.0;
    cfg.focusRange = 0.5;
    return cfg;
}

void testNanRingRatioIsRejected() {
    AutofocusService service;
    service.onRingRatio(std::numeric_limits<double>::quiet_NaN(), 1000);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    MIB_EXPECT(service.getLastRingRatioUpdateUs() == 0, "a NaN ring ratio is not a sample");
    MIB_EXPECT(service.getMedianRingRatio() == 0.0, "a NaN ring ratio never reaches the statistics");
    MIB_EXPECT(service.getFocusMetric() == AutofocusService::FocusMetric::RingRatio,
               "default metric is the ring ratio");
}

// A stage whose focus score peaks at 60 V: the Contract 2/3 feed climbs to it.
void testFocusScoreFeedFindsThePeak(mib::test::Watchdog& watchdog) {
    const auto state = std::make_shared<FakeState>();
    AutofocusService service([state](backend::nanopositioner::BackendKind) {
        return std::make_unique<FakeBackend>(state);
    });
    service.setConfig(controlConfig(20));
    MIB_REQUIRE(service.connect(fakeEndpoint()), "connect fake stage");
    service.setEnabled(true);

    uint64_t frame = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    while (std::chrono::steady_clock::now() < deadline) {
        const double v = state->voltage.load();
        const double score = 1000.0 - (v - 60.0) * (v - 60.0);
        for (int object = 1; object <= 2; ++object) {
            service.onFocusSample(af::FocusSample{score + 0.01 * object, 0, frame, object, -1});
        }
        ++frame;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        watchdog.mark("focus feed");
    }
    MIB_EXPECT(service.getFocusMetric() == AutofocusService::FocusMetric::LaplacianVariance,
               "focus samples select the focus-score metric");
    MIB_EXPECT(std::abs(state->voltage.load() - 60.0) <= 2.0,
               "focus-score control settles near the peak: " + std::to_string(state->voltage.load()));
    MIB_EXPECT(service.getMedianRingRatio() == 0.0, "ring statistics stay untouched");
    service.setEnabled(false);
    service.disconnect();
}

// Ring mode: every step waits for minSamplesPerStep new samples.
void testRingModeHonoursMinSamplesEveryStep(mib::test::Watchdog& watchdog) {
    const auto state = std::make_shared<FakeState>();
    AutofocusService service([state](backend::nanopositioner::BackendKind) {
        return std::make_unique<FakeBackend>(state);
    });
    service.setConfig(controlConfig(50));
    MIB_REQUIRE(service.connect(fakeEndpoint()), "connect fake stage");
    service.setEnabled(true);

    // Ring ratio far from the setpoint: every allowed evaluation steps.
    auto feed = [&](int n) {
        for (int i = 0; i < n; ++i) {
            service.onRingRatio(30.0, 0);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    };
    feed(60);
    const auto wait = [&](int writes) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (state->writes.load() < writes && std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            watchdog.mark("ring step");
        }
    };
    wait(1);
    MIB_REQUIRE(state->writes.load() == 1, "first step after minSamplesPerStep samples");
    feed(5); // fewer than 50 new samples
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    MIB_EXPECT(state->writes.load() == 1, "no second step before 50 new samples");
    feed(60);
    wait(2);
    MIB_EXPECT(state->writes.load() == 2, "second step once enough new samples arrived");
    service.setEnabled(false);
    service.disconnect();
}

} // namespace

int main() {
    (void)spdlog::default_logger();
    mib::test::Watchdog watchdog(40);
    testNanRingRatioIsRejected();
    watchdog.mark("nan ring ratio");
    testFocusScoreFeedFindsThePeak(watchdog);
    testRingModeHonoursMinSamplesEveryStep(watchdog);
    return mib::test::exitCode();
}
