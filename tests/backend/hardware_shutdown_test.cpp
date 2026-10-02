#include "backend/app/AppBackend.h"
#include "backend/services/SerialBus.h"
#include "backend/services/SyringePumpService.h"
#include "backend/services/PulseGeneratorService.h"
#include "backend/services/ModbusRtu.h"
#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"
#include <atomic>
#include <cstdlib>

namespace {
std::atomic<int> openPorts{0};
class FakeSerial final : public backend::services::ISerialPort {
public:
    ~FakeSerial() override { close(); }
    bool open(int, int) override {
        opened = true;
        ++openPorts;
        return true;
    }
    bool openNamed(const std::string&, const backend::services::SerialSettings&) override {
        return open(0, 0);
    }
    void close() override {
        if (opened) {
            opened = false;
            --openPorts;
        }
    }
    bool isOpen() const override { return opened; }
    int write(const std::vector<uint8_t>& request) override {
        namespace m = backend::services::modbus;
        if (request[1] == 3) {
            const int count = (request[4] << 8) | request[5];
            rx = {request[0], 3, static_cast<uint8_t>(count * 2)};
            const int reg = (request[2] << 8) | request[3];
            auto data = (reg == 0x004A || reg == 0x004C)
                            ? m::floatToRegisters(reg == 0x004A ? 1.5f : 9999.0f)
                            : std::vector<uint8_t>(count * 2, 0);
            rx.insert(rx.end(), data.begin(), data.end());
            m::appendCrc(rx);
        } else if (request[1] == 6) {
            rx = request;
        } else {
            rx.assign(request.begin(), request.begin() + 6);
            m::appendCrc(rx);
        }
        return static_cast<int>(request.size());
    }
    bool waitForBytesWritten(int) override { return true; }
    bool waitForReadyRead(int) override { return !rx.empty(); }
    std::vector<uint8_t> readAll() override {
        auto result = std::move(rx);
        rx.clear();
        return result;
    }
    std::string lastError() const override { return {}; }

private:
    bool opened = false;
    std::vector<uint8_t> rx;
};
} // namespace

int main() {
#ifdef _WIN32
    _putenv_s("MIB_CAMERA_MODE", "mock");
    _putenv_s("MIB_DISABLED_SERVICES", "yolo,auto_update");
#else
    setenv("MIB_CAMERA_MODE", "mock", 1);
    setenv("MIB_DISABLED_SERVICES", "yolo,auto_update", 1);
#endif
    mib::test::Watchdog watchdog(20);
    mib::test::TempDir dir("hardware_shutdown");
    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize(dir.path().string()), "backend initializes");
    backend.serialBus().setSerialPortFactory([] { return std::make_unique<FakeSerial>(); });
    using Pump = backend::services::SyringePumpService::PumpId;
    for (int cycle = 0; cycle < 10; ++cycle) {
        watchdog.mark("connect and shut down shared hardware");
        MIB_REQUIRE(backend.syringePump().connect(Pump::Sample, "fake-shared", 9600, 1),
                    "sample connects");
        MIB_REQUIRE(backend.syringePump().connect(Pump::Sheath, "fake-shared", 9600, 2),
                    "sheath connects");
        MIB_REQUIRE(backend.pulseGenerator().connect("fake-shared", 9600, 3), "generator connects");
        MIB_EXPECT(openPorts == 1, "clients share one port");
        backend.shutdown();
        MIB_EXPECT(!backend.syringePump().isConnected(Pump::Sample),
                   "shutdown disconnects sample before destruction");
        MIB_EXPECT(!backend.syringePump().isConnected(Pump::Sheath),
                   "shutdown disconnects sheath before destruction");
        MIB_EXPECT(!backend.pulseGenerator().isConnected(),
                   "shutdown disconnects generator before destruction");
        MIB_EXPECT(openPorts == 0, "shutdown releases the shared adapter");
        backend.shutdown();
    }
    return mib::test::exitCode();
}
