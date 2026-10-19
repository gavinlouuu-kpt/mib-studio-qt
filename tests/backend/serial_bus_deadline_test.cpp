// serial_bus_deadline_test (#720)
//
// ModbusBusSession::transact() must never wait without limit for its I/O
// thread, and nothing that happens after a caller gave up may answer a later
// caller. Driven through SerialBusManager with a fake Modbus slave whose
// write() can be held (a port call that ignores its own bound) and whose
// answers can arrive late. Proves:
//  - a held port call: transact returns Timeout at transactBudgetMs(timeout);
//  - while the I/O thread is still held, further calls fail fast and send
//    nothing (no request queues behind the stuck one, none is sent late);
//  - once released, the abandoned request's late result is dropped: the next
//    request gets its own answer;
//  - a reply that arrives after its transaction's deadline is drained by the
//    next transaction, never taken as its answer;
//  - under concurrent callers every answer belongs to its own request.
// FC03 answers carry a register-dependent value and have the same shape for
// every register, so a misattributed answer would pass the frame checks and
// show up only as a wrong value.

#include "backend/services/ISerialPort.h"
#include "backend/services/ModbusRtu.h"
#include "backend/services/SerialBus.h"

#include "support/assert.h"
#include "support/watchdog.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace modbus = backend::services::modbus;
namespace serialbus = backend::services::serialbus;
using Bytes = std::vector<uint8_t>;
using Clock = std::chrono::steady_clock;

namespace {

constexpr uint8_t kAddress = 1;

uint16_t valueOf(uint16_t reg) { return static_cast<uint16_t>(reg * 3 + 7); }

Bytes readRequest(uint16_t reg) { return modbus::buildReadRequest(kAddress, reg, 1); }

uint16_t answerValue(const serialbus::Transaction& t)
{
    return t.response.size() >= 5 ? static_cast<uint16_t>((t.response[3] << 8) | t.response[4]) : 0xFFFF;
}

long long msSince(Clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

// Test-side controls, shared with the I/O thread under `mutex`.
struct Slave {
    std::mutex mutex;
    std::condition_variable cv;
    bool holdWrites{false};   // write() blocks until cleared
    bool holding{false};      // a write() is blocked right now
    int replyDelayMs{0};      // the answer becomes readable this much later
    Clock::time_point lateReplyAt{};
    int silentEvery{0};       // >0: every Nth request is not answered (stress)
    std::uint64_t jitterSeed{1};
    std::vector<Bytes> written;
};

class SlavePort final : public backend::services::ISerialPort {
public:
    explicit SlavePort(Slave& slave) : slave_(slave), rng_(slave.jitterSeed) {}

    bool open(int, int) override { return open_ = true; }
    bool openNamed(const std::string&, const backend::services::SerialSettings&) override
    {
        return open_ = true;
    }
    void close() override { open_ = false; }
    bool isOpen() const override { return open_; }

    int write(const Bytes& q) override
    {
        std::unique_lock lock(slave_.mutex);
        slave_.written.push_back(q);
        if (slave_.holdWrites) {
            slave_.holding = true;
            slave_.cv.notify_all();
            slave_.cv.wait(lock, [this] { return !slave_.holdWrites; });
            slave_.holding = false;
        }
        const bool silent =
            slave_.silentEvery > 0 && slave_.written.size() % static_cast<size_t>(slave_.silentEvery) == 0;
        if (q.size() < 8 || q[0] != kAddress || q[1] != 3 || silent) return static_cast<int>(q.size());
        const auto reg = static_cast<uint16_t>((q[2] << 8) | q[3]);
        Bytes answer{kAddress, 3, 2, static_cast<uint8_t>(valueOf(reg) >> 8),
                     static_cast<uint8_t>(valueOf(reg) & 0xFF)};
        modbus::appendCrc(answer);
        pending_ = std::move(answer);
        int delayMs = slave_.replyDelayMs;
        if (slave_.silentEvery > 0) delayMs = static_cast<int>(rng_() % 3); // stress jitter, in time
        readyAt_ = Clock::now() + std::chrono::milliseconds(delayMs);
        if (slave_.replyDelayMs > 0) slave_.lateReplyAt = readyAt_;
        return static_cast<int>(q.size());
    }
    bool waitForBytesWritten(int) override { return true; }
    bool waitForReadyRead(int ms) override
    {
        const auto deadline = Clock::now() + std::chrono::milliseconds(ms);
        while (!readable()) {
            if (Clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }
    Bytes readAll() override
    {
        if (!readable()) return {};
        Bytes out;
        out.swap(pending_);
        return out;
    }
    std::string lastError() const override { return {}; }

private:
    bool readable() const { return !pending_.empty() && Clock::now() >= readyAt_; }

    Slave& slave_;
    std::minstd_rand rng_;
    bool open_{false};
    Bytes pending_;              // I/O thread only
    Clock::time_point readyAt_{}; // I/O thread only
};

size_t writtenCount(Slave& slave)
{
    std::scoped_lock lock(slave.mutex);
    return slave.written.size();
}

} // namespace

int main()
{
    mib::test::Watchdog watchdog(30);
    // On top of every bound: loaded CI and sanitizers are slow, a hang is not.
    constexpr long long kCeilingMs = 5000;
    constexpr int kTimeoutMs = 20;
    const int budgetMs = serialbus::ModbusBusSession::transactBudgetMs(kTimeoutMs);
    MIB_EXPECT(budgetMs >= backend::services::kSerialWriteTimeoutMs + 2 * kTimeoutMs,
               "the budget covers the transaction's own bounded steps");

    Slave slave;
    serialbus::SerialBusManager manager;
    manager.setSerialPortFactory([&slave] { return std::make_unique<SlavePort>(slave); });
    serialbus::SerialSettings settings;
    serialbus::BusError openError = serialbus::BusError::None;
    auto bus = manager.acquire("ttyFAKE0", settings, &openError);
    MIB_REQUIRE(bus != nullptr, "fake bus opens");

    // 0) Happy path: the request goes out unchanged, the answer is its own.
    watchdog.mark("happy path");
    auto t = bus->transact(readRequest(0x10), kTimeoutMs);
    MIB_EXPECT(t.error == serialbus::BusError::None && answerValue(t) == valueOf(0x10), "answered");
    MIB_EXPECT(t.detail.empty(), "no failure detail on success");
    {
        std::scoped_lock lock(slave.mutex);
        MIB_EXPECT(slave.written.size() == 1 && slave.written[0] == readRequest(0x10), "same bytes on the wire");
    }

    // 1) A port call that never returns: transact gives up at its budget.
    watchdog.mark("held port call");
    {
        std::scoped_lock lock(slave.mutex);
        slave.holdWrites = true;
    }
    auto start = Clock::now();
    t = bus->transact(readRequest(0x20), kTimeoutMs);
    long long elapsed = msSince(start);
    std::printf("held write: transact returned %s after %lld ms (budget %d ms)\n",
                serialbus::toString(t.error), elapsed, budgetMs);
    MIB_EXPECT(t.error == serialbus::BusError::Timeout, "a held I/O thread is a bus timeout");
    MIB_EXPECT(t.detail.find("no result from the serial I/O thread") != std::string::npos,
               "the detail names the timeout: " + t.detail);
    MIB_EXPECT(elapsed >= budgetMs - 5 && elapsed < budgetMs + kCeilingMs,
               "returns at the budget, not at once and not for ever");
    {
        std::scoped_lock lock(slave.mutex);
        MIB_EXPECT(slave.holding, "the I/O thread is still inside write()");
    }

    // 2) While it is held: fail fast, queue nothing, send nothing.
    watchdog.mark("fail fast while held");
    start = Clock::now();
    t = bus->transact(readRequest(0x30), kTimeoutMs);
    elapsed = msSince(start);
    std::printf("while held: transact returned %s after %lld ms\n", serialbus::toString(t.error), elapsed);
    MIB_EXPECT(t.error == serialbus::BusError::Timeout, "a stuck bus reports a timeout");
    MIB_EXPECT(t.detail.find("still stuck") != std::string::npos, "and says why: " + t.detail);
    MIB_EXPECT(elapsed < budgetMs, "without waiting a budget of its own");
    MIB_EXPECT(writtenCount(slave) == 2, "the refused request never reaches the wire, now or later");

    // 3) Released: the held request finishes late with its own answer, which
    //    must be dropped; the next request gets its own answer.
    watchdog.mark("release");
    {
        std::scoped_lock lock(slave.mutex);
        slave.holdWrites = false;
    }
    slave.cv.notify_all();
    const auto retryUntil = Clock::now() + std::chrono::seconds(10);
    do {
        t = bus->transact(readRequest(0x40), kTimeoutMs);
        if (t.error != serialbus::BusError::Timeout || t.detail.find("still stuck") == std::string::npos) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (Clock::now() < retryUntil);
    MIB_EXPECT(t.error == serialbus::BusError::None, "the bus works again once the call returns");
    MIB_EXPECT(answerValue(t) == valueOf(0x40),
               "the late answer to 0x20 is not taken as the answer to 0x40");
    MIB_EXPECT(writtenCount(slave) == 3, "0x10, 0x20 (late), 0x40; 0x30 was never sent");

    // 4) A reply after its transaction's own deadline: the transaction times
    //    out; the late bytes are drained by the next one, never attributed.
    watchdog.mark("late wire reply");
    {
        std::scoped_lock lock(slave.mutex);
        slave.replyDelayMs = 4 * kTimeoutMs;
    }
    t = bus->transact(readRequest(0x50), kTimeoutMs);
    MIB_EXPECT(t.error == serialbus::BusError::Timeout && t.detail.find("no response within") != std::string::npos,
               "a reply after the deadline is a timeout: " + t.detail);
    Clock::time_point lateAt;
    {
        std::scoped_lock lock(slave.mutex);
        slave.replyDelayMs = 0;
        lateAt = slave.lateReplyAt;
    }
    std::this_thread::sleep_until(lateAt + std::chrono::milliseconds(5)); // the late bytes are now readable
    t = bus->transact(readRequest(0x60), kTimeoutMs);
    MIB_EXPECT(t.error == serialbus::BusError::None && answerValue(t) == valueOf(0x60),
               "the late reply to 0x50 is drained, not taken as the answer to 0x60");

    // 5) Stress: concurrent callers, jittered answers, some never answered.
    //    Every success carries its own request's value.
    watchdog.mark("stress");
    {
        std::scoped_lock lock(slave.mutex);
        slave.silentEvery = 7;
    }
    std::atomic<int> answered{0}, timedOut{0}, wrong{0}, other{0};
    std::vector<std::thread> callers;
    for (int c = 0; c < 4; ++c) {
        callers.emplace_back([&, c] {
            for (int i = 0; i < 60; ++i) {
                const auto reg = static_cast<uint16_t>(0x100 + c * 0x100 + i);
                const auto r = bus->transact(readRequest(reg), 10);
                if (r.error == serialbus::BusError::None) {
                    ++answered;
                    if (answerValue(r) != valueOf(reg)) ++wrong;
                } else if (r.error == serialbus::BusError::Timeout) {
                    ++timedOut;
                } else {
                    ++other;
                }
                watchdog.mark("stress");
            }
        });
    }
    for (auto& caller : callers) caller.join();
    std::printf("stress: %d answered, %d timed out, %d wrong, %d other\n", answered.load(),
                timedOut.load(), wrong.load(), other.load());
    MIB_EXPECT(wrong.load() == 0, "no answer is ever attributed to another request");
    MIB_EXPECT(other.load() == 0, "no other failure");
    MIB_EXPECT(answered.load() > 0 && timedOut.load() > 0, "both outcomes exercised");

    bus.reset();
    if (mib::test::exitCode() == 0) std::printf("SerialBus deadlines verified\n");
    return mib::test::exitCode();
}
