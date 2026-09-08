// Modbus RTU framing primitives, extracted from SyringePumpService so the
// protocol logic (CRC, frame layout, float<->register packing) can be unit
// tested without a serial port. These are pure functions — no device I/O.
#pragma once

#include <QByteArray>

#include <cstdint>
#include <cstring>

namespace backend::services::modbus {

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

inline void appendCrc(QByteArray& frame)
{
    const uint16_t crc =
        crc16(reinterpret_cast<const uint8_t*>(frame.constData()),
              static_cast<size_t>(frame.size()));
    frame.append(static_cast<char>(crc & 0xFF));        // CRC low byte first (RTU)
    frame.append(static_cast<char>((crc >> 8) & 0xFF)); // CRC high byte
}

// float32 -> 2 registers (4 bytes), ABCD big-endian word order: high word
// first. Inverse of registersToFloat.
inline QByteArray floatToRegisters(float value)
{
    QByteArray result(4, 0);
    uint8_t bytes[4];
    std::memcpy(bytes, &value, 4);
    result[0] = static_cast<char>(bytes[3]);
    result[1] = static_cast<char>(bytes[2]);
    result[2] = static_cast<char>(bytes[1]);
    result[3] = static_cast<char>(bytes[0]);
    return result;
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
inline QByteArray buildReadRequest(uint8_t addr, uint16_t startReg, uint16_t count)
{
    QByteArray frame(6, 0);
    frame[0] = static_cast<char>(addr);
    frame[1] = static_cast<char>(kFuncReadHolding);
    frame[2] = static_cast<char>((startReg >> 8) & 0xFF);
    frame[3] = static_cast<char>(startReg & 0xFF);
    frame[4] = static_cast<char>((count >> 8) & 0xFF);
    frame[5] = static_cast<char>(count & 0xFF);
    appendCrc(frame);
    return frame;
}

// FC06 write-single-register request: [addr|0x06|reg(BE)|value(BE)|crc(LE)]
inline QByteArray buildWriteSingleRequest(uint8_t addr, uint16_t reg, uint16_t value)
{
    QByteArray frame(6, 0);
    frame[0] = static_cast<char>(addr);
    frame[1] = static_cast<char>(kFuncWriteSingle);
    frame[2] = static_cast<char>((reg >> 8) & 0xFF);
    frame[3] = static_cast<char>(reg & 0xFF);
    frame[4] = static_cast<char>((value >> 8) & 0xFF);
    frame[5] = static_cast<char>(value & 0xFF);
    appendCrc(frame);
    return frame;
}

// FC16 write-multiple-registers request:
// [addr|0x10|startReg(BE)|regCount(BE)|byteCount|data...|crc(LE)]
inline QByteArray buildWriteMultipleRequest(uint8_t addr, uint16_t startReg,
                                            const QByteArray& regData)
{
    const uint16_t regCount = static_cast<uint16_t>(regData.size() / 2);
    const uint8_t byteCount = static_cast<uint8_t>(regData.size());
    QByteArray frame;
    frame.reserve(7 + regData.size() + 2);
    frame.append(static_cast<char>(addr));
    frame.append(static_cast<char>(kFuncWriteMultiple));
    frame.append(static_cast<char>((startReg >> 8) & 0xFF));
    frame.append(static_cast<char>(startReg & 0xFF));
    frame.append(static_cast<char>((regCount >> 8) & 0xFF));
    frame.append(static_cast<char>(regCount & 0xFF));
    frame.append(static_cast<char>(byteCount));
    frame.append(regData);
    appendCrc(frame);
    return frame;
}

// --- Response parsing (pure; testable without a serial port) ---------------

// True if the trailing little-endian CRC matches the body. False for frames
// shorter than 4 bytes (addr+func+crc minimum).
inline bool responseCrcValid(const QByteArray& resp)
{
    if (resp.size() < 4) return false;
    const size_t bodyLen = static_cast<size_t>(resp.size()) - 2;
    const uint16_t received = static_cast<uint16_t>(
        (static_cast<uint8_t>(resp[resp.size() - 1]) << 8) |
        static_cast<uint8_t>(resp[resp.size() - 2]));
    return received == crc16(reinterpret_cast<const uint8_t*>(resp.constData()), bodyLen);
}

// True if the function-code high bit is set (Modbus exception response).
inline bool isExceptionFrame(const QByteArray& resp)
{
    return resp.size() >= 2 && (static_cast<uint8_t>(resp[1]) & 0x80) != 0;
}

// Validates a read-holding response and extracts the `count` registers' bytes.
// Returns false (out cleared) unless the frame is exactly
// addr+func+byteCount+data+crc long AND the device's byteCount field equals
// count*2 — so callers never index past a short/truncated/garbled frame.
inline bool extractReadData(const QByteArray& resp, uint16_t count, QByteArray& out)
{
    out.clear();
    const int dataBytes = static_cast<int>(count) * 2;
    const int expected = 3 + dataBytes + 2; // addr+func+byteCount + data + crc
    if (resp.size() != expected) return false;
    if (static_cast<uint8_t>(resp[2]) != static_cast<uint8_t>(dataBytes)) return false;
    out = resp.mid(3, dataBytes);
    return out.size() == dataBytes;
}

// Expected total length of a response frame once its header bytes are in,
// derived from the function code (and, for FC03, the byte-count field):
//  -1  -> need more bytes before the length is known
//  -2  -> unknown function code, cannot frame the stream
inline int expectedFrameLength(const QByteArray& partial)
{
    if (partial.size() < 2) return -1;
    const uint8_t func = static_cast<uint8_t>(partial[1]);
    if (func & 0x80) return 5; // exception: addr + func|0x80 + code + crc
    switch (func) {
    case kFuncReadHolding:
        if (partial.size() < 3) return -1;
        return 5 + static_cast<uint8_t>(partial[2]); // addr+func+byteCount+data+crc
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

inline ResponseVerdict classifyResponse(const QByteArray& request, const QByteArray& response)
{
    if (request.size() < 2) return ResponseVerdict::Malformed;
    if (response.size() < 4) return ResponseVerdict::TooShort;
    if (!responseCrcValid(response)) return ResponseVerdict::CrcMismatch;
    const uint8_t reqAddr = static_cast<uint8_t>(request[0]);
    const uint8_t reqFunc = static_cast<uint8_t>(request[1]);
    if (static_cast<uint8_t>(response[0]) != reqAddr) return ResponseVerdict::WrongAddress;
    const uint8_t respFunc = static_cast<uint8_t>(response[1]);
    if (respFunc == (reqFunc | 0x80)) {
        return response.size() == 5 ? ResponseVerdict::Exception : ResponseVerdict::Malformed;
    }
    if (respFunc != reqFunc) return ResponseVerdict::WrongFunction;
    switch (reqFunc) {
    case kFuncReadHolding: {
        if (request.size() < 6) return ResponseVerdict::Malformed;
        const uint16_t count = static_cast<uint16_t>(
            (static_cast<uint8_t>(request[4]) << 8) | static_cast<uint8_t>(request[5]));
        QByteArray ignored;
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
