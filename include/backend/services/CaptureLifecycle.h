// Host-camera acquisition lifecycle contract (issue #365).
//
// Qt-free value types describing the state of the single acquisition session
// owned by CaptureService. A snapshot is the *authoritative* answer to "is
// the camera ready" — request acceptance (`CaptureService::start()` returning
// true) only means a session was scheduled, never that hardware opened.
//
// State machine (one session at a time, generation increments per start):
//
//   Idle ──start()──▶ Starting ──camera->start() ok──▶ Running
//     ▲                  │                               │
//     │                  │ camera/factory failure         │ stop() / natural
//     │                  ▼                               ▼   worker exit
//     │               Faulted ◀──health lost/exception── Stopping
//     │                  │                               │
//     └──stop()/start()──┴───────────────────────────────┘
//
// Faulted means the worker exited on its own; the thread stays joinable until
// the next start()/stop()/destructor reaps it (a restart therefore can never
// std::terminate on a still-joinable thread).
#pragma once

#include <cstdint>
#include <string>

namespace backend::services {

enum class CaptureLifecycleState {
    Idle,      // no session; no worker thread running
    Starting,  // worker scheduled, camera not yet confirmed open
    Running,   // camera->start() succeeded for this generation
    Stopping,  // stop requested, worker still winding down
    Faulted,   // worker exited without a stop request (failure detail below)
};

enum class CaptureFailureKind {
    None,
    NoCameraFactory,
    CameraFactoryReturnedNull,
    UnsupportedDeliveryMode,
    CameraStartFailed,       // ICamera::start() returned false
    DeviceHealthLost,        // periodic checkDeviceHealth() failed
    StreamEnded,             // camera stopped streaming without a stop request
    Exception,               // std::exception / unknown thrown in the worker
};

inline const char* toString(CaptureLifecycleState s)
{
    switch (s) {
    case CaptureLifecycleState::Idle: return "idle";
    case CaptureLifecycleState::Starting: return "starting";
    case CaptureLifecycleState::Running: return "running";
    case CaptureLifecycleState::Stopping: return "stopping";
    case CaptureLifecycleState::Faulted: return "faulted";
    }
    return "unknown";
}

inline const char* toString(CaptureFailureKind k)
{
    switch (k) {
    case CaptureFailureKind::None: return "none";
    case CaptureFailureKind::NoCameraFactory: return "noCameraFactory";
    case CaptureFailureKind::CameraFactoryReturnedNull: return "cameraFactoryReturnedNull";
    case CaptureFailureKind::UnsupportedDeliveryMode: return "unsupportedDeliveryMode";
    case CaptureFailureKind::CameraStartFailed: return "cameraStartFailed";
    case CaptureFailureKind::DeviceHealthLost: return "deviceHealthLost";
    case CaptureFailureKind::StreamEnded: return "streamEnded";
    case CaptureFailureKind::Exception: return "exception";
    }
    return "unknown";
}

// Immutable copy of the lifecycle at one instant. Cheap to take (one mutex).
struct CaptureLifecycleSnapshot {
    CaptureLifecycleState state{CaptureLifecycleState::Idle};
    // Session generation. 0 before the first start; every accepted start()
    // increments it. Consumers compare generations to discard stale
    // completions/requests (TriggerService, UI controllers).
    uint64_t generation{0};
    // True only between a successful camera->start() and the release of that
    // camera in the same generation. This — not start() acceptance — is the
    // "hardware ready" truth.
    bool cameraReady{false};
    // Most recent failure. Retained across an explicit stop() so a UI can
    // still show *why* the previous session ended; cleared by the next
    // successful camera start.
    CaptureFailureKind lastFailure{CaptureFailureKind::None};
    std::string lastFailureMessage;
    uint64_t lastFailureGeneration{0};
    // Host monotonic microseconds (Tools::getTimestamp) of the last state
    // transition — lets consumers detect a stale snapshot.
    uint64_t transitionHostTimeUs{0};

    bool isActive() const
    {
        return state == CaptureLifecycleState::Starting ||
               state == CaptureLifecycleState::Running;
    }
};

// Result of a start request. `Accepted` is scheduling, not readiness.
enum class CaptureStartOutcome {
    Accepted,          // new generation scheduled
    AlreadyActive,     // a session is Starting/Running; nothing changed
    RejectedStopping,  // a stop is still in flight on another thread
    RejectedNoFactory, // no camera factory configured
};

inline const char* toString(CaptureStartOutcome o)
{
    switch (o) {
    case CaptureStartOutcome::Accepted: return "accepted";
    case CaptureStartOutcome::AlreadyActive: return "alreadyActive";
    case CaptureStartOutcome::RejectedStopping: return "rejectedStopping";
    case CaptureStartOutcome::RejectedNoFactory: return "rejectedNoFactory";
    }
    return "unknown";
}

} // namespace backend::services
