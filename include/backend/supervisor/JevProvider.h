// AI Experiment Supervisor — JEV decision provider (issue #422, phase C).
//
// Talks to the JEV decision service over an injected HTTP POST seam
// (ADR 0002 pattern: the backend has no HTTP client; the shell or the
// evaluation tool supplies one). The adapter builds the typed request
// (contract version, pinned model, closed answer vocabularies, the canonical
// snapshot JSON), validates the response strictly against the local
// contract, and maps every failure to a ProviderError. Unknown enum tokens,
// extra answers, out-of-range probabilities and a model other than the pinned
// one are rejected — the result then carries the fail-closed HumanReview
// answers with error set, never a partial decision.
//
// Credential: read from the environment variable named in the config at call
// time, sent only in the Authorization header, and never copied into results,
// records, logs or error messages.
#pragma once

#include "backend/supervisor/DecisionProvider.h"

#include <atomic>
#include <functional>
#include <map>
#include <string>

namespace backend::supervisor {

struct HttpPostRequest {
    std::string url;
    std::map<std::string, std::string> headers; // includes Authorization
    std::string body;                           // application/json
    int timeoutMs{10000};
};

struct HttpPostResult {
    bool ok{false};        // transport-level success (a response was received)
    long status{0};        // HTTP status when ok
    std::string body;
    std::string error;     // transport error text when !ok
    bool timedOut{false};
};

using HttpPostFn = std::function<HttpPostResult(const HttpPostRequest&)>;

struct JevConfig {
    std::string endpoint;                    // e.g. https://jev.example/v1/decide
    std::string model;                       // pinned model identifier (required)
    std::string apiKeyEnvVar{"MIB_JEV_API_KEY"};
    std::string adapterVersion{"jev-adapter/1"};
    int timeoutMs{10000};
    int maxRetries{1};                       // retried only on timeout / 5xx / 429
    std::size_t maxRationaleChars{512};
};

inline constexpr const char* kJevEndpointEnvVar = "MIB_JEV_ENDPOINT";
inline constexpr const char* kJevModelEnvVar = "MIB_JEV_MODEL";

// Reads endpoint/model/timeout from the environment (MIB_JEV_ENDPOINT,
// MIB_JEV_MODEL, MIB_JEV_TIMEOUT_MS). Returns nullopt when the endpoint or
// model is missing — the provider is then simply not configured.
std::optional<JevConfig> jevConfigFromEnvironment();

class JevProvider final : public DecisionProvider {
public:
    JevProvider(JevConfig config, HttpPostFn post);

    std::string name() const override { return "jev"; }
    std::string version() const override { return config_.adapterVersion; }
    std::string modelVersion() const override { return config_.model; }

    DecisionResult evaluate(const ExperimentSnapshot& snapshot,
                            const DecisionPolicy& policy) override;
    void cancel() override { cancelled_.store(true, std::memory_order_release); }
    void resetCancel() override { cancelled_.store(false, std::memory_order_release); }

    const JevConfig& config() const { return config_; }
    bool hasTransport() const { return static_cast<bool>(post_); }

    // Request body for a snapshot (exposed for tests and the docs).
    static std::string buildRequestBody(const JevConfig& config, const ExperimentSnapshot& snapshot);
    // Strict response validation. On success fills answers/distributions/
    // confidence/rationale/cost; on failure returns the error.
    static ProviderError parseResponse(const JevConfig& config, const std::string& body,
                                       DecisionResult& out);

private:
    JevConfig config_;
    HttpPostFn post_;
    std::atomic<bool> cancelled_{false};
};

} // namespace backend::supervisor
