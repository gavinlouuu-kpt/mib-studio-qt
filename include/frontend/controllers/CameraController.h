#pragma once

#include <QAction>
#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>

class QTimer;

namespace backend { class AppBackend; }

namespace frontend
{

    // Projection of the backend capture lifecycle for UI presentations
    // (issue #360). Every camera control (toolbar buttons, menu, keyboard
    // shortcut, empty-state affordance) derives its enabled state and text
    // from this one snapshot; none of them interprets CaptureService directly.
    struct CameraActionState
    {
        enum class Phase
        {
            Idle,          // no session
            Starting,      // start accepted, hardware not yet confirmed
            Running,       // camera confirmed open (lifecycle cameraReady)
            Stopping,      // stop in progress
            Failed,        // last session ended with a fault (see failureMessage)
            NotConfigured, // no camera selected/configured
        };

        Phase phase{Phase::NotConfigured};
        uint64_t generation{0};
        bool startEnabled{false};
        bool stopEnabled{false};
        // Non-empty when stop is refused by an active operation (experiment /
        // recording / flush). The same reason is returned to direct dispatch.
        QString stopBlockedReason;
        QString failureMessage;

        QString phaseText() const;
        bool operator==(const CameraActionState& o) const
        {
            return phase == o.phase && generation == o.generation &&
                   startEnabled == o.startEnabled && stopEnabled == o.stopEnabled &&
                   stopBlockedReason == o.stopBlockedReason && failureMessage == o.failureMessage;
        }
        bool operator!=(const CameraActionState& o) const { return !(*this == o); }
    };

    // Typed result of a camera command. `Accepted` means the request was
    // handed to the backend; the state snapshot (stateChanged signal) carries
    // the eventual hardware outcome.
    struct CameraCommandResult
    {
        enum class Outcome
        {
            Accepted,
            AlreadyInState, // start while running / stop while idle
            Blocked,        // refused by operation guard or an in-flight command
            Failed,         // backend rejected the request synchronously
        };
        Outcome outcome{Outcome::Failed};
        QString message;
        bool accepted() const { return outcome == Outcome::Accepted; }
    };

    // Describes an operation that owns the camera and forbids an ordinary
    // camera stop (active experiment, recording, HDF5 flush in progress).
    struct CameraOperationBlock
    {
        bool blocked{false};
        QString reason;
    };

    // Single authoritative UI command path for camera acquisition. Owned by
    // MainWindow; all presentations bind to startAction()/stopAction() or
    // call requestStart()/requestStop()/requestToggle() and read state().
    //
    // Guarantees:
    //  - the operation guard runs on every stop request, including direct
    //    dispatch while a button is disabled;
    //  - duplicate/rapid requests cannot issue overlapping backend commands
    //    (one command in flight; the rest are Blocked);
    //  - state is projected from CaptureService::lifecycleSnapshot() by a
    //    bounded poll (no per-frame signals) plus immediate refresh after
    //    each command, so a camera that fails/disconnects on its own is shown
    //    as Failed with the backend's structured message.
    class CameraController : public QObject
    {
        Q_OBJECT
    public:
        explicit CameraController(backend::AppBackend &backend, QObject *parent = nullptr);

        // Read-only provider of the current operation ownership (experiment /
        // recording / flush). Consulted at dispatch time.
        using OperationGuard = std::function<CameraOperationBlock()>;
        void setOperationGuard(OperationGuard guard);

        bool isRunning() const;
        bool isConfigured() const;
        CameraActionState state() const { return state_; }

        // Shared actions for toolbar / menu / shortcut presentations.
        QAction* startAction() const { return startAction_; }
        QAction* stopAction() const { return stopAction_; }

        CameraCommandResult requestStart();
        CameraCommandResult requestStop();
        // Space-bar style toggle: start when idle/failed, stop when active.
        CameraCommandResult requestToggle();

        // Re-project the backend lifecycle into state() now (also called by
        // the internal poll). Emits stateChanged when anything changed.
        void refreshState();

        // Backwards-compatible wrappers.
        bool startCapture(QString *errorMsg = nullptr);
        bool stopCapture(QString *errorMsg = nullptr);

        // Poll interval for lifecycle projection (default 250 ms; tests may
        // lower it). Transitions after a command are additionally polled at
        // a faster cadence until the session leaves Starting/Stopping.
        void setPollIntervalMs(int ms);

    signals:
        void stateChanged(const frontend::CameraActionState &state);
        void cameraStarted();
        void cameraStopped();
        // A command was refused or failed; message is operator-facing.
        void commandFailed(const QString &message);

    private:
        CameraActionState project() const;
        CameraCommandResult doRequestStart();
        CameraCommandResult doRequestStop();
        void publish(const CameraActionState &next);
        void onPoll();

        backend::AppBackend &backend_;
        OperationGuard guard_;
        CameraActionState state_{};
        QAction* startAction_{nullptr};
        QAction* stopAction_{nullptr};
        QTimer* pollTimer_{nullptr};
        bool commandInFlight_{false};
        int pollIntervalMs_{250};
        int settlingPolls_{0};
        bool lastReportedRunning_{false};
    };

} // namespace frontend
