// Startup selection/connection sequence over the discovery service (issue
// #419, ADR 0005). Preserves the pre-#419 desktop behaviour: camera discovery
// after an initial delay (400 ms), then nanopositioner discovery with retries
// (3 x 4000 ms); one identified camera is selected, one identified
// nanopositioner is connected through the existing service APIs; anything
// else is reported and left to the user.
//
// Threading: the service observer fires on a discovery worker thread; every
// decision action (hooks + listeners) is handed to the injected executor. The
// default executor runs inline on the worker; the Qt adapter posts to the UI
// thread because AppBackend's selection setters are not thread-safe against
// the widgets that read them. stop() is terminal: owned jobs are cancelled,
// pending/late actions do nothing, and an action already running on another
// thread is waited for (bounded, 5 s) so no hook or listener runs after
// stop() returns (issue #431). Vault:
// knowledge_map/services/DeviceDiscoveryService.md.
#pragma once

#include "backend/discovery/DeviceDiscoveryService.h"
#include "backend/discovery/DeviceDiscoveryTypes.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace backend::discovery {

class StartupDiscoveryCoordinator {
public:
    using Executor = std::function<void(std::function<void()>)>;

    struct Hooks {
        std::function<bool()> cameraConfigured;
        std::function<bool()> captureRunning;
        std::function<bool()> nanopositionerConnected;
        // Existing selection API (AppBackend::set*CameraSelection); returns
        // false when the selection was refused.
        std::function<bool(const DiscoveredDevice&)> selectCamera;
        // Existing connection API (AutofocusService::connect).
        std::function<bool(const nanopositioner::Endpoint&)> connectNanopositioner;
        // Saved preference (vendor filter, baud/address, remembered port).
        std::function<std::optional<nanopositioner::Endpoint>()> preferredNanopositioner;
    };

    struct Timing {
        std::chrono::milliseconds cameraDelay{400};
        int nanopositionerRetries{3};
        std::chrono::milliseconds nanopositionerRetryDelay{4000};
        std::chrono::milliseconds cameraDeadline{60000};
        std::chrono::milliseconds nanopositionerDeadline{60000};
    };

    struct CameraOutcome {
        // Started: a camera job was accepted (jobId set) — adapters flip
        // their "scanning" state here.
        enum class Kind { Started, Skipped, NoneFound, Selected, RequireSelection, Incomplete, Refused };
        Kind kind{Kind::Skipped};
        std::optional<DiscoveredDevice> device;
        std::uint64_t jobId{0};
        DiscoverySnapshot snapshot;
        std::string message;
    };

    struct NanopositionerOutcome {
        enum class Kind {
            Started,   // a nanopositioner job was accepted (jobId set)
            Skipped,
            Searching, // a retry is pending: attempt/maxAttempts are set
            NotFound,
            Connected,
            ConnectFailed,
            RequireSelection,
            Incomplete
        };
        Kind kind{Kind::Skipped};
        std::optional<nanopositioner::Endpoint> endpoint;
        int attempt{0};
        int maxAttempts{0};
        std::uint64_t jobId{0};
        DiscoverySnapshot snapshot;
        std::string message;
    };

    using CameraListener = std::function<void(const CameraOutcome&)>;
    using NanopositionerListener = std::function<void(const NanopositionerOutcome&)>;

    StartupDiscoveryCoordinator(DeviceDiscoveryService& service, Hooks hooks);
    StartupDiscoveryCoordinator(DeviceDiscoveryService& service, Hooks hooks, Timing timing);
    ~StartupDiscoveryCoordinator(); // stop()

    StartupDiscoveryCoordinator(const StartupDiscoveryCoordinator&) = delete;
    StartupDiscoveryCoordinator& operator=(const StartupDiscoveryCoordinator&) = delete;

    void setExecutor(Executor executor);
    // Replace the delays/retries (tests shorten them). Applies to steps
    // started afterwards.
    void setTiming(Timing timing);
    Timing timing() const;
    // The saved nanopositioner preference lives in the shell (vendor filter,
    // serial settings, remembered port); the shell installs it here. Invoked
    // in executor context when a nanopositioner step starts.
    void setPreferredNanopositionerHook(
        std::function<std::optional<nanopositioner::Endpoint>()> hook);
    void setCameraListener(CameraListener listener);
    void setNanopositionerListener(NanopositionerListener listener);

    // Camera step after the configured delay, then the nanopositioner step.
    void start();
    // Manual "Try again": immediate camera step, then nanopositioner. False
    // when refused (stopped, already running, capture running, configured).
    bool runCameraStep();
    // Manual "Refresh": nanopositioner step only. False when refused
    // (stopped, already running, already connected).
    bool runNanopositionerStep();

    // Terminal. Cancels owned jobs and waits (bounded) for an action already
    // running on another thread; no hook or listener runs after it returns.
    // Safe to call from inside a hook or listener (no self-wait).
    void stop();
    bool isStopped() const;

    bool cameraStepRunning() const;
    bool nanopositionerStepRunning() const;
    std::uint64_t cameraJobId() const;
    std::uint64_t nanopositionerJobId() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace backend::discovery
