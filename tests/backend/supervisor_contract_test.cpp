// supervisor_contract_test — decision contract + snapshot/record round-trip
// and fault injection (issue #422).
//
//   1. Every enum token round-trips; unknown tokens are rejected (never
//      defaulted).
//   2. ExperimentSnapshot -> JSON -> ExperimentSnapshot is lossless, canonical
//      (same bytes, same hash) and unknown metrics stay unknown (null), never 0.
//   3. Wrong schema version / malformed JSON fail cleanly.
//   4. DecisionRecord round-trips through the JSON-lines sidecar, with
//      executed == false and the fail-closed answers preserved.

#include "backend/supervisor/DecisionContract.h"
#include "backend/supervisor/DecisionRecord.h"
#include "backend/supervisor/ExperimentSnapshot.h"
#include "support/assert.h"
#include "support/tempdir.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace backend::supervisor;

namespace {

ExperimentSnapshot sample()
{
    ExperimentSnapshot s;
    s.sequence = 7;
    s.runId = "run-7";
    s.experimentState = "active";
    s.elapsedSeconds = 12.5;
    s.objective = "collect 500";
    s.targetValidObjects = 500;
    s.configuration.cameraSource = "mock";
    s.configuration.simulated = true;
    s.configuration.detectionThreshold = 8;
    s.configuration.minAreaUm2 = 60;
    s.configuration.maxAreaUm2 = 290;
    s.acquisition.captureState = "running";
    s.acquisition.cameraReady = true;
    s.acquisition.lastFailure = "none";
    s.acquisition.framesDelivered = Metric::of(1234);
    s.acquisition.transportLostFrames = Metric::unknown();
    s.detection.validObjects = Metric::of(10);
    s.detection.invalidObjects = Metric::of(5);
    s.detection.contrastMean = Metric::of(42.25);
    s.detection.rejectionReasons = {"border", "area"};
    s.detection.rejectionCounts = {3, 2};
    s.trigger.eligibleObjects = Metric::of(4);
    s.recording.state = "active";
    s.recording.outputPath = "x.h5";
    PriorRecommendation p;
    p.sequence = 6;
    p.action = "CONTINUE";
    p.target = "NONE";
    p.direction = "KEEP";
    s.priorRecommendations.push_back(p);
    return s;
}

void testEnums()
{
    for (const auto& q : questionIds()) {
        const auto& tokens = tokensFor(q);
        MIB_EXPECT(!tokens.empty(), "question has tokens: " + q);
    }
    MIB_EXPECT(tokensFor("bogus").empty(), "unknown question has no tokens");
    MIB_EXPECT(parseNextAction("CONTINUE") == NextAction::Continue, "CONTINUE parses");
    MIB_EXPECT(parseNextAction("STOP_FAILURE") == NextAction::StopFailure, "STOP_FAILURE parses");
    MIB_EXPECT(!parseNextAction("continue"), "case-sensitive: lowercase rejected");
    MIB_EXPECT(!parseNextAction("LAUNCH_MISSILES"), "unknown action rejected");
    MIB_EXPECT(!parseAdjustmentTarget("GAIN"), "unknown target rejected");
    MIB_EXPECT(!parseAdjustmentDirection(""), "empty direction rejected");
    MIB_EXPECT(!parsePrimaryProblem("NONE "), "trailing space rejected");
    MIB_EXPECT(parseRunQuality("BAD") == RunQuality::Bad, "BAD parses");
    for (int i = 0; i < 10; ++i) {
        const auto v = static_cast<PrimaryProblem>(i);
        MIB_EXPECT(parsePrimaryProblem(toString(v)) == v, "problem round trip");
    }
    for (int i = 0; i < 11; ++i) {
        const auto k = static_cast<ProviderErrorKind>(i);
        MIB_EXPECT(parseProviderErrorKind(toString(k)) == k, "error kind round trip");
    }
    Distributions d;
    d[kQuestionNextAction] = {{"CONTINUE", 0.7}, {"ADJUST", 0.3}};
    MIB_EXPECT(selectedProbability(d, kQuestionNextAction, "CONTINUE").value_or(-1) == 0.7, "selected probability");
    MIB_EXPECT(!selectedProbability(d, kQuestionNextAction, "STOP_SUCCESS"), "absent token -> nullopt");
    MIB_EXPECT(!selectedProbability(d, kQuestionRunQuality, "GOOD"), "absent question -> nullopt");
}

void testSnapshotRoundTrip()
{
    const ExperimentSnapshot s = sample();
    const std::string j1 = snapshotToJson(s);
    const std::string j2 = snapshotToJson(s);
    MIB_EXPECT(j1 == j2, "canonical serialization is deterministic");
    MIB_EXPECT(snapshotHash(s) == snapshotHash(s), "hash is deterministic");
    MIB_EXPECT(snapshotHash(s).size() == 64, "sha256 hex");
    MIB_EXPECT(j1.find("\"transport_lost_frames\":null") != std::string::npos, "unknown metric serializes as null, not 0");

    ExperimentSnapshot back;
    std::string err;
    MIB_REQUIRE(snapshotFromJson(j1, back, err), "parse: " + err);
    MIB_EXPECT(back.sequence == 7 && back.runId == "run-7" && back.elapsedSeconds == 12.5, "identity fields");
    MIB_EXPECT(back.targetValidObjects && *back.targetValidObjects == 500, "target objects");
    MIB_EXPECT(back.acquisition.framesDelivered == Metric::of(1234), "known metric");
    MIB_EXPECT(!back.acquisition.transportLostFrames.known, "unknown stays unknown");
    MIB_EXPECT(back.detection.contrastMean == Metric::of(42.25), "double metric");
    MIB_EXPECT(back.detection.rejectionReasons.size() == 2 && back.detection.rejectionCounts.size() == 2, "rejections");
    MIB_EXPECT(back.priorRecommendations.size() == 1 && back.priorRecommendations[0].action == "CONTINUE", "priors");
    MIB_EXPECT(snapshotToJson(back) == j1, "round trip is byte-identical");
    MIB_EXPECT(snapshotHash(back) == snapshotHash(s), "hash survives round trip");

    ExperimentSnapshot changed = s;
    changed.detection.validObjects = Metric::of(11);
    MIB_EXPECT(snapshotHash(changed) != snapshotHash(s), "hash changes with content");

    // Derived quantities.
    ExperimentSnapshot d = s;
    MIB_EXPECT(!frameLossFraction(d), "loss unknown when lost frames unknown");
    d.acquisition.transportLostFrames = Metric::of(100);
    d.acquisition.framesDelivered = Metric::of(900);
    MIB_EXPECT(frameLossFraction(d).value_or(-1) == 0.1, "loss fraction 100/1000");
    MIB_EXPECT(rejectionFraction(d).value_or(-1) == 5.0 / 15.0, "rejection fraction");
    d.trigger.triggersIssued = Metric::of(2);
    MIB_EXPECT(triggerSuccessFraction(d).value_or(-1) == 0.5, "trigger success");
}

void testSnapshotFaults()
{
    ExperimentSnapshot out;
    std::string err;
    MIB_EXPECT(!snapshotFromJson("not json", out, err) && !err.empty(), "malformed JSON rejected");
    MIB_EXPECT(!snapshotFromJson("[1,2]", out, err), "array rejected");
    MIB_EXPECT(!snapshotFromJson("{\"schema_version\":99}", out, err), "wrong schema version rejected");
    MIB_EXPECT(err.find("schema_version") != std::string::npos, "error names the schema version");
    MIB_EXPECT(!snapshotFromJson("{\"schema_version\":1,\"detection\":{\"valid_objects\":\"ten\"}}", out, err),
               "non-numeric metric rejected");
    MIB_REQUIRE(snapshotFromJson("{\"schema_version\":1}", out, err), "minimal snapshot parses");
    MIB_EXPECT(!out.detection.validObjects.known, "missing metric stays unknown");
}

void testRecordRoundTrip()
{
    DecisionRecord r;
    r.sequence = 3;
    r.mode = "shadow";
    r.runId = "run";
    r.snapshot = sample();
    r.snapshotHash = snapshotHash(r.snapshot);
    r.policy.requiresAction = false;
    r.policy.rulesChecked = {"policy.unresolved_fault", "policy.frame_loss"};
    r.providerConsulted = true;
    r.provider.providerName = "jev";
    r.provider.providerVersion = "jev-adapter/1";
    r.provider.modelVersion = "jev-2026-09";
    r.provider.answers.quality = RunQuality::Good;
    r.provider.answers.problem = PrimaryProblem::None;
    r.provider.answers.action = NextAction::Adjust;
    r.provider.answers.target = AdjustmentTarget::Exposure;
    r.provider.answers.direction = AdjustmentDirection::Increase;
    r.provider.distributions[kQuestionNextAction] = {{"ADJUST", 0.8}, {"CONTINUE", 0.2}};
    r.provider.confidence = 0.66;
    r.provider.latencyUs = 1234;
    r.provider.costUsd = 0.002;
    r.recommendation = r.provider.answers;
    r.decidedBy = "provider";
    r.eligibility.eligible = true;
    r.executed = false;

    const std::string j = recordToJson(r);
    MIB_EXPECT(j.find("\"executed\":false") != std::string::npos, "shadow record says not executed");
    DecisionRecord back;
    std::string err;
    MIB_REQUIRE(recordFromJson(j, back, err), "record parse: " + err);
    MIB_EXPECT(back.recommendation == r.recommendation, "recommendation round trip");
    MIB_EXPECT(back.provider.distributions == r.provider.distributions, "distributions round trip");
    MIB_EXPECT(back.provider.confidence.value_or(-1) == 0.66, "confidence");
    MIB_EXPECT(back.provider.costUsd.value_or(-1) == 0.002, "cost");
    MIB_EXPECT(back.provider.modelVersion == "jev-2026-09", "model pin");
    MIB_EXPECT(back.snapshotHash == r.snapshotHash && snapshotHash(back.snapshot) == r.snapshotHash, "snapshot replayable");
    MIB_EXPECT(back.policy.rulesChecked.size() == 2, "rules checked");
    MIB_EXPECT(!back.executed, "executed false");
    MIB_EXPECT(!recordFromJson("{\"record_schema_version\":2}", back, err), "wrong record schema rejected");

    // A record with a schema-invalid answer token is rejected on read.
    std::string bad = j;
    const auto pos = bad.find("\"ADJUST\"");
    MIB_REQUIRE(pos != std::string::npos, "find token");
    bad.replace(pos, 8, "\"REBOOT\"");
    MIB_EXPECT(!recordFromJson(bad, back, err), "unknown token in stored record rejected");

    // Sidecar log round trip + fault injection.
    mib::test::TempDir td("supervisor_log");
    const std::string path = (td / "run.supervisor.jsonl").string();
    {
        DecisionLog log;
        MIB_REQUIRE(log.open(path, "run", SupervisorMode::Shadow, "jev", "jev-adapter/1", "jev-2026-09", &err), err);
        MIB_EXPECT(log.append(r), "append 1");
        r.sequence = 4;
        MIB_EXPECT(log.append(r), "append 2");
        MIB_EXPECT(log.recordsWritten() == 2, "two records");
    }
    std::vector<DecisionRecord> read;
    MIB_REQUIRE(DecisionLog::readAll(path, read, err), "read back: " + err);
    MIB_EXPECT(read.size() == 2 && read[0].sequence == 3 && read[1].sequence == 4, "records in order, header skipped");
    DecisionLog closed;
    MIB_EXPECT(!closed.append(r, &err) && !err.empty(), "append on closed log fails cleanly");
    MIB_EXPECT(!closed.open((td / "missing" / "dir" / "x.jsonl").string(), "run", SupervisorMode::Shadow, "rule", "rule/1", "", &err),
               "open in missing directory fails cleanly");
    MIB_EXPECT(!DecisionLog::readAll((td / "nope.jsonl").string(), read, err), "read missing fails cleanly");
}

} // namespace

int main()
{
    testEnums();
    testSnapshotRoundTrip();
    testSnapshotFaults();
    testRecordRoundTrip();
    std::printf("supervisor_contract_test: %s\n", mib::test::exitCode() == 0 ? "OK" : "FAILED");
    return mib::test::exitCode();
}
