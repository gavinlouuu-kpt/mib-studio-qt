// AI Experiment Supervisor — offline, replayable evaluation harness
// (issue #422, phase B).
//
// Input: labelled cases (snapshot + expected answers + scenario metadata).
// Output: per-question agreement, confusion matrices, unsafe disagreements,
// false STOP / false CONTINUE rates, HUMAN_REVIEW rate, latency percentiles,
// provider failure rate, cost, and calibration
// P(correct | selected probability >= t) / P(correct | confidence >= t) for
// several thresholds. Every case runs through decideOnce(), i.e. the same
// policy-first path as live shadow mode, so a case the policy decides is
// reported as such.
#pragma once

#include "backend/supervisor/DecisionContract.h"
#include "backend/supervisor/DecisionProvider.h"
#include "backend/supervisor/DecisionRecord.h"
#include "backend/supervisor/ExperimentSnapshot.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace backend::supervisor {

inline constexpr uint32_t kLabelledCaseSchemaVersion = 1;

struct LabelledCase {
    std::string id;
    std::string scenario;
    std::string notes;
    std::string datasetVersion;
    ExperimentSnapshot snapshot;
    DecisionAnswers expected;
    // Set when the expected decision is the deterministic policy's (so a
    // provider disagreement there is a policy-precedence check, not a model
    // accuracy result).
    bool expectPolicy{false};
};

// One case per JSON file ({"case_schema":1,"id":..,"expected":{..},"snapshot":{..}}).
bool labelledCaseFromJson(const std::string& json, LabelledCase& out, std::string& error);
std::string labelledCaseToJson(const LabelledCase& c);
// Loads every *.json in `directory` (sorted by name). Returns false and sets
// `error` on the first unreadable/invalid file.
bool loadLabelledCases(const std::string& directory, std::vector<LabelledCase>& out,
                       std::string& error);

struct QuestionAgreement {
    std::string question;
    uint64_t total{0};
    uint64_t agree{0};
    // confusion[expected][predicted] = count
    std::map<std::string, std::map<std::string, uint64_t>> confusion;
    double agreement() const { return total ? static_cast<double>(agree) / total : 0.0; }
};

struct CalibrationBucket {
    double threshold{0.0};
    uint64_t selected{0};   // cases with metric >= threshold
    uint64_t correct{0};    // of those, next_action agreed
    double precision() const { return selected ? static_cast<double>(correct) / selected : 0.0; }
};

struct CaseOutcome {
    std::string id;
    std::string scenario;
    DecisionAnswers expected;
    DecisionAnswers actual;
    std::string decidedBy;
    bool providerOk{true};
    std::string providerError;
    std::optional<double> selectedProbability;
    std::optional<double> confidence;
    uint64_t latencyUs{0};
    bool actionAgrees{false};
    bool unsafe{false};      // expected a stop/review, got continue/adjust (or vice versa)
    std::string unsafeKind;  // "false_continue" | "false_stop" | ""
};

struct EvaluationReport {
    uint32_t contractVersion{kDecisionContractVersion};
    std::string providerName, providerVersion, modelVersion;
    std::string datasetVersion;
    uint64_t cases{0};
    uint64_t policyDecided{0};
    uint64_t providerConsulted{0};
    uint64_t providerFailures{0};
    std::vector<QuestionAgreement> agreement;   // one per question
    uint64_t falseStop{0};        // expected CONTINUE/ADJUST, got STOP_*
    uint64_t falseContinue{0};    // expected STOP_*/HUMAN_REVIEW, got CONTINUE/ADJUST
    uint64_t unsafeDisagreements{0};
    uint64_t humanReview{0};
    uint64_t adjustCasesExpected{0};
    uint64_t adjustTargetAgree{0};
    uint64_t adjustDirectionAgree{0};
    uint64_t latencyP50Us{0}, latencyP95Us{0}, latencyMaxUs{0};
    std::optional<double> totalCostUsd;
    std::vector<CalibrationBucket> selectedProbabilityCalibration;
    std::vector<CalibrationBucket> confidenceCalibration;
    std::vector<CaseOutcome> outcomes;

    double actionAgreement() const;
    double providerFailureRate() const
    {
        return providerConsulted ? static_cast<double>(providerFailures) / providerConsulted : 0.0;
    }
    double humanReviewRate() const { return cases ? static_cast<double>(humanReview) / cases : 0.0; }
};

struct EvaluationOptions {
    DecisionPolicy policy;
    std::vector<double> calibrationThresholds{0.5, 0.6, 0.7, 0.8, 0.9};
    bool stopOnProviderFailure{false};
};

EvaluationReport runEvaluation(const std::vector<LabelledCase>& cases, DecisionProvider& provider,
                               const EvaluationOptions& options);

std::string reportToJson(const EvaluationReport& report);
std::string reportToText(const EvaluationReport& report);

} // namespace backend::supervisor
