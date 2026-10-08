#include "backend/app/MethodProvenance.h"

#include "backend/profiles/ProfileRegistryWorker.h"

#include <nlohmann/json.hpp>

#include <tuple>

namespace backend::app {
namespace {
using Json = nlohmann::json;
using profiles::CentralState;

bool usable(CentralState state) {
    return state == CentralState::Published || state == CentralState::Superseded;
}

const profiles::LocalValidationRecord* validationFor(const profiles::RegistryWorkerSnapshot& registry,
                                                     const profiles::CachedRevisionSummary& revision,
                                                     const std::string& instrumentId,
                                                     const std::string& contextHash) {
    if (instrumentId.empty() || contextHash.empty()) return nullptr;
    for (const auto& record : registry.validations) { // newest first
        const auto& v = record.validation;
        if (v.revisionId == revision.revisionId && v.instrumentId == instrumentId &&
            v.contextHash == contextHash && v.contentHash == revision.contentHash)
            return &record;
    }
    return nullptr;
}

std::string shortHash(const std::string& hash) { return hash.substr(0, 12); }

std::string revisionLabel(const MethodProvenance& m) {
    return "\"" + m.displayName + "\" r" + std::to_string(m.revisionNumber);
}
} // namespace

MethodProvenance resolveMethodProvenance(const std::string& appliedConfigCanonicalSha256,
                                         const profiles::RegistryWorkerSnapshot& registry,
                                         const profiles::MethodContext& context,
                                         const std::string& instrumentName) {
    MethodProvenance m;
    m.configCanonicalSha256 = appliedConfigCanonicalSha256;
    m.registryOrigin = registry.origin;
    m.registrySession = registry.configured ? profiles::toString(registry.session) : "unconfigured";
    m.instrumentId = context.instrumentId;
    m.instrumentName = instrumentName;
    m.contextHash = profiles::methodContextHash(context);
    if (appliedConfigCanonicalSha256.empty()) return m; // source "none"

    // Pick among cached revisions whose embedded config is exactly the applied
    // one: usable state first, then validated here, then published over
    // superseded, then the newest revision.
    const profiles::CachedRevisionSummary* best = nullptr;
    const profiles::LocalValidationRecord* bestValidation = nullptr;
    auto rank = [&](const profiles::CachedRevisionSummary& r,
                    const profiles::LocalValidationRecord* v) {
        return std::make_tuple(usable(r.state) ? 1 : 0,
                               v && v->validation.passed ? 1 : 0,
                               r.state == CentralState::Published ? 1 : 0, r.revisionNumber);
    };
    for (const auto& r : registry.revisions) {
        if (r.configSha256.empty() || r.configSha256 != appliedConfigCanonicalSha256) continue;
        ++m.matchingRevisions;
        const auto* v = validationFor(registry, r, m.instrumentId, m.contextHash);
        if (!best || rank(r, v) > rank(*best, bestValidation)) {
            best = &r;
            bestValidation = v;
        }
    }
    if (!best) {
        m.source = "local";
        return m;
    }
    m.source = "central";
    m.match = "config_json";
    m.revisionId = best->revisionId;
    m.methodId = best->methodId;
    m.projectId = best->projectId;
    m.displayName = best->displayName;
    m.authorId = best->authorId;
    m.contentHash = best->contentHash;
    m.revisionNumber = best->revisionNumber;
    m.metadataVersion = best->metadataVersion;
    m.centralState = profiles::toString(best->state);
    if (!bestValidation) {
        m.validation = "none";
        return m;
    }
    m.validation = bestValidation->validation.passed ? "passed" : "failed";
    m.validatorId = bestValidation->validation.validatorId;
    m.validatedAtUtc = bestValidation->validatedAtUtc;
    try {
        m.validationEvidenceSha256 = Json::parse(bestValidation->validation.evidence)
                                         .value("run_file_sha256", std::string{});
    } catch (const Json::exception&) {
        m.validationEvidenceSha256.clear();
    }
    return m;
}

ReadinessGate methodRevisionGate(const MethodProvenance& m) {
    ReadinessGate g;
    g.id = "method.revision";
    if (m.source == "none") {
        g.status = GateStatus::NotRequired;
        g.reason = "no config.json applied";
        return g;
    }
    if (m.source != "central") {
        g.status = GateStatus::NotRequired;
        g.reason = "local method: the applied config.json matches no cached central revision";
        g.detail = "config " + shortHash(m.configCanonicalSha256);
        return g;
    }
    g.detail = revisionLabel(m) + " (" + m.revisionId + ") " + m.centralState + ", content " +
               shortHash(m.contentHash);
    if (m.matchingRevisions > 1)
        g.detail += "; " + std::to_string(m.matchingRevisions) + " cached revisions share this config";
    if (m.centralState == "revoked") {
        g.status = GateStatus::Fail;
        g.reason = "central revision " + revisionLabel(m) + " is REVOKED";
        g.remediation = "apply a published revision of this method (existing runs stay reviewable)";
        return g;
    }
    if (m.centralState != "published" && m.centralState != "superseded") {
        g.status = GateStatus::Fail;
        g.reason = "central revision " + revisionLabel(m) + " is " + m.centralState +
                   ", not published";
        g.remediation = "apply a published revision";
        return g;
    }
    if (m.instrumentId.empty()) {
        g.status = GateStatus::Warn;
        g.reason = "instrument identity unknown; local validation cannot be checked";
        g.remediation = "make the data directory writable and restart";
        return g;
    }
    if (m.validation == "passed") {
        g.status = GateStatus::Pass;
        g.detail += "; validated here by " + m.validatorId + " at " + m.validatedAtUtc + " UTC";
        if (m.centralState == "superseded") g.detail += "; a newer revision is published";
        return g;
    }
    g.status = GateStatus::Warn;
    if (m.validation == "failed") {
        g.reason = "local validation of " + revisionLabel(m) +
                   " FAILED on this instrument/context";
        g.remediation = "review the failed test run; re-validate or apply another revision";
    } else {
        g.reason = "central revision " + revisionLabel(m) +
                   " is not validated on this instrument/context";
        g.remediation = "record a test run with this revision and mark it validated";
    }
    return g;
}

std::string methodInvalidationKey(const MethodProvenance& m) {
    return m.source + "|" + m.configCanonicalSha256 + "|" + m.revisionId + "|" + m.centralState +
           "|" + std::to_string(m.metadataVersion) + "|" + m.validation + "|" + m.validatedAtUtc +
           "|" + m.contextHash;
}

std::string methodProvenanceToJson(const MethodProvenance& m) {
    Json j = {{"source", m.source},
              {"config_canonical_sha256", m.configCanonicalSha256},
              {"registry_origin", m.registryOrigin},
              {"registry_session", m.registrySession},
              {"instrument_id", m.instrumentId},
              {"instrument_name", m.instrumentName},
              {"context_hash", m.contextHash},
              {"validation", m.validation}};
    if (m.source == "central") {
        j["match"] = m.match;
        j["revision_id"] = m.revisionId;
        j["method_id"] = m.methodId;
        j["project_id"] = m.projectId;
        j["display_name"] = m.displayName;
        j["author_id"] = m.authorId;
        j["content_hash"] = m.contentHash;
        j["revision_number"] = m.revisionNumber;
        j["metadata_version"] = m.metadataVersion;
        j["central_state"] = m.centralState;
        j["matching_revisions"] = m.matchingRevisions;
        j["validator_id"] = m.validatorId;
        j["validated_at_utc"] = m.validatedAtUtc;
        j["validation_evidence_sha256"] = m.validationEvidenceSha256;
    }
    return j.dump(-1, ' ', false, Json::error_handler_t::replace);
}

} // namespace backend::app
