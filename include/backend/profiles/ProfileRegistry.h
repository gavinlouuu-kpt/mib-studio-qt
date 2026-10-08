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
    // Immutable author's notes submitted with the revision (#398 M3). Not
    // part of the content hash: metadata, never executed.
    std::string releaseNotes;
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

// SHA-256 of a config.json in the canonical form used inside method envelopes
// (sorted keys, compact, integral doubles as integers). Empty when the text is
// not a valid JSON object. Lets the backend recognise an applied config.json
// as exactly the config of a cached central revision (#398 M2).
std::string canonicalConfigSha256(const std::string& configJson) noexcept;
// The same hash of the config embedded in a canonical method envelope; empty
// when the envelope is unreadable.
std::string revisionConfigSha256(const std::string& canonicalContent) noexcept;

// The local execution context a method validation is bound to (#398 M2): a
// validation recorded on this instrument for this processing core build and
// camera source does not carry over to a different core or camera source.
struct MethodContext {
    std::string instrumentId;          // InstrumentIdentity::id
    std::string processingCoreVersion;
    std::string processingCoreSha256;
    std::string cameraSource;          // effective: "mock" | "egrabber" | "mindvision"
};
// Stable fingerprint of the context; empty when instrumentId is empty
// (unknown instrument: nothing can be validated).
std::string methodContextHash(const MethodContext& context);
// Compact JSON object describing the context (stored with validations).
std::string methodContextJson(const MethodContext& context);

// Dotted JSON paths whose value differs between two documents (objects
// recurse; arrays and scalars compare whole; 2 == 2.0). Unparsable input on
// either side yields {"<entire document>"}. At most 200 entries + "... (more)".
std::vector<std::string> jsonDifferences(const std::string& a, const std::string& b);

// A listed revision that failed integrity/canonical verification. It is never
// cached; reporting it lets the cursor advance past it instead of stalling sync.
struct RejectedRevision {
    std::string revisionId; // empty when the item carried no usable ID
    std::string reason;
};

struct RevisionPage {
    std::vector<Revision> revisions;
    std::vector<RejectedRevision> rejected;
    std::string nextCursor;
};

// A project the authenticated user is a member of, with their roles there.
struct RegistryProject {
    std::string projectId;
    std::string displayName;
    std::vector<std::string> roles;
};

// A method (the lineage revisions belong to) and its published head, used
// to detect a draft whose base is no longer the head before submitting (#398 M3).
struct RegistryMethod {
    std::string methodId;
    std::string projectId;
    std::string displayName;
    std::string description;
    std::string headRevisionId; // empty until a revision is published
};

// Review decisions and audit events of one revision (#398 M3).
struct RevisionHistory {
    struct Review {
        std::string reviewerId;
        std::string decision; // "approved" | "rejected"
        std::string reason;
        std::string contentHash;
        std::string createdAt;
    };
    struct Event {
        std::string actorId;
        std::string action; // submitted, approved, published, superseded, ...
        std::string reason;
        std::string createdAt;
    };
    std::string revisionId;
    std::vector<Review> reviews;
    std::vector<Event> events;
};

// Called only by a registry worker/control path, never acquisition, recording,
// readiness or Start. Implementations must bound requests and expose failures.
// Instances are confined to their owning thread unless otherwise documented.
class ProfileRegistry {
public:
    virtual ~ProfileRegistry() = default;
    virtual std::vector<RegistryProject> listProjects() = 0;
    virtual RevisionPage listRevisions(const std::string& projectId,
                                       const std::string& cursor = {}) = 0;
    virtual Revision fetchRevision(const std::string& revisionId) = 0;
    virtual Revision submit(const Revision& draft, const std::string& expectedHead) = 0;
    virtual Revision transition(const std::string& revisionId, CentralState state,
                                uint64_t expectedMetadataVersion, const std::string& reason) = 0;
    // #398 M3 authoring.
    virtual std::vector<RegistryMethod> listMethods(const std::string& projectId) = 0;
    // Idempotent by the caller-generated methodId.
    virtual RegistryMethod createMethod(const std::string& projectId, const std::string& methodId,
                                        const std::string& displayName,
                                        const std::string& description) = 0;
    virtual RevisionHistory revisionHistory(const std::string& revisionId) = 0;
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
