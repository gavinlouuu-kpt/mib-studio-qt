// Fake Modbus RTU bench for the shared serial bus (issue #413/#419).
//
// One `Wire` is a fake slave at a single address behind a named serial port;
// `Bench` maps port names to wires and lists ports that are held by another
// program. `Port` is the ISerialPort the SerialBusManager factory returns.
// Header-only; include as "support/fake_modbus_bench.h".
#pragma once

#include "backend/services/ISerialPort.h"
#include "backend/services/ModbusRtu.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace mib::test {

struct FakeModbusWire {
    std::atomic<bool> failWrites{false};
    std::atomic<bool> corruptRead{false};   // valid CRC is appended, then a byte is flipped
    std::atomic<bool> exceptionOnRead{false}; // answers FC03 with a Modbus exception (foreign device)
    std::atomic<int> writes{0};        // every request frame
    std::atomic<int> writeCommands{0}; // FC06/FC16 only: must stay 0 for discovery
    std::atomic<int> reads{0};         // FC03 requests
    std::uint8_t address{1};           // slave address that answers; others stay silent
    std::array<uint16_t, 12> regs{};   // exclusively accessed by the serial worker
    std::map<uint16_t, uint16_t> extra;
    uint16_t read(int index) const
    {
        if (index >= 0 && index < 12) return regs.at(static_cast<size_t>(index));
        const auto it = extra.find(static_cast<uint16_t>(index));
        return it == extra.end() ? 0 : it->second;
    }
    uint16_t& reg(int index)
    {
        if (index >= 0 && index < 12) return regs.at(static_cast<size_t>(index));
        return extra[static_cast<uint16_t>(index)];
    }
};

struct FakeModbusBench {
    // Port name -> slaves on that bus (several addresses on one adapter).
    std::map<std::string, std::vector<FakeModbusWire*>> wires;
    std::set<std::string> busy; // held by another program
    std::atomic<int> openPorts{0};
    // Optional observers for ordering assertions (called on the bus I/O thread).
    std::function<void(const std::string&)> onOpen;
    std::function<void(const std::string&)> onClose;

    void attach(const std::string& port, FakeModbusWire* wire) { wires[port].push_back(wire); }
};

class FakeModbusPort final : public backend::services::ISerialPort {
public:
    explicit FakeModbusPort(FakeModbusBench& bench) : bench_(bench) {}
    ~FakeModbusPort() override { close(); }

    bool open(int n, int) override { return openNamed("COM" + std::to_string(n), {}); }
    bool openNamed(const std::string& name, const backend::services::SerialSettings&) override
    {
        if (bench_.busy.count(name)) {
#ifdef _WIN32
            systemError_ = 5; // ERROR_ACCESS_DENIED
#else
            systemError_ = EACCES;
#endif
            return false;
        }
        const auto it = bench_.wires.find(name);
        if (it == bench_.wires.end()) {
            systemError_ = 2; // no such device
            return false;
        }
        slaves_ = &it->second;
        opened_ = true;
        name_ = name;
        ++bench_.openPorts;
        if (bench_.onOpen) bench_.onOpen(name);
        return true;
    }
    int lastSystemError() const override { return systemError_; }
    bool isOpen() const override { return opened_; }
    void close() override
    {
        if (opened_) {
            --bench_.openPorts;
            if (bench_.onClose) bench_.onClose(name_);
        }
        opened_ = false;
    }
    int write(const std::vector<uint8_t>& q) override
    {
        if (!slaves_ || q.size() < 6) return static_cast<int>(q.size());
        FakeModbusWire* wire = nullptr;
        for (auto* w : *slaves_) {
            if (w->address == q[0]) wire = w;
        }
        if (!wire) return static_cast<int>(q.size()); // silent address
        ++wire->writes;
        if (q[1] == 3) ++wire->reads;
        if (q[1] != 3) ++wire->writeCommands;
        const int start = (q[2] << 8) | q[3], count = (q[4] << 8) | q[5];
        if (q[1] != 3 && wire->failWrites) {
            rx_ = {q[0], static_cast<uint8_t>(q[1] | 0x80), 4};
            backend::services::modbus::appendCrc(rx_);
            return static_cast<int>(q.size());
        }
        if (q[1] == 3 && wire->exceptionOnRead) {
            rx_ = {q[0], static_cast<uint8_t>(0x83), 2}; // illegal data address
            backend::services::modbus::appendCrc(rx_);
        } else if (q[1] == 3) {
            rx_ = {q[0], 3, static_cast<uint8_t>(count * 2)};
            for (int i = 0; i < count; ++i) {
                const auto n = wire->read(start + i);
                rx_.push_back(static_cast<uint8_t>(n >> 8));
                rx_.push_back(static_cast<uint8_t>(n & 255));
            }
            backend::services::modbus::appendCrc(rx_);
            if (wire->corruptRead) rx_[3] ^= 1; // CRC no longer matches
        } else if (q[1] == 6) {
            wire->reg(start) = static_cast<uint16_t>(count);
            rx_ = q;
        } else {
            for (int i = 0; i < count; ++i) {
                wire->reg(start + i) =
                    static_cast<uint16_t>((q[7 + i * 2] << 8) | q[8 + i * 2]);
            }
            rx_ = {q[0], q[1], q[2], q[3], q[4], q[5]};
            backend::services::modbus::appendCrc(rx_);
        }
        return static_cast<int>(q.size());
    }
    bool waitForBytesWritten(int) override { return true; }
    bool waitForReadyRead(int ms) override
    {
        if (rx_.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        return !rx_.empty();
    }
    std::vector<uint8_t> readAll() override
    {
        auto out = rx_;
        rx_.clear();
        return out;
    }
    std::string lastError() const override { return {}; }

private:
    FakeModbusBench& bench_;
    const std::vector<FakeModbusWire*>* slaves_ = nullptr;
    bool opened_ = false;
    std::string name_;
    int systemError_ = 0;
    std::vector<uint8_t> rx_;
};

// Fill the 12-register generator map so the identity read "looks like" a
// pulse generator: channel frequencies (Hz x 100 as u32 hi/lo) and duty.
inline void makeGeneratorLike(FakeModbusWire& wire, std::uint32_t ch0FrequencyRaw = 500000)
{
    wire.regs = {};
    wire.regs[0] = static_cast<uint16_t>(ch0FrequencyRaw >> 16);
    wire.regs[1] = static_cast<uint16_t>(ch0FrequencyRaw & 0xffff);
    wire.regs[2] = 5000; // 50.00 % duty
}

} // namespace mib::test
