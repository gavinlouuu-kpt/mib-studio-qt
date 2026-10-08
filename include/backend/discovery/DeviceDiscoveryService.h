// Backend device-discovery job service (issue #419, ADR 0005).
//
// Owns job IDs, one worker thread per job (bounded), deadlines, cooperative
// cancellation, retry scheduling, provider-aware deduplication and ambiguity,
// bounded snapshots, per-resource serialization, guards, observers and
// shutdown draining. Providers (IDeviceDiscoveryProvider) supply enumeration
// and read-only identity probes. Selection/connection is a separate policy
// (StartupDiscoveryPolicy / StartupDiscoveryCoordinator): this service never
// connects, moves or configures a device.
//
// Threading: startDiscovery/cancelDiscovery/discoverySnapshot are callable
// from any thread and never block on a running probe. Observers are invoked
// on the job's worker thread with no service lock held; they must not block
// for long and must stay valid until removeObserver() or shutdownDiscovery()
// returns. Vault: knowledge_map/services/DeviceDiscoveryService.md.
#pragma once

#include "backend/discovery/DeviceDiscoveryTypes.h"
#include "backend/discovery/IDeviceDiscoveryProvider.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace backend::discovery {

class DeviceDiscoveryService {
public:
    using Observer = std::function<void(const DiscoverySnapshot&)>;
    using ResourceGuard = std::function<bool()>; // true = kind is busy right now

    DeviceDiscoveryService();
    ~DeviceDiscoveryService(); // shutdownDiscovery()

    DeviceDiscoveryService(const DeviceDiscoveryService&) = delete;
    DeviceDiscoveryService& operator=(const DeviceDiscoveryService&) = delete;

    // Registration happens at composition time (AppBackend::initialize) and
    // in tests; providers live until the service is destroyed.
    void registerProvider(std::unique_ptr<IDeviceDiscoveryProvider> provider);
    // Removes a provider by ID (test seam: swap production providers for
    // fakes). Refused while any job is in flight; returns false then or when
    // the ID is unknown.
    bool unregisterProvider(const std::string& providerId);
    std::vector<std::string> providerIds() const;
    bool hasProviderFor(DeviceKind kind) const;

    // A busy guard makes every provider of that kind report Busy instead of
    // touching hardware (e.g. camera enumeration while capture is running).
    void setResourceGuard(DeviceKind kind, ResourceGuard guard);

    // Validation happens before any work starts. Identical non-terminal
    // requests are coalesced onto the running job.
    static std::optional<std::string> validate(const DiscoveryRequest& request);
    StartResult startDiscovery(const DiscoveryRequest& request);

    // Idempotent, non-blocking; scoped to that job. Never disconnects
    // established devices.
    void cancelDiscovery(std::uint64_t jobId);

    // Value snapshot; never probes hardware or waits for a worker. Unknown or
    // evicted jobs return jobId 0 / Failed / InvalidRequest.
    DiscoverySnapshot discoverySnapshot(std::uint64_t jobId) const;

    // Convenience for tests and worker-thread compatibility wrappers only.
    bool waitForTerminal(std::uint64_t jobId, std::chrono::milliseconds timeout) const;

    std::uint64_t addObserver(Observer observer);
    // Blocks until an in-flight invocation of that observer has returned.
    void removeObserver(std::uint64_t observerId);

    // Terminal: refuse new jobs, cancel pending work, join every worker.
    void shutdownDiscovery();
    bool isShutdown() const;

    // Jobs whose worker has not yet published a terminal state.
    std::size_t activeWorkerCount() const;

private:
    struct Job;
    struct ObserverEntry;

    void run(std::shared_ptr<Job> job);
    bool waitCancellable(Job& job, std::chrono::milliseconds duration);
    void publish(Job& job, const DiscoverySnapshot& snapshot, bool terminal);
    void notifyObservers(const DiscoverySnapshot& snapshot);
    void reapFinished();
    void evictRetainedLocked();
    std::shared_ptr<std::mutex> resourceMutexFor(const std::string& resourceClass);
    std::vector<IDeviceDiscoveryProvider*> selectProviders(const DiscoveryRequest& request) const;
    static std::string coalesceKey(const DiscoveryRequest& request);

    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    std::vector<std::unique_ptr<IDeviceDiscoveryProvider>> providers_;
    std::map<DeviceKind, ResourceGuard> guards_;
    std::map<std::string, std::shared_ptr<std::mutex>> resourceMutexes_;
    std::map<std::uint64_t, std::shared_ptr<Job>> jobs_;
    std::uint64_t nextJobId_{1};
    bool shutdown_{false};

    mutable std::mutex observersMutex_;
    std::condition_variable observersCv_;
    std::map<std::uint64_t, std::shared_ptr<ObserverEntry>> observers_;
    std::uint64_t nextObserverId_{1};
};

} // namespace backend::discovery
