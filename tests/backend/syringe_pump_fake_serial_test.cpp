// syringe_pump_fake_serial_test
//
// Drives SyringePumpService end-to-end through the injected ISerialPort seam
// (epic #246), with no hardware. A FakeSerialPort acts as a minimal Modbus RTU
// slave (a dLSP pump): it answers FC03 reads with canned registers, echoes FC06
// writes, and acks FC16. This turns the previously-untested connect/read/write
// path into covered code and exercises the timeout + bad-CRC failure paths,
// and a stalled adapter in each serial call (#720).

#include "backend/services/SyringePumpService.h"
#include "backend/services/ISerialPort.h"
#include "backend/services/SerialBus.h"
#include "backend/services/ModbusRtu.h"

#include "support/assert.h"
#include "support/watchdog.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
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

// Stalled-adapter switches (#720), modelling the bounded platform ports: output that never drains
// (the drain fails at its timeout), a write the driver never takes (write fails at its deadline),
// and a port call that never returns at all (held until released).
std::atomic<bool> gNeverDrains{false};
std::atomic<bool> gWriteTimesOut{false};
std::mutex gHoldMutex;
std::condition_variable gHoldCv;
bool gHoldWrites = false; // under gHoldMutex
bool gHolding = false;    // under gHoldMutex: a write() is held right now

class FakeSerialPort final : public ISerialPort {
public:
    explicit FakeSerialPort(Mode mode, uint8_t slaveAddr) : mode_(mode), slaveAddr_(slaveAddr) {}

    bool open(int, int) override { open_ = true; return true; }
    void close() override { open_ = false; }
    bool isOpen() const override { return open_; }

    int write(const std::vector<uint8_t>& req) override
    {
        {
            std::unique_lock lock(gHoldMutex);
            if (gHoldWrites) {
                gHolding = true;
                gHoldCv.notify_all();
                gHoldCv.wait(lock, [] { return !gHoldWrites; });
                gHolding = false;
            }
        }
        if (gWriteTimesOut.load()) {
            error_ = "serial write timed out after 1000 ms (0 of " + std::to_string(req.size()) +
                     " bytes taken, discarded)";
            return -1;
        }
        if (gNeverDrains.load()) {
            queued_ = req.size(); // stays in the driver queue: never reaches the pump
            return static_cast<int>(req.size());
        }
        respondTo(req);
        return static_cast<int>(req.size());
    }
    bool waitForBytesWritten(int timeoutMs) override
    {
        if (queued_ == 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
        error_ = "serial output timed out: " + std::to_string(queued_) + " bytes not drained after " +
                 std::to_string(timeoutMs) + " ms (discarded)";
        queued_ = 0;
        return false;
    }
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
    std::string lastError() const override { return error_; }

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
    size_t queued_{0};
    std::string error_;
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
    // Fails fast with a diagnostic if a serial call ever hangs (#720).
    mib::test::Watchdog watchdog(30);

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

    watchdog.mark("no response");
    // 2) A device that never answers -> connect fails (read timeout path), no hang.
    {
        backend::services::serialbus::SerialBusManager bus;
        configure(bus, Mode::NoResponse);
        SyringePumpService svc(bus);
        MIB_EXPECT(!svc.connect(PumpId::Sheath, 4, 115200, 1), "non-responding device fails to connect");
        MIB_EXPECT(!svc.isConnected(PumpId::Sheath), "not connected after failure");
    }

    watchdog.mark("bad crc");
    // 3) Corrupted CRC on the channel-enable echo -> connect fails (frame rejected).
    {
        backend::services::serialbus::SerialBusManager bus;
        configure(bus, Mode::BadCrc);
        SyringePumpService svc(bus);
        MIB_EXPECT(!svc.connect(PumpId::Sample, 5, 115200, 1), "bad-CRC response rejected -> connect fails");
    }

    watchdog.mark("wrong address");
    // 4) Address mismatch: a device at addr 2 does not answer a scan/connect for addr 1.
    {
        backend::services::serialbus::SerialBusManager bus;
        configure(bus, Mode::Normal, /*addr=*/2);
        SyringePumpService svc(bus);
        MIB_EXPECT(!svc.connect(PumpId::Sample, 6, 115200, 1), "wrong-address device does not respond");
    }

    watchdog.mark("run facts");
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

    watchdog.mark("stalled adapter");
    // 6) A stalled adapter (#720): whichever serial call stalls, the pump command fails within its
    //    bound, names the timeout, and lets go of the pump; once the adapter recovers it works again.
    {
        using namespace std::chrono_literals;
        using Clock = std::chrono::steady_clock;
        backend::services::serialbus::SerialBusManager bus;
        configure(bus, Mode::Normal);
        SyringePumpService svc(bus);
        MIB_REQUIRE(svc.connect(PumpId::Sample, 3, 115200, 1), "connect");
        // The bound of one pump transaction (SERIAL_TIMEOUT_MS = 1000) plus a generous ceiling.
        const auto bound =
            std::chrono::milliseconds(backend::services::serialbus::ModbusBusSession::transactBudgetMs(1000)) + 5s;
        const auto msSince = [](Clock::time_point t0) {
            return static_cast<long long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count());
        };
        const auto why = [&] { return svc.lastBusError(PumpId::Sample); };

        // a) Output that never drains.
        gNeverDrains = true;
        auto t0 = Clock::now();
        MIB_EXPECT(!svc.stop(PumpId::Sample), "stop over a non-draining adapter fails");
        long long ms = msSince(t0);
        std::printf("never-drains stop: failed after %lld ms: %s\n", ms, why().c_str());
        MIB_EXPECT(Clock::now() - t0 < bound, "within its bound");
        MIB_EXPECT(why().find("write failed") != std::string::npos && why().find("timed out") != std::string::npos,
                   "the failure names the drain timeout: " + why());
        gNeverDrains = false;

        // b) A write the driver never takes (endless EAGAIN on POSIX).
        gWriteTimesOut = true;
        t0 = Clock::now();
        MIB_EXPECT(!svc.stop(PumpId::Sample), "stop over a full driver queue fails");
        MIB_EXPECT(Clock::now() - t0 < bound, "within its bound");
        MIB_EXPECT(why().find("serial write timed out") != std::string::npos,
                   "the failure names the write timeout: " + why());
        gWriteTimesOut = false;

        // c) A port call that never returns: the I/O thread is held, the command still returns.
        {
            std::scoped_lock lock(gHoldMutex);
            gHoldWrites = true;
        }
        t0 = Clock::now();
        MIB_EXPECT(!svc.stop(PumpId::Sample), "stop with a held I/O thread fails");
        ms = msSince(t0);
        std::printf("held-call stop: failed after %lld ms: %s\n", ms, why().c_str());
        MIB_EXPECT(Clock::now() - t0 < bound, "within its bound, not for ever");
        MIB_EXPECT(why().find("bus timeout") != std::string::npos, "the failure names the timeout: " + why());
        SyringePumpService::RunFacts f;
        MIB_EXPECT(svc.runFacts(PumpId::Sample, 10ms, f), "the pump's lock is free again at once");
        t0 = Clock::now();
        MIB_EXPECT(!svc.stopPurge(PumpId::Sample), "a second command while the call is held fails");
        MIB_EXPECT(msSince(t0) < backend::services::serialbus::ModbusBusSession::transactBudgetMs(1000),
                   "fast, without waiting a budget of its own");
        MIB_EXPECT(why().find("still stuck") != std::string::npos, "and says why: " + why());
        {
            std::scoped_lock lock(gHoldMutex);
            MIB_EXPECT(gHolding, "the I/O thread is still inside write()");
            gHoldWrites = false;
        }
        gHoldCv.notify_all();
        bool stopped = false;
        const auto retryUntil = Clock::now() + 10s;
        while (!stopped && Clock::now() < retryUntil) {
            stopped = svc.stop(PumpId::Sample);
            if (!stopped) std::this_thread::sleep_for(1ms);
        }
        MIB_EXPECT(stopped, "the pump answers again once the call returns");
        MIB_EXPECT(why().empty(), "a success clears the failure");
        svc.disconnect(PumpId::Sample);
    }

    if (mib::test::exitCode() == 0) {
        std::printf("SyringePumpService fake-serial round-trip verified\n");
    }
    return mib::test::exitCode();
}
