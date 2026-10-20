#include "backend/services/SerialPortClaims.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace backend::services::portclaims {

namespace {

enum class Kind { Service, Probe };

struct Entry {
    uint64_t id;
    Kind kind;
    std::string owner;
    std::string port; // key
    std::thread::id thread; // the claiming thread (a probe does not block its own thread)
    bool busSession{false};
};

struct Registry {
    std::mutex mutex;
    std::condition_variable released;
    uint64_t nextId{1};
    std::map<uint64_t, Entry> claims;
    std::map<std::string, std::set<std::string>> reserved; // owner -> keys
};

Registry& registry()
{
    static Registry r;
    return r;
}

// `exceptThread`: ignore claims that thread holds.
bool hasLocked(const Registry& r, const std::string& k, Kind kind, std::string* who = nullptr,
               std::thread::id exceptThread = std::thread::id())
{
    for (const auto& [id, e] : r.claims) {
        if (e.port == k && e.kind == kind && (exceptThread == std::thread::id() || e.thread != exceptThread)) {
            if (who) *who = e.owner;
            return true;
        }
    }
    return false;
}

void releaseId(uint64_t id)
{
    if (id == 0) return;
    auto& r = registry();
    {
        std::lock_guard<std::mutex> lock(r.mutex);
        r.claims.erase(id);
    }
    r.released.notify_all();
}

} // namespace

std::string key(const std::string& port)
{
    std::string k = port;
    k.erase(0, k.find_first_not_of(" \t"));
    if (const auto end = k.find_last_not_of(" \t"); end != std::string::npos) k.erase(end + 1);
    if (k.rfind("\\\\.\\", 0) == 0) k.erase(0, 4);
    if (const auto slash = k.find_last_of('/'); slash != std::string::npos) k = k.substr(slash + 1);
    std::transform(k.begin(), k.end(), k.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return k;
}

Claim::~Claim() { release(); }

Claim::Claim(Claim&& other) noexcept : id_(other.id_) { other.id_ = 0; }

Claim& Claim::operator=(Claim&& other) noexcept
{
    if (this != &other) {
        release();
        id_ = other.id_;
        other.id_ = 0;
    }
    return *this;
}

void Claim::release()
{
    releaseId(id_);
    id_ = 0;
}

Claim tryProbe(const std::string& port, const std::string& owner, std::string* why,
               std::chrono::milliseconds waitForProbes, Holder* heldBy)
{
    const std::string k = key(port);
    const auto self = std::this_thread::get_id();
    auto& r = registry();
    std::unique_lock<std::mutex> lock(r.mutex);
    if (heldBy) *heldBy = Holder::None;
    const auto refusedBy = [&]() {
        for (const auto& [id, e] : r.claims) {
            if (e.port == k && e.kind == Kind::Service) {
                if (why) *why = port + " is in use by " + e.owner;
                if (heldBy) *heldBy = e.busSession ? Holder::BusSession : Holder::Service;
                return true;
            }
        }
        for (const auto& [reserver, keys] : r.reserved) {
            if (keys.count(k)) {
                if (why) *why = port + " is reserved for " + reserver;
                if (heldBy) *heldBy = Holder::Reserved;
                return true;
            }
        }
        return false;
    };
    if (refusedBy()) return Claim{};
    std::string prober;
    if (hasLocked(r, k, Kind::Probe, &prober, self)) {
        const bool freed = waitForProbes.count() > 0 &&
                           r.released.wait_for(lock, waitForProbes, [&] { return !hasLocked(r, k, Kind::Probe, nullptr, self); });
        if (!freed) {
            if (why) *why = port + " is being probed by " + prober;
            if (heldBy) *heldBy = Holder::Probe;
            return Claim{};
        }
        if (refusedBy()) return Claim{}; // a service took it meanwhile
    }
    const uint64_t id = r.nextId++;
    r.claims[id] = Entry{id, Kind::Probe, owner, k, self, false};
    return Claim{id};
}

Claim claimForService(const std::string& port, const std::string& owner, std::chrono::milliseconds maxWait,
                      bool busSession)
{
    const std::string k = key(port);
    const auto self = std::this_thread::get_id();
    auto& r = registry();
    std::unique_lock<std::mutex> lock(r.mutex);
    // Registered first: from here on tryProbe refuses new probes on the port.
    const uint64_t id = r.nextId++;
    r.claims[id] = Entry{id, Kind::Service, owner, k, self, busSession};
    std::string prober;
    if (hasLocked(r, k, Kind::Probe, &prober, self)) {
        SPDLOG_INFO("SerialPortClaims: {} waits for the {} on {}", owner, prober, port);
        const bool freed = r.released.wait_for(lock, maxWait, [&] { return !hasLocked(r, k, Kind::Probe, nullptr, self); });
        if (!freed) {
            SPDLOG_WARN("SerialPortClaims: the {} still holds {} after {} ms; {} opens anyway",
                        prober, port, maxWait.count(), owner);
        }
    }
    return Claim{id};
}

void setReserved(const std::string& owner, const std::vector<std::string>& ports)
{
    auto& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    std::set<std::string> keys;
    for (const auto& p : ports) {
        if (!p.empty()) keys.insert(key(p));
    }
    if (keys.empty()) r.reserved.erase(owner);
    else r.reserved[owner] = std::move(keys);
}

bool isReserved(const std::string& port, std::string* forWhom)
{
    const std::string k = key(port);
    auto& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    for (const auto& [reserver, keys] : r.reserved) {
        if (keys.count(k)) {
            if (forWhom) *forWhom = reserver;
            return true;
        }
    }
    return false;
}

Holder holder(const std::string& port)
{
    const std::string k = key(port);
    auto& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    Holder found = Holder::None;
    for (const auto& [id, e] : r.claims) {
        if (e.port != k) continue;
        if (e.kind == Kind::Service) return e.busSession ? Holder::BusSession : Holder::Service;
        found = Holder::Probe;
    }
    return found;
}

std::string describe(const std::string& port)
{
    const std::string k = key(port);
    auto& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    std::string out;
    for (const auto& [id, e] : r.claims) {
        if (e.port != k) continue;
        if (!out.empty()) out += ", ";
        out += (e.kind == Kind::Service ? "service:" : "probe:") + e.owner;
    }
    return out;
}

} // namespace backend::services::portclaims
