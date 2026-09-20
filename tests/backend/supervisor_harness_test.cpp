// supervisor_harness_test — offline evaluation harness + labelled fixtures
// (issue #422, phase B). Network-free.
//
//   1. Every fixture under tests/supervisor loads, carries dataset/contract
//      versions and does not leak its label into the snapshot.
//   2. The deterministic rule provider agrees with every fixture (the
//      fixtures are the reviewable spec of the rule baseline); policy-decided
//      cases are reported as such.
//   3. A deliberately wrong provider produces the expected confusion,
//      false-STOP / false-CONTINUE counts and calibration buckets, so the
//      report math is verified against known numbers.
//   4. The report serializes to JSON/text and a provider failure is counted,
//      not hidden.
//
// argv[1] = fixture directory (CTest passes ${PROJECT_SOURCE_DIR}/tests/supervisor).

#include "backend/supervisor/EvaluationHarness.h"
#include "backend/supervisor/RuleProvider.h"
#include "support/assert.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <map>
#include <string>

using namespace backend::supervisor;

namespace {

struct AlwaysStopProvider final : DecisionProvider {
    std::string name() const override { return "always-stop"; }
    std::string version() const override { return "test/1"; }
    DecisionResult evaluate(const ExperimentSnapshot&, const DecisionPolicy&) override
    {
        DecisionResult r;
        r.providerName = name();
        r.answers.quality = RunQuality::Bad;
        r.answers.problem = PrimaryProblem::Other;
        r.answers.action = NextAction::StopFailure;
        r.distributions[kQuestionNextAction] = {{"STOP_FAILURE", 0.95}};
        r.confidence = 0.55;
        r.costUsd = 0.001;
        r.latencyUs = 1000;
        return r;
    }
};

struct AlwaysContinueProvider final : DecisionProvider {
    std::string name() const override { return "always-continue"; }
    std::string version() const override { return "test/1"; }
    DecisionResult evaluate(const ExperimentSnapshot&, const DecisionPolicy&) override
    {
        DecisionResult r;
        r.providerName = name();
        r.answers.quality = RunQuality::Good;
        r.answers.problem = PrimaryProblem::None;
        r.answers.action = NextAction::Continue;
        r.distributions[kQuestionNextAction] = {{"CONTINUE", 0.65}};
        r.confidence = 0.95;
        r.latencyUs = 2000;
        return r;
    }
};

struct FlakyProvider final : DecisionProvider {
    int n{0};
    std::string name() const override { return "flaky"; }
    std::string version() const override { return "test/1"; }
    DecisionResult evaluate(const ExperimentSnapshot& s, const DecisionPolicy& p) override
    {
        if (++n % 2 == 0) {
            DecisionResult r;
            r.providerName = name();
            r.error = {ProviderErrorKind::Timeout, "injected"};
            return r;
        }
        return RuleProvider{}.evaluate(s, p);
    }
};

} // namespace

int main(int argc, char** argv)
{
    MIB_REQUIRE(argc >= 2, "usage: supervisor_harness_test <fixture dir>");
    std::vector<LabelledCase> cases;
    std::string err;
    MIB_REQUIRE(loadLabelledCases(argv[1], cases, err), "load fixtures: " + err);
    MIB_EXPECT(cases.size() >= 15, "at least the 15 issue scenarios are present");
    std::map<std::string, int> ids;
    for (const auto& c : cases) {
        ++ids[c.id];
        MIB_EXPECT(!c.datasetVersion.empty(), "dataset version present: " + c.id);
        MIB_EXPECT(c.snapshot.schemaVersion == kExperimentSnapshotSchemaVersion, "snapshot schema: " + c.id);
        MIB_EXPECT(snapshotToJson(c.snapshot).find("expected") == std::string::npos, "label not in snapshot: " + c.id);
        MIB_EXPECT(!c.scenario.empty(), "scenario named: " + c.id);
    }
    for (const auto& [id, n] : ids) MIB_EXPECT(n == 1, "unique id " + id);

    // Round trip a case through the serializer.
    {
        LabelledCase back;
        MIB_REQUIRE(labelledCaseFromJson(labelledCaseToJson(cases[0]), back, err), "case round trip: " + err);
        MIB_EXPECT(back.expected == cases[0].expected && back.id == cases[0].id, "case fields survive");
        MIB_EXPECT(!labelledCaseFromJson("{\"case_schema\":1,\"id\":\"x\",\"expected\":{\"next_action\":\"NUKE\"},\"snapshot\":{\"schema_version\":1}}", back, err),
                   "unknown expected token rejected");
        MIB_EXPECT(!labelledCaseFromJson("{\"case_schema\":1,\"id\":\"x\",\"expected\":{\"run_quality\":\"GOOD\",\"primary_problem\":\"NONE\",\"next_action\":\"CONTINUE\",\"adjustment_target\":\"NONE\",\"adjustment_direction\":\"KEEP\"},\"snapshot\":{\"schema_version\":1,\"expected\":{}}}", back, err),
                   "label inside snapshot rejected");
    }

    // 2. Rule baseline agrees with every fixture.
    {
        RuleProvider rule;
        EvaluationOptions opt;
        const auto rep = runEvaluation(cases, rule, opt);
        MIB_EXPECT(rep.cases == cases.size(), "all cases evaluated");
        for (const auto& o : rep.outcomes) {
            MIB_EXPECT(o.actual == o.expected, "rule provider agrees on " + o.id + " (expected " +
                                                    toString(o.expected.action) + "/" + toString(o.expected.problem) +
                                                    ", got " + toString(o.actual.action) + "/" + toString(o.actual.problem) +
                                                    "/" + toString(o.actual.target) + " by " + o.decidedBy + ")");
        }
        for (const auto& q : rep.agreement) MIB_EXPECT(q.agree == q.total, "100% agreement on " + q.question);
        MIB_EXPECT(rep.unsafeDisagreements == 0 && rep.falseStop == 0 && rep.falseContinue == 0, "no unsafe disagreements");
        size_t expectPolicy = 0;
        for (const auto& c : cases) if (c.expectPolicy) ++expectPolicy;
        MIB_EXPECT(rep.policyDecided == expectPolicy, "policy-decided count matches fixtures");
        for (size_t i = 0; i < cases.size(); ++i) {
            MIB_EXPECT((rep.outcomes[i].decidedBy == "policy") == cases[i].expectPolicy,
                       "decided_by matches expect_policy for " + cases[i].id);
        }
        MIB_EXPECT(rep.providerConsulted == cases.size() - expectPolicy, "provider consulted only for non-policy cases");
        MIB_EXPECT(rep.providerFailures == 0, "rule provider never fails");
        MIB_EXPECT(rep.datasetVersion == cases[0].datasetVersion, "dataset version reported");
        const std::string text = reportToText(rep);
        MIB_EXPECT(text.find("none") != std::string::npos, "text report lists no disagreements");
        const auto j = nlohmann::json::parse(reportToJson(rep));
        MIB_EXPECT(j["agreement"]["next_action"]["agreement"].get<double>() == 1.0, "json agreement 1.0");
        MIB_EXPECT(j["provider"]["name"] == "rule", "json provider name");
        MIB_EXPECT(j["calibration"]["selected_probability"].size() == opt.calibrationThresholds.size(), "calibration buckets");
    }

    // 3. Known-wrong providers produce known numbers.
    size_t expectedGo = 0, expectedHold = 0, providerCases = 0;
    for (const auto& c : cases) {
        if (c.expectPolicy) continue;
        ++providerCases;
        if (c.expected.action == NextAction::Continue || c.expected.action == NextAction::Adjust) ++expectedGo;
        else ++expectedHold;
    }
    {
        AlwaysStopProvider stop;
        const auto rep = runEvaluation(cases, stop, EvaluationOptions{});
        MIB_EXPECT(rep.falseStop == expectedGo, "false STOP = every provider-decided go case (" +
                                                    std::to_string(rep.falseStop) + " vs " + std::to_string(expectedGo) + ")");
        MIB_EXPECT(rep.falseContinue == 0, "always-stop never false-continues");
        MIB_EXPECT(rep.unsafeDisagreements == rep.falseStop, "unsafe == false stops");
        MIB_EXPECT(rep.totalCostUsd && *rep.totalCostUsd > 0.0, "cost aggregated");
        MIB_EXPECT(rep.latencyP50Us == 1000 && rep.latencyP95Us == 1000, "latency percentiles from provider latencies");
        // selected probability 0.95 >= every threshold: all provider cases selected; correct = expected STOP_FAILURE among them.
        size_t correct = 0;
        for (const auto& c : cases) if (!c.expectPolicy && c.expected.action == NextAction::StopFailure) ++correct;
        for (const auto& b : rep.selectedProbabilityCalibration) {
            MIB_EXPECT(b.selected == providerCases && b.correct == correct, "selected-probability bucket counts");
        }
        for (const auto& b : rep.confidenceCalibration) {
            if (b.threshold <= 0.55) MIB_EXPECT(b.selected == providerCases, "confidence 0.55 selected at low thresholds");
            else MIB_EXPECT(b.selected == 0, "confidence 0.55 not selected above 0.55");
        }
        const auto& conf = rep.agreement[2].confusion; // next_action
        uint64_t stops = 0;
        for (const auto& [exp, row] : conf) for (const auto& [act, n] : row) if (act == "STOP_FAILURE") stops += n;
        MIB_EXPECT(stops >= providerCases, "confusion matrix counts every provider decision as STOP_FAILURE");
    }
    {
        AlwaysContinueProvider go;
        const auto rep = runEvaluation(cases, go, EvaluationOptions{});
        MIB_EXPECT(rep.falseContinue == expectedHold, "false CONTINUE = every provider-decided hold case");
        MIB_EXPECT(rep.falseStop == 0, "always-continue never false-stops");
        MIB_EXPECT(rep.humanReview == 0, "no HUMAN_REVIEW from always-continue on provider cases");
        const std::string text = reportToText(rep);
        MIB_EXPECT(text.find("UNSAFE:false_continue") != std::string::npos, "text report flags unsafe disagreements");
    }
    // 4. Provider failures are counted and fail closed.
    {
        FlakyProvider flaky;
        const auto rep = runEvaluation(cases, flaky, EvaluationOptions{});
        MIB_EXPECT(rep.providerFailures == providerCases / 2, "half the provider calls failed");
        MIB_EXPECT(rep.providerFailureRate() > 0.0, "failure rate reported");
        for (const auto& o : rep.outcomes) {
            if (!o.providerOk) {
                MIB_EXPECT(o.actual.action == NextAction::HumanReview && o.decidedBy == "fail_closed", "failed call -> fail closed");
                MIB_EXPECT(o.providerError.find("timeout") != std::string::npos, "error text carried");
            }
        }
        EvaluationOptions stopEarly;
        stopEarly.stopOnProviderFailure = true;
        FlakyProvider flaky2;
        const auto rep2 = runEvaluation(cases, flaky2, stopEarly);
        MIB_EXPECT(rep2.outcomes.size() < cases.size(), "stopOnProviderFailure halts the run");
    }
    std::printf("supervisor_harness_test: %s\n", mib::test::exitCode() == 0 ? "OK" : "FAILED");
    return mib::test::exitCode();
}
