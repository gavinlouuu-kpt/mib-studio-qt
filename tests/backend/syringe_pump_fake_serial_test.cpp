// syringe_pump_fake_serial_test
//
// Drives SyringePumpService end-to-end through the injected ISerialPort seam
// (epic #246), with no hardware. A FakeSerialPort acts as a minimal Modbus RTU
// slave (a dLSP pump): it answers FC03 reads with canned registers, echoes FC06
// writes, and acks FC16. This turns the previously-untested connect/read/write
// path into covered code and exercises the timeout + bad-CRC failure paths.

#include "backend/services/SyringePumpService.h"
#include "backend/services/ISerialPort.h"
#include "backend/services/SerialBus.h"
#include "backend/services/ModbusRtu.h"

#include "support/assert.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace m = backend::services::modbus;
using backend::services::ISerialPort;
using backend::services::SyringePumpService;

namespace {

// Register addresses the service reads during connect()/pollStatus() (from
// SyringePumpService.cpp). Only the two float rates need specific values.
constexpr uint16_t REG_MIN_FLOW_RATE = 0x004A;
constexpr uint16_t REG_MAX_FLOW_RATE = 0x004C;
constexpr uint16_t REG_REALTIME_INFUSE_FLOW = 0x0102;
constexpr uint16_t REG_ACCUM_VOLUME = 0x00C7;

enum class Mode { Normal, NoResponse, BadCrc };

// Fault switches for the readback section (#712): every FC03 answered with a bad CRC, or not at all.
std::atomic<bool> gCorruptReads{false};
std::atomic<bool> gSilentReads{false};
std::atomic<int> gReadRequests{0};

class FakeSerialPort final : public ISerialPort {
public:
    explicit FakeSerialPort(Mode mode, uint8_t slaveAddr) : mode_(mode), slaveAddr_(slaveAddr) {}

    bool open(int, int) override { open_ = true; return true; }
    void close() override { open_ = false; }
    bool isOpen() const override { return open_; }

    int write(const std::vector<uint8_t>& req) override
    {
        respondTo(req);
        return static_cast<int>(req.size());
    }
    bool waitForBytesWritten(int) override { return true; }
    bool waitForReadyRead(int timeoutMs) override
    {
        // The bus session polls until its deadline; a silent device must not
        // spin the I/O thread.
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
    void respondTo(const std::vector<uint8_t>& req)
    {
        rx_.clear();
        if (mode_ == Mode::NoResponse) return;         // -> read timeout
        if (req.size() < 6 || req[0] != slaveAddr_) return;

        const uint8_t func = req[1];
        std::vector<uint8_t> resp;
        if (func == 0x03) gReadRequests.fetch_add(1);
        if (func == 0x03 && gSilentReads.load()) return; // -> read timeout
        if (func == 0x03) { // read holding
            const uint16_t startReg = static_cast<uint16_t>((req[2] << 8) | req[3]);
            const uint16_t count = static_cast<uint16_t>((req[4] << 8) | req[5]);
            resp = {slaveAddr_, func, static_cast<uint8_t>(count * 2)};
            const std::vector<uint8_t> data = registerData(startReg, count);
            resp.insert(resp.end(), data.begin(), data.end());
            m::appendCrc(resp);
        } else if (func == 0x06) { // write single -> echo (already CRC-correct)
            resp = req;
        } else if (func == 0x10) { // write multiple -> ack header
            resp = {slaveAddr_, func, req[2], req[3], req[4], req[5]};
            m::appendCrc(resp);
        } else {
            return;
        }

        if ((mode_ == Mode::BadCrc || (func == 0x03 && gCorruptReads.load())) && !resp.empty()) {
            resp.back() ^= 0xFF; // corrupt trailing CRC byte
        }
        rx_ = std::move(resp);
    }

    static std::vector<uint8_t> registerData(uint16_t startReg, uint16_t count)
    {
        if (startReg == REG_MIN_FLOW_RATE) return m::floatToRegisters(1.5f);
        if (startReg == REG_MAX_FLOW_RATE) return m::floatToRegisters(9999.0f);
        if (startReg == REG_REALTIME_INFUSE_FLOW) return {0x00, 0x2A}; // 42
        if (startReg == REG_ACCUM_VOLUME) return m::floatToRegisters(3.25f);
        return std::vector<uint8_t>(static_cast<size_t>(count) * 2, 0);
    }

    Mode mode_;
    uint8_t slaveAddr_;
    bool open_{false};
    std::vector<uint8_t> rx_;
};

// The pump talks through the shared bus registry (SerialBus.h); the fake
// serial port is injected there, so every session the pump acquires is a
// fake dLSP slave.
void configure(backend::services::serialbus::SerialBusManager& bus, Mode mode, uint8_t addr = 1)
{
    bus.setSerialPortFactory([mode, addr] { return std::make_unique<FakeSerialPort>(mode, addr); });
}

} // namespace

int main()
{
    using PumpId = SyringePumpService::PumpId;

    // 1) Happy path: connect succeeds, min/max flow rates parse from the fake's
    //    float registers, and control + polling round-trip.
    {
        backend::services::serialbus::SerialBusManager bus;
        configure(bus, Mode::Normal);
        SyringePumpService svc(bus);
        MIB_REQUIRE(svc.connect(PumpId::Sample, 3, 115200, 1), "connect succeeds via fake serial");
        MIB_EXPECT(svc.isConnected(PumpId::Sample), "pump reports connected");

        const auto st = svc.getStatus(PumpId::Sample);
        MIB_EXPECT(st.minFlowRate == 1.5, "min flow rate parsed from FC03 float regs");
        MIB_EXPECT(st.maxFlowRate == 9999.0, "max flow rate parsed from FC03 float regs");

        MIB_EXPECT(svc.setFlowRate(PumpId::Sample, 100.0, 100), "setFlowRate acked by fake");
        MIB_EXPECT(svc.start(PumpId::Sample), "start acked");
        MIB_EXPECT(svc.stop(PumpId::Sample), "stop acked");

        svc.pollStatus(PumpId::Sample); // reads run/error/flow/volume regs (all zero)
        const auto st2 = svc.getStatus(PumpId::Sample);
        MIB_EXPECT(st2.runStatus == SyringePumpService::RunStatus::Stop, "poll sees stopped");
        MIB_EXPECT(!st2.stalled, "poll sees no stall");

        svc.disconnect(PumpId::Sample);
        MIB_EXPECT(!svc.isConnected(PumpId::Sample), "disconnect clears connected");
    }

    // 2) A device that never answers -> connect fails (read timeout path), no hang.
    {
        backend::services::serialbus::SerialBusManager bus;
        configure(bus, Mode::NoResponse);
        SyringePumpService svc(bus);
        MIB_EXPECT(!svc.connect(PumpId::Sheath, 4, 115200, 1), "non-responding device fails to connect");
        MIB_EXPECT(!svc.isConnected(PumpId::Sheath), "not connected after failure");
    }

    // 3) Corrupted CRC on the channel-enable echo -> connect fails (frame rejected).
    {
        backend::services::serialbus::SerialBusManager bus;
        configure(bus, Mode::BadCrc);
        SyringePumpService svc(bus);
        MIB_EXPECT(!svc.connect(PumpId::Sample, 5, 115200, 1), "bad-CRC response rejected -> connect fails");
    }

    // 4) Address mismatch: a device at addr 2 does not answer a scan/connect for addr 1.
    {
        backend::services::serialbus::SerialBusManager bus;
        configure(bus, Mode::Normal, /*addr=*/2);
        SyringePumpService svc(bus);
        MIB_EXPECT(!svc.connect(PumpId::Sample, 6, 115200, 1), "wrong-address device does not respond");
    }

    // 5) Run facts (#712): the readback is the last complete poll, never a command's bookkeeping;
    //    a failed poll keeps the older readback and says so; a busy pump is not waited out.
    {
        using namespace std::chrono_literals;
        backend::services::serialbus::SerialBusManager bus;
        configure(bus, Mode::Normal);
        SyringePumpService svc(bus);
        SyringePumpService::RunFacts f;
        MIB_REQUIRE(svc.runFacts(PumpId::Sample, 10ms, f), "facts of an idle slot");
        MIB_EXPECT(!f.connected && !f.readback.valid, "unconnected slot: no readback");
        MIB_REQUIRE(svc.connect(PumpId::Sample, 3, 115200, 1), "connect");
        MIB_REQUIRE(svc.runFacts(PumpId::Sample, 10ms, f), "facts after connect");
        MIB_EXPECT(f.connected && !f.readback.valid, "connect alone is not a status readback");
        MIB_EXPECT(!f.applied.flowRate && !f.applied.flowRateUnit && !f.applied.direction,
                   "a dLSP connect applies and reads no setting: all unknown (not the config defaults)");
        MIB_EXPECT(svc.setFlowRate(PumpId::Sample, 100.0, 100) && svc.start(PumpId::Sample), "flow set, started");
        MIB_REQUIRE(svc.runFacts(PumpId::Sample, 10ms, f), "facts after commands");
        MIB_EXPECT(!f.readback.valid && f.config.flowRate == 100.0 && f.config.flowRateUnit == 100,
                   "commands change the setpoint, not the readback");
        MIB_EXPECT(f.applied.flowRate == std::optional<double>(100.0) &&
                       f.applied.flowRateUnit == std::optional<uint16_t>(100) && !f.applied.direction,
                   "the applied flow rate is known, the never-written direction is not");
        MIB_EXPECT(svc.setFlowRate(PumpId::Sample, 12345.6, 100), "an over-range rate is clamped");
        MIB_REQUIRE(svc.runFacts(PumpId::Sample, 10ms, f), "facts after a clamped rate");
        MIB_EXPECT(f.applied.flowRate == std::optional<double>(9999.0) && f.config.flowRate == 12345.6,
                   "applied = the value the pump was written, config = the request");
        MIB_EXPECT(svc.setDirection(PumpId::Sample, SyringePumpService::Direction::Withdraw), "direction set");
        MIB_REQUIRE(svc.runFacts(PumpId::Sample, 10ms, f), "facts after direction");
        MIB_EXPECT(f.applied.direction == std::optional<SyringePumpService::Direction>(
                                               SyringePumpService::Direction::Withdraw),
                   "applied direction known");
        svc.pollStatus(PumpId::Sample);
        MIB_REQUIRE(svc.runFacts(PumpId::Sample, 10ms, f), "facts after a poll");
        MIB_EXPECT(f.readback.valid && f.readback.lastPollOk &&
                       f.readback.runStatus == SyringePumpService::RunStatus::Stop &&
                       f.readback.realtimeFlowRaw == std::optional<double>(42.0) &&
                       f.readback.accumulatedVolumeRaw == std::optional<double>(3.25) &&
                       f.readback.stalled == std::optional<bool>(false) && !f.readback.speedRpm,
                   "a complete poll is the readback, in the pump's own registers");
        const auto firstAt = f.readback.at;
        gCorruptReads = true;
        svc.pollStatus(PumpId::Sample);
        gCorruptReads = false;
        MIB_REQUIRE(svc.runFacts(PumpId::Sample, 10ms, f), "facts after a failed poll");
        MIB_EXPECT(f.readback.valid && !f.readback.lastPollOk && f.readback.at == firstAt,
                   "a failed poll keeps the older readback and marks the newest poll failed");
        // A poll stuck on a silent pump holds the slot: runFacts gives up instead of waiting it out.
        gSilentReads = true;
        const int before = gReadRequests.load();
        std::thread poller([&] { svc.pollStatus(PumpId::Sample); });
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (gReadRequests.load() == before && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        MIB_EXPECT(gReadRequests.load() > before, "the poll reached the silent pump");
        MIB_EXPECT(!svc.runFacts(PumpId::Sample, 20ms, f), "a busy pump is reported, not waited for");
        gSilentReads = false;
        poller.join();
        svc.disconnect(PumpId::Sample);
        MIB_REQUIRE(svc.runFacts(PumpId::Sample, 10ms, f), "facts after disconnect");
        MIB_EXPECT(!f.connected && !f.readback.valid && !f.applied.flowRate && !f.applied.direction,
                   "disconnect drops the readback and the applied settings");
    }

    if (mib::test::exitCode() == 0) {
        std::printf("SyringePumpService fake-serial round-trip verified\n");
    }
    return mib::test::exitCode();
}
