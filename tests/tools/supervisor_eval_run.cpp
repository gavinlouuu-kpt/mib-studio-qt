// supervisor_eval_run — offline / opt-in live evaluation of AI Experiment
// Supervisor decision providers over the labelled fixtures (issue #422).
//
//   supervisor_eval_run --fixtures <dir> [--provider rule|jev] [--out report.json]
//                       [--text report.txt] [--calibration 0.5,0.6,...]
//                       [--min-frames N] [--loss-limit F] [--fail-on-unsafe]
//
// `--provider rule` (default) is deterministic and network-free: CI runs it.
// `--provider jev` is opt-in: it needs MIB_JEV_ENDPOINT, MIB_JEV_MODEL and the
// credential in the env var named by MIB_JEV_API_KEY_ENV (default
// MIB_JEV_API_KEY), plus a libcurl build of this tool (MIB_SUPERVISOR_EVAL_CURL).
// The backend itself has no HTTP client (ADR 0002); the transport lives here.
//
// Exit code: 0 on success, 2 usage, 3 fixtures unreadable, 4 provider not
// configured, 5 when --fail-on-unsafe is set and unsafe disagreements exist.

#include "backend/supervisor/EvaluationHarness.h"
#include "backend/supervisor/JevProvider.h"
#include "backend/supervisor/RuleProvider.h"

#if defined(MIB_SUPERVISOR_EVAL_CURL) && MIB_SUPERVISOR_EVAL_CURL
#include <curl/curl.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace backend::supervisor;

namespace {

void usage()
{
    std::fprintf(stderr,
                 "usage: supervisor_eval_run --fixtures <dir> [--provider rule|jev] [--out report.json]\n"
                 "                           [--text report.txt] [--calibration 0.5,0.7,0.9]\n"
                 "                           [--min-frames N] [--loss-limit F] [--fail-on-unsafe]\n");
}

#if defined(MIB_SUPERVISOR_EVAL_CURL) && MIB_SUPERVISOR_EVAL_CURL
size_t writeBody(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

HttpPostResult curlPost(const HttpPostRequest& req)
{
    HttpPostResult res;
    CURL* curl = curl_easy_init();
    if (!curl) { res.error = "curl_easy_init failed"; return res; }
    struct curl_slist* headers = nullptr;
    for (const auto& [k, v] : req.headers) headers = curl_slist_append(headers, (k + ": " + v).c_str());
    curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(req.timeoutMs));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &res.body);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    const CURLcode code = curl_easy_perform(curl);
    if (code == CURLE_OK) {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        res.ok = true;
        res.status = status;
    } else {
        res.ok = false;
        res.timedOut = code == CURLE_OPERATION_TIMEDOUT;
        res.error = curl_easy_strerror(code); // never includes headers
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return res;
}
#endif

std::vector<double> parseThresholds(const std::string& csv)
{
    std::vector<double> out;
    std::stringstream ss(csv);
    std::string tok;
    while (std::getline(ss, tok, ',')) if (!tok.empty()) out.push_back(std::atof(tok.c_str()));
    return out;
}

bool writeFile(const std::string& path, const std::string& content)
{
    std::ofstream out(path);
    if (!out) return false;
    out << content;
    return static_cast<bool>(out);
}

} // namespace

int main(int argc, char** argv)
{
    std::string fixtures, providerName = "rule", outJson, outText, calibration;
    bool failOnUnsafe = false;
    EvaluationOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&](std::string& dst) { if (i + 1 >= argc) { usage(); std::exit(2); } dst = argv[++i]; };
        if (a == "--fixtures") value(fixtures);
        else if (a == "--provider") value(providerName);
        else if (a == "--out") value(outJson);
        else if (a == "--text") value(outText);
        else if (a == "--calibration") value(calibration);
        else if (a == "--min-frames") { std::string v; value(v); options.policy.minFramesForModelDecision = std::strtoull(v.c_str(), nullptr, 10); }
        else if (a == "--loss-limit") { std::string v; value(v); options.policy.hardFrameLossFraction = std::atof(v.c_str()); }
        else if (a == "--fail-on-unsafe") failOnUnsafe = true;
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown argument %s\n", a.c_str()); usage(); return 2; }
    }
    if (fixtures.empty()) { usage(); return 2; }
    if (!calibration.empty()) options.calibrationThresholds = parseThresholds(calibration);

    std::vector<LabelledCase> cases;
    std::string err;
    if (!loadLabelledCases(fixtures, cases, err)) {
        std::fprintf(stderr, "cannot load fixtures: %s\n", err.c_str());
        return 3;
    }
    std::unique_ptr<DecisionProvider> provider;
    if (providerName == "rule") {
        provider = std::make_unique<RuleProvider>();
    } else if (providerName == "jev") {
        const auto cfg = jevConfigFromEnvironment();
        if (!cfg) {
            std::fprintf(stderr, "JEV provider not configured: set %s and %s (and the credential in %s)\n",
                         kJevEndpointEnvVar, kJevModelEnvVar, "MIB_JEV_API_KEY");
            return 4;
        }
        HttpPostFn post;
#if defined(MIB_SUPERVISOR_EVAL_CURL) && MIB_SUPERVISOR_EVAL_CURL
        curl_global_init(CURL_GLOBAL_DEFAULT);
        post = curlPost;
#else
        std::fprintf(stderr, "this build of supervisor_eval_run has no HTTP transport (libcurl not found at configure time)\n");
        return 4;
#endif
        provider = std::make_unique<JevProvider>(*cfg, post);
        std::printf("live JEV evaluation: endpoint=%s model=%s timeout=%d ms (credential from %s; not printed)\n",
                    cfg->endpoint.c_str(), cfg->model.c_str(), cfg->timeoutMs, cfg->apiKeyEnvVar.c_str());
    } else {
        std::fprintf(stderr, "unknown provider '%s' (rule|jev)\n", providerName.c_str());
        return 2;
    }

    const EvaluationReport report = runEvaluation(cases, *provider, options);
    const std::string text = reportToText(report);
    std::fputs(text.c_str(), stdout);
    if (!outJson.empty() && !writeFile(outJson, reportToJson(report))) {
        std::fprintf(stderr, "cannot write %s\n", outJson.c_str());
        return 3;
    }
    if (!outText.empty() && !writeFile(outText, text)) {
        std::fprintf(stderr, "cannot write %s\n", outText.c_str());
        return 3;
    }
#if defined(MIB_SUPERVISOR_EVAL_CURL) && MIB_SUPERVISOR_EVAL_CURL
    if (providerName == "jev") curl_global_cleanup();
#endif
    if (failOnUnsafe && report.unsafeDisagreements > 0) return 5;
    return 0;
}
