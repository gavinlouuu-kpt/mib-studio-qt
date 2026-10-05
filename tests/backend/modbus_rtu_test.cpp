// modbus_rtu_test
//
// Protocol-correctness guard for the shared Modbus RTU framing
// (backend::services::modbus) used by the syringe pump, pulse generator and
// ZC300 stage. A wrong CRC, byte order, or frame layout silently breaks every
// device command, so this pins them down with known-answer vectors and
// round-trips.
//
// Frames are Qt-free std::vector<uint8_t> (epic #246 backend decoupling), so
// this test links no Qt.

#include "backend/services/ModbusRtu.h"

#include "support/assert.h"

#include <cstdint>
#include <string>
#include <vector>

namespace m = backend::services::modbus;

namespace {
using Frame = std::vector<uint8_t>;

uint8_t at(const Frame& b, int i) { return b[static_cast<size_t>(i)]; }

// CRC stored low-byte-first (RTU) over the first `bodyLen` bytes must match.
void expectTrailingCrc(const Frame& frame, int bodyLen, const char* what)
{
    MIB_REQUIRE(static_cast<int>(frame.size()) == bodyLen + 2,
                std::string("frame length: ") + what);
    const uint16_t crc = m::crc16(frame.data(), static_cast<size_t>(bodyLen));
    MIB_EXPECT(at(frame, bodyLen) == (crc & 0xFF), std::string("CRC low byte first: ") + what);
    MIB_EXPECT(at(frame, bodyLen + 1) == ((crc >> 8) & 0xFF),
               std::string("CRC high byte second: ") + what);
}
// Build a well-formed read-holding response with a correct trailing CRC.
Frame makeReadResponse(uint8_t addr, uint8_t func, const Frame& data)
{
    Frame f{addr, func, static_cast<uint8_t>(data.size())}; // addr|func|byteCount
    f.insert(f.end(), data.begin(), data.end());
    m::appendCrc(f);
    return f;
}
} // namespace

int main()
{
    // 1) CRC-16/MODBUS canonical check value: "123456789" -> 0x4B37.
    {
        const char* s = "123456789";
        MIB_EXPECT(m::crc16(reinterpret_cast<const uint8_t*>(s), 9) == 0x4B37,
                   "CRC-16/MODBUS check value (0x4B37)");
    }

    // 2) Read request layout: addr|0x03|startReg(BE)|count(BE)|crc(LE).
    {
        const Frame f = m::buildReadRequest(0x01, 0x006A, 0x0002);
        MIB_REQUIRE(f.size() == 8, "read request is 8 bytes");
        MIB_EXPECT(at(f, 0) == 0x01, "addr");
        MIB_EXPECT(at(f, 1) == 0x03, "func read-holding");
        MIB_EXPECT(at(f, 2) == 0x00 && at(f, 3) == 0x6A, "startReg big-endian");
        MIB_EXPECT(at(f, 4) == 0x00 && at(f, 5) == 0x02, "count big-endian");
        expectTrailingCrc(f, 6, "read request");
    }

    // 3) Write-single layout: addr|0x06|reg(BE)|value(BE)|crc(LE).
    {
        const Frame f = m::buildWriteSingleRequest(0x01, 0x0001, 0x0001);
        MIB_REQUIRE(f.size() == 8, "write-single is 8 bytes");
        MIB_EXPECT(at(f, 1) == 0x06, "func write-single");
        MIB_EXPECT(at(f, 2) == 0x00 && at(f, 3) == 0x01, "reg big-endian");
        MIB_EXPECT(at(f, 4) == 0x00 && at(f, 5) == 0x01, "value big-endian");
        expectTrailingCrc(f, 6, "write-single");
    }

    // 4) Write-multiple layout: addr|0x10|startReg(BE)|regCount(BE)|byteCount|data|crc.
    {
        Frame data{0x12, 0x34, 0x56, 0x78};
        const Frame f = m::buildWriteMultipleRequest(0x01, 0x006A, data);
        MIB_REQUIRE(f.size() == 7 + 4 + 2, "write-multiple length = 7+data+2");
        MIB_EXPECT(at(f, 1) == 0x10, "func write-multiple");
        MIB_EXPECT(at(f, 2) == 0x00 && at(f, 3) == 0x6A, "startReg big-endian");
        MIB_EXPECT(at(f, 4) == 0x00 && at(f, 5) == 0x02, "regCount = data/2");
        MIB_EXPECT(at(f, 6) == 0x04, "byteCount = data size");
        MIB_EXPECT(at(f, 7) == 0x12 && at(f, 10) == 0x78, "payload preserved");
        expectTrailingCrc(f, 11, "write-multiple");
    }

    // 5) Float <-> registers: ABCD big-endian word order, and round-trip.
    {
        // 1.0f == 0x3F800000; ABCD (high word first) -> 3F 80 00 00.
        const Frame r = m::floatToRegisters(1.0f);
        MIB_REQUIRE(r.size() == 4, "float packs to 4 bytes");
        MIB_EXPECT(at(r, 0) == 0x3F && at(r, 1) == 0x80 && at(r, 2) == 0x00 && at(r, 3) == 0x00,
                   "1.0f packs ABCD as 3F 80 00 00");

        for (float v : {0.0f, 1.0f, -1.5f, 3.14159f, 9999.0f, 0.001f}) {
            const Frame packed = m::floatToRegisters(v);
            const float back = m::registersToFloat(packed.data());
            MIB_EXPECT(back == v, "float register round-trip is exact");
        }
    }

    // 6) Response parsing: CRC validation, exception detection, and bounds-safe
    //    register extraction. The extraction guards against the short/garbled
    //    frame that previously caused an out-of-bounds read in pollStatus.
    {
        Frame data{0xAA, 0xBB, 0xCC, 0xDD};
        const Frame good = makeReadResponse(0x01, 0x03, data); // count=2

        MIB_EXPECT(m::responseCrcValid(good), "valid response passes CRC");
        MIB_EXPECT(!m::isExceptionFrame(good), "normal response is not an exception");

        Frame out;
        MIB_EXPECT(m::extractReadData(good, 2, out) && out.size() == 4 &&
                       out[0] == 0xAA && out[3] == 0xDD,
                   "extractReadData returns the 2 registers");

        // Corrupted CRC -> rejected.
        Frame badCrc = good;
        badCrc[badCrc.size() - 1] ^= 0xFF;
        MIB_EXPECT(!m::responseCrcValid(badCrc), "corrupted CRC is rejected");

        // Too-short frame (e.g. exception-length) for a count=2 read -> rejected,
        // out cleared (this is the case the old mid() path mishandled).
        Frame shortFrame = makeReadResponse(0x01, 0x03, Frame(2, 0)); // claims 1 reg
        MIB_EXPECT(!m::extractReadData(shortFrame, 2, out) && out.empty(),
                   "short/mismatched-length read response is rejected");

        // byteCount field that disagrees with the actual payload -> rejected.
        Frame wrongByteCount = good;
        wrongByteCount[2] = 0x02; // says 2 data bytes but carries 4
        MIB_EXPECT(!m::extractReadData(wrongByteCount, 2, out),
                   "byteCount/length mismatch is rejected");

        // Exception frame: addr|func|0x80|code|crc.
        Frame exc{0x01, 0x83, 0x02}; // 0x03 | 0x80, illegal data address
        m::appendCrc(exc);
        MIB_EXPECT(m::responseCrcValid(exc), "exception frame CRC is valid");
        MIB_EXPECT(m::isExceptionFrame(exc), "exception frame detected");

        // Runt frames never index out of bounds.
        MIB_EXPECT(!m::responseCrcValid(Frame(1, 0)), "1-byte frame rejected");
        MIB_EXPECT(!m::extractReadData(Frame{}, 2, out), "empty frame rejected");
    }

    // 7) FC04 read-input-registers (#464). Known-answer frames are the ZC300
    //    vendor manual's examples (ZC300-ModbusRTU V1.15, CRCs checked).
    {
        const Frame model = m::buildReadInputRequest(0x01, 0x7530, 0x0007); // 30001..7
        MIB_EXPECT(model == Frame({0x01, 0x04, 0x75, 0x30, 0x00, 0x07, 0xAB, 0xCB}),
                   "FC04 model read matches the vendor frame 01 04 75 30 00 07 AB CB");
        const Frame io = m::buildReadInputRequest(0x01, 0x753A, 0x0001); // 30011
        MIB_EXPECT(io == Frame({0x01, 0x04, 0x75, 0x3A, 0x00, 0x01, 0x0B, 0xCB}),
                   "FC04 IO-status read matches the vendor frame");
        MIB_EXPECT(m::buildReadRequest(0x01, 0x7570, 0x0001) ==
                       Frame({0x01, 0x03, 0x75, 0x70, 0x00, 0x01, 0x9F, 0xDD}),
                   "FC03 is unchanged by the shared builder (vendor frame)");

        // Length prediction frames FC04 from its byte-count field.
        MIB_EXPECT(m::expectedFrameLength(Frame{0x01, 0x04}) == -1,
                   "FC04 needs the byte-count byte before its length is known");
        MIB_EXPECT(m::expectedFrameLength(Frame{0x01, 0x04, 0x0E}) == 19,
                   "FC04 length = 5 + byteCount");
        MIB_EXPECT(m::expectedFrameLength(Frame{0x01, 0x84}) == 5,
                   "FC04 exception frame is 5 bytes");

        // Vendor model reply: "ZC300-3S257RIC" (7 registers).
        const Frame reply{0x01, 0x04, 0x0E, 0x5A, 0x43, 0x33, 0x30, 0x30, 0x2D, 0x33,
                          0x53, 0x32, 0x35, 0x37, 0x52, 0x49, 0x43, 0x07, 0x42};
        MIB_EXPECT(m::classifyResponse(model, reply) == m::ResponseVerdict::Ok,
                   "FC04 reply to an FC04 request correlates");
        Frame out;
        MIB_REQUIRE(m::extractReadData(reply, 7, out), "FC04 payload extracts");
        MIB_EXPECT(std::string(out.begin(), out.end()) == "ZC300-3S257RIC",
                   "FC04 payload is the model string");

        // Wrong count, wrong function, and the vendor FC04 exception example.
        MIB_EXPECT(m::classifyResponse(io, reply) == m::ResponseVerdict::Malformed,
                   "FC04 reply with the wrong register count is Malformed");
        const Frame holdingShaped = makeReadResponse(0x01, 0x03, Frame(14, 0x00));
        MIB_EXPECT(m::classifyResponse(model, holdingShaped) ==
                       m::ResponseVerdict::WrongFunction,
                   "an FC03 answer to an FC04 request is WrongFunction");
        const Frame exception{0x01, 0x84, 0x02, 0xC2, 0xC1}; // illegal data address
        MIB_EXPECT(m::classifyResponse(model, exception) == m::ResponseVerdict::Exception,
                   "FC04 exception (0x84) correlates as Exception");
    }

    if (mib::test::exitCode() == 0) {
        std::printf("Modbus RTU framing/CRC/float/response-parse verified\n");
    }
    return mib::test::exitCode();
}
