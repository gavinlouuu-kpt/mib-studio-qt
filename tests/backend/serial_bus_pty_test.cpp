// serial_bus_pty_test
//
// Regression + behavior guard for the shared RS485 bus layer (SerialBus.h)
// and PulseGeneratorService's use of it, driven over a POSIX pseudo-terminal
// so no hardware is needed. Proves:
//  - the backend opens the system port name it is given ("/dev/pts/N" — the
//    historical bug synthesized "COMn" and could never work on Linux);
//  - one SerialBusManager session per (port, settings), shared by clients,
//    refused when the settings disagree;
//  - two pulse generators at different slave addresses on one bus are
//    verified and controlled independently through the one serial owner,
//    while a generic Modbus device at a third address is never written to;
//  - concurrent requests from two clients are serialized and responses never
//    cross-associate;
//  - scan is read-only (no write function codes on the wire) and classifies
//    generators vs generic devices vs corrupt responders;
//  - wrong-address, bad-CRC, short/truncated, and Modbus-exception responses
//    produce the correct failure state.
//
// POSIX-only: on Windows this test compiles to a trivial pass (the CI stub
// lane is Linux; Windows bench verification is manual).

#if defined(_WIN32)
int main() { return 0; }
#else

#include "backend/services/ModbusRtu.h"
#include "backend/services/PulseGeneratorService.h"
#include "backend/services/SerialBus.h"
#include "backend/services/SyringePumpService.h"

#include "support/assert.h"
#include "support/watchdog.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QString>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <stdlib.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace modbus = backend::services::modbus;
namespace serialbus = backend::services::serialbus;
using backend::services::PulseGeneratorService;

namespace {

// ---------------------------------------------------------------------------
// Simulated RS485 bus on the pty master side.
//
// Devices:
//   addr 1, 2 : pulse generators (12 holding registers each)
//   addr 3    : generic Modbus device — answers every request with exception
//               0x02 (illegal data address); must never be written to
//   addr 5    : generic device that happens to serve 12 holding registers at
//               address 0, but with values implausible for the generator
//               (all 0xFFFF) — must NOT be classified as a pulse generator
//   addr 7    : truncated responder — 3 header bytes, then silence
//   addr 8    : corrupt responder — right shape, wrong CRC
//   addr 9    : responds with a frame claiming slave address 10
//   others    : silent
// ---------------------------------------------------------------------------
class BusSimulator {
public:
    explicit BusSimulator(int masterFd) : fd_(masterFd)
    {
        std::memset(regs_, 0, sizeof(regs_));
        // Seed addr 1 ch1: 5000 Hz (raw 500000), 50 % (raw 5000) — enabled.
        setFreqRaw(0, 0, 500000);
        regs_[0][2] = 5000;
        // Seed addr 2 ch1: 1000 Hz, duty 0 — disabled.
        setFreqRaw(1, 0, 100000);
        thread_ = std::thread([this] { loop(); });
    }

    ~BusSimulator()
    {
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
        ::close(fd_); // only after the simulator thread is gone
    }

    uint32_t freqRaw(int device, int channel) const
    {
        std::scoped_lock lock(regsMutex_);
        return (static_cast<uint32_t>(regs_[device][channel * 3]) << 16) |
               regs_[device][channel * 3 + 1];
    }
    uint16_t dutyRaw(int device, int channel) const
    {
        std::scoped_lock lock(regsMutex_);
        return regs_[device][channel * 3 + 2];
    }

    int writesTo(uint8_t addr) const { return writeCount_[addr].load(); }
    int totalWrites() const
    {
        int total = 0;
        for (const auto& c : writeCount_) total += c.load();
        return total;
    }

private:
    void setFreqRaw(int device, int channel, uint32_t raw)
    {
        regs_[device][channel * 3] = static_cast<uint16_t>(raw >> 16);
        regs_[device][channel * 3 + 1] = static_cast<uint16_t>(raw & 0xFFFF);
    }

    void send(const QByteArray& frame)
    {
        ssize_t off = 0;
        while (off < frame.size()) {
            const ssize_t n = ::write(fd_, frame.constData() + off,
                                      static_cast<size_t>(frame.size() - off));
            if (n <= 0) return;
            off += n;
        }
    }

    // Request length from its function code; -1 = need more bytes, -2 = junk.
    static int requestLength(const QByteArray& buf)
    {
        if (buf.size() < 2) return -1;
        switch (static_cast<uint8_t>(buf[1])) {
        case modbus::kFuncReadHolding:
        case modbus::kFuncWriteSingle:
            return 8;
        case modbus::kFuncWriteMultiple:
            if (buf.size() < 7) return -1;
            return 9 + static_cast<uint8_t>(buf[6]);
        default:
            return -2;
        }
    }

    void handle(const QByteArray& req)
    {
        std::scoped_lock lock(regsMutex_); // main thread reads regs_ in assertions
        const uint8_t addr = static_cast<uint8_t>(req[0]);
        const uint8_t func = static_cast<uint8_t>(req[1]);
        if (func == modbus::kFuncWriteSingle || func == modbus::kFuncWriteMultiple) {
            writeCount_[addr].fetch_add(1);
        }

        if (addr == 3) { // generic device: always an exception
            QByteArray resp;
            resp.append(static_cast<char>(addr));
            resp.append(static_cast<char>(func | 0x80));
            resp.append(static_cast<char>(0x02));
            modbus::appendCrc(resp);
            send(resp);
            return;
        }
        if (addr == 5 && func == modbus::kFuncReadHolding) {
            // Valid FC03 shape, implausible register values (all 0xFFFF).
            const uint16_t count = static_cast<uint16_t>(
                (static_cast<uint8_t>(req[4]) << 8) | static_cast<uint8_t>(req[5]));
            QByteArray resp;
            resp.append(static_cast<char>(addr));
            resp.append(static_cast<char>(func));
            resp.append(static_cast<char>(count * 2));
            resp.append(count * 2, static_cast<char>(0xFF));
            modbus::appendCrc(resp);
            send(resp);
            return;
        }
        if (addr == 7) { // truncated: header only, then silence
            QByteArray resp;
            resp.append(static_cast<char>(addr));
            resp.append(static_cast<char>(func));
            resp.append(static_cast<char>(24));
            send(resp);
            return;
        }
        if (addr == 8) { // corrupt CRC
            QByteArray resp;
            resp.append(static_cast<char>(addr));
            resp.append(static_cast<char>(func | 0x80));
            resp.append(static_cast<char>(0x01));
            modbus::appendCrc(resp);
            resp[resp.size() - 1] = static_cast<char>(resp[resp.size() - 1] ^ 0x5A);
            send(resp);
            return;
        }
        if (addr == 9) { // answers as somebody else
            QByteArray resp;
            resp.append(static_cast<char>(10));
            resp.append(static_cast<char>(func | 0x80));
            resp.append(static_cast<char>(0x01));
            modbus::appendCrc(resp);
            send(resp);
            return;
        }
        if (addr != 1 && addr != 2) {
            return; // silent
        }

        const int device = addr - 1;
        switch (func) {
        case modbus::kFuncReadHolding: {
            const uint16_t start = static_cast<uint16_t>(
                (static_cast<uint8_t>(req[2]) << 8) | static_cast<uint8_t>(req[3]));
            const uint16_t count = static_cast<uint16_t>(
                (static_cast<uint8_t>(req[4]) << 8) | static_cast<uint8_t>(req[5]));
            if (start + count > 12) {
                // Out-of-map read (e.g. the pump service probing its own
                // registers): answer illegal-data-address instead of silence.
                QByteArray resp;
                resp.append(static_cast<char>(addr));
                resp.append(static_cast<char>(func | 0x80));
                resp.append(static_cast<char>(0x02));
                modbus::appendCrc(resp);
                send(resp);
                return;
            }
            QByteArray resp;
            resp.append(static_cast<char>(addr));
            resp.append(static_cast<char>(func));
            resp.append(static_cast<char>(count * 2));
            for (int r = start; r < start + count; ++r) {
                resp.append(static_cast<char>(regs_[device][r] >> 8));
                resp.append(static_cast<char>(regs_[device][r] & 0xFF));
            }
            modbus::appendCrc(resp);
            send(resp);
            return;
        }
        case modbus::kFuncWriteSingle: {
            const uint16_t reg = static_cast<uint16_t>(
                (static_cast<uint8_t>(req[2]) << 8) | static_cast<uint8_t>(req[3]));
            const uint16_t value = static_cast<uint16_t>(
                (static_cast<uint8_t>(req[4]) << 8) | static_cast<uint8_t>(req[5]));
            if (reg < 12) regs_[device][reg] = value;
            send(req); // FC06 echoes the request
            return;
        }
        case modbus::kFuncWriteMultiple: {
            const uint16_t start = static_cast<uint16_t>(
                (static_cast<uint8_t>(req[2]) << 8) | static_cast<uint8_t>(req[3]));
            const uint16_t count = static_cast<uint16_t>(
                (static_cast<uint8_t>(req[4]) << 8) | static_cast<uint8_t>(req[5]));
            for (int r = 0; r < count && start + r < 12; ++r) {
                regs_[device][start + r] = static_cast<uint16_t>(
                    (static_cast<uint8_t>(req[7 + r * 2]) << 8) |
                    static_cast<uint8_t>(req[8 + r * 2]));
            }
            QByteArray resp = req.left(6);
            modbus::appendCrc(resp);
            send(resp);
            return;
        }
        default:
            return;
        }
    }

    void loop()
    {
        QByteArray buffer;
        while (!stop_.load()) {
            struct pollfd pfd{fd_, POLLIN, 0};
            const int rc = ::poll(&pfd, 1, 20);
            if (rc <= 0 || !(pfd.revents & POLLIN)) continue;
            char chunk[256];
            const ssize_t n = ::read(fd_, chunk, sizeof(chunk));
            if (n <= 0) continue;
            buffer.append(chunk, static_cast<int>(n));
            while (true) {
                const int len = requestLength(buffer);
                if (len == -1) break;
                if (len == -2) { buffer.clear(); break; }
                if (buffer.size() < len) break;
                const QByteArray req = buffer.left(len);
                buffer.remove(0, len);
                if (modbus::responseCrcValid(req)) {
                    handle(req);
                }
            }
        }
    }

    int fd_;
    mutable std::mutex regsMutex_;
    uint16_t regs_[2][12];
    std::array<std::atomic<int>, 256> writeCount_{};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    mib::test::Watchdog watchdog(60);

    // --- pty setup ---------------------------------------------------------
    watchdog.mark("open pty");
    const int masterFd = ::posix_openpt(O_RDWR | O_NOCTTY);
    MIB_REQUIRE(masterFd >= 0, "posix_openpt");
    MIB_REQUIRE(::grantpt(masterFd) == 0 && ::unlockpt(masterFd) == 0, "grantpt/unlockpt");
    char slavePathBuf[128];
    MIB_REQUIRE(::ptsname_r(masterFd, slavePathBuf, sizeof(slavePathBuf)) == 0, "ptsname_r");
    const QString slavePath = QString::fromLatin1(slavePathBuf);
    {
        struct termios tio{};
        MIB_REQUIRE(::tcgetattr(masterFd, &tio) == 0, "tcgetattr");
        ::cfmakeraw(&tio);
        MIB_REQUIRE(::tcsetattr(masterFd, TCSANOW, &tio) == 0, "tcsetattr");
    }
    std::printf("pty slave: %s\n", slavePathBuf);

    BusSimulator sim(masterFd);
    serialbus::SerialBusManager manager;
    serialbus::SerialSettings settings; // 9600 8N1

    // --- one shared session per (port, settings) ---------------------------
    watchdog.mark("manager identity");
    {
        serialbus::BusError err = serialbus::BusError::None;
        auto a = manager.acquire(slavePath, settings, &err);
        MIB_REQUIRE(a != nullptr, "acquire opens the supplied system port name");
        auto b = manager.acquire(slavePath, settings, &err);
        MIB_EXPECT(a.get() == b.get(), "same settings share one session (one serial owner)");

        serialbus::SerialSettings other = settings;
        other.baudRate = 115200;
        auto c = manager.acquire(slavePath, other, &err);
        MIB_EXPECT(c == nullptr, "conflicting settings are refused");
        MIB_EXPECT(err == serialbus::BusError::PortBusy, "conflict maps to PortBusy");
    }

    // --- strict response correlation at the session level ------------------
    watchdog.mark("correlation failures");
    {
        auto bus = manager.acquire(slavePath, settings);
        MIB_REQUIRE(bus != nullptr, "session for correlation checks");

        auto probe = [&](uint8_t addr, int timeoutMs) {
            return bus->transact(modbus::buildReadRequest(addr, 0, 12), timeoutMs);
        };
        MIB_EXPECT(probe(3, 400).error == serialbus::BusError::ModbusException,
                   "Modbus exception frame is reported as ModbusException");
        MIB_EXPECT(probe(8, 400).error == serialbus::BusError::CrcError,
                   "corrupt CRC is reported as CrcError, not attributed to a device");
        MIB_EXPECT(probe(9, 300).error == serialbus::BusError::WrongAddress,
                   "a frame from another address is rejected as WrongAddress");
        MIB_EXPECT(probe(7, 300).error == serialbus::BusError::Timeout,
                   "a truncated frame times out instead of being misparsed");
        MIB_EXPECT(probe(4, 200).error == serialbus::BusError::Timeout,
                   "silence is a Timeout");
        // The delayed/stale-byte path: after the addr-7 truncation the next
        // transaction must still succeed (stale bytes drained, not attributed).
        MIB_EXPECT(probe(1, 500).error == serialbus::BusError::None,
                   "transaction after garbage on the wire still succeeds");
    }

    // --- two generators + one generic device on one bus --------------------
    watchdog.mark("connect two generators");
    PulseGeneratorService gen1(manager);
    PulseGeneratorService gen2(manager);
    MIB_REQUIRE(gen1.connect(slavePath, settings, 1),
                "generator at addr 1 connects via the system port name (no COMn synth)");
    MIB_REQUIRE(gen2.connect(slavePath, settings, 2), "generator at addr 2 shares the bus");

    {
        const auto s1 = gen1.getStatus();
        MIB_EXPECT(s1.channels[0].frequencyHz == 5000.0, "addr1 ch1 frequency seeded from device");
        MIB_EXPECT(s1.channels[0].dutyPercent == 50.0, "addr1 ch1 duty seeded from device");
        MIB_EXPECT(s1.channels[0].outputEnabled, "addr1 ch1 enabled (non-zero duty)");
        const auto s2 = gen2.getStatus();
        MIB_EXPECT(s2.channels[0].frequencyHz == 1000.0, "addr2 ch1 frequency seeded from device");
        MIB_EXPECT(!s2.channels[0].outputEnabled, "addr2 ch1 disabled (duty 0)");
    }

    watchdog.mark("independent control");
    MIB_EXPECT(gen1.setFrequency(0, 2000.0), "write to addr1");
    MIB_EXPECT(sim.freqRaw(0, 0) == 200000, "addr1 registers updated");
    MIB_EXPECT(sim.freqRaw(1, 0) == 100000, "addr2 registers untouched by addr1 write");
    MIB_EXPECT(gen2.setFrequency(0, 3000.0), "write to addr2");
    MIB_EXPECT(sim.freqRaw(1, 0) == 300000, "addr2 registers updated");
    MIB_EXPECT(sim.freqRaw(0, 0) == 200000, "addr1 registers untouched by addr2 write");
    MIB_EXPECT(sim.writesTo(3) == 0, "generic device at addr 3 never written");

    // --- concurrent clients are serialized, responses never cross ----------
    watchdog.mark("concurrency");
    {
        std::atomic<int> failures{0};
        auto hammer = [&failures](PulseGeneratorService& gen, double base) {
            for (int i = 0; i < 25; ++i) {
                if (!gen.setFrequency(0, base + i)) failures.fetch_add(1);
            }
        };
        std::thread t1(hammer, std::ref(gen1), 2000.0);
        std::thread t2(hammer, std::ref(gen2), 3000.0);
        t1.join();
        t2.join();
        MIB_EXPECT(failures.load() == 0, "50 interleaved transactions all correlate");
        MIB_EXPECT(sim.freqRaw(0, 0) == 202400, "addr1 ends at its own last value (2024 Hz)");
        MIB_EXPECT(sim.freqRaw(1, 0) == 302400, "addr2 ends at its own last value (3024 Hz)");
    }

    // --- read-only scan with classification --------------------------------
    watchdog.mark("scan");
    {
        const int writesBefore = sim.totalWrites();
        std::atomic<bool> cancel{false};
        const auto hits = gen1.scanBus(slavePath, settings, 1, 9, cancel, 250);
        MIB_EXPECT(sim.totalWrites() == writesBefore, "scan emits no write function codes");

        auto kindOf = [&hits](uint8_t addr) -> const PulseGeneratorService::ScanHit* {
            for (const auto& h : hits) {
                if (h.address == addr) return &h;
            }
            return nullptr;
        };
        MIB_REQUIRE(kindOf(1) != nullptr && kindOf(2) != nullptr, "both generators found");
        MIB_EXPECT(kindOf(1)->kind == PulseGeneratorService::ScanHit::Kind::PulseGenerator,
                   "addr1 classified as pulse generator");
        MIB_EXPECT(kindOf(2)->kind == PulseGeneratorService::ScanHit::Kind::PulseGenerator,
                   "addr2 classified as pulse generator");
        MIB_REQUIRE(kindOf(3) != nullptr, "generic device shows up in scan");
        MIB_EXPECT(kindOf(3)->kind == PulseGeneratorService::ScanHit::Kind::ModbusDevice,
                   "addr3 classified as generic Modbus device, not a generator");
        MIB_REQUIRE(kindOf(5) != nullptr, "12-register impostor shows up in scan");
        MIB_EXPECT(kindOf(5)->kind == PulseGeneratorService::ScanHit::Kind::ModbusDevice,
                   "right-shaped but implausible register values are NOT a generator");
        MIB_REQUIRE(kindOf(8) != nullptr, "corrupt responder shows up in scan");
        MIB_EXPECT(kindOf(8)->kind == PulseGeneratorService::ScanHit::Kind::Error,
                   "addr8 classified as error/possible collision");
        MIB_EXPECT(kindOf(4) == nullptr && kindOf(6) == nullptr, "silent addresses omitted");
    }
    watchdog.mark("scan cancel");
    {
        std::atomic<bool> cancel{true};
        const auto hits = gen1.scanBus(slavePath, settings, 1, 255, cancel, 250);
        MIB_EXPECT(hits.empty(), "pre-cancelled scan probes nothing");
    }
    watchdog.mark("scan port conflict");
    {
        // Scanning the port with conflicting serial settings while the bus is
        // held must report a port error, not masquerade as a silent bus.
        serialbus::SerialSettings other = settings;
        other.baudRate = 115200;
        std::atomic<bool> cancel{false};
        PulseGeneratorService::LinkError scanError = PulseGeneratorService::LinkError::None;
        const auto hits = gen1.scanBus(slavePath, other, 1, 4, cancel, 100, &scanError);
        MIB_EXPECT(hits.empty(), "conflicting-settings scan probes nothing");
        MIB_EXPECT(scanError == PulseGeneratorService::LinkError::PortBusy,
                   "conflicting-settings scan reports PortBusy");
    }

    // --- a plausible-shaped impostor is refused on connect ------------------
    watchdog.mark("impostor connect");
    {
        PulseGeneratorService genX(manager);
        MIB_EXPECT(!genX.connect(slavePath, settings, 5),
                   "connect refuses the 12-register impostor at addr 5");
        MIB_EXPECT(genX.lastError() == PulseGeneratorService::LinkError::IncompatibleDevice,
                   "impostor refusal reports IncompatibleDevice");
    }

    // --- pump shares the same adapter through the QString port API ----------
    watchdog.mark("pump shares bus");
    {
        backend::services::SyringePumpService pump(manager);
        const auto pumpId = backend::services::SyringePumpService::PumpId::Sample;
        MIB_EXPECT(pump.connect(pumpId, slavePath, 9600, 1),
                   "pump connects via a system port name on the shared bus");
        MIB_EXPECT(pump.isConnected(pumpId), "pump reports connected");
        // Regression: reconnecting while connected used to self-deadlock on
        // the pump mutex (the watchdog would fire here).
        MIB_EXPECT(pump.connect(pumpId, slavePath, 9600, 1),
                   "pump reconnect while connected does not deadlock");
        pump.disconnect(pumpId);
        MIB_EXPECT(!pump.isConnected(pumpId), "pump disconnects cleanly");
    }

    // --- disconnect keeps the shared port open for the other client --------
    watchdog.mark("disconnect");
    gen1.disconnect();
    MIB_EXPECT(!gen1.isConnected(), "gen1 disconnected");
    MIB_EXPECT(gen2.setFrequency(0, 4000.0), "gen2 keeps working after gen1 lets go");
    MIB_EXPECT(sim.freqRaw(1, 0) == 400000, "gen2 write lands after gen1 disconnect");
    gen2.disconnect();

    watchdog.mark("done");
    // masterFd is closed by ~BusSimulator once its thread has joined.
    if (mib::test::exitCode() == 0) std::printf("serial bus pty test OK\n");
    return mib::test::exitCode();
}

#endif // !_WIN32
