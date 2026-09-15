#include "frontend/controllers/CameraController.h"

#include <QString>
#include <QTimer>
#include <spdlog/spdlog.h>

#include "backend/app/AppBackend.h"
#include "backend/services/CaptureLifecycle.h"
#include "backend/services/CaptureService.h"
#include "backend/processing/ProcessingService.h"

namespace frontend
{

    using backend::services::CaptureLifecycleState;
    using backend::services::CaptureStartOutcome;

    QString CameraActionState::phaseText() const
    {
        switch (phase)
        {
        case Phase::Idle: return QStringLiteral("Camera idle");
        case Phase::Starting: return QStringLiteral("Camera starting…");
        case Phase::Running: return QStringLiteral("Camera running");
        case Phase::Stopping: return QStringLiteral("Camera stopping…");
        case Phase::Failed: return QStringLiteral("Camera failed");
        case Phase::NotConfigured: return QStringLiteral("Camera not configured");
        }
        return QStringLiteral("Camera state unknown");
    }

    CameraController::CameraController(backend::AppBackend &backend, QObject *parent)
        : QObject(parent), backend_(backend)
    {
        startAction_ = new QAction(tr("Start Live View"), this);
        startAction_->setObjectName(QStringLiteral("startCameraAct"));
        startAction_->setToolTip(tr("Open the selected camera and start acquisition"));
        stopAction_ = new QAction(tr("Stop Camera"), this);
        stopAction_->setObjectName(QStringLiteral("stopCameraAct"));
        stopAction_->setToolTip(tr("Stop acquisition and release the camera"));
        connect(startAction_, &QAction::triggered, this, [this]() { requestStart(); });
        connect(stopAction_, &QAction::triggered, this, [this]() { requestStop(); });

        pollTimer_ = new QTimer(this);
        pollTimer_->setInterval(pollIntervalMs_);
        connect(pollTimer_, &QTimer::timeout, this, &CameraController::onPoll);
        pollTimer_->start();

        publish(project());
    }

    void CameraController::setOperationGuard(OperationGuard guard)
    {
        guard_ = std::move(guard);
        refreshState();
    }

    void CameraController::setPollIntervalMs(int ms)
    {
        pollIntervalMs_ = ms > 0 ? ms : 1;
        pollTimer_->setInterval(pollIntervalMs_);
    }

    bool CameraController::isRunning() const
    {
        return backend_.capture().isRunning();
    }

    bool CameraController::isConfigured() const
    {
        return backend_.isCameraConfigured();
    }

    CameraActionState CameraController::project() const
    {
        CameraActionState s;
        const auto snap = backend_.capture().lifecycleSnapshot();
        s.generation = snap.generation;
        const CameraOperationBlock block = guard_ ? guard_() : CameraOperationBlock{};

        switch (snap.state)
        {
        case CaptureLifecycleState::Idle:
            s.phase = snap.lastFailure != backend::services::CaptureFailureKind::None &&
                              snap.lastFailureGeneration == snap.generation
                          ? CameraActionState::Phase::Failed
                          : CameraActionState::Phase::Idle;
            break;
        case CaptureLifecycleState::Starting: s.phase = CameraActionState::Phase::Starting; break;
        case CaptureLifecycleState::Running: s.phase = CameraActionState::Phase::Running; break;
        case CaptureLifecycleState::Stopping: s.phase = CameraActionState::Phase::Stopping; break;
        case CaptureLifecycleState::Faulted: s.phase = CameraActionState::Phase::Failed; break;
        }
        if (!snap.isActive() && !backend_.isCameraConfigured())
        {
            s.phase = CameraActionState::Phase::NotConfigured;
        }
        if (s.phase == CameraActionState::Phase::Failed ||
            snap.lastFailure != backend::services::CaptureFailureKind::None)
        {
            s.failureMessage = QString::fromStdString(snap.lastFailureMessage);
            if (s.failureMessage.isEmpty())
            {
                s.failureMessage = QString::fromLatin1(backend::services::toString(snap.lastFailure));
            }
        }

        const bool active = snap.isActive();
        s.startEnabled = !active && !commandInFlight_ &&
                         s.phase != CameraActionState::Phase::NotConfigured &&
                         snap.state != CaptureLifecycleState::Stopping;
        s.stopBlockedReason = (active && block.blocked) ? block.reason : QString();
        s.stopEnabled = active && !commandInFlight_ && !block.blocked;
        return s;
    }

    void CameraController::publish(const CameraActionState &next)
    {
        const bool changed = next != state_;
        state_ = next;
        startAction_->setEnabled(next.startEnabled);
        stopAction_->setEnabled(next.stopEnabled);
        stopAction_->setToolTip(next.stopBlockedReason.isEmpty()
                                    ? tr("Stop acquisition and release the camera")
                                    : next.stopBlockedReason);
        if (changed)
        {
            emit stateChanged(next);
            const bool running = next.phase == CameraActionState::Phase::Running;
            if (running && !lastReportedRunning_)
            {
                emit cameraStarted();
            }
            else if (!running && lastReportedRunning_ &&
                     next.phase != CameraActionState::Phase::Starting)
            {
                emit cameraStopped();
            }
            lastReportedRunning_ = running;
        }
    }

    void CameraController::refreshState()
    {
        publish(project());
    }

    void CameraController::onPoll()
    {
        refreshState();
        if (settlingPolls_ > 0)
        {
            const auto p = state_.phase;
            if (p == CameraActionState::Phase::Starting || p == CameraActionState::Phase::Stopping)
            {
                --settlingPolls_;
                pollTimer_->setInterval(20);
                return;
            }
            settlingPolls_ = 0;
        }
        if (pollTimer_->interval() != pollIntervalMs_)
        {
            pollTimer_->setInterval(pollIntervalMs_);
        }
    }

    CameraCommandResult CameraController::requestStart()
    {
        if (commandInFlight_)
        {
            CameraCommandResult r;
            r.outcome = CameraCommandResult::Outcome::Blocked;
            r.message = tr("A camera command is already in progress.");
            return r;
        }
        commandInFlight_ = true;
        const CameraCommandResult r = doRequestStart();
        // Project the post-command state with the in-flight flag cleared so
        // the shared actions are immediately usable again (a synchronous
        // trigger() right after a command must not be swallowed).
        commandInFlight_ = false;
        refreshState();
        return r;
    }

    CameraCommandResult CameraController::doRequestStart()
    {
        CameraCommandResult r;

        auto &cap = backend_.capture();
        if (cap.isRunning())
        {
            r.outcome = CameraCommandResult::Outcome::AlreadyInState;
            r.message = tr("Camera is already running.");
            refreshState();
            return r;
        }
        if (!backend_.isCameraConfigured())
        {
            r.outcome = CameraCommandResult::Outcome::Blocked;
            r.message = tr("No camera is configured. Please connect to a camera or configure a mock camera first.");
            emit commandFailed(r.message);
            refreshState();
            return r;
        }

        const auto outcome = cap.requestStart();
        switch (outcome)
        {
        case CaptureStartOutcome::Accepted:
            r.outcome = CameraCommandResult::Outcome::Accepted;
            r.message = tr("Camera start requested.");
            settlingPolls_ = 500; // up to ~10 s of 20 ms polls while Starting
            pollTimer_->setInterval(20);
            break;
        case CaptureStartOutcome::AlreadyActive:
            r.outcome = CameraCommandResult::Outcome::AlreadyInState;
            r.message = tr("Camera is already running.");
            break;
        case CaptureStartOutcome::RejectedStopping:
            r.outcome = CameraCommandResult::Outcome::Blocked;
            r.message = tr("The camera is still stopping; try again in a moment.");
            emit commandFailed(r.message);
            break;
        case CaptureStartOutcome::RejectedNoFactory:
            r.outcome = CameraCommandResult::Outcome::Failed;
            r.message = tr("Failed to start camera: no camera source is configured.");
            emit commandFailed(r.message);
            break;
        }
        SPDLOG_INFO("CameraController: start -> {}", backend::services::toString(outcome));
        refreshState();
        return r;
    }

    CameraCommandResult CameraController::requestStop()
    {
        if (commandInFlight_)
        {
            CameraCommandResult r;
            r.outcome = CameraCommandResult::Outcome::Blocked;
            r.message = tr("A camera command is already in progress.");
            return r;
        }
        commandInFlight_ = true;
        const CameraCommandResult r = doRequestStop();
        commandInFlight_ = false;
        refreshState();
        return r;
    }

    CameraCommandResult CameraController::doRequestStop()
    {
        CameraCommandResult r;

        auto &cap = backend_.capture();
        if (!cap.isRunning())
        {
            r.outcome = CameraCommandResult::Outcome::AlreadyInState;
            r.message = tr("Camera is not currently running.");
            refreshState();
            return r;
        }
        // The guard runs for every stop path — including direct dispatch
        // while a presentation is disabled — so no shortcut can bypass the
        // experiment/recording lifecycle rules.
        const CameraOperationBlock block = guard_ ? guard_() : CameraOperationBlock{};
        if (block.blocked)
        {
            r.outcome = CameraCommandResult::Outcome::Blocked;
            r.message = block.reason.isEmpty()
                            ? tr("Cannot stop camera while an operation is active. Stop it first.")
                            : block.reason;
            emit commandFailed(r.message);
            refreshState();
            return r;
        }

        SPDLOG_INFO("CameraController: stop requested (gen={})", cap.lifecycleSnapshot().generation);
        cap.stop();
        backend_.processing().resetRealtimeMetrics();
        r.outcome = CameraCommandResult::Outcome::Accepted;
        r.message = tr("Camera stopped.");
        refreshState();
        return r;
    }

    CameraCommandResult CameraController::requestToggle()
    {
        return backend_.capture().isRunning() ? requestStop() : requestStart();
    }

    bool CameraController::startCapture(QString *errorMsg)
    {
        const auto r = requestStart();
        if (!r.accepted() && errorMsg) *errorMsg = r.message;
        return r.accepted();
    }

    bool CameraController::stopCapture(QString *errorMsg)
    {
        const auto r = requestStop();
        if (!r.accepted() && errorMsg) *errorMsg = r.message;
        return r.accepted();
    }

} // namespace frontend
