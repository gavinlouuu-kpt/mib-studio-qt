// method_provenance_test (#398 M2): the pure method-resolution + gate policy.
//  - no config / unmatched config -> NotRequired (local method);
//  - central published: not validated -> Warn, validated here -> Pass,
//    validated on another instrument or another context -> Warn, failed
//    validation -> Warn, unknown instrument -> Warn;
//  - revoked / not published -> Fail (blocks Start);
//  - several revisions sharing the config: usable state first, then validated
//    here, then published over superseded, then the newest;
//  - (M2b) configDifferences lists changed dotted keys; checkValidationEvidence
//    accepts only a run naming this revision/content/instrument/context;
//    localValidationFor reports the newest matching validation;
//  - the invalidation key moves with every gate input; the JSON block
//    round-trips every provenance field.
//  - (#783) attachLocalProfile: a local method names the profile_selection of
//    the applied config text (name, path, revision, profile_id); nothing for a
//    central or no method, a text without a selection, not JSON, or members of
//    the wrong type; the JSON block's local_profile (null without one) and the
//    gate's detail name it.
#include "backend/app/MethodApply.h"
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
    // M2b: config differences.
    {
        const auto d = configDifferences(R"({"a":1,"b":{"x":1,"y":[1,2]},"c":true})",
                                         R"({"a":1.0,"b":{"x":2,"y":[1,2]},"d":0})");
        MIB_EXPECT((d == std::vector<std::string>{"b.x", "c (removed)", "d (added)"}),
                   "nested change, removal and addition; 1 == 1.0");
        MIB_EXPECT(configDifferences("{}", "{}").empty(), "identical: no changes");
        MIB_EXPECT(configDifferences("{bad", "{}") == std::vector<std::string>{"<entire document>"},
                   "unparsable: whole document");
    }

    // M2b: validation evidence.
    {
        const auto ctx = methodContextHash(context());
        const auto content = std::string(64, 'h');
        const auto run = [&](const std::string& revisionId, const std::string& hash,
                             const std::string& instrument, const std::string& contextHash) {
            return Json({{"schema_version", 2},
                         {"method",
                          {{"source", "central"},
                           {"revision_id", revisionId},
                           {"content_hash", hash},
                           {"instrument_id", instrument},
                           {"context_hash", contextHash}}}})
                .dump();
        };
        MIB_EXPECT(checkValidationEvidence(run("r1", content, kInstrument, ctx), "r1", content, kInstrument, ctx)
                       .empty(),
                   "matching run accepted");
        MIB_EXPECT(checkValidationEvidence(run("r2", content, kInstrument, ctx), "r1", content, kInstrument, ctx)
                           .find("(it used r2)") != std::string::npos,
                   "other revision refused, named");
        MIB_EXPECT(!checkValidationEvidence(run("r1", std::string(64, 'x'), kInstrument, ctx), "r1", content,
                                            kInstrument, ctx)
                        .empty(),
                   "other content refused");
        MIB_EXPECT(checkValidationEvidence(run("r1", content, "other", ctx), "r1", content, kInstrument, ctx)
                           .find("different instrument") != std::string::npos,
                   "other instrument refused");
        MIB_EXPECT(checkValidationEvidence(run("r1", content, kInstrument, "other"), "r1", content, kInstrument,
                                           ctx)
                           .find("processing core or camera source") != std::string::npos,
                   "other context refused");
        MIB_EXPECT(!checkValidationEvidence(run("r1", content, kInstrument, ctx), "r1", content, "", "").empty(),
                   "unknown instrument cannot validate");
        MIB_EXPECT(checkValidationEvidence(R"({"schema_version":1})", "r1", content, kInstrument, ctx)
                           .find("predates") != std::string::npos,
                   "schema-1 run refused");
        MIB_EXPECT(!checkValidationEvidence("", "r1", content, kInstrument, ctx).empty(), "no provenance refused");
        MIB_EXPECT(!checkValidationEvidence("{bad", "r1", content, kInstrument, ctx).empty(), "garbage refused");
    }

    // M2b: local validation view.
    {
        const auto r1 = revision("r1", 1, CentralState::Published);
        auto older = validation(r1, true);
        older.validatedAtUtc = "2026-10-01 00:00:00";
        const auto newer = validation(r1, false);
        const auto snap = registry({r1}, {newer, older}); // newest first
        const auto v = localValidationFor(snap, r1, kInstrument, methodContextHash(context()));
        MIB_EXPECT(v.state == LocalValidationState::Failed && v.validatedAtUtc == "2026-10-04 10:00:00",
                   "newest validation wins");
        MIB_EXPECT(localValidationFor(snap, r1, "other", methodContextHash(context())).state ==
                       LocalValidationState::None,
                   "other instrument: none");
        MIB_EXPECT(localValidationFor(snap, r1, kInstrument, "").state == LocalValidationState::None,
                   "unknown context: none");
    }
    // #783: a local profile names the method.
    {
        const std::string rev(64, 'f');
        // As ProfileStore::apply writes it: the selection fingerprints the text it left.
        Json doc = {{"buffer_threshold", 64}, {"pixel_to_micron_factor", 0.5}};
        const auto fingerprint = localProfileAppliedConfigSha256(doc.dump());
        MIB_REQUIRE(fingerprint.size() == 64, "the fingerprint is a sha256");
        doc["profile_selection"] = {{"name", "Beads 7 um"},
                                    {"path", "/profiles/Beads 7 um/config.json"},
                                    {"revision", rev},
                                    {"profile_id", "beads-7"},
                                    {"display_fps", 30},
                                    {"applied_config_sha256", fingerprint}};
        const std::string applied = doc.dump();
        MIB_EXPECT(localProfileAppliedConfigSha256(applied) == fingerprint, "the selection is not part of the fingerprint");
        auto local = resolveMethodProvenance(std::string(64, 'z'), registry({}), context(), "MIB-01");
        attachLocalProfile(local, applied);
        MIB_EXPECT(local.source == "local" && local.hasLocalProfile() && local.localProfileName == "Beads 7 um" &&
                       local.localProfilePath == "/profiles/Beads 7 um/config.json" && local.localProfileRevision == rev &&
                       local.localProfileId == "beads-7" && !local.localProfileModified,
                   "a local method names its profile, unmodified");
        const auto j = Json::parse(methodProvenanceToJson(local));
        MIB_EXPECT(j.at("local_profile") == Json({{"name", "Beads 7 um"},
                                                  {"path", "/profiles/Beads 7 um/config.json"},
                                                  {"revision", rev},
                                                  {"profile_id", "beads-7"},
                                                  {"modified_after_apply", false}}) &&
                       j.at("config_canonical_sha256") == std::string(64, 'z'),
                   "the snapshot's method block names it beside the config hash: " + j.dump());
        const auto g = methodRevisionGate(local);
        MIB_EXPECT(g.status == GateStatus::NotRequired &&
                       g.detail == "config zzzzzzzzzzzz; local profile \"Beads 7 um\" revision ffffffffffff",
                   "the gate's detail names it: " + g.detail);

        // A settings apply after the profile keeps the selection but changes the text: modified.
        Json edited = Json::parse(applied);
        edited["buffer_threshold"] = 77;
        auto changed = resolveMethodProvenance(std::string(64, 'y'), registry({}), context(), "MIB-01");
        attachLocalProfile(changed, edited.dump());
        MIB_EXPECT(changed.hasLocalProfile() && changed.localProfileModified && changed.localProfileRevision == rev,
                   "an edited text names its base profile as modified after apply");
        MIB_EXPECT(Json::parse(methodProvenanceToJson(changed)).at("local_profile").at("modified_after_apply") == true,
                   "the snapshot says so");
        MIB_EXPECT(methodRevisionGate(changed).detail ==
                       "config yyyyyyyyyyyy; local profile \"Beads 7 um\" revision ffffffffffff, modified after apply",
                   "the gate's detail says so: " + methodRevisionGate(changed).detail);
        // The same values re-applied (key order, 64 vs 64.0) are not a modification.
        Json same = Json::parse(applied);
        same["buffer_threshold"] = 64.0;
        auto unchanged = resolveMethodProvenance(std::string(64, 'y'), registry({}), context(), "MIB-01");
        attachLocalProfile(unchanged, same.dump());
        MIB_EXPECT(unchanged.hasLocalProfile() && !unchanged.localProfileModified, "canonical: the same values match");

        auto bare = resolveMethodProvenance(std::string(64, 'z'), registry({}), context(), "MIB-01");
        attachLocalProfile(bare, R"({"buffer_threshold":64})");
        MIB_EXPECT(!bare.hasLocalProfile() && bare.localProfilePath.empty() && bare.localProfileRevision.empty(),
                   "a text without a selection names no profile");
        MIB_EXPECT(Json::parse(methodProvenanceToJson(bare)).at("local_profile").is_null(), "local_profile null");
        attachLocalProfile(bare, "not json {");
        attachLocalProfile(bare, R"({"profile_selection":"x"})");
        attachLocalProfile(bare, R"({"profile_selection":{"name":7,"revision":null}})");
        MIB_EXPECT(!bare.hasLocalProfile(), "not JSON, not an object, or members of the wrong type: nothing");
        attachLocalProfile(bare, R"({"profile_selection":{"profile_id":"only-id"}})");
        MIB_EXPECT(bare.hasLocalProfile() && bare.localProfileId == "only-id" && bare.localProfileName.empty(),
                   "a profile_id alone names it");
        MIB_EXPECT(Json::parse(methodProvenanceToJson(bare)).at("local_profile") ==
                       Json({{"name", nullptr}, {"path", nullptr}, {"revision", nullptr}, {"profile_id", "only-id"},
                             {"modified_after_apply", true}}),
                   "absent members are null, never empty strings; no fingerprint is not verified");

        auto central = resolveMethodProvenance(kConfig, registry({revision("r1", 1, CentralState::Published)}),
                                               context(), "MIB-01");
        attachLocalProfile(central, applied);
        MIB_EXPECT(central.source == "central" && !central.hasLocalProfile() &&
                       !Json::parse(methodProvenanceToJson(central)).contains("local_profile"),
                   "a central revision is named by the registry, not a profile");
        auto none = resolveMethodProvenance("", registry({}), context(), "MIB-01");
        attachLocalProfile(none, applied);
        MIB_EXPECT(none.source == "none" && !none.hasLocalProfile(), "no method: nothing");
    }
    return mib::test::exitCode();
}
