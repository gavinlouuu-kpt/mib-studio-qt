// AI Experiment Supervisor — decision contract (issue #422, ADR 0006).
//
// The supervisor asks a decision provider narrow, typed questions about a
// frozen ExperimentSnapshot and receives typed answers. Every enum here is a
// closed vocabulary: a token that does not parse is rejected by the adapter
// (never mapped to a default), so free-form model text can never become an
// action. The contract is versioned independently of the snapshot schema and
// of any provider/model version so later comparisons stay meaningful.
//
// Nothing in this header has any actuation authority. A DecisionResult is a
// recommendation; MIB Studio decides whether it is valid, safe or executable
// (SafetyPolicy), and in shadow mode nothing is ever executed.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace backend::supervisor {

inline constexpr uint32_t kDecisionContractVersion = 1;

// ---- questions --------------------------------------------------------------

enum class RunQuality { Good = 0, Marginal = 1, Bad = 2 };

enum class PrimaryProblem {
    None = 0,
    LowContrast = 1,
    Overexposure = 2,
    Underexposure = 3,
    ExcessFalseDetections = 4,
    ExcessRejections = 5,
    FrameLoss = 6,
    TriggerFailure = 7,
    InsufficientData = 8,
    Other = 9,
};

enum class NextAction {
    Continue = 0,
    Adjust = 1,
    StopSuccess = 2,
    StopFailure = 3,
    HumanReview = 4,
};

enum class AdjustmentTarget {
    None = 0,
    Exposure = 1,
    DetectionThreshold = 2,
    MinArea = 3,
    MaxArea = 4,
    TriggerTiming = 5,
};

enum class AdjustmentDirection { Keep = 0, Increase = 1, Decrease = 2 };

// Wire tokens (append only; never renumber or rename).
const char* toString(RunQuality v);
const char* toString(PrimaryProblem v);
const char* toString(NextAction v);
const char* toString(AdjustmentTarget v);
const char* toString(AdjustmentDirection v);

// Strict parsers: an unknown token yields nullopt. There is deliberately no
// "default" overload.
std::optional<RunQuality> parseRunQuality(std::string_view token);
std::optional<PrimaryProblem> parsePrimaryProblem(std::string_view token);
std::optional<NextAction> parseNextAction(std::string_view token);
std::optional<AdjustmentTarget> parseAdjustmentTarget(std::string_view token);
std::optional<AdjustmentDirection> parseAdjustmentDirection(std::string_view token);

// Stable question identifiers used in distributions, records and reports.
inline constexpr const char* kQuestionRunQuality = "run_quality";
inline constexpr const char* kQuestionPrimaryProblem = "primary_problem";
inline constexpr const char* kQuestionNextAction = "next_action";
inline constexpr const char* kQuestionAdjustmentTarget = "adjustment_target";
inline constexpr const char* kQuestionAdjustmentDirection = "adjustment_direction";

// Every legal token per question, in enum order (for prompts and validation).
const std::vector<std::string>& tokensFor(std::string_view question);
const std::vector<std::string>& questionIds();

// The five selected answers.
struct DecisionAnswers {
    RunQuality quality{RunQuality::Marginal};
    PrimaryProblem problem{PrimaryProblem::Other};
    NextAction action{NextAction::HumanReview};
    AdjustmentTarget target{AdjustmentTarget::None};
    AdjustmentDirection direction{AdjustmentDirection::Keep};

    bool operator==(const DecisionAnswers& o) const
    {
        return quality == o.quality && problem == o.problem && action == o.action &&
               target == o.target && direction == o.direction;
    }
    bool operator!=(const DecisionAnswers& o) const { return !(*this == o); }
};

// Fail-closed default: nothing to do but ask a human.
inline DecisionAnswers humanReviewAnswers(PrimaryProblem problem = PrimaryProblem::Other)
{
    DecisionAnswers a;
    a.quality = RunQuality::Marginal;
    a.problem = problem;
    a.action = NextAction::HumanReview;
    a.target = AdjustmentTarget::None;
    a.direction = AdjustmentDirection::Keep;
    return a;
}

// Probability mass per token for one question. Tokens are validated against
// tokensFor(question); values are in [0, 1].
using Distribution = std::map<std::string, double>;
using Distributions = std::map<std::string, Distribution>; // question -> distribution

// Selected-token probability for a question, if the provider returned one.
std::optional<double> selectedProbability(const Distributions& distributions,
                                          std::string_view question, std::string_view token);

// ---- provider outcome --------------------------------------------------------

enum class ProviderErrorKind {
    None = 0,
    NotConfigured = 1,   // no endpoint/credential/transport; nothing was sent
    Timeout = 2,
    Transport = 3,       // network / HTTP failure
    Authentication = 4,  // 401 / 403
    RateLimited = 5,     // 429
    MalformedResponse = 6, // not JSON / not an object
    SchemaViolation = 7, // JSON but not our contract (unknown token, missing answer)
    ModelMismatch = 8,   // provider answered with a model other than the pinned one
    Exception = 9,
    Cancelled = 10,
};
const char* toString(ProviderErrorKind k);
std::optional<ProviderErrorKind> parseProviderErrorKind(std::string_view token);

struct ProviderError {
    ProviderErrorKind kind{ProviderErrorKind::None};
    std::string message; // operator text; must never contain a credential
    bool empty() const { return kind == ProviderErrorKind::None; }
};

// What a provider returns for one snapshot. `ok()` means the answers are
// contract-valid; otherwise `answers` is the fail-closed HumanReview value
// and must not be reported as a model decision.
struct DecisionResult {
    uint32_t contractVersion{kDecisionContractVersion};
    std::string providerName;    // "rule", "jev", "scripted", ...
    std::string providerVersion; // adapter version, e.g. "jev-adapter/1"
    std::string modelVersion;    // pinned model identifier (empty for deterministic providers)
    DecisionAnswers answers{humanReviewAnswers()};
    Distributions distributions;              // full returned mass, when available
    std::optional<double> confidence;         // provider-reported, NOT P(correct)
    std::string rationale;                    // display-only text; never executed
    uint64_t latencyUs{0};
    int attempts{1};
    std::optional<double> costUsd;            // when the provider reports it
    ProviderError error;

    bool ok() const { return error.empty(); }
};

// ---- policy ----------------------------------------------------------------

// Deterministic limits the model can never override. Evaluated in ordinary
// code before any provider call (SafetyPolicy) and again on the returned
// answers (eligibility). Values are conservative defaults for shadow mode.
struct DecisionPolicy {
    // Hard invariants (SafetyPolicy).
    double hardFrameLossFraction{0.20};        // lost / delivered above this -> deterministic stop
    uint64_t minFramesForModelDecision{100};   // below this only INSUFFICIENT_DATA / CONTINUE
    double minElapsedSecondsForModel{2.0};
    std::optional<uint64_t> targetValidObjects; // objective-defined completion when set
    // Recommendation eligibility (never executed in shadow mode).
    std::vector<AdjustmentTarget> allowedTargets{AdjustmentTarget::Exposure,
                                                 AdjustmentTarget::DetectionThreshold,
                                                 AdjustmentTarget::MinArea,
                                                 AdjustmentTarget::MaxArea,
                                                 AdjustmentTarget::TriggerTiming};
    // Provider bounds.
    int providerTimeoutMs{10000};
    int providerMaxRetries{1};
    // Threshold below which a model answer is recorded but flagged
    // low-confidence (an empirical setting, see the evaluation harness).
    double minSelectedProbability{0.0};
};

bool targetAllowed(const DecisionPolicy& policy, AdjustmentTarget target);

} // namespace backend::supervisor
