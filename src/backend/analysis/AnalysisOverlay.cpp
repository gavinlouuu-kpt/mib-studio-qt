#include "backend/analysis/AnalysisOverlay.h"
#include "backend/analysis/SourceAnalysis.h"
#include "backend/database/SqliteService.h"
#include <cmath>
#include <set>
namespace backend::analysis {
using Json = nlohmann::json;
namespace {
void fields(const Json& q, const std::set<std::string>& names) {
    if (!q.is_object() || q.size() != names.size())
        throw std::invalid_argument("Incomplete overlay fields");
    for (auto i = q.begin(); i != q.end(); ++i)
        if (!names.count(i.key())) throw std::invalid_argument("Unknown overlay field");
}
std::string label(const Json& value) {
    const auto s = value.get<std::string>();
    if (s.empty() || s.size() > 128) throw std::invalid_argument("Invalid overlay identity/label");
    return s;
}
Json render(const Json& spec, const std::function<Json(const Json&)>& evaluate) {
    fields(spec, {"title", "layers"});
    label(spec.at("title"));
    if (!spec.at("layers").is_array() || spec.at("layers").empty() || spec.at("layers").size() > 8)
        throw std::invalid_argument("Overlay requires1..8 layers");
    Json first = nullptr, layers = Json::array(), sources = Json::array();
    std::set<std::string> ids;
    for (const auto& layer : spec.at("layers")) {
        fields(layer, {"id", "label", "reference", "population", "displayOffset"});
        const auto id = label(layer.at("id")), name = label(layer.at("label"));
        if (!ids.insert(id).second) throw std::invalid_argument("Duplicate overlay layer ID");
        const double offset = layer.at("displayOffset").get<double>();
        if (!std::isfinite(offset) || std::abs(offset) > 1000000)
            throw std::invalid_argument("Invalid overlay display offset");
        auto query = layer.at("reference");
        fields(query, {"planId", "revision", "source"});
        if (query.at("revision").is_null() || query.at("source").at("kind") != "retained")
            throw std::invalid_argument("Overlay requires exact plans and retained sources");
        query["action"] = "plot";
        const auto response = evaluate(query);
        if (response.value("status", std::string{}) != "ok")
            throw std::invalid_argument("Overlay source plot refused: " + response.dump());
        const auto& plot = response.at("plot");
        if (plot.at("schema") != "F-plot/1")
            throw std::invalid_argument("Overlay currently requires saved native histograms");
        if (first.is_null())
            first = plot;
        else if (plot.at("edges") != first.at("edges") || plot.at("unit") != first.at("unit") ||
                 plot.at("feature") != first.at("feature") ||
                 plot.at("parameterFingerprint") != first.at("parameterFingerprint") ||
                 plot.at("spec").at("normalization") != first.at("spec").at("normalization"))
            throw std::invalid_argument(
                "Overlay bins, parameter domain, units or normalization mismatch");
        bool found = false;
        for (const auto& candidate : plot.at("layers"))
            if (candidate.at("population") == layer.at("population")) {
                auto output = candidate;
                output["originalPopulation"] = candidate.at("population");
                output["population"] = id + ": " + name;
                output["displayOffset"] = offset;
                output["reference"] = layer.at("reference");
                output["sourceFingerprint"] = plot.at("sourceFingerprint");
                layers.push_back(output);
                found = true;
            }
        if (!found) throw std::invalid_argument("Overlay population absent from saved plot layers");
        sources.push_back(Json{{"reference", layer.at("reference")},
                               {"fingerprint", plot.at("sourceFingerprint")},
                               {"plotHash", response.at("contentHash")}});
    }
    return Json{{"schema", "F-overlay/1"},
                {"title", spec.at("title")},
                {"spec", {{"normalization", first.at("spec").at("normalization")}}},
                {"recipe", spec},
                {"sources", sources},
                {"sourceSetFingerprint", contentHash(sources)},
                {"feature", first.at("feature")},
                {"unit", first.at("unit")},
                {"parameterFingerprint", first.at("parameterFingerprint")},
                {"edges", first.at("edges")},
                {"layers", layers},
                {"scientificCompatibility", "unknown"},
                {"semantics",
                 "Exact native histogram bins/values; each layer retains its own denominator. "
                 "Explicit offsets are presentation only. Matching units/preprocessing do not "
                 "establish scientific compatibility; layers are not experimental replicates."}};
}
} // namespace
Json analysisOverlayRequest(services::SqliteService& db, const Json& q, bool write,
                            const std::function<Json(const Json&)>& evaluate) {
    const auto action = q.at("action").get<std::string>(), id = label(q.at("overlayId"));
    const auto store = [&](const Json& v, bool mutate = false) {
        return Json::parse(db.analysisWorkspaceRequest(v.dump(), mutate));
    };
    if (action == "overlaySave") {
        if (!write) throw std::invalid_argument("Overlay save requires write endpoint");
        fields(q, {"action", "overlayId", "expectedRevision", "commandId", "spec"});
        const Json doc{{"schema_version", 1}, {"type", "F-overlay-spec/1"}, {"spec", q.at("spec")}};
        const Json commit{{"action", "commit"},
                          {"workspace_id", "analysis-overlay/" + id},
                          {"expected_revision", q.at("expectedRevision")},
                          {"command_id", q.at("commandId")},
                          {"document", doc}};
        auto receipt = commit;
        receipt["action"] = "receipt";
        const auto replay = store(receipt);
        if (replay.value("status", std::string{}) == "ok") return replay;
        if (replay.value("status", std::string{}) != "not_found") return replay;
        (void)render(q.at("spec"), evaluate);
        return store(commit, true);
    }
    if (action != "overlayRead" && action != "overlayEvaluate")
        throw std::invalid_argument("Unsupported overlay action");
    fields(q, {"action", "overlayId", "revision"});
    if (!q.at("revision").is_string() || q.at("revision").get<std::string>().empty())
        throw std::invalid_argument("Exact overlay revision required");
    const auto saved = store(Json{{"action", "read"},
                                  {"workspace_id", "analysis-overlay/" + id},
                                  {"revision", q.at("revision")}});
    if (saved.value("status", std::string{}) != "ok") return saved;
    if (saved.at("document").at("type") != "F-overlay-spec/1")
        throw std::invalid_argument("Overlay type mismatch");
    if (action == "overlayRead") return saved;
    auto plot = render(saved.at("document").at("spec"), evaluate);
    plot["planId"] = id;
    plot["revision"] = q.at("revision");
    return Json{{"status", "ok"}, {"plot", plot}, {"contentHash", contentHash(plot)}};
}
} // namespace backend::analysis
