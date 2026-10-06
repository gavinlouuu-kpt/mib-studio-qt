// method_provenance_test (#398 M2): the pure method-resolution + gate policy.
//  - no config / unmatched config -> NotRequired (local method);
//  - central published: not validated -> Warn, validated here -> Pass,
//    validated on another instrument or another context -> Warn, failed
//    validation -> Warn, unknown instrument -> Warn;
//  - revoked / not published -> Fail (blocks Start);
//  - several revisions sharing the config: usable state first, then validated
//    here, then published over superseded, then the newest;
//  - the invalidation key moves with every gate input; the JSON block
//    round-trips every provenance field.
#include "backend/app/MethodProvenance.h"
#include "backend/profiles/ProfileRegistryWorker.h"

#include "support/assert.h"

#include <nlohmann/json.hpp>

using namespace backend::app;
using namespace backend::profiles;
using Json = nlohmann::json;

namespace {

const std::string kConfig = std::string(64, 'a');
const std::string kInstrument = "123e4567-e89b-42d3-a456-426614174000";

MethodContext context(const std::string& instrument = kInstrument) {
    return {instrument, "1.2.3", std::string(64, 'c'), "mock"};
}

CachedRevisionSummary revision(const std::string& id, uint64_t number, CentralState state,
                               const std::string& configSha = kConfig) {
    CachedRevisionSummary r;
    r.revisionId = id;
    r.methodId = "m1";
    r.projectId = "p1";
    r.displayName = "Cell Sorting";
    r.authorId = "alice";
    r.contentHash = std::string(63, 'h') + std::to_string(number % 10);
    r.revisionNumber = number;
    r.metadataVersion = 1;
    r.state = state;
    r.configSha256 = configSha;
    return r;
}

LocalValidationRecord validation(const CachedRevisionSummary& r, bool passed,
                                 const MethodContext& ctx = context()) {
    LocalValidationRecord v;
    v.validation.revisionId = r.revisionId;
    v.validation.instrumentId = ctx.instrumentId;
    v.validation.contentHash = r.contentHash;
    v.validation.contextHash = methodContextHash(ctx);
    v.validation.validatorId = "user-bob";
    v.validation.evidence = R"({"run_file_sha256":")" + std::string(64, 'e') + R"("})";
    v.validation.passed = passed;
    v.validatedAtUtc = "2026-10-04 10:00:00";
    return v;
}

RegistryWorkerSnapshot registry(std::vector<CachedRevisionSummary> revisions,
                                std::vector<LocalValidationRecord> validations = {}) {
    RegistryWorkerSnapshot s;
    s.configured = true;
    s.origin = "https://registry.example";
    s.session = RegistryWorkerSnapshot::Session::SignedIn;
    s.revisions = std::move(revisions);
    s.validations = std::move(validations);
    return s;
}

GateStatus gateOf(const MethodProvenance& m) { return methodRevisionGate(m).status; }

} // namespace

int main() {
    // No config applied / no match.
    {
        const auto none = resolveMethodProvenance("", registry({revision("r1", 1, CentralState::Published)}),
                                                  context(), "MIB-01");
        MIB_EXPECT(none.source == "none" && gateOf(none) == GateStatus::NotRequired, "no config: NotRequired");
        const auto local = resolveMethodProvenance(
            std::string(64, 'z'), registry({revision("r1", 1, CentralState::Published)}), context(), "MIB-01");
        MIB_EXPECT(local.source == "local" && local.revisionId.empty(), "unmatched config is local");
        MIB_EXPECT(gateOf(local) == GateStatus::NotRequired, "local method: NotRequired");
        MIB_EXPECT(!methodRevisionGate(local).blocksStart(), "local never blocks");
        RegistryWorkerSnapshot unconfigured;
        const auto offline = resolveMethodProvenance(kConfig, unconfigured, context(), "");
        MIB_EXPECT(offline.source == "local" && offline.registrySession == "unconfigured",
                   "no registry: local, session unconfigured");
    }

    // Central published: validation states.
    {
        const auto r1 = revision("r1", 1, CentralState::Published);
        auto m = resolveMethodProvenance(kConfig, registry({r1}), context(), "MIB-01");
        MIB_EXPECT(m.source == "central" && m.revisionId == "r1" && m.validation == "none",
                   "central revision matched");
        auto g = methodRevisionGate(m);
        MIB_EXPECT(g.status == GateStatus::Warn && !g.blocksStart(), "unvalidated: Warn, Start allowed");
        MIB_EXPECT(g.reason.find("not validated") != std::string::npos, "reason says why");

        m = resolveMethodProvenance(kConfig, registry({r1}, {validation(r1, true)}), context(), "MIB-01");
        g = methodRevisionGate(m);
        MIB_EXPECT(m.validation == "passed" && g.status == GateStatus::Pass, "validated here: Pass");
        MIB_EXPECT(m.validatorId == "user-bob" && m.validationEvidenceSha256 == std::string(64, 'e') &&
                       m.validatedAtUtc == "2026-10-04 10:00:00",
                   "who/when/evidence carried");

        m = resolveMethodProvenance(kConfig,
                                    registry({r1}, {validation(r1, true, context("223e4567-e89b-42d3-a456-426614174000"))}),
                                    context(), "MIB-01");
        MIB_EXPECT(m.validation == "none" && gateOf(m) == GateStatus::Warn, "other instrument's validation does not count");

        auto otherCore = context();
        otherCore.processingCoreSha256 = std::string(64, 'd');
        m = resolveMethodProvenance(kConfig, registry({r1}, {validation(r1, true, otherCore)}), context(), "MIB-01");
        MIB_EXPECT(m.validation == "none" && gateOf(m) == GateStatus::Warn, "other core build's validation does not count");

        auto stale = validation(r1, true);
        stale.validation.contentHash = std::string(64, '0');
        m = resolveMethodProvenance(kConfig, registry({r1}, {stale}), context(), "MIB-01");
        MIB_EXPECT(m.validation == "none", "validation of different content does not count");

        m = resolveMethodProvenance(kConfig, registry({r1}, {validation(r1, false)}), context(), "MIB-01");
        g = methodRevisionGate(m);
        MIB_EXPECT(m.validation == "failed" && g.status == GateStatus::Warn &&
                       g.reason.find("FAILED") != std::string::npos,
                   "failed validation: Warn naming the failure");

        m = resolveMethodProvenance(kConfig, registry({r1}, {validation(r1, true)}), context(""), "");
        MIB_EXPECT(gateOf(m) == GateStatus::Warn && m.contextHash.empty(), "unknown instrument: Warn");

        const auto sup = revision("r0", 1, CentralState::Superseded);
        m = resolveMethodProvenance(kConfig, registry({sup}, {validation(sup, true)}), context(), "MIB-01");
        g = methodRevisionGate(m);
        MIB_EXPECT(g.status == GateStatus::Pass && g.detail.find("newer revision") != std::string::npos,
                   "superseded + validated: Pass, noted");
    }

    // Revoked / not published block Start.
    {
        const auto revoked = revision("r2", 2, CentralState::Revoked);
        auto m = resolveMethodProvenance(kConfig, registry({revoked}, {validation(revoked, true)}), context(), "MIB-01");
        auto g = methodRevisionGate(m);
        MIB_EXPECT(m.centralState == "revoked" && g.status == GateStatus::Fail && g.blocksStart(),
                   "revoked blocks Start even if validated earlier");
        MIB_EXPECT(g.reason.find("REVOKED") != std::string::npos, "reason names revocation");
        for (auto state : {CentralState::Submitted, CentralState::Approved, CentralState::Rejected,
                           CentralState::Archived}) {
            m = resolveMethodProvenance(kConfig, registry({revision("rx", 3, state)}), context(), "MIB-01");
            MIB_EXPECT(gateOf(m) == GateStatus::Fail, std::string("not published blocks: ") + toString(state));
        }
    }

    // Several revisions share the config.
    {
        const auto revoked = revision("r9", 9, CentralState::Revoked);
        const auto published = revision("r3", 3, CentralState::Published);
        const auto superseded = revision("r2", 2, CentralState::Superseded);
        auto m = resolveMethodProvenance(kConfig, registry({revoked, superseded, published}), context(), "MIB-01");
        MIB_EXPECT(m.revisionId == "r3" && m.matchingRevisions == 3, "published preferred over newer revoked");
        MIB_EXPECT(methodRevisionGate(m).detail.find("3 cached revisions") != std::string::npos, "ambiguity reported");
        m = resolveMethodProvenance(kConfig, registry({revoked, superseded, published}, {validation(superseded, true)}),
                                    context(), "MIB-01");
        MIB_EXPECT(m.revisionId == "r2" && gateOf(m) == GateStatus::Pass, "validated-here revision preferred");
        const auto published5 = revision("r5", 5, CentralState::Published);
        m = resolveMethodProvenance(kConfig, registry({published, published5}), context(), "MIB-01");
        MIB_EXPECT(m.revisionId == "r5", "newest published wins a tie");
        m = resolveMethodProvenance(kConfig, registry({revoked, revision("r8", 8, CentralState::Revoked)}), context(), "MIB-01");
        MIB_EXPECT(m.revisionId == "r9" && gateOf(m) == GateStatus::Fail, "only revoked matches: still Fail");
    }

    // Invalidation key + JSON.
    {
        const auto r1 = revision("r1", 1, CentralState::Published);
        const auto unvalidated = resolveMethodProvenance(kConfig, registry({r1}), context(), "MIB-01");
        const auto validated = resolveMethodProvenance(kConfig, registry({r1}, {validation(r1, true)}), context(), "MIB-01");
        auto revokedRev = r1;
        revokedRev.state = CentralState::Revoked;
        revokedRev.metadataVersion = 2;
        const auto revoked = resolveMethodProvenance(kConfig, registry({revokedRev}), context(), "MIB-01");
        auto otherSource = context();
        otherSource.cameraSource = "egrabber";
        const auto moved = resolveMethodProvenance(kConfig, registry({r1}), otherSource, "MIB-01");
        MIB_EXPECT(methodInvalidationKey(unvalidated) != methodInvalidationKey(validated) &&
                       methodInvalidationKey(unvalidated) != methodInvalidationKey(revoked) &&
                       methodInvalidationKey(unvalidated) != methodInvalidationKey(moved),
                   "validation, revocation and context each move the key");
        MIB_EXPECT(methodInvalidationKey(unvalidated) ==
                       methodInvalidationKey(resolveMethodProvenance(kConfig, registry({r1}), context(), "MIB-01")),
                   "stable inputs keep the key");

        const auto j = Json::parse(methodProvenanceToJson(validated));
        MIB_EXPECT(j.at("source") == "central" && j.at("revision_id") == "r1" && j.at("method_id") == "m1" &&
                       j.at("project_id") == "p1" && j.at("content_hash") == r1.contentHash &&
                       j.at("revision_number") == 1 && j.at("central_state") == "published" &&
                       j.at("validation") == "passed" && j.at("validator_id") == "user-bob" &&
                       j.at("instrument_id") == kInstrument && j.at("instrument_name") == "MIB-01" &&
                       j.at("context_hash") == methodContextHash(context()) &&
                       j.at("config_canonical_sha256") == kConfig && j.at("match") == "config_json" &&
                       j.at("validation_evidence_sha256") == std::string(64, 'e') &&
                       j.at("registry_origin") == "https://registry.example" &&
                       j.at("registry_session") == "signed_in",
                   "central JSON carries every field");
        const auto local = Json::parse(methodProvenanceToJson(
            resolveMethodProvenance(std::string(64, 'z'), registry({r1}), context(), "")));
        MIB_EXPECT(local.at("source") == "local" && !local.contains("revision_id"), "local JSON has no revision");
        auto odd = validated;
        odd.displayName = std::string("bad\xff utf8 \"quoted\"");
        MIB_EXPECT(Json::accept(methodProvenanceToJson(odd)), "invalid UTF-8 / quotes still yield valid JSON");
    }
    return mib::test::exitCode();
}
