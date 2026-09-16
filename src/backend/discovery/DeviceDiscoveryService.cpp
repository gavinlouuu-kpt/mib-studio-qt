#include "backend/discovery/DeviceDiscoveryService.h"

#include <algorithm>
#include <exception>
#include <set>
#include <sstream>
#include <utility>

#include <spdlog/spdlog.h>

namespace backend::discovery {

// ---- enum names -----------------------------------------------------------

const char* toString(DeviceKind kind)
{
    switch (kind) {
    case DeviceKind::Camera: return "camera";
    case DeviceKind::Framegrabber: return "framegrabber";
    case DeviceKind::Nanopositioner: return "nanopositioner";
    case DeviceKind::PulseGenerator: return "pulse-generator";
    }
    return "unknown";
}

const char* toString(JobState state)
{
    switch (state) {
    case JobState::Queued: return "Queued";
    case JobState::Running: return "Running";
    case JobState::Completed: return "Completed";
    case JobState::Cancelled: return "Cancelled";
    case JobState::Failed: return "Failed";
    }
    return "Unknown";
}

const char* toString(IdentityStrength strength)
{
    switch (strength) {
    case IdentityStrength::None: return "None";
    case IdentityStrength::SessionLocal: return "SessionLocal";
    case IdentityStrength::Persistent: return "Persistent";
    }
    return "Unknown";
}

const char* toString(IdentificationStatus status)
{
    switch (status) {
    case IdentificationStatus::Identified: return "Identified";
    case IdentificationStatus::Unidentified: return "Unidentified";
    case IdentificationStatus::Ambiguous: return "Ambiguous";
    case IdentificationStatus::Unsupported: return "Unsupported";
    }
    return "Unknown";
}

const char* toString(ErrorKind kind)
{
    switch (kind) {
    case ErrorKind::None: return "None";
    case ErrorKind::InvalidRequest: return "InvalidRequest";
    case ErrorKind::Busy: return "Busy";
    case ErrorKind::OpenFailed: return "OpenFailed";
    case ErrorKind::PermissionDenied: return "PermissionDenied";
    case ErrorKind::Timeout: return "Timeout";
    case ErrorKind::MalformedResponse: return "MalformedResponse";
    case ErrorKind::Unsupported: return "Unsupported";
    case ErrorKind::MissingSdk: return "MissingSdk";
    case ErrorKind::ProviderException: return "ProviderException";
    case ErrorKind::Cancelled: return "Cancelled";
    case ErrorKind::Overflow: return "Overflow";
    case ErrorKind::ShuttingDown: return "ShuttingDown";
    case ErrorKind::TooManyJobs: return "TooManyJobs";
    }
    return "Unknown";
}

// ---- internals ------------------------------------------------------------

struct DeviceDiscoveryService::Job {
    std::uint64_t id{0};
    DiscoveryRequest request;
    std::string key;
    std::atomic<bool> cancel{false};
    DiscoverySnapshot snapshot;   // guarded by service mutex_
    bool done{false};             // terminal state published (mutex_)
    std::atomic<bool> exited{false}; // worker function returned; safe to join
    std::thread worker;           // joined by reap/shutdown/destructor
};

struct DeviceDiscoveryService::ObserverEntry {
    Observer fn;
    int inflight{0}; // guarded by observersMutex_
};

namespace {

std::string endpointKey(const DiscoveredDevice& d)
{
    std::ostringstream os;
    os << toString(d.kind) << '|' << d.endpoint.systemPath << '|' << d.endpoint.sdkIndex << '|'
       << d.endpoint.interfaceIndex << '/' << d.endpoint.deviceIndex << '/'
       << d.endpoint.streamIndex << '|' << d.endpoint.busAddress;
    return os.str();
}

std::string identityKey(const DiscoveredDevice& d)
{
    if (d.identityStrength == IdentityStrength::Persistent && !d.stableIdentity.empty()) {
        return d.providerId + "|id|" + d.stableIdentity;
    }
    return d.providerId + "|ep|" + endpointKey(d);
}

// Provider-aware dedup, cross-provider ambiguity, candidate bound.
void consolidate(std::vector<DiscoveredDevice>& candidates, std::vector<DiscoveryError>& errors,
                 bool& overflow)
{
    std::vector<DiscoveredDevice> out;
    std::map<std::string, std::size_t> byIdentity;
    for (auto& c : candidates) {
        const auto key = identityKey(c);
        const auto it = byIdentity.find(key);
        if (it != byIdentity.end()) {
            // Same physical device seen twice (changed SDK index/OS path, or
            // two vendor protocols on one adapter): keep one entry, union the
            // claims, and never let a merge hide a conflict.
            auto& keep = out[it->second];
            keep.diagnostics.insert(keep.diagnostics.end(), c.diagnostics.begin(),
                                    c.diagnostics.end());
            bool claimsDiffer = false;
            for (const auto& claim : c.claimedBy) {
                if (std::find(keep.claimedBy.begin(), keep.claimedBy.end(), claim) ==
                    keep.claimedBy.end()) {
                    keep.claimedBy.push_back(claim);
                    claimsDiffer = true;
                }
            }
            if (c.identification == IdentificationStatus::Ambiguous ||
                (claimsDiffer && keep.identification == IdentificationStatus::Identified &&
                 c.identification == IdentificationStatus::Identified)) {
                keep.identification = IdentificationStatus::Ambiguous;
            } else if (keep.identification == IdentificationStatus::Unidentified &&
                       c.identification == IdentificationStatus::Identified) {
                keep.identification = IdentificationStatus::Identified;
            }
            continue;
        }
        byIdentity.emplace(key, out.size());
        out.push_back(std::move(c));
    }

    // More than one identified candidate for one physical endpoint (same
    // kind, same path/address) is a conflict, whether the claims come from
    // different providers or from one provider's vendor protocols: mark them
    // ambiguous and list every claimant instead of silently choosing.
    std::map<std::string, std::size_t> identifiedAt;
    std::map<std::string, std::set<std::string>> claims;
    for (const auto& c : out) {
        if (c.endpoint.systemPath.empty()) continue;
        if (c.identification != IdentificationStatus::Identified &&
            c.identification != IdentificationStatus::Ambiguous) {
            continue;
        }
        ++identifiedAt[endpointKey(c)];
        claims[endpointKey(c)].insert(c.providerId);
        for (const auto& claim : c.claimedBy) claims[endpointKey(c)].insert(claim);
    }
    for (auto& c : out) {
        if (c.endpoint.systemPath.empty()) continue;
        const auto key = endpointKey(c);
        const auto n = identifiedAt.find(key);
        if (n == identifiedAt.end() || n->second < 2) continue;
        if (c.identification == IdentificationStatus::Identified) {
            c.identification = IdentificationStatus::Ambiguous;
        }
        const auto& set = claims[key];
        c.claimedBy.assign(set.begin(), set.end());
    }
    for (auto& c : out) {
        if (c.claimedBy.empty() && c.identification == IdentificationStatus::Identified) {
            c.claimedBy.push_back(c.providerId);
        }
    }

    if (out.size() > kMaxCandidates) {
        const auto dropped = out.size() - kMaxCandidates;
        out.resize(kMaxCandidates);
        overflow = true;
        errors.push_back({"", ErrorKind::Overflow,
                          "candidate limit reached; " + std::to_string(dropped) +
                              " candidate(s) not retained",
                          ""});
    }
    candidates = std::move(out);
}

void boundErrors(std::vector<DiscoveryError>& errors)
{
    if (errors.size() <= kMaxErrors) return;
    const auto dropped = errors.size() - kMaxErrors;
    errors.resize(kMaxErrors);
    errors.push_back({"", ErrorKind::Overflow,
                      "diagnostic limit reached; " + std::to_string(dropped) +
                          " error(s) not retained",
                      ""});
}

} // namespace

// ---- lifecycle -------------------------------------------------------------

DeviceDiscoveryService::DeviceDiscoveryService() = default;

DeviceDiscoveryService::~DeviceDiscoveryService()
{
    shutdownDiscovery();
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto& [id, job] : jobs_) {
            if (job->worker.joinable() && job->worker.get_id() != std::this_thread::get_id()) {
                threads.push_back(std::move(job->worker));
            }
        }
    }
    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }
}

void DeviceDiscoveryService::registerProvider(std::unique_ptr<IDeviceDiscoveryProvider> provider)
{
    if (!provider) return;
    std::lock_guard<std::mutex> lk(mutex_);
    SPDLOG_INFO("DeviceDiscoveryService: registered provider '{}' ({})", provider->id(),
                toString(provider->kind()));
    providers_.push_back(std::move(provider));
}

std::vector<std::string> DeviceDiscoveryService::providerIds() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<std::string> ids;
    for (const auto& p : providers_) ids.push_back(p->id());
    return ids;
}

bool DeviceDiscoveryService::hasProviderFor(DeviceKind kind) const
{
    std::lock_guard<std::mutex> lk(mutex_);
    for (const auto& p : providers_) {
        if (p->kind() == kind) return true;
    }
    return false;
}

void DeviceDiscoveryService::setResourceGuard(DeviceKind kind, ResourceGuard guard)
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (guard) {
        guards_[kind] = std::move(guard);
    } else {
        guards_.erase(kind);
    }
}

// ---- requests ----------------------------------------------------------------

std::optional<std::string> DeviceDiscoveryService::validate(const DiscoveryRequest& request)
{
    if (request.kinds.empty()) return std::string("request names no device kind");
    if (request.retry.maxRetries < 0 || request.retry.maxRetries > kMaxRetries) {
        return "retry count out of bounds (0.." + std::to_string(kMaxRetries) + ")";
    }
    if (request.retry.delay.count() < 0 || request.retry.delay > kMaxRetryDelay) {
        return std::string("retry delay out of bounds");
    }
    if (request.initialDelay.count() < 0 || request.initialDelay > kMaxInitialDelay) {
        return std::string("initial delay out of bounds");
    }
    if (request.deadline.count() <= 0 || request.deadline > kMaxDeadline) {
        return std::string("deadline out of bounds");
    }
    if (request.serialScope) {
        const auto& scope = *request.serialScope;
        if (scope.addressFrom == 0 || scope.addressFrom > scope.addressTo) {
            return std::string("serial address range invalid");
        }
        if (scope.perAddressTimeoutMs <= 0 || scope.perAddressTimeoutMs > 10000) {
            return std::string("per-address timeout out of bounds");
        }
    }
    return std::nullopt;
}

std::string DeviceDiscoveryService::coalesceKey(const DiscoveryRequest& request)
{
    std::ostringstream os;
    std::vector<int> kinds;
    for (auto k : request.kinds) kinds.push_back(static_cast<int>(k));
    std::sort(kinds.begin(), kinds.end());
    for (int k : kinds) os << 'k' << k;
    std::vector<std::string> providers = request.providers;
    std::sort(providers.begin(), providers.end());
    for (const auto& p : providers) os << "|p:" << p;
    if (request.serialScope) {
        const auto& s = *request.serialScope;
        os << "|s:" << s.portName << '/' << s.settings.baudRate << s.settings.parity
           << s.settings.dataBits << s.settings.stopBits << '/' << int(s.addressFrom) << '-'
           << int(s.addressTo) << '/' << s.perAddressTimeoutMs;
    }
    if (request.preferredNanopositioner) {
        const auto& e = *request.preferredNanopositioner;
        os << "|n:" << static_cast<int>(e.backend) << '/' << e.persistentId << '/' << e.systemPath
           << '/' << e.coremorBaudRate << '/' << int(e.coremorAddress);
    }
    os << "|d:" << request.initialDelay.count() << '/' << request.deadline.count() << '/'
       << request.retry.maxRetries << '/' << request.retry.delay.count();
    return os.str();
}

std::vector<IDeviceDiscoveryProvider*>
DeviceDiscoveryService::selectProviders(const DiscoveryRequest& request) const
{
    std::vector<IDeviceDiscoveryProvider*> selected;
    for (const auto& p : providers_) {
        const bool kindMatch =
            std::find(request.kinds.begin(), request.kinds.end(), p->kind()) != request.kinds.end();
        if (!kindMatch) continue;
        if (!request.providers.empty() &&
            std::find(request.providers.begin(), request.providers.end(), p->id()) ==
                request.providers.end()) {
            continue;
        }
        selected.push_back(p.get());
    }
    return selected;
}

StartResult DeviceDiscoveryService::startDiscovery(const DiscoveryRequest& request)
{
    StartResult result;
    if (const auto problem = validate(request)) {
        result.rejection = ErrorKind::InvalidRequest;
        result.reason = *problem;
        return result;
    }

    // Join workers that already exited and drop over-retained jobs. Joins
    // happen without the service lock so a worker still notifying observers
    // (which may call back into this service) can never deadlock us.
    reapFinished();

    std::lock_guard<std::mutex> lk(mutex_);
    if (shutdown_) {
        result.rejection = ErrorKind::ShuttingDown;
        result.reason = "discovery service is shutting down";
        return result;
    }
    for (const auto& id : request.providers) {
        bool known = false;
        for (const auto& p : providers_) known |= p->id() == id;
        if (!known) {
            result.rejection = ErrorKind::InvalidRequest;
            result.reason = "unknown provider '" + id + "'";
            return result;
        }
    }

    const std::string key = coalesceKey(request);
    std::size_t active = 0;
    for (const auto& [id, job] : jobs_) {
        if (job->done) continue;
        ++active;
        if (job->key == key) {
            result.accepted = true;
            result.coalesced = true;
            result.jobId = job->id;
            return result;
        }
    }
    if (active >= kMaxConcurrentJobs) {
        result.rejection = ErrorKind::TooManyJobs;
        result.reason = "too many discovery jobs in flight";
        return result;
    }

    auto job = std::make_shared<Job>();
    job->id = nextJobId_++;
    job->request = request;
    job->key = key;
    job->snapshot.jobId = job->id;
    job->snapshot.generation = job->id;
    job->snapshot.state = JobState::Queued;
    job->snapshot.maxAttempts = request.retry.maxRetries + 1;
    job->snapshot.origin = request.origin;
    jobs_.emplace(job->id, job);
    SPDLOG_INFO("DeviceDiscoveryService: job {} queued (origin='{}', kinds={})", job->id,
                request.origin, request.kinds.size());
    job->worker = std::thread([this, job] { run(job); });

    result.accepted = true;
    result.jobId = job->id;
    return result;
}

void DeviceDiscoveryService::cancelDiscovery(std::uint64_t jobId)
{
    std::lock_guard<std::mutex> lk(mutex_);
    const auto it = jobs_.find(jobId);
    if (it == jobs_.end() || it->second->done) return;
    if (!it->second->cancel.exchange(true)) {
        SPDLOG_INFO("DeviceDiscoveryService: job {} cancellation requested", jobId);
    }
    cv_.notify_all();
}

DiscoverySnapshot DeviceDiscoveryService::discoverySnapshot(std::uint64_t jobId) const
{
    std::lock_guard<std::mutex> lk(mutex_);
    const auto it = jobs_.find(jobId);
    if (it == jobs_.end()) {
        DiscoverySnapshot unknown;
        unknown.state = JobState::Failed;
        unknown.errors.push_back({"", ErrorKind::InvalidRequest, "unknown discovery job", ""});
        return unknown;
    }
    return it->second->snapshot;
}

bool DeviceDiscoveryService::waitForTerminal(std::uint64_t jobId,
                                             std::chrono::milliseconds timeout) const
{
    std::unique_lock<std::mutex> lk(mutex_);
    const auto it = jobs_.find(jobId);
    if (it == jobs_.end()) return false;
    const std::shared_ptr<Job> job = it->second;
    return cv_.wait_for(lk, timeout, [&] { return job->done; });
}

// ---- observers ---------------------------------------------------------------

std::uint64_t DeviceDiscoveryService::addObserver(Observer observer)
{
    std::lock_guard<std::mutex> lk(observersMutex_);
    const auto id = nextObserverId_++;
    auto entry = std::make_shared<ObserverEntry>();
    entry->fn = std::move(observer);
    observers_.emplace(id, std::move(entry));
    return id;
}

void DeviceDiscoveryService::removeObserver(std::uint64_t observerId)
{
    std::unique_lock<std::mutex> lk(observersMutex_);
    const auto it = observers_.find(observerId);
    if (it == observers_.end()) return;
    auto entry = it->second;
    observers_.erase(it);
    observersCv_.wait(lk, [&] { return entry->inflight == 0; });
}

void DeviceDiscoveryService::notifyObservers(const DiscoverySnapshot& snapshot)
{
    std::vector<std::shared_ptr<ObserverEntry>> entries;
    {
        std::lock_guard<std::mutex> lk(observersMutex_);
        for (auto& [id, entry] : observers_) {
            ++entry->inflight;
            entries.push_back(entry);
        }
    }
    for (auto& entry : entries) {
        try {
            if (entry->fn) entry->fn(snapshot);
        } catch (const std::exception& ex) {
            SPDLOG_ERROR("DeviceDiscoveryService: observer threw: {}", ex.what());
        } catch (...) {
            SPDLOG_ERROR("DeviceDiscoveryService: observer threw");
        }
        {
            std::lock_guard<std::mutex> lk(observersMutex_);
            --entry->inflight;
        }
        observersCv_.notify_all();
    }
}

// ---- shutdown ------------------------------------------------------------------

void DeviceDiscoveryService::shutdownDiscovery()
{
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        const bool first = !shutdown_;
        shutdown_ = true;
        for (auto& [id, job] : jobs_) {
            if (!job->done) job->cancel.store(true);
            if (!job->worker.joinable()) continue;
            if (job->worker.get_id() == std::this_thread::get_id()) {
                // shutdownDiscovery() from inside an observer/provider: the
                // caller's own worker cannot be joined here; the destructor
                // or a later shutdown from another thread reaps it.
                SPDLOG_ERROR("DeviceDiscoveryService: shutdown called from discovery worker of job {}",
                             id);
                continue;
            }
            threads.push_back(std::move(job->worker));
        }
        if (first) {
            SPDLOG_INFO("DeviceDiscoveryService: shutdown draining {} worker(s)", threads.size());
        }
    }
    cv_.notify_all();
    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto& [id, job] : jobs_) {
            if (job->done) continue;
            // A worker that never ran (should not happen: every thread was
            // joined above) is closed out as Cancelled for consumers.
            job->snapshot.state = JobState::Cancelled;
            job->snapshot.complete = false;
            job->done = true;
        }
    }
    cv_.notify_all();
}

bool DeviceDiscoveryService::isShutdown() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return shutdown_;
}

std::size_t DeviceDiscoveryService::activeWorkerCount() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    std::size_t n = 0;
    for (const auto& [id, job] : jobs_) {
        if (!job->done) ++n;
    }
    return n;
}

void DeviceDiscoveryService::reapFinished()
{
    // Only threads whose run() returned are joined (the join is immediate);
    // a worker still notifying observers keeps its handle until shutdown.
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto& [id, job] : jobs_) {
            if (job->exited.load() && job->worker.joinable() &&
                job->worker.get_id() != std::this_thread::get_id()) {
                threads.push_back(std::move(job->worker));
            }
        }
        evictRetainedLocked();
    }
    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }
}

void DeviceDiscoveryService::evictRetainedLocked()
{
    // Keep the newest kMaxRetainedJobs terminal jobs; a terminal job whose
    // worker handle is still attached (not yet joined) is never dropped so
    // the thread stays owned.
    std::vector<std::uint64_t> terminal;
    for (const auto& [id, job] : jobs_) {
        if (job->done) terminal.push_back(id);
    }
    std::sort(terminal.begin(), terminal.end());
    while (terminal.size() > kMaxRetainedJobs) {
        const auto id = terminal.front();
        terminal.erase(terminal.begin());
        auto it = jobs_.find(id);
        if (it == jobs_.end()) continue;
        if (it->second->worker.joinable()) continue;
        jobs_.erase(it);
    }
}

std::shared_ptr<std::mutex> DeviceDiscoveryService::resourceMutexFor(const std::string& cls)
{
    std::lock_guard<std::mutex> lk(mutex_);
    auto& slot = resourceMutexes_[cls];
    if (!slot) slot = std::make_shared<std::mutex>();
    return slot;
}

// ---- worker ---------------------------------------------------------------------

bool DeviceDiscoveryService::waitCancellable(Job& job, std::chrono::milliseconds duration)
{
    std::unique_lock<std::mutex> lk(mutex_);
    cv_.wait_for(lk, duration, [&] { return job.cancel.load() || shutdown_; });
    return !(job.cancel.load() || shutdown_);
}

void DeviceDiscoveryService::publish(Job& job, const DiscoverySnapshot& snapshot, bool terminal)
{
    {
        std::lock_guard<std::mutex> lk(mutex_);
        job.snapshot = snapshot;
        if (terminal) job.done = true;
    }
    cv_.notify_all();
    notifyObservers(snapshot);
}

void DeviceDiscoveryService::run(std::shared_ptr<Job> jobPtr)
{
    Job& job = *jobPtr;
    const DiscoveryRequest request = job.request;
    DiscoverySnapshot snap;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        snap = job.snapshot;
    }

    auto cancelled = [&job]() { return job.cancel.load(); };

    if (request.initialDelay.count() > 0) {
        waitCancellable(job, request.initialDelay);
    }
    const auto deadline = std::chrono::steady_clock::now() + request.deadline;

    std::vector<IDeviceDiscoveryProvider*> providers;
    std::map<DeviceKind, ResourceGuard> guards;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        providers = selectProviders(request);
        guards = guards_;
    }

    if (!cancelled()) {
        snap.state = JobState::Running;
        snap.attempt = 1;
        publish(job, snap, false);
    }

    std::vector<DiscoveredDevice> candidates;
    std::vector<DiscoveryError> errors;
    std::vector<std::string> providersRun;
    bool coverageComplete = true;
    bool timedOut = false;
    int succeeded = 0;
    int failed = 0;

    const int maxAttempts = request.retry.maxRetries + 1;
    for (int attempt = 1; attempt <= maxAttempts && !cancelled(); ++attempt) {
        candidates.clear();
        errors.clear();
        providersRun.clear();
        coverageComplete = true;
        succeeded = 0;
        failed = 0;
        snap.attempt = attempt;

        if (providers.empty()) {
            errors.push_back({"", ErrorKind::Unsupported,
                              "no discovery provider registered for the requested device kinds",
                              ""});
            coverageComplete = false;
        }

        for (auto* provider : providers) {
            if (cancelled()) break;
            if (std::chrono::steady_clock::now() > deadline) {
                errors.push_back({provider->id(), ErrorKind::Timeout,
                                  "discovery deadline reached before this provider ran", ""});
                timedOut = true;
                coverageComplete = false;
                break;
            }
            const auto guardIt = guards.find(provider->kind());
            if (guardIt != guards.end() && guardIt->second && guardIt->second()) {
                errors.push_back({provider->id(), ErrorKind::Busy,
                                  std::string(toString(provider->kind())) +
                                      " resource is busy; enumeration skipped",
                                  ""});
                coverageComplete = false;
                continue;
            }

            ProviderContext ctx;
            ctx.cancelled = cancelled;
            ctx.deadline = deadline;
            ctx.jobId = job.id;
            ctx.attempt = attempt;

            std::shared_ptr<std::mutex> resource;
            const std::string cls = provider->resourceClass();
            if (!cls.empty()) resource = resourceMutexFor(cls);
            std::unique_lock<std::mutex> resourceLock;
            if (resource) resourceLock = std::unique_lock<std::mutex>(*resource);

            ProviderResult r;
            bool threw = false;
            try {
                r = provider->discover(request, ctx);
            } catch (const std::exception& ex) {
                threw = true;
                r = ProviderResult{};
                r.complete = false;
                r.errors.push_back({provider->id(), ErrorKind::ProviderException, ex.what(), ""});
            } catch (...) {
                threw = true;
                r = ProviderResult{};
                r.complete = false;
                r.errors.push_back({provider->id(), ErrorKind::ProviderException,
                                    "unknown exception", ""});
            }
            if (resourceLock.owns_lock()) resourceLock.unlock();

            providersRun.push_back(provider->id());
            for (auto& c : r.candidates) {
                if (c.providerId.empty()) c.providerId = provider->id();
                candidates.push_back(std::move(c));
            }
            for (auto& e : r.errors) {
                if (e.providerId.empty()) e.providerId = provider->id();
                errors.push_back(std::move(e));
            }
            if (!r.complete || !r.errors.empty()) coverageComplete = false;
            const bool providerFailed = threw || (!r.errors.empty() && r.candidates.empty());
            if (providerFailed) {
                ++failed;
            } else {
                ++succeeded;
            }
            if (std::chrono::steady_clock::now() > deadline && !cancelled()) {
                // The provider itself overran the deadline: keep its partial
                // results but stop the job.
                if (provider != providers.back()) {
                    errors.push_back({"", ErrorKind::Timeout,
                                      "discovery deadline reached; remaining providers skipped",
                                      ""});
                    timedOut = true;
                    coverageComplete = false;
                    break;
                }
            }
        }

        bool anyIdentified = false;
        for (const auto& c : candidates) {
            anyIdentified |= c.identification == IdentificationStatus::Identified && !c.synthetic;
        }
        const bool lastAttempt = attempt == maxAttempts;
        if (anyIdentified || lastAttempt || timedOut || cancelled()) break;

        // Publish the partial attempt (visible to pollers), then wait.
        {
            DiscoverySnapshot partial = snap;
            partial.candidates = candidates;
            partial.errors = errors;
            partial.providersRun = providersRun;
            partial.complete = false;
            publish(job, partial, false);
        }
        if (!waitCancellable(job, request.retry.delay)) break;
    }

    bool overflow = false;
    consolidate(candidates, errors, overflow);
    if (cancelled()) {
        errors.push_back({"", ErrorKind::Cancelled, "discovery cancelled", ""});
    }
    boundErrors(errors);

    JobState finalState = JobState::Completed;
    if (cancelled()) {
        finalState = JobState::Cancelled;
    } else if (timedOut) {
        finalState = JobState::Failed;
    } else if (!providers.empty() && succeeded == 0 && failed > 0) {
        finalState = JobState::Failed;
    }

    snap.state = finalState;
    snap.candidates = std::move(candidates);
    snap.errors = std::move(errors);
    snap.providersRun = std::move(providersRun);
    snap.overflow = overflow;
    snap.complete = finalState == JobState::Completed && coverageComplete && !overflow &&
                    !providers.empty();
    SPDLOG_INFO("DeviceDiscoveryService: job {} {} ({} candidate(s), {} error(s), complete={})",
                job.id, toString(finalState), snap.candidates.size(), snap.errors.size(),
                snap.complete);
    publish(job, snap, true);
    job.exited.store(true);
}

} // namespace backend::discovery
