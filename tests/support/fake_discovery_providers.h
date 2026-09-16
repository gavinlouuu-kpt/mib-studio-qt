// Scripted and blocking fake providers for the device-discovery service
// (issue #419). No hardware, no SDKs. Header-only; include as
// "support/fake_discovery_providers.h".
#pragma once

#include "backend/discovery/IDeviceDiscoveryProvider.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mib::test {

namespace disc = backend::discovery;

inline disc::DiscoveredDevice makeCandidate(
    disc::DeviceKind kind, std::string providerId, std::string identity,
    std::string systemPath = {}, int sdkIndex = -1, int busAddress = -1,
    disc::IdentificationStatus status = disc::IdentificationStatus::Identified,
    disc::IdentityStrength strength = disc::IdentityStrength::Persistent)
{
    disc::DiscoveredDevice d;
    d.kind = kind;
    d.providerId = std::move(providerId);
    d.stableIdentity = std::move(identity);
    d.displayName = d.stableIdentity.empty() ? systemPath : d.stableIdentity;
    d.endpoint.systemPath = std::move(systemPath);
    d.endpoint.sdkIndex = sdkIndex;
    d.endpoint.busAddress = busAddress;
    d.identification = status;
    d.identityStrength = strength;
    return d;
}

// A provider whose result is scripted. Optionally blocks inside discover()
// until release() (or cancellation when honourCancel is set) so tests can
// observe the service while a probe is "stuck".
class ScriptedProvider final : public disc::IDeviceDiscoveryProvider {
public:
    ScriptedProvider(std::string id, disc::DeviceKind kind, std::string resourceClass = {})
        : id_(std::move(id)), kind_(kind), resource_(std::move(resourceClass))
    {
    }

    // ---- script ----------------------------------------------------------
    disc::ProviderResult result;
    std::function<disc::ProviderResult(const disc::DiscoveryRequest&, const disc::ProviderContext&)>
        script; // when set, replaces `result`
    std::atomic<bool> block{false};
    bool honourCancel = true;  // blocked discover() returns when cancelled
    bool throwOnDiscover = false;
    bool cancellable_ = true;

    // ---- observations ----------------------------------------------------
    std::atomic<int> calls{0};
    std::atomic<bool> entered{false};
    std::atomic<bool> sawCancel{false};

    std::string id() const override { return id_; }
    disc::DeviceKind kind() const override { return kind_; }
    std::string resourceClass() const override { return resource_; }
    bool cancellable() const override { return cancellable_; }

    disc::ProviderResult discover(const disc::DiscoveryRequest& request,
                                  const disc::ProviderContext& ctx) override
    {
        ++calls;
        entered.store(true);
        if (throwOnDiscover) throw std::runtime_error("scripted provider failure");
        if (block.load()) {
            std::unique_lock<std::mutex> lk(mutex_);
            // Poll the token: a real provider checks it between steps, so the
            // fake re-checks every few milliseconds rather than needing a wake.
            while (!released_ && !(honourCancel && ctx.cancelled && ctx.cancelled())) {
                cv_.wait_for(lk, std::chrono::milliseconds(5));
            }
            if (ctx.cancelled && ctx.cancelled()) {
                sawCancel.store(true);
                disc::ProviderResult cancelled;
                cancelled.complete = false;
                return cancelled;
            }
        }
        if (script) return script(request, ctx);
        return result;
    }

    bool waitUntilEntered(std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!entered.load()) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    void release()
    {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            released_ = true;
        }
        cv_.notify_all();
    }

    // Wake a blocked discover() so it can re-check the cancellation token
    // (the service calls this indirectly when a provider is cancellable).
    void poke() { cv_.notify_all(); }

    void reset()
    {
        std::lock_guard<std::mutex> lk(mutex_);
        released_ = false;
        entered.store(false);
        sawCancel.store(false);
    }

private:
    std::string id_;
    disc::DeviceKind kind_;
    std::string resource_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool released_ = false;
};

} // namespace mib::test
