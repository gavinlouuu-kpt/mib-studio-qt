// Per-metric telemetry validity and freshness (issue #368).
//
// Every acquisition metric carries its own validity, sample time, and
// session generation. "Unavailable"/"Unsupported" must never be rendered or
// stored as a measured zero; a metric older than the freshness window is
// Stale, structurally distinct from a current value.
#pragma once

#include "backend/camera/common/TimestampValue.h"

#include <cstdint>
#include <string>

namespace backend::services {

enum class MetricValidity {
    Valid,
    Unavailable, // backend has not produced this metric (yet) in this session
    Unsupported, // backend can never produce it (e.g. MindVision queue depth)
    Error,       // the last poll failed
    Stale,       // was Valid, but older than the freshness window
};

inline const char* toString(MetricValidity v)
{
    switch (v) {
    case MetricValidity::Valid: return "valid";
    case MetricValidity::Unavailable: return "unavailable";
    case MetricValidity::Unsupported: return "unsupported";
    case MetricValidity::Error: return "error";
    case MetricValidity::Stale: return "stale";
    }
    return "unknown";
}

inline MetricValidity metricValidityFromString(const std::string& s)
{
    if (s == "valid") return MetricValidity::Valid;
    if (s == "unsupported") return MetricValidity::Unsupported;
    if (s == "error") return MetricValidity::Error;
    if (s == "stale") return MetricValidity::Stale;
    return MetricValidity::Unavailable;
}

struct MetricSample {
    uint64_t value{0};
    MetricValidity validity{MetricValidity::Unavailable};
    uint64_t sampleHostTimeUs{0};  // Tools::getTimestamp() when sampled (0 = never)
    uint64_t sessionGeneration{0}; // acquisition session the sample belongs to
    uint64_t ageUs{0};             // now - sampleHostTimeUs at snapshot time

    bool isValid() const { return validity == MetricValidity::Valid; }
    // A measured value exists (valid or stale); unavailable/unsupported/error do not.
    bool hasValue() const { return validity == MetricValidity::Valid || validity == MetricValidity::Stale; }
};

// Consistent view of the acquisition telemetry at one instant.
struct AcquisitionTelemetrySnapshot {
    uint64_t sessionGeneration{0};
    uint64_t snapshotHostTimeUs{0};
    uint64_t freshnessWindowUs{0};
    bool sessionActive{false};

    MetricSample framesDelivered;      // frames handed to the application by CaptureService
    MetricSample captureFrameRate;     // device/SDK-reported fps
    MetricSample captureDataRateMBps;  // device/SDK-reported MB/s
    MetricSample sdkCompletedQueueDepth;
    MetricSample sdkInputBufferCount;
    MetricSample bufferUnderruns;
    MetricSample transportLostFrames;  // transport/SDK loss (never merged with the next)
    MetricSample intentionallyDiscardedFrames; // LatestFrame policy discards (deliberate)
    MetricSample frameAgeUs;           // device stamp -> host dequeue (only host-comparable domains)
    MetricSample publishLatencyUs;     // host dequeue -> FrameStore publish

    ::camera::common::TimestampDescriptor timestampDescriptor;
};

} // namespace backend::services
