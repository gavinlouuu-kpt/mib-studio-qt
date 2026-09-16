#include "backend/discovery/StartupDiscoveryCoordinator.h"

#include "backend/discovery/StartupDiscoveryPolicy.h"

#include <atomic>
#include <mutex>
#include <utility>

#include <spdlog/spdlog.h>

namespace backend::discovery {

struct StartupDiscoveryCoordinator::Impl : std::enable_shared_from_this<Impl> {
    Impl(DeviceDiscoveryService& s, Hooks h, Timing t)
        : service(s), hooks(std::move(h)), timing(t)
    {
        executor = [](std::function<void()> fn) { fn(); };
    }

    DeviceDiscoveryService& service;
    Hooks hooks;
    Timing timing;

    mutable std::mutex mutex;
    Executor executor;                       // mutex
    CameraListener cameraListener;           // mutex
    NanopositionerListener nanoListener;     // mutex
    std::uint64_t cameraJob{0};              // mutex
    std::uint64_t nanoJob{0};                // mutex
    bool cameraRunning{false};               // mutex
    bool nanoRunning{false};                 // mutex
    bool chainNanoAfterCamera{true};         // mutex
    int lastReportedAttempt{0};              // mutex
    std::uint64_t observerId{0};
    std::atomic<bool> stopped{false};

    // ---- helpers -----------------------------------------------------------
    bool hook(const std::function<bool()>& fn) const { return fn ? fn() : false; }

    void post(std::function<void(Impl&)> action)
    {
        Executor exec;
        {
            std::lock_guard<std::mutex> lk(mutex);
            exec = executor;
        }
        std::weak_ptr<Impl> weak = weak_from_this();
        exec([weak, action = std::move(action)] {
            auto self = weak.lock();
            if (!self || self->stopped.load()) return;
            action(*self);
        });
    }

    void emitCamera(const CameraOutcome& outcome)
    {
        if (stopped.load()) return;
        CameraListener listener;
        {
            std::lock_guard<std::mutex> lk(mutex);
            listener = cameraListener;
        }
        if (listener) listener(outcome);
    }

    void emitNano(const NanopositionerOutcome& outcome)
    {
        if (stopped.load()) return;
        NanopositionerListener listener;
        {
            std::lock_guard<std::mutex> lk(mutex);
            listener = nanoListener;
        }
        if (listener) listener(outcome);
    }

    // ---- steps ---------------------------------------------------------------
    bool beginCamera(std::chrono::milliseconds delay, bool chain, const char* origin)
    {
        if (stopped.load()) return false;
        DiscoveryRequest request;
        request.kinds = {DeviceKind::Camera, DeviceKind::Framegrabber};
        request.initialDelay = delay;
        request.deadline = timing.cameraDeadline;
        request.origin = origin;
        {
            std::lock_guard<std::mutex> lk(mutex);
            if (cameraRunning) return false;
        }
        const auto start = service.startDiscovery(request);
        if (!start.accepted) {
            SPDLOG_WARN("StartupDiscovery: camera step refused: {}", start.reason);
            CameraOutcome outcome;
            outcome.kind = CameraOutcome::Kind::Refused;
            outcome.message = start.reason;
            emitCamera(outcome);
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(mutex);
            cameraJob = start.jobId;
            cameraRunning = true;
            chainNanoAfterCamera = chain;
        }
        SPDLOG_INFO("StartupDiscovery: camera step started (job {}, delay {} ms)", start.jobId,
                    delay.count());
        return true;
    }

    bool beginNano(const char* origin)
    {
        if (stopped.load()) return false;
        if (hook(hooks.nanopositionerConnected)) {
            NanopositionerOutcome outcome;
            outcome.kind = NanopositionerOutcome::Kind::Skipped;
            outcome.message = "nanopositioner already connected";
            emitNano(outcome);
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(mutex);
            if (nanoRunning) return false;
        }
        DiscoveryRequest request;
        request.kinds = {DeviceKind::Nanopositioner};
        request.retry.maxRetries = timing.nanopositionerRetries;
        request.retry.delay = timing.nanopositionerRetryDelay;
        request.deadline = timing.nanopositionerDeadline;
        request.origin = origin;
        if (hooks.preferredNanopositioner) request.preferredNanopositioner = hooks.preferredNanopositioner();
        const auto start = service.startDiscovery(request);
        if (!start.accepted) {
            SPDLOG_WARN("StartupDiscovery: nanopositioner step refused: {}", start.reason);
            NanopositionerOutcome outcome;
            outcome.kind = NanopositionerOutcome::Kind::Incomplete;
            outcome.message = start.reason;
            emitNano(outcome);
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(mutex);
            nanoJob = start.jobId;
            nanoRunning = true;
            lastReportedAttempt = 1;
        }
        SPDLOG_INFO("StartupDiscovery: nanopositioner step started (job {})", start.jobId);
        return true;
    }

    // ---- observer (discovery worker thread) --------------------------------------
    void onSnapshot(const DiscoverySnapshot& snapshot)
    {
        if (stopped.load()) return;
        bool isCamera = false;
        bool isNano = false;
        bool reportSearching = false;
        {
            std::lock_guard<std::mutex> lk(mutex);
            isCamera = cameraRunning && snapshot.jobId == cameraJob;
            isNano = nanoRunning && snapshot.jobId == nanoJob;
            if (isNano && snapshot.state == JobState::Running && snapshot.attempt >= 1 &&
                snapshot.attempt + 1 <= snapshot.maxAttempts && snapshot.attempt + 1 > lastReportedAttempt) {
                // The service publishes the finished attempt before waiting to
                // retry; the next attempt number is what the operator sees.
                lastReportedAttempt = snapshot.attempt + 1;
                reportSearching = true;
            }
        }
        if (!isCamera && !isNano) return;
        if (reportSearching) {
            post([snapshot](Impl& self) {
                NanopositionerOutcome outcome;
                outcome.kind = NanopositionerOutcome::Kind::Searching;
                outcome.attempt = snapshot.attempt + 1;
                outcome.maxAttempts = snapshot.maxAttempts;
                outcome.jobId = snapshot.jobId;
                outcome.snapshot = snapshot;
                self.emitNano(outcome);
            });
        }
        if (!isTerminal(snapshot.state)) return;
        if (isCamera) {
            post([snapshot](Impl& self) { self.finishCamera(snapshot); });
        } else {
            post([snapshot](Impl& self) { self.finishNano(snapshot); });
        }
    }

    // ---- decisions (executor context) ---------------------------------------------
    void finishCamera(const DiscoverySnapshot& snapshot)
    {
        bool chain = false;
        {
            std::lock_guard<std::mutex> lk(mutex);
            if (!cameraRunning || snapshot.jobId != cameraJob) return; // stale
            cameraRunning = false;
            chain = chainNanoAfterCamera;
        }
        CameraOutcome outcome;
        outcome.jobId = snapshot.jobId;
        outcome.snapshot = snapshot;
        const auto decision = decideCamera(snapshot);
        switch (decision.kind) {
        case CameraDecision::Kind::SelectUnique:
            if (hook(hooks.cameraConfigured) || hook(hooks.captureRunning)) {
                outcome.kind = CameraOutcome::Kind::Skipped;
                outcome.message = "camera already configured";
            } else if (hooks.selectCamera && hooks.selectCamera(*decision.device)) {
                outcome.kind = CameraOutcome::Kind::Selected;
                outcome.device = decision.device;
                SPDLOG_INFO("StartupDiscovery: selected camera '{}'", decision.device->displayName);
            } else {
                outcome.kind = CameraOutcome::Kind::Refused;
                outcome.device = decision.device;
                outcome.message = "camera selection refused";
            }
            break;
        case CameraDecision::Kind::NoneFound:
            outcome.kind = CameraOutcome::Kind::NoneFound;
            break;
        case CameraDecision::Kind::RequireSelection:
            outcome.kind = CameraOutcome::Kind::RequireSelection;
            break;
        case CameraDecision::Kind::NotDecidable:
            outcome.kind = CameraOutcome::Kind::Incomplete;
            outcome.message = std::string("camera discovery ") + toString(snapshot.state) +
                              (snapshot.complete ? "" : " (incomplete coverage)");
            break;
        }
        emitCamera(outcome);
        if (chain) beginNano("startup-nanopositioner");
    }

    void finishNano(const DiscoverySnapshot& snapshot)
    {
        {
            std::lock_guard<std::mutex> lk(mutex);
            if (!nanoRunning || snapshot.jobId != nanoJob) return; // stale
            nanoRunning = false;
        }
        NanopositionerOutcome outcome;
        outcome.jobId = snapshot.jobId;
        outcome.snapshot = snapshot;
        outcome.attempt = snapshot.attempt;
        outcome.maxAttempts = snapshot.maxAttempts;
        std::string preferred;
        if (hooks.preferredNanopositioner) {
            if (const auto ep = hooks.preferredNanopositioner()) preferred = ep->persistentId;
        }
        const auto decision = decideNanopositioner(snapshot, preferred);
        switch (decision.kind) {
        case NanopositionerDecision::Kind::ConnectUnique:
            outcome.endpoint = decision.endpoint;
            if (hook(hooks.nanopositionerConnected)) {
                outcome.kind = NanopositionerOutcome::Kind::Skipped;
                outcome.message = "nanopositioner already connected";
            } else if (hooks.connectNanopositioner && decision.endpoint &&
                       hooks.connectNanopositioner(*decision.endpoint)) {
                outcome.kind = NanopositionerOutcome::Kind::Connected;
                SPDLOG_INFO("StartupDiscovery: auto-connected nanopositioner on {}",
                            decision.endpoint->systemPath);
            } else {
                outcome.kind = NanopositionerOutcome::Kind::ConnectFailed;
            }
            break;
        case NanopositionerDecision::Kind::NotFound:
            outcome.kind = NanopositionerOutcome::Kind::NotFound;
            break;
        case NanopositionerDecision::Kind::RequireSelection:
            outcome.kind = NanopositionerOutcome::Kind::RequireSelection;
            break;
        case NanopositionerDecision::Kind::NotDecidable:
            outcome.kind = NanopositionerOutcome::Kind::Incomplete;
            outcome.message = std::string("nanopositioner discovery ") + toString(snapshot.state) +
                              (snapshot.complete ? "" : " (incomplete coverage)");
            break;
        }
        emitNano(outcome);
    }
};

// ---- public surface ----------------------------------------------------------------

StartupDiscoveryCoordinator::StartupDiscoveryCoordinator(DeviceDiscoveryService& service, Hooks hooks,
                                                         Timing timing)
    : impl_(std::make_shared<Impl>(service, std::move(hooks), timing))
{
    std::weak_ptr<Impl> weak = impl_;
    impl_->observerId = service.addObserver([weak](const DiscoverySnapshot& snapshot) {
        if (auto self = weak.lock()) self->onSnapshot(snapshot);
    });
}

StartupDiscoveryCoordinator::~StartupDiscoveryCoordinator()
{
    stop();
    impl_->service.removeObserver(impl_->observerId);
}

void StartupDiscoveryCoordinator::setExecutor(Executor executor)
{
    std::lock_guard<std::mutex> lk(impl_->mutex);
    impl_->executor = executor ? std::move(executor) : Executor([](std::function<void()> fn) { fn(); });
}

void StartupDiscoveryCoordinator::setCameraListener(CameraListener listener)
{
    std::lock_guard<std::mutex> lk(impl_->mutex);
    impl_->cameraListener = std::move(listener);
}

void StartupDiscoveryCoordinator::setNanopositionerListener(NanopositionerListener listener)
{
    std::lock_guard<std::mutex> lk(impl_->mutex);
    impl_->nanoListener = std::move(listener);
}

void StartupDiscoveryCoordinator::start()
{
    if (impl_->stopped.load()) return;
    if (impl_->hook(impl_->hooks.cameraConfigured) || impl_->hook(impl_->hooks.captureRunning)) {
        CameraOutcome outcome;
        outcome.kind = CameraOutcome::Kind::Skipped;
        outcome.message = "camera already configured or capturing";
        SPDLOG_INFO("StartupDiscovery: camera step skipped ({})", outcome.message);
        impl_->emitCamera(outcome);
        impl_->beginNano("startup-nanopositioner");
        return;
    }
    impl_->beginCamera(impl_->timing.cameraDelay, true, "startup-camera");
}

bool StartupDiscoveryCoordinator::runCameraStep()
{
    if (impl_->stopped.load()) return false;
    {
        std::lock_guard<std::mutex> lk(impl_->mutex);
        if (impl_->cameraRunning) {
            SPDLOG_INFO("StartupDiscovery: camera step already running, skipping");
            return false;
        }
    }
    if (impl_->hook(impl_->hooks.captureRunning)) {
        SPDLOG_INFO("StartupDiscovery: camera step skipped (capture running)");
        return false;
    }
    if (impl_->hook(impl_->hooks.cameraConfigured)) {
        SPDLOG_INFO("StartupDiscovery: camera step skipped (already configured)");
        return false;
    }
    return impl_->beginCamera(std::chrono::milliseconds(0), true, "manual-camera");
}

bool StartupDiscoveryCoordinator::runNanopositionerStep()
{
    if (impl_->stopped.load()) return false;
    if (impl_->hook(impl_->hooks.nanopositionerConnected)) return false;
    {
        std::lock_guard<std::mutex> lk(impl_->mutex);
        if (impl_->nanoRunning) return false;
    }
    return impl_->beginNano("manual-nanopositioner");
}

void StartupDiscoveryCoordinator::stop()
{
    if (impl_->stopped.exchange(true)) return;
    std::uint64_t cameraJob = 0;
    std::uint64_t nanoJob = 0;
    {
        std::lock_guard<std::mutex> lk(impl_->mutex);
        cameraJob = impl_->cameraRunning ? impl_->cameraJob : 0;
        nanoJob = impl_->nanoRunning ? impl_->nanoJob : 0;
        impl_->cameraRunning = false;
        impl_->nanoRunning = false;
    }
    if (cameraJob) impl_->service.cancelDiscovery(cameraJob);
    if (nanoJob) impl_->service.cancelDiscovery(nanoJob);
    SPDLOG_INFO("StartupDiscovery: stopped (camera job {}, nanopositioner job {})", cameraJob, nanoJob);
}

bool StartupDiscoveryCoordinator::isStopped() const { return impl_->stopped.load(); }

bool StartupDiscoveryCoordinator::cameraStepRunning() const
{
    std::lock_guard<std::mutex> lk(impl_->mutex);
    return impl_->cameraRunning;
}

bool StartupDiscoveryCoordinator::nanopositionerStepRunning() const
{
    std::lock_guard<std::mutex> lk(impl_->mutex);
    return impl_->nanoRunning;
}

std::uint64_t StartupDiscoveryCoordinator::cameraJobId() const
{
    std::lock_guard<std::mutex> lk(impl_->mutex);
    return impl_->cameraJob;
}

std::uint64_t StartupDiscoveryCoordinator::nanopositionerJobId() const
{
    std::lock_guard<std::mutex> lk(impl_->mutex);
    return impl_->nanoJob;
}

} // namespace backend::discovery
