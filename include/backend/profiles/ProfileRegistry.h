#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace backend::profiles {

enum class RegistryErrorCode {
    Offline,
    Authentication,
    Permission,
    Conflict,
    Integrity,
    Invalid,
    Storage,
    NotFound,
    Unsupported
};
class RegistryError : public std::runtime_error {
public:
    RegistryError(RegistryErrorCode code, const std::string& message)
        : std::runtime_error(message), code(code) {}
    RegistryErrorCode code;
};

enum class CentralState { Submitted, Approved, Rejected, Published, Superseded, Archived, Revoked };
const char* toString(CentralState state);
CentralState centralStateFromString(const std::string& state);

// An envelope around the existing config.json + egrabberConfig.js payload;
// no alternate processing configuration model. Metadata is independently versioned.
struct Revision {
    std::string methodId;
    std::string revisionId;
    std::string parentRevisionId;
    std::string projectId;
    std::string displayName;
    std::string authorId;
    std::string canonicalContent;
    std::string contentHash;
    uint64_t revisionNumber{0};
    uint64_t metadataVersion{0};
    CentralState state{CentralState::Submitted};
};

// MIB canonical method format v1: UTF-8, sorted object keys, compact JSON,
// nlohmann 3.x number encoding; integral doubles in the safe integer range
// normalize to integers. Reject duplicate keys, invalid UTF-8, and non-object
// config. Hash covers config AND camera script AND compatibility declaration.
std::string canonicalMethod(const std::string& configJson, const std::string& cameraScript,
                            const std::string& processingCoreId, int processingContractVersion,
                            const std::string& hardwareCompatibilityJson = "{}");
std::string contentHash(const std::string& bytes);
void verifyRevision(const Revision& revision);

struct RevisionPage {
    std::vector<Revision> revisions;
    std::string nextCursor;
};

// Called only by a registry worker/control path, never acquisition, recording,
// readiness or Start. Implementations must bound requests and expose failures.
// Instances are confined to their owning thread unless otherwise documented.
class ProfileRegistry {
public:
    virtual ~ProfileRegistry() = default;
    virtual RevisionPage listRevisions(const std::string& projectId,
                                       const std::string& cursor = {}) = 0;
    virtual Revision fetchRevision(const std::string& revisionId) = 0;
    virtual Revision submit(const Revision& draft, const std::string& expectedHead) = 0;
    virtual Revision transition(const std::string& revisionId, CentralState state,
                                uint64_t expectedMetadataVersion, const std::string& reason) = 0;
};

struct LocalValidation {
    std::string revisionId;
    std::string instrumentId;
    std::string contentHash;
    // Exact local context fingerprint: backend/core/device/calibration identities.
    std::string contextHash;
    std::string validatorId;
    std::string evidence;
    bool passed{false};
};

// This is method eligibility only, never hardware readiness or permission to Start.
struct Eligibility {
    bool eligible{false};
    std::string reason;
};

} // namespace backend::profiles
