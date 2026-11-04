// hardware_parity_facade_test (Qt-parity epic #868; rows HW-033, HW-004/HW-026/HW-049, HW-023)
//
// Three additive facade surfaces the Tauri shell needs from services the Qt tabs call directly:
//  1. the autofocus status message and sequence (AutofocusService::setStatusCallback), failures
//     also as BackendError events, routine "Voltage: ..." never; detached on shutdown without a
//     late emission or a call into a dead facade;
//  2. listSerialPorts(): the simulated rig exactly as Qt offers it, nothing opened;
//  3. reservePumpPorts(): a second reservation owner next to config.json's, validated like the
//     pump connect endpoint.

#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "backend/services/AutofocusService.h"
#include "backend/services/SerialPortClaims.h"
#include "backend/services/SimulatedSerial.h"

#include "support/assert.h"
#include "support/watchdog.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace bridge = backend::bridge;
namespace nano = backend::nanopositioner;
namespace sim = backend::services::serialsim;
namespace claims = backend::services::portclaims;

namespace
{
    // A nanopositioner whose writes can be made to fail, so the control loop reports
    // "Voltage change failed: ..." on demand.
    class FakeNanopositioner final : public nano::INanopositionerBackend
    {
    public:
        std::atomic<bool> failWrites{false};

        nano::BackendKind kind() const override { return nano::BackendKind::Oeabt; }
        bool connect(const nano::Endpoint &value, std::string &) override
        {
            endpoint = value.persistentId;
            connected = true;
            return true;
        }
        void disconnect() override { connected = false; }
        bool isConnected() const override { return connected; }
        bool readVoltage(double &value, std::string &) override
        {
            value = 10.0;
            return true;
        }
        bool setVoltage(double, std::string &error) override
        {
            if (failWrites.load())
            {
                error = "injected write failure";
                return false;
            }
            return true;
        }
        std::optional<double> maximumVoltage() const override { return 100.0; }
        std::string connectedEndpoint() const override { return endpoint; }

    private:
        bool connected{false};
        std::string endpoint;
    };

    std::filesystem::path makeTempDir()
    {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        std::uniform_int_distribution<unsigned long long> dist;
        const auto path = std::filesystem::temp_directory_path() /
                          ("mib_hw_parity_facade_" + std::to_string(dist(gen)));
        std::filesystem::create_directories(path);
        return path;
    }

    bool waitFor(const std::function<bool()> &condition, std::chrono::milliseconds limit = std::chrono::seconds(10))
    {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (!condition())
        {
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return true;
    }

    // The sink runs on backend threads too (the autofocus control thread): guard it.
    struct EventLog
    {
        std::mutex mutex;
        std::vector<bridge::BackendEvent> events;

        void push(const bridge::BackendEvent &event)
        {
            std::scoped_lock lock(mutex);
            events.push_back(event);
        }
        // Autofocus errors whose message contains `needle` ("" = all of them).
        std::size_t autofocusErrors(const std::string &needle = {})
        {
            std::scoped_lock lock(mutex);
            std::size_t count = 0;
            for (const auto &event : events)
            {
                const auto *error = std::get_if<bridge::BackendErrorEvent>(&event);
                if (error && error->command == bridge::BackendCommandType::Autofocus &&
                    error->source == bridge::BackendErrorSource::Hardware &&
                    error->message.find(needle) != std::string::npos)
                    ++count;
            }
            return count;
        }
    };

    bridge::BackendAutofocusStatus status(bridge::BackendFacade &facade)
    {
        bridge::BackendAutofocusStatus out;
        MIB_REQUIRE(facade.fetchAutofocusStatus(out), "autofocus status readable");
        return out;
    }

    void testAutofocusStatusMessages(bridge::BackendFacade &facade, backend::AppBackend &app, EventLog &log)
    {
        auto first = status(facade);
        MIB_EXPECT(first.statusMessage.empty() && first.statusSequence == 0, "no message before the service speaks");

        // Qt's "No nanopositioner backend" path: a failure, so a Hardware/Autofocus event too.
        MIB_REQUIRE(app.autofocus().setBackendFactory([](nano::BackendKind) { return nullptr; }), "factory set");
        bridge::AutofocusCommand connect;
        connect.action = bridge::AutofocusCommandAction::Connect;
        nano::Endpoint endpoint;
        endpoint.backend = nano::BackendKind::Oeabt;
        endpoint.persistentId = "fake-oeabt";
        endpoint.systemPath = "/dev/fake-oeabt";
        connect.endpoint = endpoint;
        MIB_EXPECT(!facade.dispatch(connect).ok, "connect without a backend fails");
        auto refused = status(facade);
        MIB_EXPECT(refused.statusMessage.rfind("No nanopositioner", 0) == 0, refused.statusMessage);
        MIB_EXPECT(refused.statusSequence > first.statusSequence, "the sequence grows with the message");
        MIB_EXPECT(log.autofocusErrors("No nanopositioner") == 1, "the failure reaches the event log once");

        // A successful connect: the message is kept, nothing is logged as an error.
        auto fake = std::make_shared<FakeNanopositioner>();
        struct Shared final : nano::INanopositionerBackend
        {
            explicit Shared(std::shared_ptr<FakeNanopositioner> inner) : inner_(std::move(inner)) {}
            nano::BackendKind kind() const override { return inner_->kind(); }
            bool connect(const nano::Endpoint &e, std::string &err) override { return inner_->connect(e, err); }
            void disconnect() override { inner_->disconnect(); }
            bool isConnected() const override { return inner_->isConnected(); }
            bool readVoltage(double &v, std::string &err) override { return inner_->readVoltage(v, err); }
            bool setVoltage(double v, std::string &err) override { return inner_->setVoltage(v, err); }
            std::optional<double> maximumVoltage() const override { return inner_->maximumVoltage(); }
            std::string connectedEndpoint() const override { return inner_->connectedEndpoint(); }
            std::shared_ptr<FakeNanopositioner> inner_;
        };
        MIB_REQUIRE(app.autofocus().setBackendFactory([fake](nano::BackendKind) { return std::make_unique<Shared>(fake); }),
                    "factory replaced");
        const auto errorsBefore = log.autofocusErrors();
        MIB_REQUIRE(facade.dispatch(connect).ok, "connect succeeds");
        auto connected = status(facade);
        MIB_EXPECT(connected.statusMessage.rfind("Connected to oeabt nanopositioner", 0) == 0, connected.statusMessage);
        MIB_EXPECT(connected.statusSequence > refused.statusSequence, "a new message, a higher sequence");
        MIB_EXPECT(log.autofocusErrors() == errorsBefore, "a connect message is not an error event");

        // The routine voltage line moves the status but never the event log.
        bridge::AutofocusCommand jog;
        jog.action = bridge::AutofocusCommandAction::IncreaseVoltage;
        MIB_REQUIRE(facade.dispatch(jog).ok, "jog accepted");
        MIB_REQUIRE(waitFor([&] { return status(facade).statusMessage.rfind("Voltage: ", 0) == 0; }),
                    "the routine voltage line arrives: " + status(facade).statusMessage);
        MIB_EXPECT(log.autofocusErrors() == errorsBefore, "a routine voltage line is not an error event");

        // A failed write: the service's own text, and an event with the same text.
        fake->failWrites.store(true);
        MIB_REQUIRE(facade.dispatch(jog).ok, "jog accepted");
        MIB_REQUIRE(waitFor([&] { return status(facade).statusMessage.rfind("Voltage change failed: ", 0) == 0; }),
                    "the failure line arrives: " + status(facade).statusMessage);
        MIB_EXPECT(waitFor([&] { return log.autofocusErrors("Voltage change failed: ") == 1; }),
                   "the failed write is logged once as a Hardware/Autofocus error");
        fake->failWrites.store(false);

        // Sequence is strictly monotonic across everything above.
        const auto last = status(facade).statusSequence;
        MIB_EXPECT(last > connected.statusSequence, "sequence only grows");

        bridge::AutofocusCommand disconnect;
        disconnect.action = bridge::AutofocusCommandAction::Disconnect;
        MIB_REQUIRE(facade.dispatch(disconnect).ok, "disconnect");
        auto gone = status(facade);
        MIB_EXPECT(gone.statusMessage == "Disconnected from nanopositioner", gone.statusMessage);
        MIB_EXPECT(gone.statusSequence > last, "disconnect is a message too");
    }

    // shutdown() detaches the callback: messages the service sends afterwards (here from another
    // thread, racing the shutdown) emit nothing once shutdown returned, and never touch the facade.
    void testDetachOnShutdown(bridge::BackendFacade &facade, backend::AppBackend &app, EventLog &log)
    {
        MIB_REQUIRE(app.autofocus().setBackendFactory([](nano::BackendKind) { return nullptr; }), "factory set");
        std::atomic<bool> stop{false};
        std::atomic<int> fired{0};
        std::thread hammer([&] {
            while (!stop.load())
            {
                app.autofocus().connect(0, 0, 0); // "No nanopositioner backend ...": a failure message each call
                fired.fetch_add(1);
            }
        });
        MIB_REQUIRE(waitFor([&] { return fired.load() > 50; }), "the service is speaking");
        facade.shutdown();
        const auto atShutdown = log.autofocusErrors();
        const int firedAtShutdown = fired.load();
        MIB_REQUIRE(waitFor([&] { return fired.load() > firedAtShutdown + 50; }), "the service keeps speaking");
        stop.store(true);
        hammer.join();
        MIB_EXPECT(log.autofocusErrors() == atShutdown, "nothing is emitted after shutdown() returned");
        bridge::BackendAutofocusStatus out;
        MIB_EXPECT(!facade.fetchAutofocusStatus(out), "an uninitialized facade has no status");
    }

    void testSerialPorts(bridge::BackendFacade &facade)
    {
        std::vector<bridge::BackendSerialPortInfo> ports;
        MIB_EXPECT(!facade.listSerialPorts(ports) && ports.empty(), "not initialized: no list");
    }

    void testSerialPortsWithRig(bridge::BackendFacade &facade)
    {
        auto rig = std::make_shared<sim::Rig>(7);
        sim::PortSpec rs485;
        rs485.systemName = "COM3";
        sim::applyChip(rs485, *sim::findUsbSerialChip("ch340"));
        rig->addPort(rs485);
        sim::PortSpec generator;
        generator.systemName = "COM10";
        sim::applyChip(generator, *sim::findUsbSerialChip("ft232r"));
        generator.serialNumber = "SIMPG001";
        rig->addPort(generator);
        sim::PortSpec late;
        late.systemName = "COM6";
        late.presentAfterMs = 3600000; // not plugged in yet: absent from every list
        rig->addPort(late);
        sim::install(rig);

        std::vector<bridge::BackendSerialPortInfo> ports;
        MIB_REQUIRE(facade.listSerialPorts(ports), "initialized: a list");
        MIB_REQUIRE(ports.size() == 2, "only the rig's present ports, as Qt offers them: " + std::to_string(ports.size()));
        MIB_EXPECT(ports[0].systemPath == "COM3" && ports[0].comPort == 3, ports[0].systemPath);
        MIB_EXPECT(ports[1].systemPath == "COM10" && ports[1].comPort == 10, "COMn by number, not by text: " + ports[1].systemPath);
        for (const auto &port : ports) MIB_EXPECT(port.simulated, port.systemPath + " is simulated");
        MIB_EXPECT(ports[1].serialNumber == "SIMPG001", ports[1].serialNumber);
        MIB_EXPECT(ports[1].vidPid == "0403:6001", "lower-case VID:PID of the FT232R: " + ports[1].vidPid);
        MIB_EXPECT(ports[0].vidPid == "1a86:7523", "the CH340: " + ports[0].vidPid);
        MIB_EXPECT(!ports[0].description.empty(), "the description");

        // A read: no port was opened or taken.
        MIB_EXPECT(rig->events(sim::Event::Kind::Opened).empty() && rig->events(sim::Event::Kind::OpenDenied).empty() &&
                       rig->events(sim::Event::Kind::OpenFailed).empty(),
                   "listing opens nothing");
        MIB_EXPECT(claims::describe("COM3").empty(), "listing claims nothing");

        // An unplugged port leaves the list, as in Qt's enumeration.
        rig->unplug("COM3");
        MIB_REQUIRE(facade.listSerialPorts(ports), "list again");
        MIB_EXPECT(ports.size() == 1 && ports[0].systemPath == "COM10", "unplugged COM3 is gone");

        // The OS again: nothing in the list is simulated.
        sim::install(nullptr);
        MIB_REQUIRE(facade.listSerialPorts(ports), "OS list");
        for (const auto &port : ports) MIB_EXPECT(!port.simulated && !port.systemPath.empty(), port.systemPath);
    }

    void testReservePumpPorts(bridge::BackendFacade &facade, backend::AppBackend &app)
    {
        const std::string prefsOwner = "the syringe pumps (desktop preferences)";
        const std::string configOwner = "the syringe pumps (config.json)";
        std::string who;

        // Refused as a whole, nothing changes: the pump connect endpoint's rule.
        const std::string longPort(513, 'x');
        for (const auto &bad : std::vector<std::vector<std::string>>{{""},
                                                                    {" \t"},
                                                                    {"COM5", std::string("CO\0M6", 5)},
                                                                    {"COM5", longPort},
                                                                    {"1", "2", "3", "4", "5", "6", "7", "8", "9"}})
        {
            const auto result = facade.reservePumpPorts(bad);
            MIB_EXPECT(!result.ok && !result.message.empty(), "refused: " + result.message);
            MIB_EXPECT(!claims::isReserved("COM5"), "a refusal reserves nothing");
        }

        const auto reserved = facade.reservePumpPorts({"COM5", "/dev/ttyUSB0", "COM5"});
        MIB_REQUIRE(reserved.ok, reserved.message);
        MIB_EXPECT(reserved.command == bridge::BackendCommandType::Pump, "reported as a Pump command");
        MIB_EXPECT(claims::isReserved("COM5", &who) && who == prefsOwner, who);
        MIB_EXPECT(claims::isReserved("\\\\.\\COM5") && claims::isReserved("/dev/ttyUSB0"), "the same key forms as every claim");

        // The startup probe really is refused on a reserved port.
        std::string why;
        claims::Holder heldBy = claims::Holder::None;
        auto probe = claims::tryProbe("COM5", "startup nanopositioner probe", &why, std::chrono::milliseconds(0), &heldBy);
        MIB_EXPECT(!probe && heldBy == claims::Holder::Reserved, "the probe is turned away: " + why);
        MIB_EXPECT(why.find(prefsOwner) != std::string::npos, why);

        // config.json's reservation is another owner: it neither wipes ours nor is wiped by it.
        app.setLastConfigJson(R"({"pump_sample_com_port":7,"pump_sheath_com_port":8})");
        MIB_EXPECT(claims::isReserved("COM7", &who) && who == configOwner, who);
        MIB_EXPECT(claims::isReserved("COM5", &who) && who == prefsOwner, "ours survives a config.json load: " + who);
        MIB_REQUIRE(facade.reservePumpPorts({"COM9"}).ok, "replace our list");
        MIB_EXPECT(!claims::isReserved("COM5") && !claims::isReserved("/dev/ttyUSB0"), "the previous list is replaced");
        MIB_EXPECT(claims::isReserved("COM9"), "the new one holds");
        MIB_EXPECT(claims::isReserved("COM7") && claims::isReserved("COM8"), "config.json's ports stay reserved");
        MIB_REQUIRE(facade.reservePumpPorts({}).ok, "clear ours");
        MIB_EXPECT(!claims::isReserved("COM9"), "cleared");
        MIB_EXPECT(claims::isReserved("COM7") && claims::isReserved("COM8"), "clearing ours leaves config.json's alone");
        app.setLastConfigJson("{}");
        MIB_EXPECT(!claims::isReserved("COM7"), "config.json cleared its own");
        (void)facade;
    }
} // namespace

int main()
{
    mib::test::Watchdog watchdog(120);
    spdlog::set_level(spdlog::level::off);
#if defined(_WIN32)
    _putenv_s("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", "file:///nonexistent/manifest.json");
#else
    setenv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", "file:///nonexistent/manifest.json", 1);
#endif
    const auto dataDir = makeTempDir();
    {
        backend::AppBackend app;
        bridge::BackendFacade facade(app);
        EventLog log;
        facade.setEventSink([&log](const bridge::BackendEvent &event) { log.push(event); });

        watchdog.mark("before initialize");
        testSerialPorts(facade);
        // Reservations are process-wide: they work before the backend is initialized.
        MIB_EXPECT(facade.reservePumpPorts({"COM11"}).ok, "reserve before initialize");
        MIB_EXPECT(claims::isReserved("COM11"), "reserved");
        MIB_EXPECT(facade.reservePumpPorts({}).ok && !claims::isReserved("COM11"), "cleared");

        MIB_REQUIRE(facade.initialize(dataDir.string()), "facade initializes");
        watchdog.mark("autofocus messages");
        testAutofocusStatusMessages(facade, app, log);
        watchdog.mark("serial ports");
        testSerialPortsWithRig(facade);
        watchdog.mark("reservations");
        testReservePumpPorts(facade, app);
        watchdog.mark("detach on shutdown");
        testDetachOnShutdown(facade, app, log);
    }

    {
        // A facade destroyed without shutdown() detaches too: the service outlives it and speaks on.
        backend::AppBackend app;
        EventLog log;
        std::atomic<bool> stop{false};
        std::thread hammer;
        {
            bridge::BackendFacade facade(app);
            facade.setEventSink([&log](const bridge::BackendEvent &event) { log.push(event); });
            MIB_REQUIRE(facade.initialize(dataDir.string()), "second facade initializes");
            MIB_REQUIRE(app.autofocus().setBackendFactory([](nano::BackendKind) { return nullptr; }), "factory set");
            hammer = std::thread([&] {
                while (!stop.load()) app.autofocus().connect(0, 0, 0);
            });
            MIB_REQUIRE(waitFor([&] { return log.autofocusErrors() > 20; }), "failures reach the log");
        } // ~BackendFacade without shutdown()
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); // let the service speak into the void
        stop.store(true);
        hammer.join();
    }

    std::error_code cleanupError;
    std::filesystem::remove_all(dataDir, cleanupError);
    if (mib::test::exitCode() == 0) std::printf("hardware_parity_facade_test passed\n");
    return mib::test::exitCode();
}
