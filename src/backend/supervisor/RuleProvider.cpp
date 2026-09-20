#include "backend/supervisor/RuleProvider.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace backend::supervisor {

namespace {

// Synthetic one-hot-with-margin distribution: the selected token gets
// `p`, the rest share the remainder equally. Deterministic; exercises the
// calibration paths without pretending to be a probability of correctness.
Distribution margin(std::string_view question, std::string_view selected, double p)
{
    const auto& tokens = tokensFor(question);
    Distribution d;
    const double rest = tokens.size() > 1 ? (1.0 - p) / static_cast<double>(tokens.size() - 1) : 0.0;
    for (const auto& t : tokens) d[t] = (t == selected) ? p : rest;
    return d;
}

double clamp01(double v) { return std::max(0.0, std::min(1.0, v)); }

} // namespace

RuleProvider::RuleProvider(RuleThresholds thresholds) : thresholds_(thresholds) {}

DecisionResult RuleProvider::evaluate(const ExperimentSnapshot& s, const DecisionPolicy& policy)
{
    const auto t0 = std::chrono::steady_clock::now();
    const auto& th = thresholds_;
    DecisionResult r;
    r.providerName = name();
    r.providerVersion = version();

    DecisionAnswers a;
    double p = 0.9; // synthetic margin for the selected next_action

    const auto loss = frameLossFraction(s);
    const auto rejection = rejectionFraction(s);
    const auto triggerSuccess = triggerSuccessFraction(s);
    const auto contrast = s.detection.contrastMean.get();
    const auto q4 = s.detection.brightnessMaxMean.get();
    const auto q2 = s.detection.brightnessMedianMean.get();
    const auto areaMean = s.detection.areaMeanUm2.get();
    const auto validPerSecond = s.detection.validPerSecond.get();
    const auto invalidPerSecond = s.detection.invalidPerSecond.get();
    const auto valid = s.detection.validObjects.get();
    const auto invalid = s.detection.invalidObjects.get();
    const double objectsPerSecond = (validPerSecond ? *validPerSecond : 0.0) +
                                    (invalidPerSecond ? *invalidPerSecond : 0.0);
    const double totalObjects = (valid ? *valid : 0.0) + (invalid ? *invalid : 0.0);
    const bool detectionGood = contrast && *contrast >= th.lowContrast && rejection &&
                               *rejection < th.excessRejectionFraction;

    auto set = [&](RunQuality q, PrimaryProblem pr, NextAction act,
                   AdjustmentTarget tg = AdjustmentTarget::None,
                   AdjustmentDirection dir = AdjustmentDirection::Keep, double margin = 0.9) {
        a.quality = q; a.problem = pr; a.action = act; a.target = tg; a.direction = dir; p = margin;
    };

    // Rules in priority order (documented in SupervisorService.md).
    if (loss && *loss >= policy.hardFrameLossFraction) {
        // Mirrors the policy; reachable only when the harness calls the
        // provider directly.
        set(RunQuality::Bad, PrimaryProblem::FrameLoss, NextAction::StopFailure);
    } else if (loss && *loss >= th.marginalFrameLossFraction && detectionGood) {
        // Conflicting state: good detection quality with growing frame loss
        // — a human should weigh the trade-off, the rule will not guess.
        set(RunQuality::Marginal, PrimaryProblem::FrameLoss, NextAction::HumanReview,
            AdjustmentTarget::None, AdjustmentDirection::Keep, 0.6);
    } else if (loss && *loss >= th.marginalFrameLossFraction) {
        set(RunQuality::Marginal, PrimaryProblem::FrameLoss, NextAction::HumanReview,
            AdjustmentTarget::None, AdjustmentDirection::Keep, 0.7);
    } else if (s.configuration.triggerEnabled && triggerSuccess &&
               (1.0 - *triggerSuccess) >= th.triggerFailureFraction) {
        set(RunQuality::Bad, PrimaryProblem::TriggerFailure, NextAction::Adjust,
            AdjustmentTarget::TriggerTiming, AdjustmentDirection::Increase, 0.7);
    } else if (s.configuration.triggerEnabled && s.trigger.eligibleObjects.get() &&
               s.trigger.triggersIssued.get() &&
               *s.trigger.triggersIssued.get() > *s.trigger.eligibleObjects.get()) {
        // More pulses than eligible objects: trigger/detection accounting
        // disagrees — never "fix" that automatically.
        set(RunQuality::Bad, PrimaryProblem::TriggerFailure, NextAction::HumanReview,
            AdjustmentTarget::None, AdjustmentDirection::Keep, 0.8);
    } else if (q4 && *q4 >= th.saturationBrightness) {
        set(RunQuality::Bad, PrimaryProblem::Overexposure, NextAction::Adjust,
            AdjustmentTarget::Exposure, AdjustmentDirection::Decrease);
    } else if (q2 && *q2 <= th.underexposureMedian) {
        set(RunQuality::Bad, PrimaryProblem::Underexposure, NextAction::Adjust,
            AdjustmentTarget::Exposure, AdjustmentDirection::Increase);
    } else if (contrast && *contrast < th.lowContrast) {
        set(RunQuality::Marginal, PrimaryProblem::LowContrast, NextAction::Adjust,
            AdjustmentTarget::Exposure, AdjustmentDirection::Increase, 0.75);
    } else if (totalObjects <= 0.0 && s.elapsedSeconds >= th.noObjectsAfterSeconds) {
        set(RunQuality::Bad, PrimaryProblem::InsufficientData, NextAction::HumanReview,
            AdjustmentTarget::None, AdjustmentDirection::Keep, 0.8);
    } else if (objectsPerSecond >= th.excessFalseDetectionObjectsPerSecond &&
               rejection && *rejection >= th.excessRejectionFraction) {
        set(RunQuality::Bad, PrimaryProblem::ExcessFalseDetections, NextAction::Adjust,
            AdjustmentTarget::DetectionThreshold, AdjustmentDirection::Increase, 0.8);
    } else if (areaMean && s.configuration.minAreaUm2 > 0 &&
               *areaMean < th.debrisAreaFractionOfMin * s.configuration.minAreaUm2 &&
               rejection && *rejection >= th.excessRejectionFraction) {
        set(RunQuality::Marginal, PrimaryProblem::ExcessFalseDetections, NextAction::Adjust,
            AdjustmentTarget::MinArea, AdjustmentDirection::Increase, 0.7);
    } else if (rejection && *rejection >= th.excessRejectionFraction) {
        set(RunQuality::Marginal, PrimaryProblem::ExcessRejections, NextAction::Adjust,
            AdjustmentTarget::DetectionThreshold, AdjustmentDirection::Decrease, 0.7);
    } else if (objectsPerSecond >= th.highConcentrationObjectsPerSecond) {
        set(RunQuality::Marginal, PrimaryProblem::Other, NextAction::HumanReview,
            AdjustmentTarget::None, AdjustmentDirection::Keep, 0.6);
    } else if (rejection) {
        set(RunQuality::Good, PrimaryProblem::None, NextAction::Continue,
            AdjustmentTarget::None, AdjustmentDirection::Keep, 0.95);
    } else {
        set(RunQuality::Marginal, PrimaryProblem::InsufficientData, NextAction::Continue,
            AdjustmentTarget::None, AdjustmentDirection::Keep, 0.6);
    }

    r.answers = a;
    r.distributions[kQuestionRunQuality] = margin(kQuestionRunQuality, toString(a.quality), clamp01(p));
    r.distributions[kQuestionPrimaryProblem] = margin(kQuestionPrimaryProblem, toString(a.problem), clamp01(p));
    r.distributions[kQuestionNextAction] = margin(kQuestionNextAction, toString(a.action), clamp01(p));
    r.distributions[kQuestionAdjustmentTarget] = margin(kQuestionAdjustmentTarget, toString(a.target), clamp01(p));
    r.distributions[kQuestionAdjustmentDirection] = margin(kQuestionAdjustmentDirection, toString(a.direction), clamp01(p));
    r.confidence = clamp01(p);
    r.rationale = "rule/1 deterministic thresholds";
    r.latencyUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                            std::chrono::steady_clock::now() - t0).count());
    return r;
}

} // namespace backend::supervisor
