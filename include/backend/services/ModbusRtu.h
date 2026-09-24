// Modbus RTU framing primitives, extracted from SyringePumpService so the
// protocol logic (CRC, frame layout, float<->register packing) can be unit
// tested without a serial port. These are pure functions — no device I/O.
//
// Frames are represented as std::vector<uint8_t> so this contract carries no
// Qt dependency (part of the Qt -> React/Tauri backend decoupling, epic #246).
// SyringePumpService converts to/from QByteArray only at the QSerialPort seam.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace backend::services::modbus {

using Frame = std::vector<uint8_t>;

// Standard Modbus function codes.
inline constexpr uint8_t kFuncReadHolding = 0x03;
inline constexpr uint8_t kFuncWriteSingle = 0x06;
inline constexpr uint8_t kFuncWriteMultiple = 0x10;

// CRC-16 with the reflected Modbus polynomial (0xA001).
inline uint16_t crc16(const uint8_t* data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint16_t>(data[i]);
        for (int j = 0; j < 8; ++j) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

inline void appendCrc(Frame& frame)
{
    const uint16_t crc = crc16(frame.data(), frame.size());
    frame.push_back(static_cast<uint8_t>(crc & 0xFF));        // CRC low byte first (RTU)
    frame.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF)); // CRC high byte
}

// float32 -> 2 registers (4 bytes), ABCD big-endian word order: high word
// first. Inverse of registersToFloat.
inline Frame floatToRegisters(float value)
{
    uint8_t bytes[4];
    std::memcpy(bytes, &value, 4);
    return Frame{bytes[3], bytes[2], bytes[1], bytes[0]};
}

// 4 bytes (ABCD big-endian word order) -> float32.
inline float registersToFloat(const uint8_t* data)
{
    uint8_t bytes[4];
    bytes[3] = data[0];
    bytes[2] = data[1];
    bytes[1] = data[2];
    bytes[0] = data[3];
    float value;
    std::memcpy(&value, bytes, 4);
    return value;
}

// FC03 read-holding-registers request: [addr|0x03|startReg(BE)|count(BE)|crc(LE)]
inline Frame buildReadRequest(uint8_t addr, uint16_t startReg, uint16_t count)
{
    Frame frame{
        addr,
        kFuncReadHolding,
        static_cast<uint8_t>((startReg >> 8) & 0xFF),
        static_cast<uint8_t>(startReg & 0xFF),
        static_cast<uint8_t>((count >> 8) & 0xFF),
        static_cast<uint8_t>(count & 0xFF),
    };
    appendCrc(frame);
    return frame;
}

// FC06 write-single-register request: [addr|0x06|reg(BE)|value(BE)|crc(LE)]
inline Frame buildWriteSingleRequest(uint8_t addr, uint16_t reg, uint16_t value)
{
    Frame frame{
        addr,
        kFuncWriteSingle,
        static_cast<uint8_t>((reg >> 8) & 0xFF),
        static_cast<uint8_t>(reg & 0xFF),
        static_cast<uint8_t>((value >> 8) & 0xFF),
        static_cast<uint8_t>(value & 0xFF),
    };
    appendCrc(frame);
    return frame;
}

// FC16 write-multiple-registers request:
// [addr|0x10|startReg(BE)|regCount(BE)|byteCount|data...|crc(LE)]
inline Frame buildWriteMultipleRequest(uint8_t addr, uint16_t startReg,
                                       const Frame& regData)
{
    const uint16_t regCount = static_cast<uint16_t>(regData.size() / 2);
    const uint8_t byteCount = static_cast<uint8_t>(regData.size());
    Frame frame;
    frame.reserve(7 + regData.size() + 2);
    frame.push_back(addr);
    frame.push_back(kFuncWriteMultiple);
    frame.push_back(static_cast<uint8_t>((startReg >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>(startReg & 0xFF));
    frame.push_back(static_cast<uint8_t>((regCount >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>(regCount & 0xFF));
    frame.push_back(byteCount);
    frame.insert(frame.end(), regData.begin(), regData.end());
    appendCrc(frame);
    return frame;
}

// --- Response parsing (pure; testable without a serial port) ---------------

// True if the trailing little-endian CRC matches the body. False for frames
// shorter than 4 bytes (addr+func+crc minimum).
inline bool responseCrcValid(const Frame& resp)
{
    if (resp.size() < 4) return false;
    const size_t bodyLen = resp.size() - 2;
    const uint16_t received = static_cast<uint16_t>(
        (static_cast<uint16_t>(resp[resp.size() - 1]) << 8) |
        static_cast<uint16_t>(resp[resp.size() - 2]));
    return received == crc16(resp.data(), bodyLen);
}

// True if the function-code high bit is set (Modbus exception response).
inline bool isExceptionFrame(const Frame& resp)
{
    return resp.size() >= 2 && (resp[1] & 0x80) != 0;
}

// Validates a read-holding response and extracts the `count` registers' bytes.
// Returns false (out cleared) unless the frame is exactly
// addr+func+byteCount+data+crc long AND the device's byteCount field equals
// count*2 — so callers never index past a short/truncated/garbled frame.
inline bool extractReadData(const Frame& resp, uint16_t count, Frame& out)
{
    out.clear();
    const size_t dataBytes = static_cast<size_t>(count) * 2;
    const size_t expected = 3 + dataBytes + 2; // addr+func+byteCount + data + crc
    if (resp.size() != expected) return false;
    if (resp[2] != static_cast<uint8_t>(dataBytes)) return false;
    out.assign(resp.begin() + 3, resp.begin() + 3 + dataBytes);
    return out.size() == dataBytes;
}

// Expected total length of a response frame once its header bytes are in,
// derived from the function code (and, for FC03, the byte-count field):
//  -1  -> need more bytes before the length is known
//  -2  -> unknown function code, cannot frame the stream
inline int expectedFrameLength(const Frame& partial)
{
    if (partial.size() < 2) return -1;
    const uint8_t func = partial[1];
    if (func & 0x80) return 5; // exception: addr + func|0x80 + code + crc
    switch (func) {
    case kFuncReadHolding:
        if (partial.size() < 3) return -1;
        return 5 + partial[2]; // addr+func+byteCount+data+crc
    case kFuncWriteSingle:
    case kFuncWriteMultiple:
        return 8; // echo frame
    default:
        return -2;
    }
}

// Strict request/response correlation. A response is accepted only when CRC,
// slave address, function code, and the frame's own length fields all match
// the outstanding request — anything else is classified, never guessed at.
enum class ResponseVerdict {
    Ok,            // well-formed reply to this exact request
    Exception,     // well-formed Modbus exception from the addressed device
    TooShort,      // fewer than the 4-byte RTU minimum
    CrcMismatch,   // corrupt frame (possible duplicate-address collision)
    WrongAddress,  // valid frame from a different slave address (stale/delayed)
    WrongFunction, // valid frame from the right device, wrong function code
    Malformed      // right addr+func but length/byte-count/echo fields disagree
};

inline ResponseVerdict classifyResponse(const Frame& request, const Frame& response)
{
    if (request.size() < 2) return ResponseVerdict::Malformed;
    if (response.size() < 4) return ResponseVerdict::TooShort;
    if (!responseCrcValid(response)) return ResponseVerdict::CrcMismatch;
    const uint8_t reqAddr = request[0];
    const uint8_t reqFunc = request[1];
    if (response[0] != reqAddr) return ResponseVerdict::WrongAddress;
    const uint8_t respFunc = response[1];
    if (respFunc == (reqFunc | 0x80)) {
        return response.size() == 5 ? ResponseVerdict::Exception : ResponseVerdict::Malformed;
    }
    if (respFunc != reqFunc) return ResponseVerdict::WrongFunction;
    switch (reqFunc) {
    case kFuncReadHolding: {
        if (request.size() < 6) return ResponseVerdict::Malformed;
        const uint16_t count = static_cast<uint16_t>((request[4] << 8) | request[5]);
        Frame ignored;
        return extractReadData(response, count, ignored) ? ResponseVerdict::Ok
                                                         : ResponseVerdict::Malformed;
    }
    case kFuncWriteSingle:
    case kFuncWriteMultiple: {
        // Echo frames repeat the register address (FC06: + value, FC16: + count).
        if (response.size() != 8 || request.size() < 6) return ResponseVerdict::Malformed;
        if (response[2] != request[2] || response[3] != request[3]) {
            return ResponseVerdict::Malformed;
        }
        return ResponseVerdict::Ok;
    }
    default:
        return ResponseVerdict::Malformed;
    }
}

} // namespace backend::services::modbus
