// serial_sim_test
//
// The simulated serial rig (SimulatedSerial.h) behaves like the hardware it
// stands in for, so a race it reports is real and a clean run means something:
//  - exclusive open: a second open is refused with the OS "access denied"
//    code, which SerialBusManager classifies as PortBusy, and the ledger names
//    the holder; non-exclusive mode records the open as SharedOpen instead;
//  - late enumeration, silence after open, scheduled and manual unplugs;
//  - the real services connect through it: dLSP and Tushui pumps, the pulse
//    generator (connect and scan), ZC300 identity (FC04), and the OEABT
//    nanopositioner over fragmented replies;
//  - a frame at the wrong baud is never answered (BaudMismatch), and an ASCII
//    command on an RS485 bus is a ForeignFrame;
//  - scenario JSON and MIB_SERIAL_MODE=sim install a rig; a broken scenario
//    installs nothing; uninstall restores the OS ports.

#include "backend/app/Tools.h"
#include "backend/nanopositioner/INanopositionerBackend.h"
#include "backend/services/ModbusRtu.h"
#include "backend/services/PulseGeneratorService.h"
#include "backend/services/SerialBus.h"
#include "backend/services/SimulatedSerial.h"
#include "backend/services/SimulatedSerialScenario.h"
#include "backend/services/SyringePumpService.h"
#include "backend/stage/zc300/Zc300Protocol.h"

#include "support/assert.h"
#include "support/watchdog.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>

namespace serialsim = backend::services::serialsim;
namespace serialbus = backend::services::serialbus;
namespace modbus = backend::services::modbus;
using backend::services::PulseGeneratorService;
using backend::services::SerialSettings;
using backend::services::SyringePumpService;
using namespace std::chrono_literals;

namespace {

SerialSettings at(int baud)
{
    SerialSettings s;
    s.baudRate = baud;
    return s;
}

int count(const serialsim::Rig& rig, serialsim::Event::Kind kind)
{
    return static_cast<int>(rig.events(kind).size());
}

void setEnv(const char* name, const char* value)
{
#if defined(_WIN32)
    _putenv_s(name, value ? value : "");
#else
    if (value) ::setenv(name, value, 1);
    else ::unsetenv(name);
#endif
}

} // namespace

int main()
{
    mib::test::Watchdog watchdog(30);

    // ---- exclusive open, ledger, PortBusy classification ---------------------
    {
        watchdog.mark("exclusive");
        auto rig = std::make_shared<serialsim::Rig>(3);
        serialsim::PortSpec spec;
        spec.systemName = "COM3";
        rig->addPort(spec);
        rig->addDevice("COM3", serialsim::makeDlspPump(1));
        serialsim::install(rig);

        auto first = backend::services::makePlatformSerialPort();
        MIB_REQUIRE(first->openNamed("COM3", at(115200)), "first open succeeds");
        auto request = modbus::buildReadRequest(1, 0x4A, 2);
        MIB_EXPECT(first->write(request) == static_cast<int>(request.size()), "write accepted");

        auto second = backend::services::makePlatformSerialPort();
        MIB_EXPECT(!second->openNamed("\\\\.\\COM3", at(115200)), "second open of a held port is refused");
        MIB_EXPECT(second->lastError().find("Access is denied") != std::string::npos, "the error says why");
        const auto denied = rig->events(serialsim::Event::Kind::OpenDenied);
        MIB_EXPECT(denied.size() == 1 && denied[0].detail.find("modbus addr=1 fc=3") != std::string::npos,
                   "the ledger names what the holder was doing");

        serialbus::SerialBusManager manager;
        serialbus::BusError error = serialbus::BusError::None;
        MIB_EXPECT(!manager.acquire("COM3", at(115200), &error) && error == serialbus::BusError::PortBusy,
                   "SerialBusManager reports a held port as PortBusy, as on Windows");
        first->close();
        MIB_EXPECT(manager.acquire("COM3", at(115200), &error) != nullptr, "released port opens");
        MIB_EXPECT(rig->contentionCount() == 2, "both refused opens count as contention");

        rig->setExclusiveOpen(false);
        auto a = rig->makePort();
        auto b = rig->makePort();
        MIB_EXPECT(a->openNamed("COM3", at(115200)) && b->openNamed("COM3", at(115200)),
                   "non-exclusive mode lets both open, as POSIX does");
        MIB_EXPECT(count(*rig, serialsim::Event::Kind::SharedOpen) >= 1, "and records it");
        serialsim::install(nullptr);
    }

    // ---- services connect through the rig ------------------------------------
    {
        watchdog.mark("services");
        auto rig = std::make_shared<serialsim::Rig>(5);
        serialsim::PortSpec bus;
        bus.systemName = "COM3";
        bus.replyLatencyMinMs = 1;
        bus.replyLatencyMaxMs = 6;
        bus.fragmentReplies = true;
        rig->addPort(bus);
        rig->addDevice("COM3", serialsim::makeDlspPump(1));
        rig->addDevice("COM3", serialsim::makeTushuiPump(3));
        serialsim::PortSpec gen;
        gen.systemName = "COM4";
        rig->addPort(gen);
        rig->addDevice("COM4", serialsim::makePulseGenerator(2));
        serialsim::PortSpec stage;
        stage.systemName = "COM6";
        rig->addPort(stage);
        rig->addDevice("COM6", serialsim::makeZc300(1, 4242));
        serialsim::install(rig);

        serialbus::SerialBusManager manager;
        SyringePumpService pumps(manager);
        MIB_EXPECT(pumps.connect(SyringePumpService::PumpId::Sample, 3, 115200, 1), "dLSP pump connects by COM number");
        MIB_EXPECT(pumps.connect(SyringePumpService::PumpId::Sheath, "COM3", 115200, 3,
                                 SyringePumpService::PumpModel::TushuiPeristaltic, 25.0),
                   "Tushui pump connects on the same bus");
        MIB_EXPECT(rig->holders("COM3") == 1, "two pumps share one bus session, one OS handle");

        PulseGeneratorService generator(manager);
        MIB_EXPECT(!generator.connect("COM4", 115200, 2), "generator at the wrong baud is not verified");
        MIB_EXPECT(count(*rig, serialsim::Event::Kind::BaudMismatch) == 1, "BaudMismatch recorded");
        MIB_EXPECT(generator.connect("COM4", 9600, 2), "generator at 9600 connects");
        generator.disconnect();
        std::atomic<bool> cancel{false};
        const auto hits = generator.scanBus("COM4", at(9600), 1, 3, cancel, 50);
        MIB_EXPECT(hits.size() == 1 && hits[0].address == 2 &&
                       hits[0].kind == PulseGeneratorService::ScanHit::Kind::PulseGenerator,
                   "scan finds the generator at address 2 only");

        auto session = manager.acquire("COM6", at(115200));
        MIB_REQUIRE(session != nullptr, "stage bus opens");
        const auto ident = session->transact(backend::stage::zc300::buildRead(1, backend::stage::zc300::kRegSerial, 2), 500);
        std::vector<uint8_t> data;
        MIB_EXPECT(ident.error == serialbus::BusError::None && modbus::extractReadData(ident.response, 2, data) &&
                       ((data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3]) == 4242,
                   "ZC300 serial reads over FC04");
        session.reset();

        // An OEABT probe written onto the pump bus is a foreign frame.
        pumps.disconnect(SyringePumpService::PumpId::Sample);
        pumps.disconnect(SyringePumpService::PumpId::Sheath);
        auto stray = rig->makePort();
        MIB_REQUIRE(stray->openNamed("COM3", at(115200)), "stray opener gets the bus");
        const std::string probe = "/1&\r";
        stray->write(std::vector<uint8_t>(probe.begin(), probe.end()));
        MIB_EXPECT(!stray->waitForReadyRead(50), "no pump answers ASCII");
        MIB_EXPECT(count(*rig, serialsim::Event::Kind::ForeignFrame) == 1, "ForeignFrame recorded");
        serialsim::install(nullptr);
    }

    // ---- late enumeration, silence after open, OEABT, unplug -----------------
    {
        watchdog.mark("timing");
        auto rig = std::make_shared<serialsim::Rig>(9);
        serialsim::PortSpec nano;
        nano.systemName = "COM5";
        nano.vendorId = 0x1a86;
        nano.productId = 0x7523;
        nano.presentAfterMs = 150;
        nano.silentAfterOpenMs = 100;
        nano.fragmentReplies = true;
        nano.unpluggedWindowsMs = {{60000, -1}}; // far future: must not affect this block
        rig->addPort(nano);
        auto controller = std::make_shared<serialsim::OeabtDevice>();
        rig->addDevice("COM5", controller);
        serialsim::install(rig);

        MIB_EXPECT(backend::services::enumerateSerialPorts().empty(), "absent before it enumerates");
        MIB_EXPECT(backend::Tools::availableComPortNumbers().empty(), "no COM numbers yet");
        auto early = rig->makePort();
        MIB_EXPECT(!early->openNamed("COM5", at(115200)), "open fails before it enumerates");
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (backend::services::enumerateSerialPorts().empty() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(10ms);
        MIB_REQUIRE(backend::Tools::availableComPortNumbers() == std::vector<int>{5}, "COM5 appears");

        auto port = rig->makePort();
        MIB_REQUIRE(port->openNamed("COM5", at(115200)), "opens once present");
        const std::string identify = "/1&\r";
        port->write(std::vector<uint8_t>(identify.begin(), identify.end()));
        MIB_EXPECT(!port->waitForReadyRead(30), "a frame right after open is lost (adapter boot)");
        port->close();

        std::this_thread::sleep_for(120ms);
        backend::nanopositioner::Endpoint endpoint;
        endpoint.backend = backend::nanopositioner::BackendKind::Oeabt;
        endpoint.systemPath = backend::services::enumerateSerialPorts().at(0).systemLocation;
        endpoint.knownOeabtCandidate = true;
        auto oeabt = backend::nanopositioner::createNanopositionerBackend(backend::nanopositioner::BackendKind::Oeabt);
        std::string error;
        MIB_REQUIRE(oeabt && oeabt->connect(endpoint, error), "OEABT connects through the platform factory: " + error);
        double volts = 0;
        MIB_EXPECT(oeabt->readVoltage(volts, error) && volts == 25.0, "voltage over fragmented replies");

        rig->unplug("COM5");
        MIB_EXPECT(!oeabt->readVoltage(volts, error), "I/O fails after the cable is pulled");
        MIB_EXPECT(backend::services::enumerateSerialPorts().empty(), "unplugged port leaves enumeration");
        oeabt->disconnect();
        rig->replug("COM5");
        MIB_EXPECT(oeabt->connect(endpoint, error), "reconnects after replug: " + error);
        oeabt->disconnect();
        MIB_EXPECT(rig->holders("COM5") == 0, "disconnect released the port");
        serialsim::install(nullptr);
    }

    // ---- scheduled unplug ------------------------------------------------------
    {
        watchdog.mark("scheduled unplug");
        auto rig = std::make_shared<serialsim::Rig>(1);
        serialsim::PortSpec spec;
        spec.systemName = "COM7";
        spec.unpluggedWindowsMs = {{100, 250}};
        rig->addPort(spec);
        rig->addDevice("COM7", serialsim::makeDlspPump(1));
        auto port = rig->makePort();
        MIB_REQUIRE(port->openNamed("COM7", at(115200)), "opens before the pull");
        std::this_thread::sleep_for(150ms);
        const auto request = modbus::buildReadRequest(1, 0, 1);
        MIB_EXPECT(port->write(request) < 0, "a handle across the pull is dead");
        MIB_EXPECT(!rig->makePort()->openNamed("COM7", at(115200)), "cannot open while pulled");
        std::this_thread::sleep_for(150ms);
        auto fresh = rig->makePort();
        MIB_EXPECT(fresh->openNamed("COM7", at(115200)), "a fresh open works after it is back");
        MIB_EXPECT(port->write(request) < 0, "the old handle stays dead");
    }

    // ---- scenario JSON and the environment switch ------------------------------
    {
        watchdog.mark("scenario");
        const auto rig = serialsim::loadScenario(R"({
            "seed": 11, "exclusiveOpen": false,
            "ports": [
              {"name": "COM8", "vid": "1a86", "pid": "7523", "replyLatencyMs": [0, 1],
               "unplugged": [{"fromMs": 50000}],
               "devices": [{"type": "dlsp-pump", "address": 4},
                           {"type": "modbus", "address": 9, "baud": 115200, "holding": {"5": 77}}]}
            ]})");
        MIB_EXPECT(rig->seed() == 11, "seed from the file");
        const auto ports = rig->enumerate();
        MIB_EXPECT(ports.size() == 1 && ports[0].vendorId == 0x1a86 && ports[0].productId == 0x7523, "port identity");
        auto port = rig->makePort();
        MIB_REQUIRE(port->openNamed("COM8", at(115200)), "scenario port opens");
        port->write(modbus::buildReadRequest(9, 5, 1));
        MIB_EXPECT(port->waitForReadyRead(500), "generic Modbus device answers");
        std::vector<uint8_t> data;
        MIB_EXPECT(modbus::extractReadData(port->readAll(), 1, data) && data == std::vector<uint8_t>({0, 77}),
                   "with its configured register");
        MIB_EXPECT(serialsim::loadScenario(R"({"ports": []})", 99)->seed() == 99, "seed override wins");

        bool threw = false;
        try {
            serialsim::loadScenario(R"({"ports": [{"name": "COM1", "devices": [{"type": "toaster"}]}]})");
        } catch (const std::runtime_error& e) {
            threw = std::string(e.what()).find("toaster") != std::string::npos;
        }
        MIB_EXPECT(threw, "an unknown device type is refused by name");

        std::string error;
        setEnv("MIB_SERIAL_MODE", nullptr);
        MIB_EXPECT(!serialsim::installFromEnvironment(error) && error.empty(), "unset: hardware, no error");
        setEnv("MIB_SERIAL_MODE", "sim");
        setEnv("MIB_SERIAL_SIM_SCENARIO", "/nonexistent/scenario.json");
        MIB_EXPECT(!serialsim::installFromEnvironment(error) && !error.empty() && !serialsim::installed(),
                   "unreadable scenario installs nothing and says why");
        setEnv("MIB_SERIAL_SIM_SCENARIO", nullptr);
        setEnv("MIB_SERIAL_SIM_SEED", "42");
        MIB_EXPECT(serialsim::installFromEnvironment(error) && serialsim::installed() &&
                       serialsim::installed()->seed() == 42,
                   "sim with no scenario installs the bench rig, seeded");
        const auto bench = backend::Tools::availableComPortNumbers();
        MIB_EXPECT(std::find(bench.begin(), bench.end(), 4) != bench.end(), "bench COM4 enumerates at once");
        setEnv("MIB_SERIAL_MODE", "bogus");
        MIB_EXPECT(!serialsim::installFromEnvironment(error) && !error.empty(), "an unknown mode is an error");
        setEnv("MIB_SERIAL_MODE", nullptr);
        setEnv("MIB_SERIAL_SIM_SEED", nullptr);
        serialsim::install(nullptr);
        MIB_EXPECT(!serialsim::installed(), "uninstalled");
    }

    return mib::test::exitCode();
}
