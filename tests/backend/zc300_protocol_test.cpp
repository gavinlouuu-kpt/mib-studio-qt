// zc300_protocol_test
//
// Pure-function guard for the ZC300 Modbus protocol layer (#464): the
// register map, frame builders, decoding, exception mapping, configuration
// matching and micrometre encoding. Known-answer frames are the vendor
// manual's examples (ZC300-ModbusRTU V1.15; every CRC checked).

#include "backend/stage/zc300/Zc300Protocol.h"

#include "support/assert.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace z = backend::stage::zc300;
using backend::stage::StageError;
using Frame = std::vector<uint8_t>;

namespace {
Frame hex(std::initializer_list<int> bytes)
{
    Frame f;
    for (int b : bytes) f.push_back(static_cast<uint8_t>(b));
    return f;
}
} // namespace

int main()
{
    // 1) Register addressing: wire = number - 1; FC04 below 30050, FC03 above.
    {
        MIB_EXPECT(z::wireAddress(30050) == 0x7561, "30050 -> 0x7561");
        MIB_EXPECT(z::isInputRegister(30049) && !z::isInputRegister(30050), "input/holding boundary");
        MIB_EXPECT(z::reg32(z::kRegLead, 2) == 30088, "Z lead at 30088 (the manual's '30089..89' is a typo)");
        MIB_EXPECT(z::buildRead(1, z::kRegModel, 7) == hex({0x01, 0x04, 0x75, 0x30, 0x00, 0x07, 0xAB, 0xCB}),
                   "model read is FC04 (vendor frame)");
        MIB_EXPECT(z::buildRead(1, z::kRegSpeed, 2) == hex({0x01, 0x03, 0x75, 0xB0, 0x00, 0x02, 0xDF, 0xE0}),
                   "speed read is FC03 (vendor frame)");
        MIB_EXPECT(z::buildRead(1, z::kRegUnit, 3) == hex({0x01, 0x03, 0x75, 0x77, 0x00, 0x03, 0xAF, 0xDD}),
                   "unit read (vendor frame)");
    }

    // 2) Opcode and write frames match the vendor examples byte for byte.
    {
        MIB_EXPECT(z::buildOpcode(1, z::Opcode::MoveRelative, 2, z::axisCode(0), z::kDirPositive) ==
                       hex({0x01, 0x10, 0x75, 0x61, 0x00, 0x03, 0x06, 0x00, 0x65, 0x00, 0x31, 0x00, 0x50, 0x12, 0x0D}),
                   "relative move X+ (vendor frame)");
        MIB_EXPECT(z::buildOpcode(1, z::Opcode::Jog, 2, z::axisCode(0), z::kDirPositive) ==
                       hex({0x01, 0x10, 0x75, 0x61, 0x00, 0x03, 0x06, 0x00, 0x66, 0x00, 0x31, 0x00, 0x50, 0x56, 0x0D}),
                   "continuous move X+ (vendor frame)");
        MIB_EXPECT(z::buildOpcode(1, z::Opcode::Stop, 1, z::axisCode(0)) ==
                       hex({0x01, 0x10, 0x75, 0x61, 0x00, 0x02, 0x04, 0x00, 0x68, 0x00, 0x31, 0x2E, 0xD1}),
                   "immediate stop X (vendor frame)");
        MIB_EXPECT(z::buildOpcode(1, z::Opcode::Save) ==
                       hex({0x01, 0x10, 0x75, 0x61, 0x00, 0x01, 0x02, 0x00, 0x6D, 0x4B, 0x0B}),
                   "save parameters (vendor frame)");
        MIB_EXPECT(z::buildWriteFloat(1, z::kRegSpeed, 15.0f) ==
                       hex({0x01, 0x10, 0x75, 0xB0, 0x00, 0x02, 0x04, 0x41, 0x70, 0x00, 0x00, 0xB7, 0xAE}),
                   "speed = 15 float write (vendor frame)");
        MIB_EXPECT(z::buildWriteU16(1, z::kRegUnit, 1) ==
                       hex({0x01, 0x10, 0x75, 0x77, 0x00, 0x01, 0x02, 0x00, 0x01, 0x49, 0xD0}),
                   "unit = mm (vendor frame)");
        MIB_EXPECT(z::buildWriteLong(1, z::kRegPulsesPerRev, 1600) ==
                       hex({0x01, 0x10, 0x75, 0x7A, 0x00, 0x02, 0x04, 0x00, 0x00, 0x06, 0x40, 0x2C, 0x36}),
                   "pulses/rev = 1600 LONG write (vendor frame)");
        MIB_EXPECT(z::buildWriteFloat(1, z::kRegStepDistance, 5.0f) ==
                       hex({0x01, 0x10, 0x75, 0xA1, 0x00, 0x02, 0x04, 0x40, 0xA0, 0x00, 0x00, 0x77, 0x6B}),
                   "step distance = 5 (vendor frame)");
    }

    // 3) Decoding.
    {
        const Frame serial = hex({0x01, 0x34, 0x63, 0xA9});
        MIB_EXPECT(z::decodeLong(serial.data()) == 20210601, "LONG serial decodes (vendor example)");
        const Frame model = hex({0x5A, 0x43, 0x33, 0x30, 0x30, 0x2D, 0x31, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
        MIB_EXPECT(z::decodeAscii(model) == "ZC300-1A", "model string trims NUL padding");
        const Frame f15 = hex({0x41, 0x70, 0x00, 0x00});
        MIB_EXPECT(z::decodeFloat(f15.data()) == 15.0f, "float decodes ABCD");

        const auto none = z::decodeSwitches(0x0120, 0); // bench unit: Y/Z home floating
        MIB_EXPECT(!none.limitPositive && !none.limitNegative && !none.home && !none.emergencyStop,
                   "X clear while floating Y/Z home bits are set");
        const auto y = z::decodeSwitches(0x0120, 1);
        MIB_EXPECT(y.home && !y.limitPositive, "Y home bit is bit 5");
        const auto x = z::decodeSwitches(0x0603, 0);
        MIB_EXPECT(x.limitPositive && x.limitNegative && x.emergencyStop && x.driverAlarm,
                   "X limits, e-stop (bit 9) and X alarm (bit 10)");
    }

    // 4) Exception codes map to stage errors.
    {
        MIB_EXPECT(z::errorFromException(0x06) == StageError::Busy, "06 -> Busy");
        MIB_EXPECT(z::errorFromException(0x07) == StageError::LimitSwitch, "07 -> LimitSwitch");
        MIB_EXPECT(z::errorFromException(0x08) == StageError::EmergencyStop, "08 -> EmergencyStop");
        MIB_EXPECT(z::errorFromException(0x09) == StageError::NotEnabled, "09 -> NotEnabled");
        MIB_EXPECT(z::errorFromException(0x04) == StageError::Busy, "04 save failed -> Busy");
        MIB_EXPECT(z::errorFromException(0x02) == StageError::Protocol, "02 -> Protocol");
    }

    // 5) Configuration matching and calibration for the TBZF6-60.
    {
        const auto profile = backend::stage::tbzf6_60Profile();
        z::ControllerConfig good{z::Unit::Millimetres, z::StageType::Linear, 0.7f, 1600};
        MIB_EXPECT(z::configMatches(good, profile), "saved bench configuration matches");
        MIB_EXPECT(std::abs(z::calibrationFor(good).umPerPulse - 0.4375) < 1e-6, "0.4375 um/pulse");
        MIB_EXPECT(z::calibrationFor(good).commandQuantumUm == 1.0, "1 um command quantum");

        z::ControllerConfig factory{z::Unit::Pulses, z::StageType::Linear, 4.0f, 1600};
        MIB_EXPECT(!z::configMatches(factory, profile), "factory configuration is refused");
        z::ControllerConfig wrongPulses = good;
        wrongPulses.pulsesPerRev = 3200;
        MIB_EXPECT(!z::configMatches(wrongPulses, profile), "pulses/rev mismatch is refused");
        z::ControllerConfig rotary = good;
        rotary.stageType = z::StageType::Rotary;
        MIB_EXPECT(!z::configMatches(rotary, profile), "rotary stage type is refused");
    }

    // 6) Micrometre encoding: whole micrometres only, with the truncation guard.
    {
        MIB_EXPECT(!z::encodeMicronsAsMm(12.5), "12.5 um is OffGrid");
        MIB_EXPECT(!z::encodeMicronsAsMm(std::nan("")), "NaN is rejected");
        // The controller truncates the float to 0.001 mm: the guard must keep
        // every whole micrometre from 0 to 6000 exact after float32 rounding.
        bool allExact = true;
        for (int um = 0; um <= 6000; ++um) {
            const float mm = *z::encodeMicronsAsMm(um);
            const double truncated = std::trunc(static_cast<double>(mm) * 1000.0);
            if (static_cast<int>(truncated) != um) allExact = false;
        }
        MIB_EXPECT(allExact, "0..6000 um survive float32 + 0.001 mm truncation exactly");
        MIB_EXPECT(*z::encodeMicronsAsMm(-100.0) == *z::encodeMicronsAsMm(100.0),
                   "encoding is a magnitude; the direction is sent separately");

        z::ControllerConfig mm{z::Unit::Millimetres, z::StageType::Linear, 0.7f, 1600};
        MIB_EXPECT(std::abs(z::positionToMicrons(0.1001875f, mm) - 100.1875) < 1e-3,
                   "mm readback converts to um (229 pulses = 100.19 um)");
        z::ControllerConfig pp{z::Unit::Pulses, z::StageType::Linear, 0.7f, 1600};
        MIB_EXPECT(std::abs(z::positionToMicrons(229.0f, pp) - 100.1875) < 1e-3,
                   "pulse readback converts through the calibration");
    }

    if (mib::test::exitCode() == 0) std::printf("ZC300 protocol verified\n");
    return mib::test::exitCode();
}
