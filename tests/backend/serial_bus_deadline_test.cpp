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
//  - under concurrent callers every answer belongs to its own request;
//  - destroying a session while its I/O thread is still held is bounded:
//    a call that honours ISerialPort::interrupt() is woken and the thread
//    joined (port closed before the destructor returns); a call that ignores
//    it (stuck in the driver) is detached after kShutdownJoinMs, and the
//    detached thread still closes its port, on its own, once released;
//  - until it has, the port stays reserved: a new session on it is refused at
//    once with the reason, and opens again once the thread closed the port;
//    the registry lock is free meanwhile (another port opens during the
//    teardown); an acquire racing a close that finishes in time waits for it
//    and then succeeds, never opening the device twice;
//  - the I/O thread logs through the session's own logger, so a detached
//    thread's late warning still lands after spdlog::drop_all().
// FC03 answers carry a register-dependent value and have the same shape for
// every register, so a misattributed answer would pass the frame checks and
// show up only as a wrong value.

#include "backend/services/ISerialPort.h"
#include "backend/services/ModbusRtu.h"
#include "backend/services/SerialBus.h"

#include "support/assert.h"
#include "support/watchdog.h"

#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/spdlog.h>

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
    bool honourInterrupt{false}; // interrupt() releases a held write
    bool interrupted{false};
    int interrupts{0};
    int livePorts{0};            // SlavePort objects alive (the I/O thread owns them)
};

class SlavePort final : public backend::services::ISerialPort {
public:
    explicit SlavePort(Slave& slave) : slave_(slave), rng_(slave.jitterSeed)
    {
        std::scoped_lock lock(slave_.mutex);
        ++slave_.livePorts;
    }
    ~SlavePort() override
    {
        // Notify under the lock: once a waiter sees livePorts drop it may
        // destroy the Slave, so nothing here may touch it after unlocking.
        std::scoped_lock lock(slave_.mutex);
        --slave_.livePorts;
        slave_.cv.notify_all();
    }

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
            slave_.cv.wait(lock, [this] {
                return !slave_.holdWrites || (slave_.honourInterrupt && slave_.interrupted);
            });
            slave_.holding = false;
            if (slave_.holdWrites) return -1; // woken by interrupt()
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
    void interrupt() override
    {
        {
            std::scoped_lock lock(slave_.mutex);
            slave_.interrupted = true;
            ++slave_.interrupts;
        }
        slave_.cv.notify_all();
    }
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

void waitForInterrupt(Slave& slave)
{
    std::unique_lock lock(slave.mutex);
    slave.cv.wait(lock, [&] { return slave.interrupts >= 1; });
}

size_t writtenCount(Slave& slave)
{
    std::scoped_lock lock(slave.mutex);
    return slave.written.size();
}

int livePorts(Slave& slave)
{
    std::scoped_lock lock(slave.mutex);
    return slave.livePorts;
}

// Open a session on its own fake, hold its next write and let transact()
// abandon it: the I/O thread is left inside write().
std::shared_ptr<serialbus::ModbusBusSession> heldSession(serialbus::SerialBusManager& manager, Slave& slave,
                                                         const std::string& port, int timeoutMs)
{
    manager.setSerialPortFactory([&slave] { return std::make_unique<SlavePort>(slave); });
    auto session = manager.acquire(port, serialbus::SerialSettings{});
    MIB_REQUIRE(session != nullptr, "fake bus opens");
    {
        std::scoped_lock lock(slave.mutex);
        slave.holdWrites = true;
    }
    const auto t = session->transact(readRequest(0x70), timeoutMs);
    MIB_EXPECT(t.error == serialbus::BusError::Timeout, "the held transaction is abandoned");
    std::scoped_lock lock(slave.mutex);
    MIB_REQUIRE(slave.holding, "the I/O thread is inside write()");
    return session;
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

    {
        std::scoped_lock lock(slave.mutex);
        MIB_EXPECT(slave.interrupts == 0, "no transaction ran so far without a caller: nothing interrupted");
    }
    bus.reset();
    MIB_EXPECT(livePorts(slave) == 0, "an idle session closes its port before the destructor returns");

    // 6) Teardown while the I/O thread is held (#720 review): bounded.
    //    a) The port honours interrupt(): the held call is woken, the thread
    //       closes the port and is joined.
    watchdog.mark("teardown, interruptible call");
    {
        Slave wakeable;
        wakeable.honourInterrupt = true;
        serialbus::SerialBusManager teardown;
        auto session = heldSession(teardown, wakeable, "ttyFAKE1", kTimeoutMs);
        start = Clock::now();
        session.reset();
        elapsed = msSince(start);
        std::printf("teardown, interruptible: destructor returned after %lld ms\n", elapsed);
        MIB_EXPECT(livePorts(wakeable) == 0, "joined: the port is closed when the destructor returns");
        std::scoped_lock lock(wakeable.mutex);
        MIB_EXPECT(wakeable.interrupts == 1, "the held call was interrupted");
    }
    //    b) The call ignores interrupt() (stuck in the driver): the
    //       destructor gives up at kShutdownJoinMs and detaches the thread,
    //       which still owns the port and closes it once released. Until it
    //       has, the port stays reserved; the registry lock stays free. The
    //       session logs through its own logger: the detached thread's late
    //       warning lands even after logging was torn down (drop_all).
    watchdog.mark("teardown, stuck call");
    {
        const auto previousLogger = spdlog::default_logger();
        const auto ring = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(128);
        const auto testLogger = std::make_shared<spdlog::logger>("serial-bus-deadline", ring);
        testLogger->set_level(spdlog::level::trace);
        spdlog::set_default_logger(testLogger); // the session takes this one

        Slave stuck;
        Slave other;
        serialbus::SerialBusManager teardown;
        auto session = heldSession(teardown, stuck, "ttyFAKE2", kTimeoutMs);
        std::atomic<long long> destroyedAfter{-1};
        start = Clock::now();
        std::thread destroyer([&] {
            session.reset();
            destroyedAfter = msSince(start);
        });
        waitForInterrupt(stuck); // the destructor now waits its bound

        // The registry is not locked by the dying session: another port opens.
        teardown.setSerialPortFactory([&other] { return std::make_unique<SlavePort>(other); });
        auto unrelated = teardown.acquire("ttyFAKE3", serialbus::SerialSettings{});
        const bool stillDestroying = destroyedAfter.load() < 0;
        MIB_EXPECT(unrelated != nullptr, "another port opens during the teardown");
        MIB_EXPECT(stillDestroying, "while the stuck session's destructor is still waiting");
        unrelated.reset();

        destroyer.join();
        elapsed = destroyedAfter.load();
        std::printf("teardown, stuck: destructor returned after %lld ms (bound %d ms)\n", elapsed,
                    serialbus::ModbusBusSession::kShutdownJoinMs);
        MIB_EXPECT(elapsed >= serialbus::ModbusBusSession::kShutdownJoinMs - 5 &&
                       elapsed < serialbus::ModbusBusSession::kShutdownJoinMs + kCeilingMs,
                   "the destructor waits its bound, not for ever");
        {
            std::scoped_lock lock(stuck.mutex);
            MIB_EXPECT(stuck.holding && stuck.livePorts == 1,
                       "the session is gone while its detached thread is still held, owning the port");
        }

        // Still reserved: refused at once, with the reason; nothing opened.
        serialbus::BusError busError = serialbus::BusError::None;
        std::string detail;
        auto refusedAt = Clock::now();
        auto again = teardown.acquire("ttyFAKE2", serialbus::SerialSettings{}, &busError, &detail);
        const long long refusedMs = msSince(refusedAt);
        std::printf("teardown, stuck: same port refused after %lld ms: %s\n", refusedMs, detail.c_str());
        MIB_EXPECT(again == nullptr && busError == serialbus::BusError::PortBusy,
                   "the port is refused while the detached thread still holds it");
        MIB_EXPECT(detail == "serial port ttyFAKE2 is still closing after a stalled transaction",
                   "and the refusal says why: " + detail);
        MIB_EXPECT(refusedMs < serialbus::ModbusBusSession::kShutdownJoinMs / 2, "at once, without waiting");
        MIB_EXPECT(livePorts(other) == 0, "no second port was opened");

        // Logging goes away (as at exit) before the detached thread finishes.
        spdlog::drop_all();
        {
            std::scoped_lock lock(stuck.mutex);
            stuck.holdWrites = false;
        }
        stuck.cv.notify_all();
        {
            std::unique_lock lock(stuck.mutex);
            const bool closed =
                stuck.cv.wait_for(lock, std::chrono::seconds(10), [&] { return stuck.livePorts == 0; });
            MIB_EXPECT(closed, "released, the detached thread closes its port and leaves");
            MIB_EXPECT(stuck.written.size() == 1, "and sends nothing more");
        }
        // Its warning (logged before it closed the port) went to the session's logger.
        bool warned = false;
        for (const auto& line : ring->last_formatted()) {
            if (line.find("ttyFAKE2 dropped the late result") != std::string::npos) warned = true;
        }
        MIB_EXPECT(warned, "the detached thread's late warning reached the session's logger");
        spdlog::set_default_logger(previousLogger);

        // Closed: the port opens again (the thread releases it right after closing).
        teardown.setSerialPortFactory([&stuck] { return std::make_unique<SlavePort>(stuck); });
        std::shared_ptr<serialbus::ModbusBusSession> reopened;
        const auto reopenUntil = Clock::now() + std::chrono::seconds(10);
        do {
            reopened = teardown.acquire("ttyFAKE2", serialbus::SerialSettings{}, &busError, &detail);
            if (reopened) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (Clock::now() < reopenUntil);
        MIB_EXPECT(reopened != nullptr, "the port opens again once the detached thread closed it: " + detail);
        reopened.reset();
        MIB_EXPECT(livePorts(stuck) == 0, "and closes again");
    }
    //    c) A close that finishes within the bound, raced by an acquire of the
    //       same port on another thread: the acquire waits for the port to
    //       close (never two sessions on one device), then opens it. It is
    //       not refused: only a stalled teardown is.
    watchdog.mark("teardown raced by an acquire");
    {
        Slave slow;
        serialbus::SerialBusManager teardown;
        auto session = heldSession(teardown, slow, "ttyFAKE4", kTimeoutMs);
        std::atomic<long long> destroyedAfter{-1};
        std::atomic<long long> acquiredAfter{-1};
        start = Clock::now();
        std::thread destroyer([&] {
            session.reset();
            destroyedAfter = msSince(start);
        });
        waitForInterrupt(slow);
        std::shared_ptr<serialbus::ModbusBusSession> raced;
        serialbus::BusError racedError = serialbus::BusError::None;
        std::string racedDetail;
        std::thread racer([&] {
            raced = teardown.acquire("ttyFAKE4", serialbus::SerialSettings{}, &racedError, &racedDetail);
            acquiredAfter = msSince(start);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        MIB_EXPECT(acquiredAfter.load() < 0, "the racing acquire waits while the old port is open");
        {
            std::scoped_lock lock(slow.mutex);
            MIB_EXPECT(slow.livePorts == 1, "and has not opened a second port");
            slow.holdWrites = false; // the call returns within the destructor's bound
        }
        slow.cv.notify_all();
        racer.join();
        destroyer.join();
        std::printf("teardown raced: destructor returned after %lld ms, acquire after %lld ms\n",
                    destroyedAfter.load(), acquiredAfter.load());
        MIB_EXPECT(destroyedAfter.load() < serialbus::ModbusBusSession::kShutdownJoinMs,
                   "the call returned in time: joined, not detached");
        MIB_EXPECT(raced != nullptr && racedError == serialbus::BusError::None,
                   "the racing acquire opens the port once it is closed: " + racedDetail);
        MIB_EXPECT(livePorts(slow) == 1, "exactly one port: the new session's");
        raced.reset();
        MIB_EXPECT(livePorts(slow) == 0, "which closes in turn");
    }

    if (mib::test::exitCode() == 0) std::printf("SerialBus deadlines verified\n");
    return mib::test::exitCode();
}
