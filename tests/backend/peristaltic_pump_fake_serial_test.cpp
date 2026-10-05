// peristaltic_pump_fake_serial_test
//
// The Tushui peristaltic pump in a SyringePumpService slot, against a fake
// Modbus slave holding registers 100-107 (TushuiPumpProtocol.h):
//  - protocol conversions match the vendor document's worked examples;
//  - connect only reads (no FC06/FC10 reaches the pump) and adopts the
//    pump's setpoints, converted with the µL/rev calibration;
//  - flow rate -> head speed (0.4 rpm = 10 µL/min at 25 µL/rev); rates the
//    pump cannot run fail instead of clamping;
//  - start sets continuous turns before run; purge runs at the purge speed
//    and stop restores the flow speed and direction;
//  - poll maps run state and direction, integrates delivered volume;
//  - disconnect stops a running pump.

#include "backend/services/SerialBus.h"
#include "backend/services/SyringePumpService.h"
#include "backend/services/TushuiPumpProtocol.h"

#include "support/assert.h"
#include "support/fake_modbus_bench.h"
#include "support/watchdog.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

namespace tushui = backend::services::tushui;
using backend::services::SyringePumpService;
using PumpId = SyringePumpService::PumpId;
using PumpModel = SyringePumpService::PumpModel;

namespace {

constexpr const char* kPort = "/dev/ttyPS1";

bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

// The state read from the instrument's pump on 2026-10-04.
void makeTushuiLike(mib::test::FakeModbusWire& wire)
{
    wire.address = 3;
    wire.reg(100) = 20000; // 200.00 rpm
    wire.reg(101) = 1;     // CCW
    wire.reg(102) = 0;
    wire.reg(103) = 1000;  // 1.000 turn
    wire.reg(104) = 0;     // stopped
    wire.reg(105) = 0x0001;
    wire.reg(106) = 0xC200; // 115200
    wire.reg(107) = 3;
}

void protocolMatchesVendorExamples()
{
    MIB_EXPECT(tushui::rpmToRegister(12.34) == 1234, "12.34 rpm -> 1234");
    MIB_EXPECT(tushui::rpmToRegister(0.0) == 1, "speed clamps to 0.01 rpm");
    MIB_EXPECT(tushui::rpmToRegister(900.0) == 50000, "speed clamps to 500 rpm");
    const auto turns = tushui::turnsPayload(100.0);
    MIB_EXPECT((turns == std::vector<uint8_t>{0x00, 0x01, 0x86, 0xA0}), "100 turns -> 0x000186A0");
    MIB_EXPECT((tushui::turnsPayload(9999.999) == std::vector<uint8_t>{0x00, 0x98, 0x96, 0x7F}),
               "9999.999 turns -> 0x0098967F");
    MIB_EXPECT(near(tushui::flowToRpm(10.0, tushui::kDefaultMicrolitersPerRev), 0.4),
               "10 µL/min at the default calibration is 0.4 rpm");

    // Doc reply 01 03 10 ... for registers 100-107.
    const std::vector<uint8_t> regs = {0x04, 0xD2, 0x00, 0x01, 0x00, 0x01, 0x86, 0xA0,
                                       0x00, 0x04, 0x00, 0x01, 0xC2, 0x00, 0x00, 0x07};
    tushui::Status st;
    MIB_REQUIRE(tushui::decodeStatus(regs, st), "16 bytes decode");
    MIB_EXPECT(near(st.speedRpm, 12.34), "speed 12.34 rpm");
    MIB_EXPECT(st.direction == tushui::kCounterClockwise, "direction CCW");
    MIB_EXPECT(near(st.turns, 100.0), "turns 100.000");
    MIB_EXPECT(st.runState == tushui::RunState::Paused, "run state 4 = paused");
    MIB_EXPECT(st.baudRate == 115200, "baud 115200");
    MIB_EXPECT(st.slaveId == 7, "slave id");
    MIB_EXPECT(!tushui::decodeStatus(std::vector<uint8_t>(15, 0), st), "short data rejected");
}

} // namespace

int main()
{
    mib::test::Watchdog watchdog(30);
    protocolMatchesVendorExamples();

    mib::test::FakeModbusBench bench;
    mib::test::FakeModbusWire wire;
    makeTushuiLike(wire);
    bench.attach(kPort, &wire);
    backend::services::serialbus::SerialBusManager bus;
    bus.setSerialPortFactory([&] { return std::make_unique<mib::test::FakeModbusPort>(bench); });
    SyringePumpService svc(bus);

    // Connect reads registers 100-107 and writes nothing.
    MIB_REQUIRE(svc.connect(PumpId::Sample, kPort, 115200, 3, PumpModel::TushuiPeristaltic, 25.0),
                "peristaltic pump connects at address 3");
    watchdog.mark("connected");
    MIB_EXPECT(wire.writeCommands == 0, "connect sends no write command");
    {
        const auto cfg = svc.getConfig(PumpId::Sample);
        const auto st = svc.getStatus(PumpId::Sample);
        MIB_EXPECT(cfg.model == PumpModel::TushuiPeristaltic, "slot remembers the model");
        MIB_EXPECT(near(cfg.flowRate, 5000.0) && cfg.flowRateUnit == 100,
                   "200 rpm at 25 µL/rev reads back as 5000 µL/min");
        MIB_EXPECT(cfg.direction == SyringePumpService::Direction::Infuse,
                   "CCW maps to Infuse (PZ7035 bench)");
        MIB_EXPECT(near(st.speedRpm, 200.0), "speed setpoint adopted");
        MIB_EXPECT(near(st.minFlowRate, 0.25) && near(st.maxFlowRate, 12500.0),
                   "flow limits follow the calibration");
        MIB_EXPECT(st.runStatus == SyringePumpService::RunStatus::Stop, "pump is stopped");
    }

    // Flow rate -> head speed.
    MIB_EXPECT(svc.setFlowRate(PumpId::Sample, 10.0, 100), "10 µL/min accepted");
    MIB_EXPECT(wire.read(100) == 40, "10 µL/min writes 0.40 rpm");
    MIB_EXPECT(svc.setFlowRate(PumpId::Sample, 1.0, 103), "1 mL/min accepted");
    MIB_EXPECT(wire.read(100) == 4000, "1 mL/min writes 40.00 rpm");
    MIB_EXPECT(!svc.setFlowRate(PumpId::Sample, 0.0, 100), "zero flow rejected");
    MIB_EXPECT(!svc.setFlowRate(PumpId::Sample, 20.0, 103), "20 mL/min (800 rpm) rejected");
    MIB_EXPECT(!svc.setFlowRate(PumpId::Sample, 10.0, 7), "unknown unit rejected");
    MIB_EXPECT(wire.read(100) == 4000, "rejected rates leave the speed alone");
    MIB_EXPECT(svc.setFlowRate(PumpId::Sample, 10.0, 100), "back to 10 µL/min");
    MIB_EXPECT(svc.setDirection(PumpId::Sample, SyringePumpService::Direction::Infuse),
               "direction accepted");
    MIB_EXPECT(wire.read(101) == tushui::kCounterClockwise, "Infuse turns counter-clockwise (PZ7035 bench)");
    MIB_EXPECT(!svc.setSyringeVolume(PumpId::Sample, 10, 100), "syringe volume does not apply");

    // Start: continuous turns, then run.
    MIB_REQUIRE(svc.start(PumpId::Sample), "start accepted");
    MIB_EXPECT(wire.read(102) == 0 && wire.read(103) == 0, "start clears the turn count");
    MIB_EXPECT(wire.read(104) == 1, "start runs the pump");
    MIB_EXPECT(wire.read(100) == 40, "start keeps the flow speed");
    svc.pollStatus(PumpId::Sample);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    svc.pollStatus(PumpId::Sample);
    watchdog.mark("polled while running");
    {
        const auto st = svc.getStatus(PumpId::Sample);
        MIB_EXPECT(st.runStatus == SyringePumpService::RunStatus::Forward, "running counter-clockwise = Forward");
        MIB_EXPECT(near(st.currentFlowRate, 10.0), "live flow 10 µL/min");
        MIB_EXPECT(st.accumulatedVolume > 0.0 && st.accumulatedVolume < 0.1,
                   "volume integrates between polls (µL)");
    }
    MIB_EXPECT(svc.stop(PumpId::Sample), "stop accepted");
    MIB_EXPECT(wire.read(104) == 0, "stop writes run state 0");

    // Purge: fixed purge speed, stop restores the flow setpoint and direction.
    MIB_REQUIRE(svc.purge(PumpId::Sample, SyringePumpService::Direction::Withdraw), "purge accepted");
    MIB_EXPECT(wire.read(100) == tushui::rpmToRegister(tushui::kPurgeRpm), "purge runs at purge speed");
    MIB_EXPECT(wire.read(101) == tushui::kClockwise && wire.read(104) == 1,
               "purge withdraw runs clockwise");
    svc.pollStatus(PumpId::Sample);
    MIB_EXPECT(svc.getStatus(PumpId::Sample).runStatus == SyringePumpService::RunStatus::Backward,
               "purge withdraw reads as Backward");
    MIB_EXPECT(near(svc.getStatus(PumpId::Sample).speedRpm, 0.4), "purge keeps the flow setpoint");
    MIB_EXPECT(svc.stop(PumpId::Sample), "stop during purge accepted");
    MIB_EXPECT(wire.read(104) == 0, "purge stopped");
    MIB_EXPECT(wire.read(100) == 40, "flow speed restored after purge");
    MIB_EXPECT(wire.read(101) == tushui::kCounterClockwise, "direction restored after purge");

    // Disconnect stops a running pump.
    MIB_REQUIRE(svc.start(PumpId::Sample), "restart");
    svc.disconnect(PumpId::Sample);
    MIB_EXPECT(wire.read(104) == 0, "disconnect stops the pump");
    MIB_EXPECT(!svc.isConnected(PumpId::Sample), "disconnected");
    watchdog.mark("disconnected");

    // A silent address fails to connect and leaves the slot disconnected.
    MIB_EXPECT(!svc.connect(PumpId::Sheath, kPort, 115200, 1, PumpModel::TushuiPeristaltic, 25.0),
               "wrong address does not connect");
    MIB_EXPECT(!svc.isConnected(PumpId::Sheath), "sheath slot stays disconnected");

    if (mib::test::exitCode() == 0) {
        std::printf("Tushui peristaltic pump fake-serial round-trip verified\n");
    }
    return mib::test::exitCode();
}
