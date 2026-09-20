#include "backend/supervisor/DecisionRecord.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cstdio>
#include <exception>
#include <stdexcept>
#include <fstream>

namespace backend::supervisor {

using json = nlohmann::json;

const char* toString(SupervisorMode m)
{
    switch (m) {
    case SupervisorMode::Off: return "off";
    case SupervisorMode::Shadow: return "shadow";
    }
    return "off";
}

std::optional<SupervisorMode> parseSupervisorMode(std::string_view token)
{
    if (token == "off") return SupervisorMode::Off;
    if (token == "shadow") return SupervisorMode::Shadow;
    return std::nullopt;
}

namespace {

json answersToJson(const DecisionAnswers& a)
{
    json j;
    j[kQuestionRunQuality] = toString(a.quality);
    j[kQuestionPrimaryProblem] = toString(a.problem);
    j[kQuestionNextAction] = toString(a.action);
    j[kQuestionAdjustmentTarget] = toString(a.target);
    j[kQuestionAdjustmentDirection] = toString(a.direction);
    return j;
}

bool answersFromJson(const json& j, DecisionAnswers& out, std::string& error)
{
    if (!j.is_object()) { error = "answers is not an object"; return false; }
    auto need = [&](const char* key, std::string& token) {
        if (!j.contains(key) || !j.at(key).is_string()) { error = std::string("missing answer ") + key; return false; }
        token = j.at(key).get<std::string>();
        return true;
    };
    std::string q, p, a, t, d;
    if (!need(kQuestionRunQuality, q) || !need(kQuestionPrimaryProblem, p) || !need(kQuestionNextAction, a) ||
        !need(kQuestionAdjustmentTarget, t) || !need(kQuestionAdjustmentDirection, d)) return false;
    const auto pq = parseRunQuality(q);
    const auto pp = parsePrimaryProblem(p);
    const auto pa = parseNextAction(a);
    const auto pt = parseAdjustmentTarget(t);
    const auto pd = parseAdjustmentDirection(d);
    if (!pq) { error = "unknown run_quality '" + q + "'"; return false; }
    if (!pp) { error = "unknown primary_problem '" + p + "'"; return false; }
    if (!pa) { error = "unknown next_action '" + a + "'"; return false; }
    if (!pt) { error = "unknown adjustment_target '" + t + "'"; return false; }
    if (!pd) { error = "unknown adjustment_direction '" + d + "'"; return false; }
    out.quality = *pq; out.problem = *pp; out.action = *pa; out.target = *pt; out.direction = *pd;
    return true;
}

json resultToJson(const DecisionResult& r)
{
    json j;
    j["contract_version"] = r.contractVersion;
    j["provider_name"] = r.providerName;
    j["provider_version"] = r.providerVersion;
    j["model_version"] = r.modelVersion;
    j["answers"] = answersToJson(r.answers);
    j["distributions"] = r.distributions;
    j["confidence"] = r.confidence ? json(*r.confidence) : json(nullptr);
    j["rationale"] = r.rationale;
    j["latency_us"] = r.latencyUs;
    j["attempts"] = r.attempts;
    j["cost_usd"] = r.costUsd ? json(*r.costUsd) : json(nullptr);
    j["error"] = {{"kind", toString(r.error.kind)}, {"message", r.error.message}};
    return j;
}

bool resultFromJson(const json& j, DecisionResult& r, std::string& error)
{
    r.contractVersion = j.value("contract_version", kDecisionContractVersion);
    r.providerName = j.value("provider_name", "");
    r.providerVersion = j.value("provider_version", "");
    r.modelVersion = j.value("model_version", "");
    if (j.contains("answers") && !answersFromJson(j.at("answers"), r.answers, error)) return false;
    if (j.contains("distributions") && j.at("distributions").is_object()) {
        r.distributions = j.at("distributions").get<Distributions>();
    }
    if (j.contains("confidence") && j.at("confidence").is_number()) r.confidence = j.at("confidence").get<double>();
    r.rationale = j.value("rationale", "");
    r.latencyUs = j.value("latency_us", 0ULL);
    r.attempts = j.value("attempts", 1);
    if (j.contains("cost_usd") && j.at("cost_usd").is_number()) r.costUsd = j.at("cost_usd").get<double>();
    if (j.contains("error") && j.at("error").is_object()) {
        const auto kind = parseProviderErrorKind(j.at("error").value("kind", "none"));
        r.error.kind = kind ? *kind : ProviderErrorKind::Exception;
        r.error.message = j.at("error").value("message", "");
    }
    return true;
}

} // namespace

std::string recordToJson(const DecisionRecord& rec)
{
    json j;
    j["record_schema_version"] = rec.recordSchemaVersion;
    j["sequence"] = rec.sequence;
    j["wall_clock_ns"] = rec.wallClockNs;
    j["host_time_us"] = rec.hostTimeUs;
    j["mode"] = rec.mode;
    j["run_id"] = rec.runId;
    j["snapshot_hash"] = rec.snapshotHash;
    j["snapshot_schema_version"] = rec.snapshotSchemaVersion;
    j["snapshot"] = json::parse(snapshotToJson(rec.snapshot));
    j["policy"] = {{"requires_action", rec.policy.requiresAction},
                   {"rule_id", rec.policy.ruleId},
                   {"reason", rec.policy.reason},
                   {"answers", answersToJson(rec.policy.answers)},
                   {"rules_checked", rec.policy.rulesChecked}};
    j["provider_consulted"] = rec.providerConsulted;
    j["provider"] = resultToJson(rec.provider);
    j["recommendation"] = answersToJson(rec.recommendation);
    j["decided_by"] = rec.decidedBy;
    j["eligibility"] = {{"eligible", rec.eligibility.eligible}, {"reason", rec.eligibility.reason}};
    j["executed"] = rec.executed;
    j["operator_action"] = rec.operatorAction;
    j["operator_action_wall_clock_ns"] = rec.operatorActionWallClockNs;
    j["outcome_label"] = rec.outcomeLabel;
    return j.dump();
}

bool recordFromJson(const std::string& text, DecisionRecord& out, std::string& error)
{
    try {
        const json j = json::parse(text);
        if (!j.is_object()) { error = "record is not an object"; return false; }
        DecisionRecord r;
        r.recordSchemaVersion = j.value("record_schema_version", 0u);
        if (r.recordSchemaVersion != kDecisionRecordSchemaVersion) {
            error = "unsupported record_schema_version " + std::to_string(r.recordSchemaVersion);
            return false;
        }
        r.sequence = j.value("sequence", 0ULL);
        r.wallClockNs = j.value("wall_clock_ns", 0ULL);
        r.hostTimeUs = j.value("host_time_us", 0ULL);
        r.mode = j.value("mode", "shadow");
        r.runId = j.value("run_id", "");
        r.snapshotHash = j.value("snapshot_hash", "");
        r.snapshotSchemaVersion = j.value("snapshot_schema_version", kExperimentSnapshotSchemaVersion);
        if (!j.contains("snapshot")) { error = "record has no snapshot"; return false; }
        if (!snapshotFromJson(j.at("snapshot").dump(), r.snapshot, error)) return false;
        if (j.contains("policy")) {
            const auto& p = j.at("policy");
            r.policy.requiresAction = p.value("requires_action", false);
            r.policy.ruleId = p.value("rule_id", "");
            r.policy.reason = p.value("reason", "");
            if (p.contains("answers") && !answersFromJson(p.at("answers"), r.policy.answers, error)) return false;
            if (p.contains("rules_checked")) r.policy.rulesChecked = p.at("rules_checked").get<std::vector<std::string>>();
        }
        r.providerConsulted = j.value("provider_consulted", false);
        if (j.contains("provider") && !resultFromJson(j.at("provider"), r.provider, error)) return false;
        if (j.contains("recommendation") && !answersFromJson(j.at("recommendation"), r.recommendation, error)) return false;
        r.decidedBy = j.value("decided_by", "");
        if (j.contains("eligibility")) {
            r.eligibility.eligible = j.at("eligibility").value("eligible", false);
            r.eligibility.reason = j.at("eligibility").value("reason", "");
        }
        r.executed = j.value("executed", false);
        r.operatorAction = j.value("operator_action", "");
        r.operatorActionWallClockNs = j.value("operator_action_wall_clock_ns", 0ULL);
        r.outcomeLabel = j.value("outcome_label", "");
        out = std::move(r);
        return true;
    } catch (const std::exception& e) {
        error = std::string("record parse error: ") + e.what();
        return false;
    }
}

// ---- DecisionLog -------------------------------------------------------------

DecisionLog::~DecisionLog() { close(); }

bool DecisionLog::open(const std::string& path, const std::string& runId, SupervisorMode mode,
                       const std::string& providerName, const std::string& providerVersion,
                       const std::string& modelVersion, std::string* error)
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (file_) {
        std::fclose(static_cast<FILE*>(file_));
        file_ = nullptr;
    }
    FILE* f = std::fopen(path.c_str(), "ab");
    if (!f) {
        if (error) *error = "cannot open supervisor log '" + path + "'";
        return false;
    }
    json header;
    header["supervisor_log"] = 1;
    header["record_schema_version"] = kDecisionRecordSchemaVersion;
    header["snapshot_schema_version"] = kExperimentSnapshotSchemaVersion;
    header["contract_version"] = kDecisionContractVersion;
    header["run_id"] = runId;
    header["mode"] = toString(mode);
    header["provider_name"] = providerName;
    header["provider_version"] = providerVersion;
    header["model_version"] = modelVersion;
    header["executed"] = false;
    header["note"] = "shadow-mode supervisor sidecar: recommendations only, nothing was executed";
    const std::string line = header.dump() + "\n";
    if (std::fwrite(line.data(), 1, line.size(), f) != line.size() || std::fflush(f) != 0) {
        std::fclose(f);
        if (error) *error = "cannot write supervisor log header to '" + path + "'";
        return false;
    }
    file_ = f;
    path_ = path;
    written_ = 0;
    return true;
}

void DecisionLog::close()
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (file_) {
        std::fclose(static_cast<FILE*>(file_));
        file_ = nullptr;
    }
}

bool DecisionLog::isOpen() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return file_ != nullptr;
}

std::string DecisionLog::path() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return path_;
}

bool DecisionLog::append(const DecisionRecord& record, std::string* error)
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (!file_) {
        if (error) *error = "supervisor log is not open";
        return false;
    }
    const std::string line = recordToJson(record) + "\n";
    FILE* f = static_cast<FILE*>(file_);
    if (std::fwrite(line.data(), 1, line.size(), f) != line.size() || std::fflush(f) != 0) {
        if (error) *error = "supervisor log write failed";
        SPDLOG_WARN("Supervisor: decision log write failed ({})", path_);
        return false;
    }
    ++written_;
    return true;
}

uint64_t DecisionLog::recordsWritten() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return written_;
}

bool DecisionLog::readAll(const std::string& path, std::vector<DecisionRecord>& out, std::string& error)
{
    std::ifstream in(path);
    if (!in) { error = "cannot read '" + path + "'"; return false; }
    std::string line;
    size_t lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        if (line.empty()) continue;
        if (lineNo == 1) {
            try {
                const json h = json::parse(line);
                if (h.is_object() && h.contains("supervisor_log")) continue;
            } catch (...) {
            }
        }
        DecisionRecord r;
        std::string err;
        if (!recordFromJson(line, r, err)) {
            error = "line " + std::to_string(lineNo) + ": " + err;
            return false;
        }
        out.push_back(std::move(r));
    }
    return true;
}

} // namespace backend::supervisor
