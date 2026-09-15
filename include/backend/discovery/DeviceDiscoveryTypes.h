// Value types of the backend device-discovery job service (issue #419,
// ADR 0005). Frontend-neutral: C++17 values only, no Qt, no vendor handles.
// Enum values are stable and append-only because the bridge contract mirrors
// them (crates/mib-bridge/contract/bridge-contract.json).
#pragma once

#include "backend/nanopositioner/INanopositionerBackend.h"
#include "backend/services/CameraControlService.h"
#include "backend/services/ISerialPort.h"
#include "backend/services/PulseGeneratorService.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace backend::discovery {

enum class DeviceKind : int {
    Camera = 0,
    Framegrabber = 1,
    Nanopositioner = 2,
    PulseGenerator = 3,
};

enum class JobState : int {
    Queued = 0,
    Running = 1,
    Completed = 2,
    Cancelled = 3,
    Failed = 4,
};

// How much a candidate's identity can be trusted across sessions. Transient
// SDK indices and OS port paths are SessionLocal and must never be persisted
// as identity.
enum class IdentityStrength : int {
    None = 0,
    SessionLocal = 1,
    Persistent = 2,
};

enum class IdentificationStatus : int {
    Identified = 0,   // a read-only protocol/SDK probe confirmed the instrument
    Unidentified = 1, // enumerated only (adapter VID/PID, bare Modbus reply, ...)
    Ambiguous = 2,    // conflicting provider/vendor claims for one endpoint
    Unsupported = 3,  // enumerated but no usable identity probe exists
};

enum class ErrorKind : int {
    None = 0,
    InvalidRequest = 1,
    Busy = 2,
    OpenFailed = 3,
    PermissionDenied = 4,
    Timeout = 5,
    MalformedResponse = 6,
    Unsupported = 7,
    MissingSdk = 8,
    ProviderException = 9,
    Cancelled = 10,
    Overflow = 11,
    ShuttingDown = 12,
    TooManyJobs = 13,
};

const char* toString(DeviceKind kind);
const char* toString(JobState state);
const char* toString(IdentityStrength strength);
const char* toString(IdentificationStatus status);
const char* toString(ErrorKind kind);

inline bool isTerminal(JobState state)
{
    return state == JobState::Completed || state == JobState::Cancelled ||
           state == JobState::Failed;
}

using PulseGeneratorHit = services::PulseGeneratorService::ScanHit;

// Structured endpoint. `systemPath` is the current OS path (COM7, ttyUSB0);
// `persistentId` is the adapter identity that survives re-enumeration;
// `busAddress` is the Modbus/serial device address on a shared bus. Camera
// entries use the SDK/GenTL indices.
struct DeviceEndpoint {
    std::string systemPath;
    std::string persistentId;
    int sdkIndex{-1};
    int interfaceIndex{-1};
    int deviceIndex{-1};
    int streamIndex{-1};
    int busAddress{-1};
    std::optional<std::uint16_t> vendorId;
    std::optional<std::uint16_t> productId;
};

struct Diagnostic {
    ErrorKind kind{ErrorKind::None};
    std::string message;
};

struct DiscoveredDevice {
    DeviceKind kind{DeviceKind::Camera};
    std::string providerId;
    std::string displayName;
    DeviceEndpoint endpoint;
    std::string stableIdentity;
    IdentityStrength identityStrength{IdentityStrength::None};
    IdentificationStatus identification{IdentificationStatus::Unidentified};
    std::vector<std::string> claimedBy;   // provider IDs that identified this endpoint
    std::vector<std::string> capabilities; // known from safe discovery only
    std::vector<Diagnostic> diagnostics;
    bool synthetic{false}; // explicit synthetic source (mock), never a physical match

    // Legacy per-kind payloads preserved verbatim for existing consumers.
    std::optional<services::DiscoveredCamera> camera;
    std::optional<services::DiscoveredFramegrabber> framegrabber;
    std::optional<nanopositioner::Endpoint> nanopositioner;
    std::optional<PulseGeneratorHit> pulseGenerator;
};

// Explicit serial scope for pulse-generator scans (never a broad sweep).
struct SerialScanScope {
    std::string portName;
    services::SerialSettings settings{};
    std::uint8_t addressFrom{1};
    std::uint8_t addressTo{16};
    int perAddressTimeoutMs{250};
};

struct RetryPolicy {
    int maxRetries{0}; // additional attempts after the first
    std::chrono::milliseconds delay{0};
};

struct DiscoveryRequest {
    std::vector<DeviceKind> kinds;
    std::vector<std::string> providers; // empty = every provider of the kinds
    std::optional<nanopositioner::Endpoint> preferredNanopositioner;
    std::optional<SerialScanScope> serialScope;
    std::chrono::milliseconds initialDelay{0};
    std::chrono::milliseconds deadline{60000};
    RetryPolicy retry;
    std::string origin; // diagnostics only ("startup", "connect-tab", ...)
};

struct DiscoveryError {
    std::string providerId;
    ErrorKind kind{ErrorKind::None};
    std::string message;
    std::string endpoint;
};

struct DiscoverySnapshot {
    std::uint64_t jobId{0};
    std::uint64_t generation{0};
    JobState state{JobState::Queued};
    bool complete{false}; // identity coverage complete: no errors, no overflow
    bool overflow{false};
    int attempt{0};
    int maxAttempts{1};
    std::vector<DiscoveredDevice> candidates;
    std::vector<DiscoveryError> errors;
    std::vector<std::string> providersRun;
    std::string origin;
};

struct StartResult {
    bool accepted{false};
    bool coalesced{false};
    std::uint64_t jobId{0};
    ErrorKind rejection{ErrorKind::None};
    std::string reason;
};

inline constexpr std::size_t kMaxCandidates = 256;
inline constexpr std::size_t kMaxErrors = 64;
inline constexpr std::size_t kMaxRetainedJobs = 16;
inline constexpr std::size_t kMaxConcurrentJobs = 4;
inline constexpr int kMaxRetries = 10;
inline constexpr std::chrono::milliseconds kMaxRetryDelay{60000};
inline constexpr std::chrono::milliseconds kMaxInitialDelay{60000};
inline constexpr std::chrono::milliseconds kMaxDeadline{600000};

} // namespace backend::discovery
