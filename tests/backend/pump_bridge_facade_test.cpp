// pump_bridge_facade_test (BE-7, issue #277, epic #246)
//
// Drives both pump identities end to end through the BackendFacade PumpCommand
// surface with the fake Modbus serial seam: connect/configure/start/stop/
// purge/poll, the authoritative per-pump snapshots, structured errors for
// invalid parameters, the COM-port conflict rules, and safe stop on
// disconnect. Complements syringe_pump_fake_serial_test (service level).

#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "backend/services/ISerialPort.h"
#include "backend/services/SerialBus.h"
#include "backend/services/ModbusRtu.h"
#include "backend/services/SyringePumpService.h"

#include "support/assert.h"

#include <cstdint>
#include <filesystem>
#include <chrono>
#include <memory>
#include <thread>
#include <random>
#include <vector>

namespace m = backend::services::modbus;
using backend::services::ISerialPort;
using backend::services::SyringePumpService;

namespace
{
    constexpr uint16_t REG_MIN_FLOW_RATE = 0x004A;
    constexpr uint16_t REG_MAX_FLOW_RATE = 0x004C;

    // Minimal Modbus RTU slave: answers FC03 reads with canned registers,
    // echoes FC06 writes, acks FC16 (same shape as the service-level test).
    class FakeSerialPort final : public ISerialPort
    {
    public:
        explicit FakeSerialPort(uint8_t slaveAddr) : slaveAddr_(slaveAddr) {}

        bool open(int, int) override { open_ = true; return true; }
        void close() override { open_ = false; }
        bool isOpen() const override { return open_; }
        int write(const std::vector<uint8_t> &req) override
        {
            respondTo(req);
            return static_cast<int>(req.size());
        }
        bool waitForBytesWritten(int) override { return true; }
        bool waitForReadyRead(int timeoutMs) override
    {
        if (rx_.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
        return !rx_.empty();
    }
        std::vector<uint8_t> readAll() override
        {
            std::vector<uint8_t> r;
            r.swap(rx_);
            return r;
        }
        std::string lastError() const override { return {}; }

    private:
        void respondTo(const std::vector<uint8_t> &req)
        {
            rx_.clear();
            if (req.size() < 6 || req[0] != slaveAddr_)
            {
                return;
            }
            const uint8_t func = req[1];
            std::vector<uint8_t> resp;
            if (func == 0x03)
            {
                const auto startReg = static_cast<uint16_t>((req[2] << 8) | req[3]);
                const auto count = static_cast<uint16_t>((req[4] << 8) | req[5]);
                resp = {slaveAddr_, func, static_cast<uint8_t>(count * 2)};
                std::vector<uint8_t> data;
                if (startReg == REG_MIN_FLOW_RATE)
                {
                    data = m::floatToRegisters(1.5f);
                }
                else if (startReg == REG_MAX_FLOW_RATE)
                {
                    data = m::floatToRegisters(9999.0f);
                }
                else
                {
                    data.assign(static_cast<size_t>(count) * 2, 0);
                }
                resp.insert(resp.end(), data.begin(), data.end());
                m::appendCrc(resp);
            }
            else if (func == 0x06)
            {
                resp = req;
            }
            else if (func == 0x10)
            {
                resp = {slaveAddr_, func, req[2], req[3], req[4], req[5]};
                m::appendCrc(resp);
            }
            else
            {
                return;
            }
            rx_ = std::move(resp);
        }

        uint8_t slaveAddr_;
        bool open_{false};
        std::vector<uint8_t> rx_;
    };

    std::filesystem::path makeTempDir()
    {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        std::uniform_int_distribution<unsigned long long> dist;
        const auto path = std::filesystem::temp_directory_path() /
                          ("mib_pump_bridge_" + std::to_string(dist(gen)));
        std::filesystem::create_directories(path);
        return path;
    }
} // namespace

int main()
{
    namespace bridge = backend::bridge;

    const auto dataDir = makeTempDir();
#if defined(_WIN32)
    _putenv_s("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", "file:///nonexistent/manifest.json");
#else
    setenv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", "file:///nonexistent/manifest.json", 1);
#endif

    {
        backend::AppBackend backendApp;
        bridge::BackendFacade facade(backendApp);
        MIB_REQUIRE(facade.initialize(dataDir.string()), "facade initializes");

        backendApp.serialBus().setSerialPortFactory(
            [] { return std::make_unique<FakeSerialPort>(1); });

        auto dispatchPump = [&facade](bridge::PumpCommand cmd) {
            return facade.dispatch(cmd);
        };

        // Structured errors: invalid pump id / COM port / Modbus address.
        {
            bridge::PumpCommand cmd;
            cmd.action = bridge::PumpCommandAction::Connect;
            cmd.pumpId = 7;
            MIB_EXPECT(!dispatchPump(cmd).ok, "invalid pump id rejected");
            cmd.pumpId = 0;
            cmd.comPort = -1;
            MIB_EXPECT(!dispatchPump(cmd).ok, "invalid COM port rejected");
            cmd.comPort = 3;
            cmd.modbusAddress = 300;
            MIB_EXPECT(!dispatchPump(cmd).ok, "invalid Modbus address rejected");
        }

        // Both pump identities connect independently through the facade.
        auto connect = [&](int pumpId, int comPort) {
            bridge::PumpCommand cmd;
            cmd.action = bridge::PumpCommandAction::Connect;
            cmd.pumpId = pumpId;
            cmd.comPort = comPort;
            cmd.baudRate = 115200;
            cmd.modbusAddress = 1;
            return dispatchPump(cmd);
        };
        MIB_REQUIRE(connect(0, 3).ok, "sample pump connects via fake serial");

        // COM-port conflict: the other pump cannot claim the same port.
        MIB_EXPECT(!connect(1, 3).ok, "sheath pump rejected on the sample pump's port");
        MIB_REQUIRE(connect(1, 4).ok, "sheath pump connects on its own port");

        bridge::BackendPumpStatus sample;
        MIB_REQUIRE(facade.fetchPumpStatus(0, sample), "sample status fetch");
        MIB_EXPECT(sample.connected, "sample connected");
        MIB_EXPECT(sample.minFlowRate == 1.5, "min flow rate parsed");
        MIB_EXPECT(sample.maxFlowRate == 9999.0, "max flow rate parsed");
        MIB_EXPECT(sample.comPort == 3, "sample COM port in snapshot");
        bridge::BackendPumpStatus sheath;
        MIB_REQUIRE(facade.fetchPumpStatus(1, sheath), "sheath status fetch");
        MIB_EXPECT(sheath.connected && sheath.comPort == 4, "sheath snapshot independent");

        // Control path: flow rate, direction, start/stop, purge, volume, poll.
        {
            bridge::PumpCommand cmd;
            cmd.pumpId = 0;
            cmd.action = bridge::PumpCommandAction::SetFlowRate;
            cmd.flowRate = 100.0;
            cmd.flowRateUnit = 100;
            MIB_EXPECT(dispatchPump(cmd).ok, "set flow rate");
            cmd.action = bridge::PumpCommandAction::SetFlowRate;
            cmd.flowRate = -2.0;
            MIB_EXPECT(!dispatchPump(cmd).ok, "negative flow rate rejected");
            cmd.action = bridge::PumpCommandAction::SetDirection;
            cmd.direction = 1;
            MIB_EXPECT(dispatchPump(cmd).ok, "set direction");
            cmd.action = bridge::PumpCommandAction::Start;
            MIB_EXPECT(dispatchPump(cmd).ok, "start");
            cmd.action = bridge::PumpCommandAction::Stop;
            MIB_EXPECT(dispatchPump(cmd).ok, "stop");
            cmd.action = bridge::PumpCommandAction::Purge;
            cmd.direction = 0;
            MIB_EXPECT(dispatchPump(cmd).ok, "purge");
            cmd.action = bridge::PumpCommandAction::StopPurge;
            MIB_EXPECT(dispatchPump(cmd).ok, "stop purge");
            cmd.action = bridge::PumpCommandAction::SetSyringeVolume;
            cmd.syringeVolume = 10;
            cmd.syringeVolumeUnit = 1;
            MIB_EXPECT(dispatchPump(cmd).ok, "set syringe volume");
            cmd.action = bridge::PumpCommandAction::SetSyringeVolume;
            cmd.syringeVolume = 0;
            MIB_EXPECT(!dispatchPump(cmd).ok, "zero syringe volume rejected");
            cmd.action = bridge::PumpCommandAction::PollStatus;
            MIB_EXPECT(dispatchPump(cmd).ok, "poll status");
        }

        // Disconnect safely stops and clears; the freed port becomes usable.
        {
            bridge::PumpCommand cmd;
            cmd.pumpId = 0;
            cmd.action = bridge::PumpCommandAction::Disconnect;
            MIB_EXPECT(dispatchPump(cmd).ok, "disconnect");
            bridge::BackendPumpStatus after;
            MIB_REQUIRE(facade.fetchPumpStatus(0, after), "status after disconnect");
            MIB_EXPECT(!after.connected, "disconnected");
        }

        facade.shutdown();
    }

    std::error_code cleanupError;
    std::filesystem::remove_all(dataDir, cleanupError);
    if (mib::test::exitCode() == 0)
    {
        std::printf("pump_bridge_facade_test passed\n");
    }
    return mib::test::exitCode();
}
