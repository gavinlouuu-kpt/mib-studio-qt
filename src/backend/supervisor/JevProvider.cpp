#include "backend/supervisor/JevProvider.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <thread>

namespace backend::supervisor {

using json = nlohmann::json;

std::optional<JevConfig> jevConfigFromEnvironment()
{
    const char* endpoint = std::getenv(kJevEndpointEnvVar);
    const char* model = std::getenv(kJevModelEnvVar);
    if (!endpoint || !*endpoint || !model || !*model) return std::nullopt;
    JevConfig c;
    c.endpoint = endpoint;
    c.model = model;
    if (const char* t = std::getenv("MIB_JEV_TIMEOUT_MS")) {
        const int v = std::atoi(t);
        if (v > 0 && v <= 120000) c.timeoutMs = v;
    }
    if (const char* k = std::getenv("MIB_JEV_API_KEY_ENV")) {
        if (*k) c.apiKeyEnvVar = k;
    }
    return c;
}

JevProvider::JevProvider(JevConfig config, HttpPostFn post)
    : config_(std::move(config)), post_(std::move(post))
{
}

std::string JevProvider::buildRequestBody(const JevConfig& config, const ExperimentSnapshot& snapshot)
{
    json j;
    j["contract_version"] = kDecisionContractVersion;
    j["adapter_version"] = config.adapterVersion;
    j["model"] = config.model;
    j["task"] = "experiment_supervisor_shadow";
    j["instructions"] =
        "You supervise a deformability-cytometry acquisition. Answer each question with exactly one "
        "token from its allowed list. Do not propose numeric hardware values. Recommendations are "
        "advisory only and are never executed automatically.";
    json questions = json::object();
    for (const auto& q : questionIds()) questions[q] = tokensFor(q);
    j["questions"] = questions;
    j["response_schema"] = {
        {"model", "string (must equal request.model)"},
        {"answers", "object: question id -> token"},
        {"distributions", "optional object: question id -> {token: probability}"},
        {"confidence", "optional number in [0,1]"},
        {"rationale", "optional short string"},
        {"cost_usd", "optional number"},
    };
    j["snapshot"] = json::parse(snapshotToJson(snapshot));
    return j.dump();
}

ProviderError JevProvider::parseResponse(const JevConfig& config, const std::string& body, DecisionResult& out)
{
    json j;
    try {
        j = json::parse(body);
    } catch (const std::exception& e) {
        return {ProviderErrorKind::MalformedResponse, std::string("response is not JSON: ") + e.what()};
    }
    if (!j.is_object()) return {ProviderErrorKind::MalformedResponse, "response is not a JSON object"};

    if (j.contains("model")) {
        if (!j.at("model").is_string()) return {ProviderErrorKind::SchemaViolation, "'model' is not a string"};
        const auto m = j.at("model").get<std::string>();
        if (m != config.model) {
            return {ProviderErrorKind::ModelMismatch, "response model '" + m + "' != pinned '" + config.model + "'"};
        }
        out.modelVersion = m;
    } else {
        out.modelVersion = config.model;
    }

    if (!j.contains("answers") || !j.at("answers").is_object()) {
        return {ProviderErrorKind::SchemaViolation, "missing 'answers' object"};
    }
    const auto& answers = j.at("answers");
    for (auto it = answers.begin(); it != answers.end(); ++it) {
        if (tokensFor(it.key()).empty()) {
            return {ProviderErrorKind::SchemaViolation, "unknown question '" + it.key() + "'"};
        }
    }
    auto token = [&](const char* q, std::string& out) -> std::optional<ProviderError> {
        if (!answers.contains(q) || !answers.at(q).is_string()) {
            return ProviderError{ProviderErrorKind::SchemaViolation, std::string("missing answer '") + q + "'"};
        }
        out = answers.at(q).get<std::string>();
        return std::nullopt;
    };
    std::string q, p, a, t, d;
    if (auto e = token(kQuestionRunQuality, q)) return *e;
    if (auto e = token(kQuestionPrimaryProblem, p)) return *e;
    if (auto e = token(kQuestionNextAction, a)) return *e;
    if (auto e = token(kQuestionAdjustmentTarget, t)) return *e;
    if (auto e = token(kQuestionAdjustmentDirection, d)) return *e;
    const auto pq = parseRunQuality(q);
    const auto pp = parsePrimaryProblem(p);
    const auto pa = parseNextAction(a);
    const auto pt = parseAdjustmentTarget(t);
    const auto pd = parseAdjustmentDirection(d);
    if (!pq) return {ProviderErrorKind::SchemaViolation, "unknown run_quality token '" + q + "'"};
    if (!pp) return {ProviderErrorKind::SchemaViolation, "unknown primary_problem token '" + p + "'"};
    if (!pa) return {ProviderErrorKind::SchemaViolation, "unknown next_action token '" + a + "'"};
    if (!pt) return {ProviderErrorKind::SchemaViolation, "unknown adjustment_target token '" + t + "'"};
    if (!pd) return {ProviderErrorKind::SchemaViolation, "unknown adjustment_direction token '" + d + "'"};

    Distributions dists;
    if (j.contains("distributions") && !j.at("distributions").is_null()) {
        if (!j.at("distributions").is_object()) return {ProviderErrorKind::SchemaViolation, "'distributions' is not an object"};
        for (auto it = j.at("distributions").begin(); it != j.at("distributions").end(); ++it) {
            const auto& allowed = tokensFor(it.key());
            if (allowed.empty()) return {ProviderErrorKind::SchemaViolation, "distribution for unknown question '" + it.key() + "'"};
            if (!it.value().is_object()) return {ProviderErrorKind::SchemaViolation, "distribution '" + it.key() + "' is not an object"};
            Distribution dist;
            for (auto d2 = it.value().begin(); d2 != it.value().end(); ++d2) {
                if (std::find(allowed.begin(), allowed.end(), d2.key()) == allowed.end()) {
                    return {ProviderErrorKind::SchemaViolation, "distribution '" + it.key() + "' has unknown token '" + d2.key() + "'"};
                }
                if (!d2.value().is_number()) return {ProviderErrorKind::SchemaViolation, "probability is not a number"};
                const double v = d2.value().get<double>();
                if (v < 0.0 || v > 1.0) return {ProviderErrorKind::SchemaViolation, "probability outside [0,1]"};
                dist[d2.key()] = v;
            }
            dists[it.key()] = std::move(dist);
        }
    }
    std::optional<double> confidence;
    if (j.contains("confidence") && !j.at("confidence").is_null()) {
        if (!j.at("confidence").is_number()) return {ProviderErrorKind::SchemaViolation, "'confidence' is not a number"};
        const double c = j.at("confidence").get<double>();
        if (c < 0.0 || c > 1.0) return {ProviderErrorKind::SchemaViolation, "'confidence' outside [0,1]"};
        confidence = c;
    }
    std::optional<double> cost;
    if (j.contains("cost_usd") && j.at("cost_usd").is_number()) cost = j.at("cost_usd").get<double>();
    std::string rationale;
    if (j.contains("rationale") && j.at("rationale").is_string()) {
        rationale = j.at("rationale").get<std::string>();
        if (rationale.size() > config.maxRationaleChars) rationale.resize(config.maxRationaleChars);
    }

    out.answers.quality = *pq; out.answers.problem = *pp; out.answers.action = *pa;
    out.answers.target = *pt; out.answers.direction = *pd;
    out.distributions = std::move(dists);
    out.confidence = confidence;
    out.costUsd = cost;
    out.rationale = std::move(rationale);
    return {};
}

DecisionResult JevProvider::evaluate(const ExperimentSnapshot& snapshot, const DecisionPolicy& policy)
{
    const auto t0 = std::chrono::steady_clock::now();
    DecisionResult r;
    r.providerName = name();
    r.providerVersion = version();
    r.modelVersion = config_.model;
    r.answers = humanReviewAnswers();
    auto finish = [&](ProviderError e) {
        r.error = std::move(e);
        if (!r.ok()) r.answers = humanReviewAnswers();
        r.latencyUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                std::chrono::steady_clock::now() - t0).count());
        return r;
    };

    if (config_.endpoint.empty() || config_.model.empty()) {
        return finish({ProviderErrorKind::NotConfigured, "JEV endpoint/model not configured"});
    }
    if (!post_) {
        return finish({ProviderErrorKind::NotConfigured, "no HTTP transport injected (shell must supply HttpPostFn)"});
    }
    const char* key = std::getenv(config_.apiKeyEnvVar.c_str());
    if (!key || !*key) {
        return finish({ProviderErrorKind::NotConfigured,
                       "credential environment variable '" + config_.apiKeyEnvVar + "' is not set"});
    }

    HttpPostRequest req;
    req.url = config_.endpoint;
    req.headers["Content-Type"] = "application/json";
    req.headers["Accept"] = "application/json";
    req.headers["Authorization"] = std::string("Bearer ") + key; // never logged/recorded
    req.body = buildRequestBody(config_, snapshot);
    const int timeoutMs = policy.providerTimeoutMs > 0 ? std::min(policy.providerTimeoutMs, config_.timeoutMs)
                                                       : config_.timeoutMs;
    req.timeoutMs = timeoutMs;
    const int maxAttempts = 1 + std::max(0, std::min(config_.maxRetries, policy.providerMaxRetries));

    ProviderError last;
    for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
        r.attempts = attempt;
        if (cancelled_.load(std::memory_order_acquire)) return finish({ProviderErrorKind::Cancelled, "cancelled"});
        HttpPostResult res;
        try {
            res = post_(req);
        } catch (const std::exception& e) {
            res.ok = false;
            res.error = std::string("transport threw: ") + e.what();
        }
        bool retryable = false;
        if (!res.ok) {
            last = res.timedOut ? ProviderError{ProviderErrorKind::Timeout, "request timed out after " +
                                                                             std::to_string(timeoutMs) + " ms"}
                                : ProviderError{ProviderErrorKind::Transport, res.error.empty() ? "transport failure" : res.error};
            retryable = true;
        } else if (res.status == 401 || res.status == 403) {
            last = {ProviderErrorKind::Authentication, "HTTP " + std::to_string(res.status)};
        } else if (res.status == 429) {
            last = {ProviderErrorKind::RateLimited, "HTTP 429"};
            retryable = true;
        } else if (res.status >= 500) {
            last = {ProviderErrorKind::Transport, "HTTP " + std::to_string(res.status)};
            retryable = true;
        } else if (res.status < 200 || res.status >= 300) {
            last = {ProviderErrorKind::Transport, "HTTP " + std::to_string(res.status)};
        } else {
            DecisionResult parsed = r;
            const ProviderError e = parseResponse(config_, res.body, parsed);
            if (e.empty()) {
                r = parsed;
                return finish({});
            }
            last = e;
        }
        if (!retryable || attempt == maxAttempts) break;
        // Bounded, short backoff; cancellable.
        for (int i = 0; i < 10 && !cancelled_.load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    SPDLOG_WARN("Supervisor: JEV provider failed ({}: {})", toString(last.kind), last.message);
    return finish(last);
}

} // namespace backend::supervisor
