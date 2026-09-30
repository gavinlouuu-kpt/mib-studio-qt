#pragma once

#include <cstdint>

namespace backend::recording {

// Why a dequeued sort request did or did not drive a TTL pulse. Stored as a
// uint8 column in /trigger_events; keep values stable (append only).
enum class TriggerOutcome : uint8_t {
    Fired = 0,           // rising + falling edge driven on the bound camera
    DroppedNoCamera = 1, // dequeued with no camera bound
    DroppedSetFailed = 2,// camera refused setTriggerOutput(true)
    DroppedStale = 3,    // request belonged to an earlier camera session
    DroppedQueueFull = 4,// evicted from the pending queue before dequeue
};

// One sort-trigger request, from the classified source frame to the pulse
// (or the reason there was none). Every stamp is host monotonic microseconds
// (Tools::getTimestamp clock) so it is comparable with Frame::hostTimestampUs
// and, on grabbers whose frame clock is that same domain (Coaxlink on
// Windows), with Frame::timestamp directly. 0 means "not reached".
//
// This is the canonical record that lets a sort pulse be placed against the
// frame sequence after the fact: the source frame says which frame was
// classified, fireUs says when the PC drove the line, and lineEdgeUs (when a
// camera stamps its own inputs and the pulse is looped back) says when the
// hardware saw the edge.
struct TriggerEventRecord {
    uint64_t sequence{0};     // monotonically increasing per TriggerService
    uint64_t frameIndex{0};   // FrameStore write index of the classified frame
    uint64_t grabUs{0};       // host receipt stamp of that frame (Frame::hostTimestampUs)
    int32_t objectId{-1};
    int32_t trackId{-1};
    uint64_t generation{0};   // camera session the request was made under
    uint64_t requestUs{0};    // onTargetGroupResult entered (realtime thread)
    uint64_t wakeUs{0};       // trigger thread dequeued the request
    uint64_t fireUs{0};       // setTriggerOutput(true) returned
    uint64_t pulseDoneUs{0};  // setTriggerOutput(false) returned
    // Hardware-stamped rising edge of the looped-back pulse, in the camera's
    // Frame::timestamp domain (see ICamera::LineEvent). 0 = no loopback.
    uint64_t lineEdgeTimestamp{0};
    // Host stamp taken when that line event was delivered (0 = none).
    uint64_t lineEdgeHostUs{0};
    uint8_t outcome{static_cast<uint8_t>(TriggerOutcome::Fired)};
};

} // namespace backend::recording
