#include "backend/supervisor/EvaluationHarness.h"

#include "backend/supervisor/SupervisorService.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace backend::supervisor {

using json = nlohmann::json;

namespace {

json answersJson(const DecisionAnswers& a)
{
    return json{{kQuestionRunQuality, toString(a.quality)},
                {kQuestionPrimaryProblem, toString(a.problem)},
                {kQuestionNextAction, toString(a.action)},
                {kQuestionAdjustmentTarget, toString(a.target)},
                {kQuestionAdjustmentDirection, toString(a.direction)}};
}

bool answersFrom(const json& j, DecisionAnswers& out, std::string& error)
{
    if (!j.is_object()) { error = "expected is not an object"; return false; }
    auto tok = [&](const char* key) -> std::string {
        return j.contains(key) && j.at(key).is_string() ? j.at(key).get<std::string>() : std::string();
    };
    const auto q = parseRunQuality(tok(kQuestionRunQuality));
    const auto p = parsePrimaryProblem(tok(kQuestionPrimaryProblem));
    const auto a = parseNextAction(tok(kQuestionNextAction));
    const auto t = parseAdjustmentTarget(tok(kQuestionAdjustmentTarget));
    const auto d = parseAdjustmentDirection(tok(kQuestionAdjustmentDirection));
    if (!q || !p || !a || !t || !d) {
        error = "expected answers contain an unknown or missing token";
        return false;
    }
    out.quality = *q; out.problem = *p; out.action = *a; out.target = *t; out.direction = *d;
    return true;
}

bool isStop(NextAction a) { return a == NextAction::StopSuccess || a == NextAction::StopFailure; }
bool isHold(NextAction a) { return isStop(a) || a == NextAction::HumanReview; }
bool isGo(NextAction a) { return a == NextAction::Continue || a == NextAction::Adjust; }

uint64_t percentile(std::vector<uint64_t> v, double p)
{
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const auto idx = static_cast<size_t>(std::min<double>(v.size() - 1, std::max(0.0, p * (v.size() - 1))));
    return v[idx];
}

const char* tokenOf(const DecisionAnswers& a, const std::string& q)
{
    if (q == kQuestionRunQuality) return toString(a.quality);
    if (q == kQuestionPrimaryProblem) return toString(a.problem);
    if (q == kQuestionNextAction) return toString(a.action);
    if (q == kQuestionAdjustmentTarget) return toString(a.target);
    return toString(a.direction);
}

} // namespace

bool labelledCaseFromJson(const std::string& text, LabelledCase& out, std::string& error)
{
    try {
        const json j = json::parse(text);
        if (!j.is_object()) { error = "case is not an object"; return false; }
        const auto schema = j.value("case_schema", 0u);
        if (schema != kLabelledCaseSchemaVersion) {
            error = "unsupported case_schema " + std::to_string(schema);
            return false;
        }
        LabelledCase c;
        c.id = j.value("id", "");
        c.scenario = j.value("scenario", "");
        c.notes = j.value("notes", "");
        c.datasetVersion = j.value("dataset_version", "");
        c.expectPolicy = j.value("expect_policy", false);
        if (c.id.empty()) { error = "case has no id"; return false; }
        if (!j.contains("expected") || !answersFrom(j.at("expected"), c.expected, error)) return false;
        if (!j.contains("snapshot")) { error = "case has no snapshot"; return false; }
        if (!snapshotFromJson(j.at("snapshot").dump(), c.snapshot, error)) return false;
        // The label must never leak into the model input.
        if (j.at("snapshot").contains("expected")) { error = "snapshot must not contain 'expected'"; return false; }
        out = std::move(c);
        return true;
    } catch (const std::exception& e) {
        error = std::string("case parse error: ") + e.what();
        return false;
    }
}

std::string labelledCaseToJson(const LabelledCase& c)
{
    json j;
    j["case_schema"] = kLabelledCaseSchemaVersion;
    j["id"] = c.id;
    j["scenario"] = c.scenario;
    j["notes"] = c.notes;
    j["dataset_version"] = c.datasetVersion;
    j["expect_policy"] = c.expectPolicy;
    j["expected"] = answersJson(c.expected);
    j["snapshot"] = json::parse(snapshotToJson(c.snapshot));
    return j.dump(2);
}

bool loadLabelledCases(const std::string& directory, std::vector<LabelledCase>& out, std::string& error)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(directory, ec)) { error = "not a directory: " + directory; return false; }
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(directory, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
        std::ifstream in(f);
        if (!in) { error = "cannot read " + f.string(); return false; }
        std::stringstream ss;
        ss << in.rdbuf();
        LabelledCase c;
        std::string err;
        if (!labelledCaseFromJson(ss.str(), c, err)) { error = f.string() + ": " + err; return false; }
        out.push_back(std::move(c));
    }
    return true;
}

double EvaluationReport::actionAgreement() const
{
    for (const auto& q : agreement) if (q.question == kQuestionNextAction) return q.agreement();
    return 0.0;
}

EvaluationReport runEvaluation(const std::vector<LabelledCase>& cases, DecisionProvider& provider,
                               const EvaluationOptions& options)
{
    EvaluationReport rep;
    rep.providerName = provider.name();
    rep.providerVersion = provider.version();
    rep.modelVersion = provider.modelVersion();
    for (const auto& q : questionIds()) {
        QuestionAgreement qa;
        qa.question = q;
        rep.agreement.push_back(qa);
    }
    for (double t : options.calibrationThresholds) {
        rep.selectedProbabilityCalibration.push_back(CalibrationBucket{t, 0, 0});
        rep.confidenceCalibration.push_back(CalibrationBucket{t, 0, 0});
    }
    std::vector<uint64_t> latencies;
    double cost = 0.0;
    bool anyCost = false;

    for (const auto& c : cases) {
        if (!c.datasetVersion.empty() && rep.datasetVersion.empty()) rep.datasetVersion = c.datasetVersion;
        ++rep.cases;
        DecisionRecord rec = decideOnce(c.snapshot, options.policy, &provider, SupervisorMode::Shadow);
        CaseOutcome o;
        o.id = c.id;
        o.scenario = c.scenario;
        o.expected = c.expected;
        o.actual = rec.recommendation;
        o.decidedBy = rec.decidedBy;
        if (rec.decidedBy == "policy") ++rep.policyDecided;
        if (rec.providerConsulted) {
            ++rep.providerConsulted;
            o.providerOk = rec.provider.ok();
            if (!o.providerOk) {
                ++rep.providerFailures;
                o.providerError = std::string(toString(rec.provider.error.kind)) + ": " + rec.provider.error.message;
            }
            o.latencyUs = rec.provider.latencyUs;
            latencies.push_back(rec.provider.latencyUs);
            o.selectedProbability = selectedProbability(rec.provider.distributions, kQuestionNextAction,
                                                        toString(rec.provider.answers.action));
            o.confidence = rec.provider.confidence;
            if (rec.provider.costUsd) { cost += *rec.provider.costUsd; anyCost = true; }
        }
        for (auto& qa : rep.agreement) {
            const std::string exp = tokenOf(c.expected, qa.question);
            const std::string act = tokenOf(o.actual, qa.question);
            ++qa.total;
            if (exp == act) ++qa.agree;
            ++qa.confusion[exp][act];
        }
        o.actionAgrees = c.expected.action == o.actual.action;
        if (isGo(c.expected.action) && isStop(o.actual.action)) {
            ++rep.falseStop; o.unsafe = true; o.unsafeKind = "false_stop";
        } else if (isHold(c.expected.action) && isGo(o.actual.action)) {
            ++rep.falseContinue; o.unsafe = true; o.unsafeKind = "false_continue";
        }
        if (o.unsafe) ++rep.unsafeDisagreements;
        if (o.actual.action == NextAction::HumanReview) ++rep.humanReview;
        if (c.expected.action == NextAction::Adjust) {
            ++rep.adjustCasesExpected;
            if (o.actual.target == c.expected.target) ++rep.adjustTargetAgree;
            if (o.actual.direction == c.expected.direction) ++rep.adjustDirectionAgree;
        }
        if (rec.providerConsulted && o.providerOk) {
            for (auto& b : rep.selectedProbabilityCalibration) {
                if (o.selectedProbability && *o.selectedProbability >= b.threshold) {
                    ++b.selected;
                    if (o.actionAgrees) ++b.correct;
                }
            }
            for (auto& b : rep.confidenceCalibration) {
                if (o.confidence && *o.confidence >= b.threshold) {
                    ++b.selected;
                    if (o.actionAgrees) ++b.correct;
                }
            }
        }
        rep.outcomes.push_back(std::move(o));
        if (options.stopOnProviderFailure && rec.providerConsulted && !rec.provider.ok()) break;
    }
    rep.latencyP50Us = percentile(latencies, 0.50);
    rep.latencyP95Us = percentile(latencies, 0.95);
    rep.latencyMaxUs = latencies.empty() ? 0 : *std::max_element(latencies.begin(), latencies.end());
    if (anyCost) rep.totalCostUsd = cost;
    return rep;
}

std::string reportToJson(const EvaluationReport& r)
{
    json j;
    j["contract_version"] = r.contractVersion;
    j["provider"] = {{"name", r.providerName}, {"version", r.providerVersion}, {"model", r.modelVersion}};
    j["dataset_version"] = r.datasetVersion;
    j["cases"] = r.cases;
    j["policy_decided"] = r.policyDecided;
    j["provider_consulted"] = r.providerConsulted;
    j["provider_failures"] = r.providerFailures;
    j["provider_failure_rate"] = r.providerFailureRate();
    json agreement = json::object();
    for (const auto& q : r.agreement) {
        agreement[q.question] = {{"total", q.total}, {"agree", q.agree}, {"agreement", q.agreement()},
                                 {"confusion", q.confusion}};
    }
    j["agreement"] = agreement;
    j["false_stop"] = r.falseStop;
    j["false_continue"] = r.falseContinue;
    j["unsafe_disagreements"] = r.unsafeDisagreements;
    j["human_review"] = r.humanReview;
    j["human_review_rate"] = r.humanReviewRate();
    j["adjust_cases_expected"] = r.adjustCasesExpected;
    j["adjust_target_agree"] = r.adjustTargetAgree;
    j["adjust_direction_agree"] = r.adjustDirectionAgree;
    j["latency_us"] = {{"p50", r.latencyP50Us}, {"p95", r.latencyP95Us}, {"max", r.latencyMaxUs}};
    j["total_cost_usd"] = r.totalCostUsd ? json(*r.totalCostUsd) : json(nullptr);
    auto cal = [](const std::vector<CalibrationBucket>& v) {
        json a = json::array();
        for (const auto& b : v) a.push_back({{"threshold", b.threshold}, {"selected", b.selected},
                                             {"correct", b.correct}, {"p_correct", b.precision()}});
        return a;
    };
    j["calibration"] = {{"selected_probability", cal(r.selectedProbabilityCalibration)},
                        {"confidence", cal(r.confidenceCalibration)}};
    json outcomes = json::array();
    for (const auto& o : r.outcomes) {
        outcomes.push_back({{"id", o.id}, {"scenario", o.scenario}, {"expected", answersJson(o.expected)},
                            {"actual", answersJson(o.actual)}, {"decided_by", o.decidedBy},
                            {"provider_ok", o.providerOk}, {"provider_error", o.providerError},
                            {"selected_probability", o.selectedProbability ? json(*o.selectedProbability) : json(nullptr)},
                            {"confidence", o.confidence ? json(*o.confidence) : json(nullptr)},
                            {"latency_us", o.latencyUs}, {"action_agrees", o.actionAgrees},
                            {"unsafe", o.unsafe}, {"unsafe_kind", o.unsafeKind}});
    }
    j["outcomes"] = outcomes;
    return j.dump(2);
}

std::string reportToText(const EvaluationReport& r)
{
    std::ostringstream o;
    o << "AI Experiment Supervisor — offline evaluation\n";
    o << "provider: " << r.providerName << " " << r.providerVersion;
    if (!r.modelVersion.empty()) o << " model=" << r.modelVersion;
    o << "\ncontract: v" << r.contractVersion << "  dataset: " << (r.datasetVersion.empty() ? "-" : r.datasetVersion) << "\n";
    o << "cases: " << r.cases << "  policy-decided: " << r.policyDecided << "  provider-consulted: "
      << r.providerConsulted << "  provider failures: " << r.providerFailures << "\n\n";
    o << "agreement by question\n";
    for (const auto& q : r.agreement) {
        o << "  " << q.question << ": " << q.agree << "/" << q.total << "\n";
    }
    o << "\nsafety\n  false STOP: " << r.falseStop << "  false CONTINUE: " << r.falseContinue
      << "  unsafe disagreements: " << r.unsafeDisagreements << "  HUMAN_REVIEW: " << r.humanReview << "/" << r.cases << "\n";
    o << "  adjust target agree: " << r.adjustTargetAgree << "/" << r.adjustCasesExpected
      << "  direction agree: " << r.adjustDirectionAgree << "/" << r.adjustCasesExpected << "\n";
    o << "\nlatency us  p50=" << r.latencyP50Us << "  p95=" << r.latencyP95Us << "  max=" << r.latencyMaxUs << "\n";
    if (r.totalCostUsd) o << "cost usd total=" << *r.totalCostUsd << "\n";
    o << "\ncalibration  P(correct | selected probability >= t)\n";
    for (const auto& b : r.selectedProbabilityCalibration)
        o << "  t=" << b.threshold << ": " << b.correct << "/" << b.selected << "\n";
    o << "calibration  P(correct | confidence >= t)\n";
    for (const auto& b : r.confidenceCalibration)
        o << "  t=" << b.threshold << ": " << b.correct << "/" << b.selected << "\n";
    o << "\ndisagreements\n";
    bool any = false;
    for (const auto& c : r.outcomes) {
        if (c.actionAgrees && c.providerOk) continue;
        any = true;
        o << "  " << c.id << " [" << c.scenario << "] expected " << toString(c.expected.action)
          << " got " << toString(c.actual.action) << " (" << c.decidedBy << ")";
        if (!c.unsafeKind.empty()) o << " UNSAFE:" << c.unsafeKind;
        if (!c.providerError.empty()) o << " error=" << c.providerError;
        o << "\n";
    }
    if (!any) o << "  none\n";
    return o.str();
}

} // namespace backend::supervisor
