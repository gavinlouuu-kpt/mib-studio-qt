#include "backend/app/MethodApply.h"

#include "backend/profiles/ProfileRegistryWorker.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace backend::app {
namespace {
using Json = nlohmann::json;
constexpr std::size_t kMaxListedChanges = 200;

void diff(const Json& a, const Json& b, const std::string& prefix, std::vector<std::string>& out) {
    if (out.size() >= kMaxListedChanges) return;
    if (a.is_object() && b.is_object()) {
        for (const auto& [key, value] : a.items()) {
            const auto path = prefix.empty() ? key : prefix + "." + key;
            if (!b.contains(key))
                out.push_back(path + " (removed)");
            else
                diff(value, b.at(key), path, out);
        }
        for (const auto& [key, value] : b.items())
            if (!a.contains(key)) out.push_back((prefix.empty() ? key : prefix + "." + key) + " (added)");
        return;
    }
    // Same normalization rule as the canonical hash: 2.0 == 2.
    const bool bothNumbers = a.is_number() && b.is_number();
    if (bothNumbers ? a.get<double>() != b.get<double>() : a != b)
        out.push_back(prefix.empty() ? "<entire document>" : prefix);
}

std::string readFile(const std::filesystem::path& path, bool& ok) {
    std::ifstream in(path, std::ios::binary);
    ok = static_cast<bool>(in);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}
} // namespace

std::vector<std::string> configDifferences(const std::string& currentJson, const std::string& nextJson) {
    std::vector<std::string> out;
    try {
        diff(Json::parse(currentJson), Json::parse(nextJson), {}, out);
    } catch (const Json::exception&) {
        out = {"<entire document>"};
    }
    if (out.size() >= kMaxListedChanges) out.push_back("... (more)");
    return out;
}

MethodApplyPlan planMethodApply(const profiles::RegistryWorkerSnapshot& registry,
                                const std::string& revisionId,
                                const std::string& currentConfigJson) {
    MethodApplyPlan plan;
    plan.revisionId = revisionId;
    const profiles::CachedRevisionSummary* revision = nullptr;
    for (const auto& r : registry.revisions)
        if (r.revisionId == revisionId) revision = &r;
    if (!revision) {
        plan.error = "Revision is not in the local cache";
        return plan;
    }
    plan.displayName = revision->displayName;
    plan.revisionNumber = revision->revisionNumber;
    plan.centralState = profiles::toString(revision->state);
    if (revision->state != profiles::CentralState::Published &&
        revision->state != profiles::CentralState::Superseded) {
        plan.error = "A " + plan.centralState + " revision cannot be applied";
        return plan;
    }
    if (revision->materializedDir.empty()) {
        plan.error = "Revision files are not materialized yet";
        return plan;
    }
    const std::filesystem::path dir(revision->materializedDir);
    bool readable = false;
    plan.configText = readFile(dir / "config.json", readable);
    if (!readable || revision->configSha256.empty() ||
        profiles::canonicalConfigSha256(plan.configText) != revision->configSha256) {
        plan.configText.clear();
        plan.error = "Materialized config.json no longer matches the revision; materialize it again";
        return plan;
    }
    plan.cameraScriptPath = (dir / "egrabberConfig.js").string();
    plan.changedKeys = configDifferences(currentConfigJson, plan.configText);
    plan.ok = true;
    return plan;
}

LocalValidationView localValidationFor(const profiles::RegistryWorkerSnapshot& registry,
                                       const profiles::CachedRevisionSummary& revision,
                                       const std::string& instrumentId,
                                       const std::string& contextHash) {
    LocalValidationView view;
    if (instrumentId.empty() || contextHash.empty()) return view;
    for (const auto& record : registry.validations) { // newest first
        const auto& v = record.validation;
        if (v.revisionId != revision.revisionId || v.instrumentId != instrumentId ||
            v.contextHash != contextHash || v.contentHash != revision.contentHash)
            continue;
        view.state = v.passed ? LocalValidationState::Passed : LocalValidationState::Failed;
        view.validatorId = v.validatorId;
        view.validatedAtUtc = record.validatedAtUtc;
        break;
    }
    return view;
}

std::string checkValidationEvidence(const std::string& runSnapshotJson, const std::string& revisionId,
                                    const std::string& contentHash, const std::string& instrumentId,
                                    const std::string& contextHash) {
    if (runSnapshotJson.empty())
        return "The file has no run provenance (not a run recorded by this software, or too old)";
    Json run;
    try {
        run = Json::parse(runSnapshotJson);
    } catch (const Json::exception&) {
        return "The file's run provenance is unreadable";
    }
    const auto method = run.find("method");
    if (method == run.end() || !method->is_object())
        return "The test run predates method provenance (run snapshot schema 1)";
    const auto field = [&](const char* key) { return method->value(key, std::string{}); };
    if (field("source") != "central" || field("revision_id") != revisionId)
        return "The test run was not recorded with this revision applied" +
               (field("revision_id").empty() ? std::string{} : " (it used " + field("revision_id") + ")");
    if (field("content_hash") != contentHash)
        return "The test run used different content for this revision";
    if (instrumentId.empty() || field("instrument_id") != instrumentId)
        return "The test run was recorded on a different instrument";
    if (contextHash.empty() || field("context_hash") != contextHash)
        return "The test run used a different processing core or camera source than this instrument "
               "has now";
    return {};
}

} // namespace backend::app
