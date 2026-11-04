// autofocus_failure_events_test (HW-033, #868, Tauri parity wave 1)
//
// The Qt tab shows every AutofocusService status message through the status callback. The Tauri app only sees
// what the bridge reports, so the facade turns the service's FAILURES (a connect that did not succeed with the
// device's reason, a voltage write that failed in the control thread, a safe shutdown voltage that was not
// applied) into BackendErrorEvents of command type Autofocus. This test drives the facade over a fake
// nanopositioner and checks: the device's reason reaches the shell; a failing jog is reported once per window
// (the control step retries every 50 ms); a different message always passes; information and progress messages
// are not errors; and a late failure after the facade is gone calls nothing.

#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "backend/nanopositioner/INanopositionerBackend.h"

#include "support/assert.h"
#include "support/watchdog.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

namespace bridge = backend::bridge;
namespace npos = backend::nanopositioner;

namespace
{
    struct FakeState
    {
        std::atomic<bool> failConnect{false};
        std::atomic<bool> failWrites{false};
        std::atomic<int> writeAttempts{0};
        std::mutex mutex;
        std::string connectError{"device did not answer"};
        std::string writeError{"write refused by controller"};
        double voltage{10.0};
        bool connected{false};
    };

    class FakeBackend final : public npos::INanopositionerBackend
    {
    public:
        explicit FakeBackend(std::shared_ptr<FakeState> state) : state_(std::move(state)) {}
        npos::BackendKind kind() const override { return npos::BackendKind::Oeabt; }
        bool connect(const npos::Endpoint &, std::string &error) override
        {
            if (state_->failConnect.load())
            {
                std::scoped_lock lock(state_->mutex);
                error = state_->connectError;
                return false;
            }
            std::scoped_lock lock(state_->mutex);
            state_->connected = true;
            return true;
        }
        void disconnect() override
        {
            std::scoped_lock lock(state_->mutex);
            state_->connected = false;
        }
        bool isConnected() const override
        {
            std::scoped_lock lock(state_->mutex);
            return state_->connected;
        }
        bool readVoltage(double &volts, std::string &) override
        {
            std::scoped_lock lock(state_->mutex);
            volts = state_->voltage;
            return true;
        }
        bool setVoltage(double volts, std::string &error) override
        {
            state_->writeAttempts.fetch_add(1);
            std::scoped_lock lock(state_->mutex);
            if (state_->failWrites.load())
            {
                error = state_->writeError;
                return false;
            }
            state_->voltage = volts;
            return true;
        }
        std::optional<double> maximumVoltage() const override { return 100.0; }
        std::string connectedEndpoint() const override { return "/dev/fake-oeabt"; }

    private:
        std::shared_ptr<FakeState> state_;
    };

    npos::Endpoint fakeEndpoint()
    {
        npos::Endpoint endpoint;
        endpoint.backend = npos::BackendKind::Oeabt;
        endpoint.persistentId = "fake-oeabt";
        endpoint.systemPath = "/dev/fake-oeabt";
        endpoint.displayName = "Fake OEABT";
        return endpoint;
    }

    std::filesystem::path makeTempDir()
    {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        std::uniform_int_distribution<unsigned long long> dist;
        const auto path = std::filesystem::temp_directory_path() / ("mib_af_failures_" + std::to_string(dist(gen)));
        std::filesystem::create_directories(path);
        return path;
    }

    struct Seen
    {
        std::mutex mutex;
        std::vector<std::string> autofocusErrors;
        std::vector<bridge::BackendErrorSource> sources;
        std::size_t count() { std::scoped_lock lock(mutex); return autofocusErrors.size(); }
        std::vector<std::string> all() { std::scoped_lock lock(mutex); return autofocusErrors; }
    };

    bool waitFor(const std::function<bool()> &ready, mib::test::Watchdog &watchdog, int timeoutMs = 3000)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (ready()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            watchdog.mark("waiting");
        }
        return ready();
    }

    int occurrences(const std::vector<std::string> &messages, const std::string &text)
    {
        int n = 0;
        for (const auto &m : messages)
            if (m == text) ++n;
        return n;
    }
} // namespace

int main()
{
    (void)spdlog::default_logger();
    spdlog::set_level(spdlog::level::off);
    mib::test::Watchdog watchdog(40);
#if defined(_WIN32)
    _putenv_s("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", "file:///nonexistent/manifest.json");
#else
    setenv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", "file:///nonexistent/manifest.json", 1);
#endif
    const auto dataDir = makeTempDir();
    const auto state = std::make_shared<FakeState>();
    auto seen = std::make_shared<Seen>();

    backend::AppBackend backendApp;
    {
        bridge::BackendFacade facade(backendApp);
        MIB_REQUIRE(facade.initialize(dataDir.string()), "facade initializes");
        MIB_REQUIRE(backendApp.autofocus().setBackendFactory([state](npos::BackendKind) {
            return std::make_unique<FakeBackend>(state);
        }), "fake nanopositioner installed");
        facade.setEventSink([seen](const bridge::BackendEvent &event) {
            if (const auto *error = std::get_if<bridge::BackendErrorEvent>(&event))
            {
                if (error->command != bridge::BackendCommandType::Autofocus) return;
                std::scoped_lock lock(seen->mutex);
                seen->autofocusErrors.push_back(error->message);
                seen->sources.push_back(error->source);
            }
        });

        auto connect = [&] {
            bridge::AutofocusCommand cmd;
            cmd.action = bridge::AutofocusCommandAction::Connect;
            cmd.endpoint = fakeEndpoint();
            return facade.dispatch(cmd);
        };
        auto jog = [&](bool up) {
            bridge::AutofocusCommand cmd;
            cmd.action = up ? bridge::AutofocusCommandAction::IncreaseVoltage
                            : bridge::AutofocusCommandAction::DecreaseVoltage;
            return facade.dispatch(cmd);
        };

        // 1. A connect the device refuses: the device's reason reaches the shell, then the command's own failure.
        state->failConnect.store(true);
        const auto refused = connect();
        MIB_EXPECT(!refused.ok, "connect to a device that does not answer fails");
        {
            const auto messages = seen->all();
            MIB_EXPECT(occurrences(messages, "device did not answer") == 1, "the device's reason is reported once");
            MIB_EXPECT(occurrences(messages, "Autofocus controller connect failed") == 1, "the command failure is still reported");
        }
        {
            std::scoped_lock lock(seen->mutex);
            for (const auto source : seen->sources)
                MIB_EXPECT(source == bridge::BackendErrorSource::Hardware, "autofocus failures carry the Hardware source");
        }
        state->failConnect.store(false);
        const auto before = seen->count();
        MIB_REQUIRE(connect().ok, "connect succeeds once the device answers");
        MIB_EXPECT(seen->count() == before, "'Connected to ...' is information, not an error event");

        // 2. Enable and disable are information, not errors.
        bridge::AutofocusCommand enable;
        enable.action = bridge::AutofocusCommandAction::SetEnabled;
        enable.enabled = true;
        MIB_EXPECT(facade.dispatch(enable).ok, "enable");
        enable.enabled = false;
        MIB_EXPECT(facade.dispatch(enable).ok, "disable");
        MIB_EXPECT(seen->count() == before, "'Autofocus enabled/disabled' are not error events");

        // 3. A manual jog that works emits no error ('Voltage: ...' is progress).
        MIB_REQUIRE(jog(true).ok, "jog accepted");
        MIB_REQUIRE(waitFor([&] { return state->writeAttempts.load() >= 1; }, watchdog), "jog reached the device");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        MIB_EXPECT(seen->count() == before, "a successful voltage change is not an error event");

        // 4. A jog the controller rejects is reported by the control thread; a repeat of the same message inside
        //    the window is not passed on a second time.
        state->failWrites.store(true);
        const int attemptsBefore = state->writeAttempts.load();
        MIB_REQUIRE(jog(true).ok, "jog accepted (the failure is asynchronous)");
        MIB_REQUIRE(waitFor([&] { return state->writeAttempts.load() >= attemptsBefore + 1; }, watchdog), "first failing write");
        MIB_REQUIRE(waitFor([&] {
            return occurrences(seen->all(), "Voltage change failed: write refused by controller") == 1;
        }, watchdog), "the background failure reaches the shell");
        MIB_REQUIRE(jog(false).ok, "second jog accepted");
        MIB_REQUIRE(waitFor([&] { return state->writeAttempts.load() >= attemptsBefore + 2; }, watchdog), "second failing write");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        MIB_EXPECT(occurrences(seen->all(), "Voltage change failed: write refused by controller") == 1,
                   "the same failure inside the window is reported once");

        // 5. A different message always passes.
        {
            std::scoped_lock lock(state->mutex);
            state->writeError = "controller reports over-temperature";
        }
        MIB_REQUIRE(jog(true).ok, "third jog accepted");
        MIB_REQUIRE(waitFor([&] {
            return occurrences(seen->all(), "Voltage change failed: controller reports over-temperature") == 1;
        }, watchdog), "a different failure is reported at once");

        // 6. The safe shutdown voltage that could not be applied is reported at disconnect (the session wrote
        //    before, so the service tries to park the controller).
        state->failWrites.store(true);
        const auto disconnected = [&] {
            bridge::AutofocusCommand cmd;
            cmd.action = bridge::AutofocusCommandAction::Disconnect;
            return facade.dispatch(cmd);
        }();
        MIB_EXPECT(disconnected.ok, "disconnect completes even when the safe voltage write fails");
        MIB_EXPECT(occurrences(seen->all(), "Failed to apply safe shutdown voltage: controller reports over-temperature") == 1,
                   "the failed safe shutdown voltage is reported");
    }

    // The facade is gone; the service lives on. A late failure must call nothing that was destroyed.
    state->failWrites.store(false);
    state->failConnect.store(true);
    MIB_EXPECT(!backendApp.autofocus().connect(fakeEndpoint()), "connect still fails after the facade is gone");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::filesystem::remove_all(dataDir);
    return mib::test::exitCode();
}
