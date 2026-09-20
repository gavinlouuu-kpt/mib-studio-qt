// supervisor_policy_test — deterministic policy precedence (issue #422).
//
//   1. Each hard invariant fires on its own with the documented rule id.
//   2. The model can never override the policy: decideOnce() with a provider
//      that insists on CONTINUE still yields the policy answer and the
//      provider is not even consulted.
//   3. Provider error / timeout / exception -> fail closed to HUMAN_REVIEW,
//      never eligible.
//   4. Eligibility rejects incoherent or out-of-limit adjustments.

#include "backend/supervisor/DecisionContract.h"
#include "backend/supervisor/DecisionProvider.h"
#include "backend/supervisor/SafetyPolicy.h"
#include "backend/supervisor/SupervisorService.h"
#include "support/assert.h"

#include <cstdio>
#include <stdexcept>

using namespace backend::supervisor;

namespace {

ExperimentSnapshot healthy()
{
    ExperimentSnapshot s;
    s.experimentState = "active";
    s.elapsedSeconds = 60.0;
    s.acquisition.cameraReady = true;
    s.acquisition.lastFailure = "none";
    s.acquisition.framesDelivered = Metric::of(30000);
    s.acquisition.transportLostFrames = Metric::of(0);
    s.detection.framesProcessed = Metric::of(30000);
    s.detection.validObjects = Metric::of(900);
    s.detection.invalidObjects = Metric::of(300);
    s.detection.contrastMean = Metric::of(60);
    s.detection.brightnessMedianMean = Metric::of(120);
    s.detection.brightnessMaxMean = Metric::of(200);
    s.recording.persistenceFailed = Metric::of(0);
    return s;
}

struct InsistingProvider final : DecisionProvider {
    int calls{0};
    std::string name() const override { return "insist"; }
    std::string version() const override { return "insist/1"; }
    DecisionResult evaluate(const ExperimentSnapshot&, const DecisionPolicy&) override
    {
        ++calls;
        DecisionResult r;
        r.providerName = name();
        r.answers.quality = RunQuality::Good;
        r.answers.problem = PrimaryProblem::None;
        r.answers.action = NextAction::Continue;
        r.distributions[kQuestionNextAction] = {{"CONTINUE", 0.99}};
        r.confidence = 0.99;
        return r;
    }
};

struct FailingProvider final : DecisionProvider {
    ProviderErrorKind kind;
    explicit FailingProvider(ProviderErrorKind k) : kind(k) {}
    std::string name() const override { return "failing"; }
    std::string version() const override { return "failing/1"; }
    DecisionResult evaluate(const ExperimentSnapshot&, const DecisionPolicy&) override
    {
        if (kind == ProviderErrorKind::Exception) throw std::runtime_error("boom");
        DecisionResult r;
        r.providerName = name();
        r.error = {kind, "injected"};
        // A misbehaving adapter that also fills answers must still be ignored.
        r.answers.action = NextAction::StopFailure;
        return r;
    }
};

void expectRule(const ExperimentSnapshot& s, const DecisionPolicy& p, const char* rule, NextAction action)
{
    const auto out = evaluateSafetyPolicy(s, p);
    MIB_EXPECT(out.requiresAction, std::string("policy fires: ") + rule);
    MIB_EXPECT(out.ruleId == rule, std::string("rule id ") + rule + " got " + out.ruleId);
    MIB_EXPECT(out.answers.action == action, std::string("action for ") + rule);
    MIB_EXPECT(!out.rulesChecked.empty(), "rules checked recorded");
}

void testRules()
{
    DecisionPolicy p;
    {
        const auto out = evaluateSafetyPolicy(healthy(), p);
        MIB_EXPECT(!out.requiresAction, "healthy snapshot needs no policy action");
        MIB_EXPECT(out.rulesChecked.size() == 7, "all seven rules evaluated");
    }
    { auto s = healthy(); s.recording.faultCode = "save.fatal"; expectRule(s, p, "policy.unresolved_fault", NextAction::StopFailure); }
    { auto s = healthy(); s.experimentState = "failed"; expectRule(s, p, "policy.unresolved_fault", NextAction::StopFailure); }
    { auto s = healthy(); s.recording.persistenceFailed = Metric::of(1); expectRule(s, p, "policy.storage_failure", NextAction::StopFailure); }
    { auto s = healthy(); s.acquisition.cameraReady = false; expectRule(s, p, "policy.camera_unavailable", NextAction::StopFailure); }
    { auto s = healthy(); s.acquisition.lastFailure = "deviceHealthLost"; expectRule(s, p, "policy.camera_unavailable", NextAction::StopFailure); }
    { auto s = healthy(); s.acquisition.transportLostFrames = Metric::of(10000); expectRule(s, p, "policy.frame_loss", NextAction::StopFailure); }
    { auto s = healthy(); s.acquisition.transportLostFrames = Metric::of(1000); MIB_EXPECT(!evaluateSafetyPolicy(s, p).requiresAction, "3% loss below hard limit"); }
    { auto s = healthy(); s.acquisition.transportLostFrames = Metric::unknown(); MIB_EXPECT(!evaluateSafetyPolicy(s, p).requiresAction, "unknown loss never counts as loss"); }
    { auto s = healthy(); s.targetValidObjects = 500; expectRule(s, p, "policy.target_reached", NextAction::StopSuccess); }
    { auto s = healthy(); DecisionPolicy q = p; q.targetValidObjects = 900; expectRule(s, q, "policy.target_reached", NextAction::StopSuccess); }
    { auto s = healthy(); s.targetValidObjects = 5000; MIB_EXPECT(!evaluateSafetyPolicy(s, p).requiresAction, "target not reached"); }
    { auto s = healthy(); s.experimentState = "idle"; expectRule(s, p, "policy.not_active", NextAction::HumanReview); }
    { auto s = healthy(); s.elapsedSeconds = 0.5; expectRule(s, p, "policy.insufficient_data", NextAction::Continue); }
    { auto s = healthy(); s.detection.framesProcessed = Metric::of(10); expectRule(s, p, "policy.insufficient_data", NextAction::Continue); }
    { auto s = healthy(); s.detection.framesProcessed = Metric::unknown(); expectRule(s, p, "policy.insufficient_data", NextAction::Continue); }
    // Precedence: a fault beats a reached target.
    { auto s = healthy(); s.targetValidObjects = 500; s.recording.faultCode = "x"; expectRule(s, p, "policy.unresolved_fault", NextAction::StopFailure); }
}

void testModelCannotOverride()
{
    DecisionPolicy p;
    InsistingProvider provider;
    auto s = healthy();
    s.acquisition.transportLostFrames = Metric::of(20000);
    const auto rec = decideOnce(s, p, &provider, SupervisorMode::Shadow);
    MIB_EXPECT(rec.decidedBy == "policy", "policy decided");
    MIB_EXPECT(!rec.providerConsulted && provider.calls == 0, "provider not consulted when policy requires action");
    MIB_EXPECT(rec.recommendation.action == NextAction::StopFailure, "policy answer wins");
    MIB_EXPECT(!rec.eligibility.eligible, "policy outcome is not an executable recommendation");
    MIB_EXPECT(!rec.executed, "never executed");
    MIB_EXPECT(!rec.snapshotHash.empty(), "record carries snapshot hash");

    const auto ok = decideOnce(healthy(), p, &provider, SupervisorMode::Shadow);
    MIB_EXPECT(ok.decidedBy == "provider" && ok.providerConsulted && provider.calls == 1, "healthy: provider consulted");
    MIB_EXPECT(ok.recommendation.action == NextAction::Continue, "provider answer recorded");
    MIB_EXPECT(ok.eligibility.eligible, "coherent CONTINUE is eligible");
    MIB_EXPECT(!ok.executed, "shadow: still not executed");

    const auto none = decideOnce(healthy(), p, nullptr, SupervisorMode::Shadow);
    MIB_EXPECT(none.decidedBy == "fail_closed" && none.recommendation.action == NextAction::HumanReview, "no provider -> fail closed");
}

void testProviderFailuresFailClosed()
{
    DecisionPolicy p;
    for (auto kind : {ProviderErrorKind::Timeout, ProviderErrorKind::Transport, ProviderErrorKind::SchemaViolation,
                      ProviderErrorKind::Exception, ProviderErrorKind::NotConfigured}) {
        FailingProvider fp(kind);
        const auto rec = decideOnce(healthy(), p, &fp, SupervisorMode::Shadow);
        MIB_EXPECT(rec.providerConsulted, "provider consulted");
        MIB_EXPECT(rec.decidedBy == "fail_closed", std::string("fail closed on ") + toString(kind));
        MIB_EXPECT(rec.recommendation.action == NextAction::HumanReview, "HUMAN_REVIEW on provider failure");
        MIB_EXPECT(rec.provider.answers.action == NextAction::HumanReview, "adapter answers discarded on error");
        MIB_EXPECT(!rec.eligibility.eligible, "never eligible on failure");
        MIB_EXPECT(!rec.provider.error.empty(), "error kind recorded");
        if (kind == ProviderErrorKind::Exception) {
            MIB_EXPECT(rec.provider.error.kind == ProviderErrorKind::Exception, "exception mapped");
        } else {
            MIB_EXPECT(rec.provider.error.kind == kind, "error kind preserved");
        }
    }
}

void testEligibility()
{
    DecisionPolicy p;
    DecisionResult r;
    r.answers.action = NextAction::Adjust;
    r.answers.target = AdjustmentTarget::Exposure;
    r.answers.direction = AdjustmentDirection::Increase;
    MIB_EXPECT(evaluateEligibility(r, p).eligible, "coherent adjust eligible");
    r.answers.direction = AdjustmentDirection::Keep;
    MIB_EXPECT(!evaluateEligibility(r, p).eligible, "ADJUST + KEEP not eligible");
    r.answers.direction = AdjustmentDirection::Increase;
    r.answers.target = AdjustmentTarget::None;
    MIB_EXPECT(!evaluateEligibility(r, p).eligible, "ADJUST without target not eligible");
    r.answers.target = AdjustmentTarget::TriggerTiming;
    DecisionPolicy narrow = p;
    narrow.allowedTargets = {AdjustmentTarget::Exposure};
    MIB_EXPECT(!evaluateEligibility(r, narrow).eligible, "target outside configured limits rejected");
    MIB_EXPECT(evaluateEligibility(r, p).eligible, "target inside limits accepted");
    r.answers.action = NextAction::Continue;
    MIB_EXPECT(!evaluateEligibility(r, p).eligible, "adjust fields on CONTINUE rejected");
    r.answers.target = AdjustmentTarget::None;
    r.answers.direction = AdjustmentDirection::Keep;
    MIB_EXPECT(evaluateEligibility(r, p).eligible, "plain CONTINUE eligible");
    r.answers.action = NextAction::HumanReview;
    MIB_EXPECT(!evaluateEligibility(r, p).eligible, "HUMAN_REVIEW never executable");
    r.answers.action = NextAction::Continue;
    DecisionPolicy thr = p;
    thr.minSelectedProbability = 0.8;
    MIB_EXPECT(!evaluateEligibility(r, thr).eligible, "no probability below threshold policy");
    r.distributions[kQuestionNextAction] = {{"CONTINUE", 0.7}};
    MIB_EXPECT(!evaluateEligibility(r, thr).eligible, "0.7 < 0.8 rejected");
    r.distributions[kQuestionNextAction] = {{"CONTINUE", 0.9}};
    MIB_EXPECT(evaluateEligibility(r, thr).eligible, "0.9 >= 0.8 accepted");
    r.error = {ProviderErrorKind::Timeout, "x"};
    MIB_EXPECT(!evaluateEligibility(r, p).eligible, "error never eligible");
}

} // namespace

int main()
{
    testRules();
    testModelCannotOverride();
    testProviderFailuresFailClosed();
    testEligibility();
    std::printf("supervisor_policy_test: %s\n", mib::test::exitCode() == 0 ? "OK" : "FAILED");
    return mib::test::exitCode();
}
