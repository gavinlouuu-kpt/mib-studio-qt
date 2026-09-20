#include "backend/supervisor/DecisionContract.h"

#include <algorithm>

namespace backend::supervisor {

const char* toString(RunQuality v)
{
    switch (v) {
    case RunQuality::Good: return "GOOD";
    case RunQuality::Marginal: return "MARGINAL";
    case RunQuality::Bad: return "BAD";
    }
    return "MARGINAL";
}

const char* toString(PrimaryProblem v)
{
    switch (v) {
    case PrimaryProblem::None: return "NONE";
    case PrimaryProblem::LowContrast: return "LOW_CONTRAST";
    case PrimaryProblem::Overexposure: return "OVEREXPOSURE";
    case PrimaryProblem::Underexposure: return "UNDEREXPOSURE";
    case PrimaryProblem::ExcessFalseDetections: return "EXCESS_FALSE_DETECTIONS";
    case PrimaryProblem::ExcessRejections: return "EXCESS_REJECTIONS";
    case PrimaryProblem::FrameLoss: return "FRAME_LOSS";
    case PrimaryProblem::TriggerFailure: return "TRIGGER_FAILURE";
    case PrimaryProblem::InsufficientData: return "INSUFFICIENT_DATA";
    case PrimaryProblem::Other: return "OTHER";
    }
    return "OTHER";
}

const char* toString(NextAction v)
{
    switch (v) {
    case NextAction::Continue: return "CONTINUE";
    case NextAction::Adjust: return "ADJUST";
    case NextAction::StopSuccess: return "STOP_SUCCESS";
    case NextAction::StopFailure: return "STOP_FAILURE";
    case NextAction::HumanReview: return "HUMAN_REVIEW";
    }
    return "HUMAN_REVIEW";
}

const char* toString(AdjustmentTarget v)
{
    switch (v) {
    case AdjustmentTarget::None: return "NONE";
    case AdjustmentTarget::Exposure: return "EXPOSURE";
    case AdjustmentTarget::DetectionThreshold: return "DETECTION_THRESHOLD";
    case AdjustmentTarget::MinArea: return "MIN_AREA";
    case AdjustmentTarget::MaxArea: return "MAX_AREA";
    case AdjustmentTarget::TriggerTiming: return "TRIGGER_TIMING";
    }
    return "NONE";
}

const char* toString(AdjustmentDirection v)
{
    switch (v) {
    case AdjustmentDirection::Keep: return "KEEP";
    case AdjustmentDirection::Increase: return "INCREASE";
    case AdjustmentDirection::Decrease: return "DECREASE";
    }
    return "KEEP";
}

namespace {

template <typename E, int Count>
std::optional<E> parseEnum(std::string_view token)
{
    for (int i = 0; i < Count; ++i) {
        const auto v = static_cast<E>(i);
        if (token == toString(v)) return v;
    }
    return std::nullopt;
}

template <typename E, int Count>
std::vector<std::string> allTokens()
{
    std::vector<std::string> out;
    out.reserve(Count);
    for (int i = 0; i < Count; ++i) out.emplace_back(toString(static_cast<E>(i)));
    return out;
}

} // namespace

std::optional<RunQuality> parseRunQuality(std::string_view token) { return parseEnum<RunQuality, 3>(token); }
std::optional<PrimaryProblem> parsePrimaryProblem(std::string_view token) { return parseEnum<PrimaryProblem, 10>(token); }
std::optional<NextAction> parseNextAction(std::string_view token) { return parseEnum<NextAction, 5>(token); }
std::optional<AdjustmentTarget> parseAdjustmentTarget(std::string_view token) { return parseEnum<AdjustmentTarget, 6>(token); }
std::optional<AdjustmentDirection> parseAdjustmentDirection(std::string_view token) { return parseEnum<AdjustmentDirection, 3>(token); }

const std::vector<std::string>& questionIds()
{
    static const std::vector<std::string> ids{kQuestionRunQuality, kQuestionPrimaryProblem,
                                             kQuestionNextAction, kQuestionAdjustmentTarget,
                                             kQuestionAdjustmentDirection};
    return ids;
}

const std::vector<std::string>& tokensFor(std::string_view question)
{
    static const std::vector<std::string> quality = allTokens<RunQuality, 3>();
    static const std::vector<std::string> problem = allTokens<PrimaryProblem, 10>();
    static const std::vector<std::string> action = allTokens<NextAction, 5>();
    static const std::vector<std::string> target = allTokens<AdjustmentTarget, 6>();
    static const std::vector<std::string> direction = allTokens<AdjustmentDirection, 3>();
    static const std::vector<std::string> none;
    if (question == kQuestionRunQuality) return quality;
    if (question == kQuestionPrimaryProblem) return problem;
    if (question == kQuestionNextAction) return action;
    if (question == kQuestionAdjustmentTarget) return target;
    if (question == kQuestionAdjustmentDirection) return direction;
    return none;
}

std::optional<double> selectedProbability(const Distributions& distributions,
                                          std::string_view question, std::string_view token)
{
    const auto q = distributions.find(std::string(question));
    if (q == distributions.end()) return std::nullopt;
    const auto t = q->second.find(std::string(token));
    if (t == q->second.end()) return std::nullopt;
    return t->second;
}

const char* toString(ProviderErrorKind k)
{
    switch (k) {
    case ProviderErrorKind::None: return "none";
    case ProviderErrorKind::NotConfigured: return "notConfigured";
    case ProviderErrorKind::Timeout: return "timeout";
    case ProviderErrorKind::Transport: return "transport";
    case ProviderErrorKind::Authentication: return "authentication";
    case ProviderErrorKind::RateLimited: return "rateLimited";
    case ProviderErrorKind::MalformedResponse: return "malformedResponse";
    case ProviderErrorKind::SchemaViolation: return "schemaViolation";
    case ProviderErrorKind::ModelMismatch: return "modelMismatch";
    case ProviderErrorKind::Exception: return "exception";
    case ProviderErrorKind::Cancelled: return "cancelled";
    }
    return "exception";
}

std::optional<ProviderErrorKind> parseProviderErrorKind(std::string_view token)
{
    return parseEnum<ProviderErrorKind, 11>(token);
}

bool targetAllowed(const DecisionPolicy& policy, AdjustmentTarget target)
{
    if (target == AdjustmentTarget::None) return true;
    return std::find(policy.allowedTargets.begin(), policy.allowedTargets.end(), target) !=
           policy.allowedTargets.end();
}

} // namespace backend::supervisor
