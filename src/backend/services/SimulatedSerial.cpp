#include "backend/services/SimulatedSerial.h"

#include "backend/services/ModbusRtu.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <condition_variable>
#include <deque>
#include <set>
#include <sstream>

namespace backend::services::serialsim {

namespace modbus = backend::services::modbus;

namespace {

#if defined(_WIN32)
constexpr int kErrAccessDenied = 5;    // ERROR_ACCESS_DENIED
constexpr int kErrNotFound = 2;        // ERROR_FILE_NOT_FOUND
constexpr int kErrDeviceRemoved = 22;  // ERROR_BAD_COMMAND, what a pulled USB adapter gives
#else
constexpr int kErrAccessDenied = EACCES;
constexpr int kErrNotFound = ENOENT;
constexpr int kErrDeviceRemoved = ENODEV;
#endif

std::mutex gInstallMutex;
std::shared_ptr<Rig> gInstalled;

std::string threadName(std::thread::id id)
{
    std::ostringstream out;
    out << id;
    return out.str();
}

long long msBetween(Clock::time_point from, Clock::time_point to)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
}

// A short protocol fingerprint of one request, for the ledger.
std::string fingerprint(const Bytes& frame)
{
    if (frame.size() >= 4 && modbus::responseCrcValid(frame)) {
        return "modbus addr=" + std::to_string(frame[0]) + " fc=" + std::to_string(frame[1]);
    }
    if (!frame.empty() && frame.front() == '/') {
        std::string text;
        for (const uint8_t c : frame) {
            if (c == '\r') text += "\\r";
            else if (std::isprint(c)) text += static_cast<char>(c);
            else text += '.';
        }
        return "ascii '" + text + "'";
    }
    return std::to_string(frame.size()) + " unframed bytes";
}

Bytes exceptionFrame(uint8_t address, uint8_t function, uint8_t code)
{
    Bytes reply{address, static_cast<uint8_t>(function | 0x80), code};
    modbus::appendCrc(reply);
    return reply;
}

void putFloat(std::map<uint16_t, uint16_t>& regs, uint16_t at, float value)
{
    const auto bytes = modbus::floatToRegisters(value);
    regs[at] = static_cast<uint16_t>((bytes[0] << 8) | bytes[1]);
    regs[static_cast<uint16_t>(at + 1)] = static_cast<uint16_t>((bytes[2] << 8) | bytes[3]);
}

std::string oeabtFrame(const std::string& payload)
{
    std::string reply{"\xff\x1f\x30\x60", 4};
    reply += payload;
    reply += "\x03\r\n";
    return reply;
}

} // namespace

// ---------------------------------------------------------------------------
// Devices
// ---------------------------------------------------------------------------

ModbusRegisterDevice::ModbusRegisterDevice(std::string kind, uint8_t address, int baudRate)
    : kind_(std::move(kind)), address_(address), baudRate_(baudRate)
{
}

bool ModbusRegisterDevice::understands(const Bytes& frame) const
{
    return frame.size() >= 4 && modbus::responseCrcValid(frame);
}

std::optional<Bytes> ModbusRegisterDevice::onFrame(const Bytes& q)
{
    if (!understands(q) || q[0] != address_ || q.size() < 8) return std::nullopt;
    const uint8_t function = q[1];
    const auto start = static_cast<uint16_t>((q[2] << 8) | q[3]);
    const auto count = static_cast<uint16_t>((q[4] << 8) | q[5]);

    if (function == modbus::kFuncReadHolding || function == modbus::kFuncReadInput) {
        if (count == 0 || count > 125) return exceptionFrame(address_, function, 3);
        const auto& regs = function == modbus::kFuncReadHolding ? holding : input;
        Bytes reply{address_, function, static_cast<uint8_t>(count * 2)};
        for (uint16_t i = 0; i < count; ++i) {
            const auto it = regs.find(static_cast<uint16_t>(start + i));
            const uint16_t value = it == regs.end() ? 0 : it->second;
            reply.push_back(static_cast<uint8_t>(value >> 8));
            reply.push_back(static_cast<uint8_t>(value & 0xff));
        }
        modbus::appendCrc(reply);
        return reply;
    }
    if (function == modbus::kFuncWriteSingle) {
        holding[start] = count; // FC06 carries the value where FC03 carries the count
        ++writeCount;
        return q;
    }
    if (function == modbus::kFuncWriteMultiple) {
        if (q.size() < 9u + q[6] || q[6] != count * 2) return exceptionFrame(address_, function, 3);
        for (uint16_t i = 0; i < count; ++i) {
            holding[static_cast<uint16_t>(start + i)] =
                static_cast<uint16_t>((q[7 + i * 2] << 8) | q[8 + i * 2]);
        }
        ++writeCount;
        Bytes reply(q.begin(), q.begin() + 6);
        modbus::appendCrc(reply);
        return reply;
    }
    return exceptionFrame(address_, function, 1); // illegal function
}

bool OeabtDevice::understands(const Bytes& frame) const
{
    return frame.size() >= 3 && frame.front() == '/' && frame.back() == '\r';
}

std::optional<Bytes> OeabtDevice::onFrame(const Bytes& frame)
{
    if (!understands(frame)) return std::nullopt;
    const std::string command(frame.begin(), frame.end());
    std::string reply;
    if (command == "/1&\r" || command == "/1&R\r") {
        // V0.5.4 prints a diagnostic line before the identity frame.
        reply = "hal_pwm_io_set_vol_input_mode 0\n" + oeabtFrame("Oeabt pzt controller");
    } else if (command == "/1?aVtmR\r") {
        reply = oeabtFrame("100000,0,0,0");
    } else if (command == "/1?aSR\r") {
        reply = oeabtFrame("60,0,0,0");
    } else if (command == "/1?et1R\r" || command == "/1?et2R\r") {
        reply = oeabtFrame("1");
    } else if (command == "/1?aVtR\r") {
        reply = oeabtFrame(std::to_string(voltageMv) + ",0,0,0");
    } else if (command.rfind("/1aM1Vt", 0) == 0) {
        voltageMv = std::atoi(command.c_str() + 7);
        reply = oeabtFrame("");
    } else if (command.rfind("/1", 0) == 0) {
        reply = oeabtFrame("");
    } else {
        return std::nullopt; // another controller address
    }
    return Bytes(reply.begin(), reply.end());
}

std::unique_ptr<ModbusRegisterDevice> makePulseGenerator(uint8_t address, int baudRate)
{
    auto device = std::make_unique<ModbusRegisterDevice>("pulse-generator", address, baudRate);
    // Channel 1 at 5 kHz (Hz x100 as u32 hi/lo), 50.00 % duty; the rest 0 Hz,
    // as a module that was set up once and power-cycled.
    const uint32_t frequencyRaw = 500000;
    device->holding[0] = static_cast<uint16_t>(frequencyRaw >> 16);
    device->holding[1] = static_cast<uint16_t>(frequencyRaw & 0xffff);
    device->holding[2] = 5000;
    for (uint16_t r = 3; r < 12; ++r) device->holding[r] = 0;
    return device;
}

std::unique_ptr<ModbusRegisterDevice> makeDlspPump(uint8_t address, int baudRate)
{
    auto device = std::make_unique<ModbusRegisterDevice>("dlsp-pump", address, baudRate);
    putFloat(device->holding, 0x004A, 0.001f); // min flow rate
    putFloat(device->holding, 0x004C, 100.0f); // max flow rate
    return device;
}

std::unique_ptr<ModbusRegisterDevice> makeTushuiPump(uint8_t address, int baudRate)
{
    auto device = std::make_unique<ModbusRegisterDevice>("tushui-pump", address, baudRate);
    device->holding[100] = 1000; // 10.00 rpm
    device->holding[101] = 1;    // counter-clockwise
    device->holding[102] = 0;
    device->holding[103] = 0;
    device->holding[104] = 0;    // stopped
    device->holding[105] = static_cast<uint16_t>(static_cast<uint32_t>(baudRate) >> 16);
    device->holding[106] = static_cast<uint16_t>(static_cast<uint32_t>(baudRate) & 0xffff);
    device->holding[107] = address;
    return device;
}

std::unique_ptr<ModbusRegisterDevice> makeZc300(uint8_t address, int32_t serial)
{
    auto device = std::make_unique<ModbusRegisterDevice>("zc300", address, 115200);
    // Manual register numbers are wire address + 1; 30001.. are FC04 inputs.
    const std::string model = "ZC300-1A";
    for (uint16_t i = 0; i < 7; ++i) {
        const auto at = [&](size_t k) { return k < model.size() ? static_cast<uint8_t>(model[k]) : 0; };
        device->input[static_cast<uint16_t>(30000 + i)] =
            static_cast<uint16_t>((at(i * 2u) << 8) | at(i * 2u + 1));
    }
    device->input[30007] = static_cast<uint16_t>(static_cast<uint32_t>(serial) >> 16);
    device->input[30008] = static_cast<uint16_t>(static_cast<uint32_t>(serial) & 0xffff);
    device->input[30009] = 12; // firmware
    return device;
}

// ---------------------------------------------------------------------------
// Rig
// ---------------------------------------------------------------------------

struct Rig::PortState {
    PortSpec spec;
    std::string key;
    bool unplugged{false};
    std::vector<std::shared_ptr<SimDevice>> devices;
    std::vector<Holder> holders;
    std::set<const void*> staleHandles; // open across an unplug: I/O fails
};

const char* toString(Event::Kind kind)
{
    switch (kind) {
    case Event::Kind::Opened: return "Opened";
    case Event::Kind::OpenDenied: return "OpenDenied";
    case Event::Kind::SharedOpen: return "SharedOpen";
    case Event::Kind::OpenFailed: return "OpenFailed";
    case Event::Kind::Closed: return "Closed";
    case Event::Kind::ForeignFrame: return "ForeignFrame";
    case Event::Kind::BaudMismatch: return "BaudMismatch";
    case Event::Kind::Unplugged: return "Unplugged";
    case Event::Kind::Replugged: return "Replugged";
    }
    return "?";
}

Rig::Rig(uint64_t seed) : seed_(seed), start_(Clock::now()), rng_(seed) {}
Rig::~Rig() = default;

std::string Rig::key(const std::string& portName)
{
    std::string name = portName;
    for (const std::string prefix : {"\\\\.\\", "/dev/"}) {
        if (name.rfind(prefix, 0) == 0) name.erase(0, prefix.size());
    }
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return name;
}

void Rig::setExclusiveOpen(bool exclusive)
{
    std::lock_guard<std::mutex> lock(mutex_);
    exclusive_ = exclusive;
}

void Rig::addPort(PortSpec spec)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto state = std::make_unique<PortState>();
    state->key = key(spec.systemName);
    state->spec = std::move(spec);
    ports_.push_back(std::move(state));
}

void Rig::addDevice(const std::string& port, std::shared_ptr<SimDevice> device)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto* state = findLocked(port)) state->devices.push_back(std::move(device));
}

Rig::PortState* Rig::findLocked(const std::string& portName)
{
    const std::string k = key(portName);
    for (auto& port : ports_) {
        if (port->key == k) return port.get();
    }
    return nullptr;
}

const Rig::PortState* Rig::findLocked(const std::string& portName) const
{
    return const_cast<Rig*>(this)->findLocked(portName);
}

bool Rig::presentLocked(const PortState& port, Clock::time_point now) const
{
    const auto t = msBetween(start_, now);
    if (port.unplugged || t < port.spec.presentAfterMs) return false;
    for (const auto& [from, to] : port.spec.unpluggedWindowsMs) {
        if (t >= from && (to < 0 || t < to)) return false;
    }
    return true;
}

bool Rig::staleLocked(const PortState& port, const void* handle, Clock::time_point now) const
{
    if (port.staleHandles.count(handle)) return true;
    for (const auto& holder : port.holders) {
        if (holder.handle != handle) continue;
        const auto openedAt = msBetween(start_, holder.since);
        const auto t = msBetween(start_, now);
        for (const auto& window : port.spec.unpluggedWindowsMs) {
            if (window.first >= openedAt && window.first <= t) return true;
        }
    }
    return false;
}

void Rig::recordLocked(Event::Kind kind, const std::string& port, std::string detail)
{
    const auto thread = std::this_thread::get_id();
    if (kind == Event::Kind::Opened || kind == Event::Kind::Closed) {
        SPDLOG_DEBUG("serial-sim: {} {} [{}] {}", toString(kind), port, threadName(thread), detail);
    } else {
        SPDLOG_WARN("serial-sim: {} {} [{}] {}", toString(kind), port, threadName(thread), detail);
    }
    events_.push_back(Event{kind, port, std::move(detail), Clock::now(), thread});
}

int Rig::latencyLocked(const PortState& port)
{
    const int lo = std::max(0, port.spec.replyLatencyMinMs);
    const int hi = std::max(lo, port.spec.replyLatencyMaxMs);
    return std::uniform_int_distribution<int>(lo, hi)(rng_);
}

void Rig::unplug(const std::string& port)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto* state = findLocked(port);
    if (!state || state->unplugged) return;
    state->unplugged = true;
    for (const auto& holder : state->holders) state->staleHandles.insert(holder.handle);
    recordLocked(Event::Kind::Unplugged, state->spec.systemName,
                 std::to_string(state->holders.size()) + " handle(s) left open");
}

void Rig::replug(const std::string& port)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto* state = findLocked(port);
    if (!state || !state->unplugged) return;
    state->unplugged = false;
    recordLocked(Event::Kind::Replugged, state->spec.systemName, {});
}

std::vector<SerialPortInfo> Rig::enumerate() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = Clock::now();
    std::vector<SerialPortInfo> out;
    for (const auto& port : ports_) {
        if (!presentLocked(*port, now)) continue;
        SerialPortInfo info;
        info.systemName = port->spec.systemName;
#if defined(_WIN32)
        info.systemLocation = "\\\\.\\" + port->spec.systemName;
#else
        info.systemLocation = "/dev/" + port->spec.systemName;
#endif
        info.description = port->spec.description;
        info.manufacturer = port->spec.manufacturer;
        info.serialNumber = port->spec.serialNumber;
        info.vendorId = port->spec.vendorId;
        info.productId = port->spec.productId;
        out.push_back(std::move(info));
    }
    return out;
}

std::vector<int> Rig::comPortNumbers() const
{
    std::vector<int> out;
    for (const auto& info : enumerate()) {
        const std::string k = key(info.systemName);
        if (k.size() > 3 && k.rfind("COM", 0) == 0 &&
            std::all_of(k.begin() + 3, k.end(), [](unsigned char c) { return std::isdigit(c); })) {
            out.push_back(std::stoi(k.substr(3)));
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<Event> Rig::events() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return events_;
}

std::vector<Event> Rig::events(Event::Kind kind) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Event> out;
    std::copy_if(events_.begin(), events_.end(), std::back_inserter(out),
                 [&](const Event& e) { return e.kind == kind; });
    return out;
}

int Rig::contentionCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(std::count_if(events_.begin(), events_.end(), [](const Event& e) {
        return e.kind == Event::Kind::OpenDenied || e.kind == Event::Kind::SharedOpen;
    }));
}

int Rig::holders(const std::string& port) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto* state = findLocked(port);
    return state ? static_cast<int>(state->holders.size()) : 0;
}

std::string Rig::summary() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<Event::Kind, int> counts;
    for (const auto& e : events_) ++counts[e.kind];
    std::ostringstream out;
    out << "seed=" << seed_;
    for (const auto& [kind, n] : counts) out << ' ' << toString(kind) << '=' << n;
    for (const auto& e : events_) {
        if (e.kind == Event::Kind::Opened || e.kind == Event::Kind::Closed) continue;
        out << "\n  +" << msBetween(start_, e.at) << "ms " << toString(e.kind) << ' ' << e.port
            << " [" << threadName(e.thread) << "] " << e.detail;
    }
    return out.str();
}

// ---------------------------------------------------------------------------
// The port a component holds
// ---------------------------------------------------------------------------

class SimPort final : public ISerialPort {
public:
    explicit SimPort(std::shared_ptr<Rig> rig) : rig_(std::move(rig)) {}
    ~SimPort() override { close(); }

    bool open(int comPort, int baudRate) override
    {
        SerialSettings settings;
        settings.baudRate = baudRate;
        return openNamed("COM" + std::to_string(comPort), settings);
    }

    bool openNamed(const std::string& systemName, const SerialSettings& settings) override
    {
        close();
        int openLatencyMs = 0;
        {
            std::lock_guard<std::mutex> lock(rig_->mutex_);
            if (const auto* state = rig_->findLocked(systemName)) openLatencyMs = state->spec.openLatencyMs;
        }
        // Outside the lock: a slow CreateFile delays only this caller.
        if (openLatencyMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(openLatencyMs));

        std::lock_guard<std::mutex> lock(rig_->mutex_);
        const auto now = Clock::now();
        auto* state = rig_->findLocked(systemName);
        if (!state || !rig_->presentLocked(*state, now)) {
            systemError_ = kErrNotFound;
            error_ = "open " + systemName + ": no such device (simulated)";
            rig_->recordLocked(Event::Kind::OpenFailed, systemName,
                               !state ? "unknown port" : "unplugged or not yet enumerated");
            return false;
        }
        std::vector<const Rig::Holder*> live;
        for (const auto& holder : state->holders) {
            if (!rig_->staleLocked(*state, holder.handle, now)) live.push_back(&holder);
        }
        if (!live.empty()) {
            const auto* other = live.front();
            const std::string detail = "held by [" + threadName(other->thread) + "] for " +
                                       std::to_string(msBetween(other->since, now)) + "ms (" +
                                       (other->firstFrame.empty() ? "no traffic yet" : other->firstFrame) + ")";
            if (rig_->exclusive_) {
                systemError_ = kErrAccessDenied;
                error_ = "open " + systemName + ": Access is denied (simulated; " + detail + ")";
                rig_->recordLocked(Event::Kind::OpenDenied, state->spec.systemName, detail);
                return false;
            }
            rig_->recordLocked(Event::Kind::SharedOpen, state->spec.systemName, detail);
        }
        state->holders.push_back(Rig::Holder{this, std::this_thread::get_id(), now, {}});
        port_ = state;
        settings_ = settings;
        openedAt_ = now;
        systemError_ = 0;
        error_.clear();
        rig_->recordLocked(Event::Kind::Opened, state->spec.systemName,
                           std::to_string(settings.baudRate) + " baud");
        return true;
    }

    void close() override
    {
        std::lock_guard<std::mutex> lock(rig_->mutex_);
        if (!port_) return;
        auto& holders = port_->holders;
        holders.erase(std::remove_if(holders.begin(), holders.end(),
                                     [this](const Rig::Holder& h) { return h.handle == this; }),
                      holders.end());
        port_->staleHandles.erase(this);
        rig_->recordLocked(Event::Kind::Closed, port_->spec.systemName, {});
        port_ = nullptr;
        pending_.clear();
        mismatched_.clear();
    }

    bool isOpen() const override
    {
        std::lock_guard<std::mutex> lock(rig_->mutex_);
        return port_ != nullptr;
    }

    int write(const std::vector<uint8_t>& data) override
    {
        std::lock_guard<std::mutex> lock(rig_->mutex_);
        if (!port_) {
            error_ = "write: port not open";
            return -1;
        }
        const auto now = Clock::now();
        if (rig_->staleLocked(*port_, this, now)) {
            systemError_ = kErrDeviceRemoved;
            error_ = "write " + port_->spec.systemName + ": device removed (simulated unplug)";
            return -1;
        }
        for (auto& holder : port_->holders) {
            if (holder.handle == this && holder.firstFrame.empty()) holder.firstFrame = fingerprint(data);
        }
        if (msBetween(openedAt_, now) < port_->spec.silentAfterOpenMs) {
            return static_cast<int>(data.size()); // adapter still booting: frame lost
        }

        bool understood = false;
        Bytes reply;
        for (const auto& device : port_->devices) {
            if (device->baudRate() != settings_.baudRate) {
                if (mismatched_.insert(device.get()).second) {
                    rig_->recordLocked(Event::Kind::BaudMismatch, port_->spec.systemName,
                                       device->kind() + " listens at " + std::to_string(device->baudRate()) +
                                           ", frame sent at " + std::to_string(settings_.baudRate));
                }
                understood = true; // line noise to it, not a protocol violation
                continue;
            }
            if (!device->understands(data)) continue;
            understood = true;
            if (auto bytes = device->onFrame(data)) {
                // Two answers on one bus collide on the wire: concatenate.
                reply.insert(reply.end(), bytes->begin(), bytes->end());
            }
        }
        if (!understood && !port_->devices.empty()) {
            rig_->recordLocked(Event::Kind::ForeignFrame, port_->spec.systemName,
                               fingerprint(data) + " understood by none of " +
                                   std::to_string(port_->devices.size()) + " device(s)");
        }
        if (!reply.empty()) {
            const auto at = now + std::chrono::milliseconds(rig_->latencyLocked(*port_));
            if (port_->spec.fragmentReplies && reply.size() > 1) {
                const auto half = reply.begin() + static_cast<std::ptrdiff_t>(reply.size() / 2);
                pending_.push_back({at, Bytes(reply.begin(), half)});
                pending_.push_back({at + std::chrono::milliseconds(2), Bytes(half, reply.end())});
            } else {
                pending_.push_back({at, std::move(reply)});
            }
        }
        return static_cast<int>(data.size());
    }

    bool waitForBytesWritten(int) override { return isOpen(); }

    bool waitForReadyRead(int timeoutMs) override
    {
        std::unique_lock<std::mutex> lock(rig_->mutex_);
        const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(0, timeoutMs));
        for (;;) {
            const auto now = Clock::now();
            if (dueLocked(now)) return true;
            if (now >= deadline) return false;
            auto wakeAt = deadline;
            for (const auto& [at, bytes] : pending_) wakeAt = std::min(wakeAt, at);
            // Only this handle's own write() queues replies, so a timed wait
            // is enough; nothing needs to notify.
            wake_.wait_until(lock, wakeAt);
        }
    }

    std::vector<uint8_t> readAll() override
    {
        std::lock_guard<std::mutex> lock(rig_->mutex_);
        const auto now = Clock::now();
        Bytes out;
        while (!pending_.empty() && pending_.front().first <= now) {
            out.insert(out.end(), pending_.front().second.begin(), pending_.front().second.end());
            pending_.pop_front();
        }
        return out;
    }

    std::string lastError() const override
    {
        std::lock_guard<std::mutex> lock(rig_->mutex_);
        return error_;
    }
    int lastSystemError() const override
    {
        std::lock_guard<std::mutex> lock(rig_->mutex_);
        return systemError_;
    }

private:
    bool dueLocked(Clock::time_point now) const
    {
        return !pending_.empty() && pending_.front().first <= now;
    }

    std::shared_ptr<Rig> rig_;
    Rig::PortState* port_{nullptr};
    SerialSettings settings_;
    Clock::time_point openedAt_{};
    std::deque<std::pair<Clock::time_point, Bytes>> pending_;
    std::set<const SimDevice*> mismatched_;
    std::condition_variable wake_;
    std::string error_;
    int systemError_{0};
};

std::unique_ptr<ISerialPort> Rig::makePort()
{
    return std::make_unique<SimPort>(shared_from_this());
}

void install(std::shared_ptr<Rig> rig)
{
    std::lock_guard<std::mutex> lock(gInstallMutex);
    if (rig && !gInstalled) {
        SPDLOG_WARN("serial-sim: SIMULATED serial hardware installed (seed {}); no real COM port is used",
                    rig->seed());
    } else if (!rig && gInstalled) {
        SPDLOG_INFO("serial-sim: uninstalled; {}", gInstalled->summary());
    }
    gInstalled = std::move(rig);
}

std::shared_ptr<Rig> installed()
{
    std::lock_guard<std::mutex> lock(gInstallMutex);
    return gInstalled;
}

std::shared_ptr<Rig> makeBenchRig(uint64_t seed)
{
    auto rig = std::make_shared<Rig>(seed);

    PortSpec rs485;
    rs485.systemName = "COM3";
    rs485.description = "USB-SERIAL CH340 (RS485 pump bus)";
    rs485.vendorId = 0x1a86;
    rs485.productId = 0x7523;
    rs485.presentAfterMs = 300;
    rs485.openLatencyMs = 15;
    rs485.replyLatencyMinMs = 2;
    rs485.replyLatencyMaxMs = 12;
    rig->addPort(rs485);
    rig->addDevice("COM3", makeDlspPump(1));
    rig->addDevice("COM3", makeDlspPump(2));

    PortSpec generator;
    generator.systemName = "COM4";
    generator.description = "FT232R USB UART (pulse generator)";
    generator.vendorId = 0x0403;
    generator.productId = 0x6001;
    generator.serialNumber = "SIMPG001";
    generator.openLatencyMs = 10;
    rig->addPort(generator);
    rig->addDevice("COM4", makePulseGenerator(1));

    PortSpec nano;
    nano.systemName = "COM5";
    nano.description = "USB-SERIAL CH340 (OEABT nanopositioner)";
    nano.vendorId = 0x1a86;
    nano.productId = 0x7523;
    nano.presentAfterMs = 1200; // enumerates late: the first startup attempt misses it
    nano.openLatencyMs = 25;
    nano.silentAfterOpenMs = 150;
    nano.replyLatencyMinMs = 3;
    nano.replyLatencyMaxMs = 20;
    nano.fragmentReplies = true;
    rig->addPort(nano);
    rig->addDevice("COM5", std::make_shared<OeabtDevice>());

    PortSpec stage;
    stage.systemName = "COM6";
    stage.description = "FT231X USB UART (ZC300 Z stage)";
    stage.vendorId = 0x0403;
    stage.productId = 0x6015;
    stage.serialNumber = "SIMZC300";
    rig->addPort(stage);
    rig->addDevice("COM6", makeZc300(1));
    return rig;
}

} // namespace backend::services::serialsim
