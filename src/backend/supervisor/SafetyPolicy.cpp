#include "backend/supervisor/SafetyPolicy.h"

namespace backend::supervisor {

namespace {

DecisionAnswers make(RunQuality q, PrimaryProblem p, NextAction a)
{
    DecisionAnswers d;
    d.quality = q;
    d.problem = p;
    d.action = a;
    d.target = AdjustmentTarget::None;
    d.direction = AdjustmentDirection::Keep;
    return d;
}

bool isStoppedState(const std::string& state)
{
    return state == "idle" || state == "stopping" || state == "failed";
}

} // namespace

PolicyOutcome evaluateSafetyPolicy(const ExperimentSnapshot& s, const DecisionPolicy& policy)
{
    PolicyOutcome out;
    auto fire = [&](const char* rule, std::string reason, DecisionAnswers answers) {
        out.requiresAction = true;
        out.ruleId = rule;
        out.reason = std::move(reason);
        out.answers = answers;
    };

    // 1. Unresolved fault / failed run: the existing deterministic error path
    //    owns this; the model has nothing to add.
    out.rulesChecked.emplace_back("policy.unresolved_fault");
    if (!s.recording.faultCode.empty() || s.experimentState == "failed") {
        fire("policy.unresolved_fault",
             "unresolved fault '" + s.recording.faultCode + "': " + s.recording.faultMessage,
             make(RunQuality::Bad, PrimaryProblem::Other, NextAction::StopFailure));
        return out;
    }

    // 2. Storage / finalization failure -> existing deterministic error path.
    out.rulesChecked.emplace_back("policy.storage_failure");
    if (const auto failed = s.recording.persistenceFailed.get(); failed && *failed > 0.0) {
        fire("policy.storage_failure",
             "persistence reported " + std::to_string(static_cast<uint64_t>(*failed)) + " failed frames",
             make(RunQuality::Bad, PrimaryProblem::Other, NextAction::StopFailure));
        return out;
    }

    // 3. Camera unavailable / capture failed while the experiment is active.
    out.rulesChecked.emplace_back("policy.camera_unavailable");
    const bool captureFailed = !s.acquisition.lastFailure.empty() && s.acquisition.lastFailure != "none";
    if (s.experimentState == "active" && (!s.acquisition.cameraReady || captureFailed)) {
        fire("policy.camera_unavailable",
             captureFailed ? "capture failure: " + s.acquisition.lastFailure
                           : "camera not ready while the experiment is active",
             make(RunQuality::Bad, PrimaryProblem::FrameLoss, NextAction::StopFailure));
        return out;
    }

    // 4. Hard frame-loss threshold.
    out.rulesChecked.emplace_back("policy.frame_loss");
    if (const auto loss = frameLossFraction(s); loss && *loss >= policy.hardFrameLossFraction) {
        fire("policy.frame_loss",
             "transport frame loss " + std::to_string(*loss) + " >= hard limit " +
                 std::to_string(policy.hardFrameLossFraction),
             make(RunQuality::Bad, PrimaryProblem::FrameLoss, NextAction::StopFailure));
        return out;
    }

    // 5. Target count reached -> deterministic completion when the objective
    //    defines it unambiguously.
    out.rulesChecked.emplace_back("policy.target_reached");
    const auto target = s.targetValidObjects ? s.targetValidObjects : policy.targetValidObjects;
    if (target && *target > 0) {
        if (const auto valid = s.detection.validObjects.get(); valid && *valid >= static_cast<double>(*target)) {
            fire("policy.target_reached",
                 "valid objects " + std::to_string(static_cast<uint64_t>(*valid)) + " >= target " +
                     std::to_string(*target),
                 make(RunQuality::Good, PrimaryProblem::None, NextAction::StopSuccess));
            return out;
        }
    }

    // 6. Experiment not running: nothing to supervise; no model call.
    out.rulesChecked.emplace_back("policy.not_active");
    if (isStoppedState(s.experimentState)) {
        fire("policy.not_active", "experiment state '" + s.experimentState + "' is not active",
             make(RunQuality::Marginal, PrimaryProblem::InsufficientData, NextAction::HumanReview));
        return out;
    }

    // 7. Insufficient observations early in a run: CONTINUE, do not ask a
    //    model to extrapolate from nothing.
    out.rulesChecked.emplace_back("policy.insufficient_data");
    const auto frames = s.detection.framesProcessed.get();
    const bool tooFewFrames = !frames || *frames < static_cast<double>(policy.minFramesForModelDecision);
    const bool tooEarly = s.elapsedSeconds < policy.minElapsedSecondsForModel;
    if (tooFewFrames || tooEarly) {
        fire("policy.insufficient_data",
             tooEarly ? "elapsed " + std::to_string(s.elapsedSeconds) + " s below minimum"
                      : "frames processed below minimum for a model decision",
             make(RunQuality::Marginal, PrimaryProblem::InsufficientData, NextAction::Continue));
        return out;
    }

    return out;
}

EligibilityOutcome evaluateEligibility(const DecisionResult& result, const DecisionPolicy& policy)
{
    EligibilityOutcome e;
    if (!result.ok()) {
        e.reason = std::string("provider error: ") + toString(result.error.kind);
        return e;
    }
    const auto& a = result.answers;
    if (a.action == NextAction::HumanReview) {
        e.reason = "HUMAN_REVIEW is never executable";
        return e;
    }
    if (a.action == NextAction::Adjust) {
        if (a.target == AdjustmentTarget::None) { e.reason = "ADJUST without a target"; return e; }
        if (a.direction == AdjustmentDirection::Keep) { e.reason = "ADJUST with direction KEEP"; return e; }
        if (!targetAllowed(policy, a.target)) {
            e.reason = std::string("target ") + toString(a.target) + " outside configured limits";
            return e;
        }
    } else if (a.target != AdjustmentTarget::None || a.direction != AdjustmentDirection::Keep) {
        e.reason = "adjustment fields set on a non-ADJUST action";
        return e;
    }
    if (policy.minSelectedProbability > 0.0) {
        const auto p = selectedProbability(result.distributions, kQuestionNextAction, toString(a.action));
        if (!p) { e.reason = "no selected probability below threshold policy"; return e; }
        if (*p < policy.minSelectedProbability) {
            e.reason = "selected probability " + std::to_string(*p) + " below threshold";
            return e;
        }
    }
    e.eligible = true;
    return e;
}

} // namespace backend::supervisor
