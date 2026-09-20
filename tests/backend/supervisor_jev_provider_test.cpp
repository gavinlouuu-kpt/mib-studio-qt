// supervisor_jev_provider_test — JEV adapter over a fake HTTP transport
// (issue #422, phase C). Network-free; the live path is opt-in via the
// supervisor_eval tool.
//
//   1. Request: pinned model, contract version, closed vocabularies, the
//      canonical snapshot, and no label/expected field. Credential only in
//      the Authorization header, read from the configured env var at call
//      time.
//   2. Response validation: valid -> answers + distributions + confidence +
//      cost + truncated rationale; unknown token / extra question / bad
//      probability / model mismatch / not JSON -> typed error, fail-closed
//      answers, never partial.
//   3. Errors: 401 no retry, 429/5xx/timeout retried once, non-retryable 4xx.
//   4. Missing credential / no transport -> NotConfigured without any HTTP
//      call; the credential never appears in any result or record JSON.
//   5. cancel() aborts the retry backoff.

#include "backend/supervisor/DecisionRecord.h"
#include "backend/supervisor/JevProvider.h"
#include "backend/supervisor/SupervisorService.h"
#include "support/assert.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

using namespace backend::supervisor;
using json = nlohmann::json;

namespace {

void setEnv(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void unsetEnv(const char* name)
{
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

constexpr const char* kSecret = "sk-test-SECRET-1234567890";

ExperimentSnapshot snapshot()
{
    ExperimentSnapshot s;
    s.sequence = 3;
    s.runId = "jev-run";
    s.experimentState = "active";
    s.elapsedSeconds = 30.0;
    s.acquisition.cameraReady = true;
    s.acquisition.lastFailure = "none";
    s.detection.framesProcessed = Metric::of(15000);
    s.detection.validObjects = Metric::of(100);
    return s;
}

JevConfig config()
{
    JevConfig c;
    c.endpoint = "https://jev.example/v1/decide";
    c.model = "jev-2026-09-pinned";
    c.apiKeyEnvVar = "MIB_JEV_TEST_KEY";
    c.timeoutMs = 250;
    c.maxRetries = 1;
    c.maxRationaleChars = 16;
    return c;
}

std::string goodBody(const char* model = "jev-2026-09-pinned")
{
    json j;
    j["model"] = model;
    j["answers"] = {{"run_quality", "MARGINAL"}, {"primary_problem", "LOW_CONTRAST"}, {"next_action", "ADJUST"},
                    {"adjustment_target", "EXPOSURE"}, {"adjustment_direction", "INCREASE"}};
    j["distributions"] = {{"next_action", {{"ADJUST", 0.7}, {"CONTINUE", 0.2}, {"HUMAN_REVIEW", 0.1}}}};
    j["confidence"] = 0.66;
    j["cost_usd"] = 0.0021;
    j["rationale"] = "contrast is low so raise exposure a little bit more than before";
    return j.dump();
}

struct FakeTransport {
    std::vector<HttpPostResult> script;
    std::vector<HttpPostRequest> seen;
    size_t next{0};
    HttpPostFn fn()
    {
        return [this](const HttpPostRequest& r) {
            seen.push_back(r);
            if (next < script.size()) return script[next++];
            HttpPostResult res;
            res.ok = true;
            res.status = 200;
            res.body = goodBody();
            return res;
        };
    }
};

HttpPostResult http(long status, std::string body = {})
{
    HttpPostResult r;
    r.ok = true;
    r.status = status;
    r.body = std::move(body);
    return r;
}

HttpPostResult timeout()
{
    HttpPostResult r;
    r.ok = false;
    r.timedOut = true;
    r.error = "timed out";
    return r;
}

void testRequest()
{
    setEnv("MIB_JEV_TEST_KEY", kSecret);
    FakeTransport t;
    JevProvider p(config(), t.fn());
    DecisionPolicy policy;
    policy.providerTimeoutMs = 100; // stricter than config -> wins
    const auto r = p.evaluate(snapshot(), policy);
    MIB_REQUIRE(r.ok(), std::string("good response ok: ") + r.error.message);
    MIB_REQUIRE(t.seen.size() == 1, "one request");
    const auto& req = t.seen[0];
    MIB_EXPECT(req.url == "https://jev.example/v1/decide", "endpoint");
    MIB_EXPECT(req.timeoutMs == 100, "policy timeout applied");
    MIB_EXPECT(req.headers.at("Authorization") == std::string("Bearer ") + kSecret, "bearer header");
    MIB_EXPECT(req.headers.at("Content-Type") == "application/json", "content type");
    const json body = json::parse(req.body);
    MIB_EXPECT(body["model"] == "jev-2026-09-pinned", "pinned model in request");
    MIB_EXPECT(body["contract_version"] == kDecisionContractVersion, "contract version in request");
    MIB_EXPECT(body["questions"]["next_action"].size() == 5, "closed vocabulary sent");
    MIB_EXPECT(body["snapshot"]["run_id"] == "jev-run", "canonical snapshot embedded");
    MIB_EXPECT(!body.contains("expected") && !body["snapshot"].contains("expected"), "no label leak");
    MIB_EXPECT(req.body.find(kSecret) == std::string::npos, "credential not in body");
    MIB_EXPECT(r.answers.action == NextAction::Adjust && r.answers.target == AdjustmentTarget::Exposure &&
                   r.answers.direction == AdjustmentDirection::Increase && r.answers.problem == PrimaryProblem::LowContrast,
               "answers parsed");
    MIB_EXPECT(selectedProbability(r.distributions, kQuestionNextAction, "ADJUST").value_or(-1) == 0.7, "distribution kept");
    MIB_EXPECT(r.confidence.value_or(-1) == 0.66, "confidence kept");
    MIB_EXPECT(r.costUsd.value_or(-1) == 0.0021, "cost kept");
    MIB_EXPECT(r.rationale.size() == 16, "rationale truncated to the configured bound");
    MIB_EXPECT(r.modelVersion == "jev-2026-09-pinned" && r.providerVersion == "jev-adapter/1", "versions pinned");
    MIB_EXPECT(r.attempts == 1 && r.latencyUs > 0, "attempt and latency recorded");
    // The credential must not leak into any serialized artifact.
    DecisionRecord rec = decideOnce(snapshot(), policy, &p, SupervisorMode::Shadow);
    MIB_EXPECT(recordToJson(rec).find(kSecret) == std::string::npos, "credential not in record JSON");
    MIB_EXPECT(rec.decidedBy == "provider" && rec.eligibility.eligible, "coherent ADJUST is eligible (recorded, not executed)");
    MIB_EXPECT(!rec.executed, "not executed");
}

void expectSchemaError(const std::string& body, ProviderErrorKind kind, const char* what)
{
    FakeTransport t;
    t.script = {http(200, body)};
    JevProvider p(config(), t.fn());
    const auto r = p.evaluate(snapshot(), DecisionPolicy{});
    MIB_EXPECT(!r.ok(), std::string("rejected: ") + what);
    MIB_EXPECT(r.error.kind == kind, std::string("kind for ") + what + " = " + toString(r.error.kind));
    MIB_EXPECT(r.answers == humanReviewAnswers(), std::string("fail-closed answers for ") + what);
    MIB_EXPECT(r.distributions.empty() && !r.confidence, std::string("no partial payload for ") + what);
    MIB_EXPECT(t.seen.size() == 1, std::string("schema errors are not retried: ") + what);
}

void testValidation()
{
    setEnv("MIB_JEV_TEST_KEY", kSecret);
    expectSchemaError("not json at all", ProviderErrorKind::MalformedResponse, "not JSON");
    expectSchemaError("[1,2,3]", ProviderErrorKind::MalformedResponse, "array");
    expectSchemaError("{}", ProviderErrorKind::SchemaViolation, "missing answers");
    {
        json j = json::parse(goodBody());
        j["answers"]["next_action"] = "REBOOT";
        expectSchemaError(j.dump(), ProviderErrorKind::SchemaViolation, "unknown action token");
    }
    {
        json j = json::parse(goodBody());
        j["answers"].erase("adjustment_direction");
        expectSchemaError(j.dump(), ProviderErrorKind::SchemaViolation, "missing answer");
    }
    {
        json j = json::parse(goodBody());
        j["answers"]["exposure_us"] = "1200";
        expectSchemaError(j.dump(), ProviderErrorKind::SchemaViolation, "extra free-form answer");
    }
    {
        json j = json::parse(goodBody());
        j["distributions"]["next_action"]["ADJUST"] = 1.5;
        expectSchemaError(j.dump(), ProviderErrorKind::SchemaViolation, "probability > 1");
    }
    {
        json j = json::parse(goodBody());
        j["distributions"]["next_action"]["FIRE"] = 0.1;
        expectSchemaError(j.dump(), ProviderErrorKind::SchemaViolation, "unknown token in distribution");
    }
    {
        json j = json::parse(goodBody());
        j["confidence"] = "high";
        expectSchemaError(j.dump(), ProviderErrorKind::SchemaViolation, "non-numeric confidence");
    }
    expectSchemaError(goodBody("jev-latest"), ProviderErrorKind::ModelMismatch, "unpinned model alias");
    {
        // Optional fields may be absent.
        json j;
        j["answers"] = json::parse(goodBody())["answers"];
        FakeTransport t;
        t.script = {http(200, j.dump())};
        JevProvider p(config(), t.fn());
        const auto r = p.evaluate(snapshot(), DecisionPolicy{});
        MIB_EXPECT(r.ok() && r.distributions.empty() && !r.confidence && r.modelVersion == "jev-2026-09-pinned",
                   "minimal valid response accepted");
    }
}

void testHttpErrors()
{
    setEnv("MIB_JEV_TEST_KEY", kSecret);
    DecisionPolicy policy;
    policy.providerMaxRetries = 1;
    {
        FakeTransport t;
        t.script = {http(401, "{}")};
        JevProvider p(config(), t.fn());
        const auto r = p.evaluate(snapshot(), policy);
        MIB_EXPECT(!r.ok() && r.error.kind == ProviderErrorKind::Authentication, "401 -> authentication");
        MIB_EXPECT(t.seen.size() == 1, "auth failure not retried");
        MIB_EXPECT(r.error.message.find(kSecret) == std::string::npos, "credential not in error text");
    }
    {
        FakeTransport t;
        t.script = {http(500, "oops")};
        JevProvider p(config(), t.fn());
        const auto r = p.evaluate(snapshot(), policy);
        MIB_EXPECT(r.ok() && r.attempts == 2 && t.seen.size() == 2, "5xx retried once then succeeded");
    }
    {
        FakeTransport t;
        t.script = {http(429, ""), http(429, "")};
        JevProvider p(config(), t.fn());
        const auto r = p.evaluate(snapshot(), policy);
        MIB_EXPECT(!r.ok() && r.error.kind == ProviderErrorKind::RateLimited && t.seen.size() == 2, "429 twice -> rate limited after one retry");
    }
    {
        FakeTransport t;
        t.script = {timeout(), timeout()};
        JevProvider p(config(), t.fn());
        const auto r = p.evaluate(snapshot(), policy);
        MIB_EXPECT(!r.ok() && r.error.kind == ProviderErrorKind::Timeout && r.attempts == 2, "timeout retried once");
        MIB_EXPECT(r.answers.action == NextAction::HumanReview, "timeout -> HUMAN_REVIEW");
    }
    {
        FakeTransport t;
        t.script = {http(404, "")};
        JevProvider p(config(), t.fn());
        const auto r = p.evaluate(snapshot(), policy);
        MIB_EXPECT(!r.ok() && r.error.kind == ProviderErrorKind::Transport && t.seen.size() == 1, "404 not retried");
    }
    {
        DecisionPolicy noRetry = policy;
        noRetry.providerMaxRetries = 0;
        FakeTransport t;
        t.script = {timeout()};
        JevProvider p(config(), t.fn());
        const auto r = p.evaluate(snapshot(), noRetry);
        MIB_EXPECT(!r.ok() && t.seen.size() == 1, "policy retries=0 respected");
    }
    {
        // Transport that throws is contained.
        JevProvider p(config(), [](const HttpPostRequest&) -> HttpPostResult { throw std::runtime_error("socket"); });
        const auto r = p.evaluate(snapshot(), policy);
        MIB_EXPECT(!r.ok() && r.error.kind == ProviderErrorKind::Transport, "throwing transport -> transport error");
    }
    {
        // cancel() before/while evaluating: no HTTP call is made, the result
        // is Cancelled, and resetCancel() (called by the service on start)
        // restores normal operation.
        FakeTransport t;
        t.script = {http(503, ""), http(503, "")};
        JevProvider p(config(), t.fn());
        p.cancel();
        const auto r = p.evaluate(snapshot(), policy);
        MIB_EXPECT(!r.ok() && r.error.kind == ProviderErrorKind::Cancelled && t.seen.empty(), "cancelled before the first attempt");
        p.resetCancel();
        const auto r2 = p.evaluate(snapshot(), policy);
        MIB_EXPECT(!r2.ok() && r2.error.kind == ProviderErrorKind::Transport && t.seen.size() == 2, "after reset: 503 retried once then fails");
    }
}

void testNotConfigured()
{
    unsetEnv("MIB_JEV_TEST_KEY");
    {
        FakeTransport t;
        JevProvider p(config(), t.fn());
        const auto r = p.evaluate(snapshot(), DecisionPolicy{});
        MIB_EXPECT(!r.ok() && r.error.kind == ProviderErrorKind::NotConfigured, "missing credential -> not configured");
        MIB_EXPECT(t.seen.empty(), "no HTTP call without a credential");
        MIB_EXPECT(r.error.message.find("MIB_JEV_TEST_KEY") != std::string::npos, "error names the env var, not its value");
    }
    setEnv("MIB_JEV_TEST_KEY", kSecret);
    {
        JevProvider p(config(), {});
        MIB_EXPECT(!p.hasTransport(), "no transport");
        const auto r = p.evaluate(snapshot(), DecisionPolicy{});
        MIB_EXPECT(!r.ok() && r.error.kind == ProviderErrorKind::NotConfigured, "no transport -> not configured");
    }
    {
        JevConfig c = config();
        c.model.clear();
        FakeTransport t;
        JevProvider p(c, t.fn());
        const auto r = p.evaluate(snapshot(), DecisionPolicy{});
        MIB_EXPECT(!r.ok() && r.error.kind == ProviderErrorKind::NotConfigured && t.seen.empty(), "unpinned model -> not configured");
    }
    // Environment config helper.
    unsetEnv(kJevEndpointEnvVar);
    unsetEnv(kJevModelEnvVar);
    MIB_EXPECT(!jevConfigFromEnvironment(), "no env -> nullopt");
    setEnv(kJevEndpointEnvVar, "https://jev.example/v1");
    MIB_EXPECT(!jevConfigFromEnvironment(), "endpoint without model -> nullopt");
    setEnv(kJevModelEnvVar, "jev-x");
    setEnv("MIB_JEV_TIMEOUT_MS", "2500");
    const auto c = jevConfigFromEnvironment();
    MIB_EXPECT(c && c->endpoint == "https://jev.example/v1" && c->model == "jev-x" && c->timeoutMs == 2500, "env config parsed");
    unsetEnv(kJevEndpointEnvVar);
    unsetEnv(kJevModelEnvVar);
    unsetEnv("MIB_JEV_TIMEOUT_MS");
}

} // namespace

int main()
{
    testRequest();
    testValidation();
    testHttpErrors();
    testNotConfigured();
    std::printf("supervisor_jev_provider_test: %s\n", mib::test::exitCode() == 0 ? "OK" : "FAILED");
    return mib::test::exitCode();
}
