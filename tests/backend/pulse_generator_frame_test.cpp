// pulse_generator_frame_test
//
// Protocol-correctness guard for PulseGeneratorService's Modbus frames against
// the Zhongsheng pulse-output module. The known-answer vectors below are the
// worked examples printed in the vendor manual (脉冲频率与占空比输出系列使用手册
// V2.0 §2.3), so a wrong register address, word order, or scale factor fails
// against the manufacturer's own numbers.

#include "backend/services/PulseGeneratorService.h"

#include "support/assert.h"

#include <QByteArray>

#include <cstdint>

using backend::services::PulseGeneratorService;

namespace {
uint8_t at(const QByteArray& b, int i) { return static_cast<uint8_t>(b.at(i)); }

bool frameEquals(const QByteArray& frame, std::initializer_list<uint8_t> expected)
{
    if (frame.size() != static_cast<int>(expected.size())) return false;
    int i = 0;
    for (uint8_t byte : expected) {
        if (at(frame, i++) != byte) return false;
    }
    return true;
}
} // namespace

int main()
{
    // 1) Manual §2.3: "控制第一通道输出 1000Hz" — FC16, ch1 (registers 0x0000
    //    and 0x0001), value 100000 = 0x000186A0, CRC 0xC0 0x77.
    {
        const QByteArray f = PulseGeneratorService::buildFrequencyFrame(0x01, 0, 1000.0);
        MIB_EXPECT(frameEquals(f, {0x01, 0x10, 0x00, 0x00, 0x00, 0x02, 0x04,
                                   0x00, 0x01, 0x86, 0xA0, 0xC0, 0x77}),
                   "manual example: ch1 1000 Hz FC16 frame");
    }

    // 2) Manual §2.3: "控制第一通道输出 50% 占空比" — FC06, register 0x0002,
    //    value 5000 = 0x1388, CRC 0x25 0x5C.
    {
        const QByteArray f = PulseGeneratorService::buildDutyFrame(0x01, 0, 50.0);
        MIB_EXPECT(frameEquals(f, {0x01, 0x06, 0x00, 0x02, 0x13, 0x88, 0x25, 0x5C}),
                   "manual example: ch1 50% duty FC06 frame");
    }

    // 3) Register layout per channel: freq at 3N/3N+1, duty at 3N+2.
    {
        const QByteArray f4 = PulseGeneratorService::buildFrequencyFrame(0x01, 3, 1000.0);
        MIB_EXPECT(at(f4, 2) == 0x00 && at(f4, 3) == 0x09, "ch4 frequency starts at register 0x0009");
        const QByteArray d2 = PulseGeneratorService::buildDutyFrame(0x01, 1, 50.0);
        MIB_EXPECT(at(d2, 2) == 0x00 && at(d2, 3) == 0x05, "ch2 duty at register 0x0005");
    }

    // 4) u32 word split: high word in the low-address register (manual §2.2:
    //    1234567.89 Hz -> 123456789 = 0x075BCD15, high 0x075B first).
    //    40 kHz cap makes that raw example unreachable, so verify at 40 kHz:
    //    4000000 = 0x003D0900.
    {
        const QByteArray f = PulseGeneratorService::buildFrequencyFrame(0x01, 0, 40000.0);
        MIB_EXPECT(at(f, 7) == 0x00 && at(f, 8) == 0x3D, "high word first");
        MIB_EXPECT(at(f, 9) == 0x09 && at(f, 10) == 0x00, "low word second");
    }

    // 5) Scale factors: register value = real value * 100.
    {
        MIB_EXPECT(PulseGeneratorService::frequencyToRegisterValue(400.0) == 40000,
                   "400 Hz -> 40000");
        MIB_EXPECT(PulseGeneratorService::dutyToRegisterValue(50.23) == 5023,
                   "manual example: 50.23% -> 5023");
        MIB_EXPECT(PulseGeneratorService::dutyToRegisterValue(100.0) == 10000,
                   "100% -> 10000");
    }

    // 6) Clamping to the module's documented ranges.
    {
        MIB_EXPECT(PulseGeneratorService::clampFrequency(100.0) == 400.0,
                   "frequency below module minimum clamps to 400 Hz");
        MIB_EXPECT(PulseGeneratorService::clampFrequency(50000.0) == 40000.0,
                   "frequency above module maximum clamps to 40 kHz");
        MIB_EXPECT(PulseGeneratorService::clampDuty(-1.0) == 0.0, "duty floor 0%");
        MIB_EXPECT(PulseGeneratorService::clampDuty(150.0) == 100.0, "duty ceiling 100%");
        MIB_EXPECT(PulseGeneratorService::frequencyToRegisterValue(50000.0) == 4000000,
                   "clamp applies before encoding");
    }

    return mib::test::exitCode();
}
