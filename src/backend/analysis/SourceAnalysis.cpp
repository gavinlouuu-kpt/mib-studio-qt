#include "backend/analysis/SourceAnalysis.h"
#include "backend/analysis/PopulationGraph.h"
#include "backend/analysis/PopulationStatistics.h"
#include "backend/evidence/AnalysisMapping.h"
#include "backend/processing/ProcessingCoreSha256.h"
#include <cmath>
#include <charconv>
#include <algorithm>
#include <set>

namespace backend::analysis {
using Json = nlohmann::json;
namespace {
struct Graph {
    PopulationPlan plan;
    std::string output;
    std::map<std::string, std::pair<std::string, Interval>> gates;
    std::map<std::string, Json> geometries;
};
Graph parseGraph(const Json& q) {
    if (!q.is_object() || q.size() != 4 || !q.at("nodes").is_array())
        throw std::invalid_argument("Expected graph id, revision, output and nodes");
    Graph g;
    g.plan.id = q.at("id").get<std::string>();
    g.plan.revision = q.at("revision").get<std::string>();
    g.output = q.at("output").get<std::string>();
    for (const auto& v : q.at("nodes")) {
        PopulationNode node;
        node.id = v.at("id").get<std::string>();
        node.parent = v.at("parent").get<std::string>();
        const auto kind = v.at("kind").get<std::string>();
        if (kind == "interval") {
            if (v.size() != 6) throw std::invalid_argument("Invalid interval node fields");
            node.predicate = node.id;
            Interval bounds;
            if (!v.at("minimum").is_null()) bounds.minimum = v.at("minimum").get<double>();
            if (!v.at("maximum").is_null()) bounds.maximum = v.at("maximum").get<double>();
            validateInterval(bounds);
            const auto feature = v.at("feature").get<std::string>();
            if (feature.empty() || feature.size() > 64 ||
                feature.find_first_not_of(
                    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
                    std::string::npos)
                throw std::invalid_argument("Unsupported graph feature");
            g.gates.emplace(node.id, std::make_pair(feature, bounds));
        } else if (kind == "ellipse" || kind == "quadrant") {
            if (v.size() != (kind == "ellipse" ? 9 : 8)) throw std::invalid_argument("Invalid geometry node fields");
            for (const char* key : {"x", "y"}) {
                const auto feature = v.at(key).get<std::string>();
                if (feature.empty() || feature.size() > 64 || feature.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos)
                    throw std::invalid_argument("Unsupported geometry parameter");
            }
            if (v.at("x") == v.at("y")) throw std::invalid_argument("Geometry requires distinct parameters");
            if (kind == "ellipse") {
                for (const char* key : {"centerX", "centerY", "radiusX", "radiusY"})
                    if (!std::isfinite(v.at(key).get<double>())) throw std::invalid_argument("Ellipse geometry must be finite");
                if (v.at("radiusX").get<double>() <= 0 || v.at("radiusY").get<double>() <= 0)
                    throw std::invalid_argument("Ellipse radii must be positive");
            } else {
                if (!std::isfinite(v.at("thresholdX").get<double>()) || !std::isfinite(v.at("thresholdY").get<double>()))
                    throw std::invalid_argument("Quadrant thresholds must be finite");
                const std::set<std::string> choices{"lower-left", "lower-right", "upper-left", "upper-right"};
                if (!choices.count(v.at("quadrant").get<std::string>())) throw std::invalid_argument("Unknown quadrant");
            }
            node.predicate = node.id;
            g.geometries.emplace(node.id, v);
        } else {
            if (v.size() != 4) throw std::invalid_argument("Invalid Boolean node fields");
            if (kind == "and")
                node.kind = NodeKind::And;
            else if (kind == "or")
                node.kind = NodeKind::Or;
            else if (kind == "not")
                node.kind = NodeKind::Not;
            else
                throw std::invalid_argument("Unsupported graph node kind");
            node.operands = v.at("operands").get<std::vector<std::string>>();
        }
        g.plan.nodes.push_back(std::move(node));
    }
    const auto order = validatePlan(g.plan);
    if (std::find(order.begin(), order.end(), g.output) == order.end())
        throw std::invalid_argument("Graph output is not a population");
    return g;
}
std::optional<double> measurement(const Json& row, const std::string& feature) {
    const auto& values = row.at("measurements");
    if (!values.contains(feature) || values.at(feature).is_null()) return {};
    const double n = values.at(feature).get<double>();
    return std::isfinite(n) ? std::optional<double>(n) : std::nullopt;
}
} // namespace
std::string contentHash(const Json& value) {
    const auto bytes = value.dump();
    return "sha256:" + processing::processingCoreBytesSha256(
                           reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
}
void validateAnalysisGraph(const Json& graph) {
    (void)parseGraph(graph);
}
Json freezeEvidence(evidence::LiveEvidence& owner, const std::string& scope) {
    auto initial = owner.snapshot();
    if (!initial) throw std::invalid_argument(owner.reason());
    auto held = owner.acquireSnapshot(initial->identity);
    if (held.status != evidence::Status::Ok) throw std::invalid_argument("Evidence source changed");
    struct Release {
        evidence::LiveEvidence& owner;
        uint64_t lease;
        ~Release() { owner.release(lease); }
    } release{owner, held.lease};
    auto metadata = owner.metadata(initial->identity, held.lease);
    if (!metadata) throw std::invalid_argument("Evidence snapshot expired");
    Json rows = Json::array();
    std::size_t cursor = 0;
    for (;;) {
        auto page = owner.page(metadata->identity, held.lease, cursor, 1024);
        if (page.status != evidence::Status::Ok)
            throw std::invalid_argument("Evidence snapshot invalidated");
        if (rows.size() + page.rows.size() > 10000)
            throw std::invalid_argument("Evidence exceeds 10000 event budget");
        for (const auto& row : page.rows)
            rows.push_back(evidence::analysisRow(metadata->identity, row));
        if (page.done) break;
        if (page.next <= cursor) throw std::invalid_argument("Evidence cursor did not advance");
        cursor = page.next;
    }
    if (!owner.metadata(metadata->identity, held.lease))
        throw std::invalid_argument("Evidence source changed before snapshot completion");
    Json parameters = Json::array(), gaps = Json::array();
    for (const auto& p : metadata->provenance.parameters)
        parameters.push_back({{"id", p.id}, {"unit", p.unit}});
    for (const auto& g : metadata->gaps)
        gaps.push_back({{"begin", std::to_string(g.batches.begin)},
                        {"endExclusive", std::to_string(g.batches.end)},
                        {"reason", g.reason}});
    Json archive{{"schema", "E-run-events/1"},
                 {"available", true},
                 {"sourceKey", evidence::analysisSourceKey(metadata->identity)},
                 {"finalized", metadata->finalized},
                 {"coverage", evidence::analysisCoverage(*metadata)},
                 {"parameters", parameters},
                 {"rows", rows},
                 {"gaps", gaps},
                 {"durableAcknowledged", false},
                 {"imageRetentionGuaranteed", false}};
    auto result = adaptEvidenceArchive(archive);
    result["scope"] = scope;
    return result;
}
Json adaptEvidenceArchive(const Json& archive) {
    if (archive.at("schema") != "E-run-events/1" || !archive.at("available").get<bool>())
        throw std::invalid_argument("Run event archive unavailable or unsupported");
    const auto sourceKey = archive.at("sourceKey").get<std::string>();
    const auto& parameters = archive.at("parameters");
    if (!parameters.is_array() || parameters.size() > 32 || !archive.at("rows").is_array() ||
        archive.at("rows").size() > 10000)
        throw std::invalid_argument("Evidence archive budget exceeded");
    Json units = Json::object(), rows = Json::array();
    for (const auto& p : parameters) {
        auto id = p.at("id").get<std::string>();
        if (units.contains(id)) throw std::invalid_argument("Duplicate evidence parameter");
        units[id] = p.at("unit");
    }
    std::set<std::string> keys;
    for (const auto& r : archive.at("rows")) {
        if (r.at("sourceKey") != sourceKey || r.at("values").size() != parameters.size())
            throw std::invalid_argument("Evidence row source/schema mismatch");
        const auto set = r.at("set").get<std::string>();
        const auto ordinal = r.at("ordinal").get<std::string>();
        if (set != "valid" && set != "invalid")
            throw std::invalid_argument("Unsupported evidence set");
        if (ordinal.empty() || ordinal.find_first_not_of("0123456789") != std::string::npos ||
            (ordinal.size() > 1 && ordinal[0] == '0'))
            throw std::invalid_argument("Invalid exact event ordinal");
        uint64_t ordinalNumber{};
        const auto parsed =
            std::from_chars(ordinal.data(), ordinal.data() + ordinal.size(), ordinalNumber);
        if (parsed.ec != std::errc{} || parsed.ptr != ordinal.data() + ordinal.size())
            throw std::invalid_argument("Event ordinal exceeds uint64");
        const auto key = Json::array({sourceKey, set, ordinal}).dump();
        if (!keys.insert(key).second) throw std::invalid_argument("Duplicate evidence event key");
        Json values = Json::object(), missing = Json::object();
        for (std::size_t i = 0; i < parameters.size(); ++i) {
            const auto id = parameters[i].at("id").get<std::string>();
            values[id] = r.at("values")[i].at("value");
            missing[id] = r.at("values")[i].at("missingReason");
        }
        rows.push_back(
            {{"key", key},
             {"set", set},
             {"ordinal", ordinal},
             {"measurements", values},
             {"missingReasons", missing},
             {"scientificQc", "unknown"},
             {"image",
              {{"available", false}, {"reason", "E provider has no retained-image locator"}}}});
    }
    const auto& coverage = archive.at("coverage");
    if (coverage.at("unit") != "batch_sequence" || coverage.at("interval") != "half_open")
        throw std::invalid_argument("Unsupported evidence watermark unit");
    Json scientific{{"sourceKey", sourceKey},
                    {"coverage", coverage},
                    {"parameters", units},
                    {"rows", rows},
                    {"gaps", archive.at("gaps")}};
    const auto fingerprint = contentHash(scientific);
    return {{"schema", "F-source/1"},
            {"sourceKey", sourceKey},
            {"fingerprint", fingerprint},
            {"fingerprintKind", "immutable-evidence-projection"},
            {"parameters", units},
            {"rows", rows},
            {"coverage", coverage},
            {"gaps", archive.at("gaps")},
            {"populationComplete",
             coverage.at("eventCountKnown").get<bool>() && archive.at("gaps").empty() &&
                 coverage.at("requestedBegin") == coverage.at("observedBegin") &&
                 coverage.at("requestedEndExclusive") == coverage.at("observedEndExclusive") &&
                 coverage.at("observedEvents") == std::to_string(rows.size()) &&
                 coverage.at("knownMissingEvents") == "0"},
            {"finalized", archive.at("finalized")},
            {"imageRetentionGuaranteed", false},
            {"providerPin", "a26d26bac596a229cc2116f8666aa73de45b7ed1"}};
}
Json evaluateSource(const Json& rawSource, const Json& request) {
    const auto source = prepareAnalysisSource(rawSource, request);
    if (source.at("schema") != "F-source/1" || source.at("rows").size() > 10000)
        throw std::invalid_argument("Unsupported analysis source");
    auto graph = parseGraph(request.at("graph"));
    graph.plan.revision = contentHash(request.at("graph"));
    const auto feature = request.at("feature").get<std::string>();
    const auto& units = source.at("parameters");
    if (!units.contains(feature))
        throw std::invalid_argument("Reduction feature unavailable in source");
    for (const auto& [id, gate] : graph.gates)
        if (!units.contains(gate.first))
            throw std::invalid_argument("Gate feature unavailable in source: " + gate.first);
    for (const auto& [id, gate] : graph.geometries)
        for (const char* axis : {"x", "y"})
            if (!units.contains(gate.at(axis).get<std::string>()))
                throw std::invalid_argument("Geometry parameter unavailable in source");
    const auto qc = request.value("qc", Json{{"policy", "all"}, {"decisions", Json::array()}});
    if (!qc.is_object() || qc.size() != 2 || !qc.at("decisions").is_array() ||
        qc.at("decisions").size() > 10000)
        throw std::invalid_argument("Expected QC policy and bounded decisions");
    const auto qcPolicy = qc.at("policy").get<std::string>();
    if (qcPolicy != "all" && qcPolicy != "good-only")
        throw std::invalid_argument("Unsupported QC policy");
    std::map<std::string, Truth> decisions;
    std::set<std::string> sourceKeys;
    for (const auto& row : source.at("rows"))
        sourceKeys.insert(row.at("key").get<std::string>());
    for (const auto& decision : qc.at("decisions")) {
        if (!decision.is_object() || decision.size() != 4)
            throw std::invalid_argument("QC decision requires key, status, reason and actor");
        const auto key = decision.at("key").get<std::string>();
        const auto status = decision.at("status").get<std::string>();
        const auto reason = decision.at("reason").get<std::string>(),
                   actor = decision.at("actor").get<std::string>();
        if (!sourceKeys.count(key) || decisions.count(key) || reason.empty() ||
            reason.size() > 512 || actor.empty() || actor.size() > 128)
            throw std::invalid_argument(
                "QC decision has foreign/duplicate key or missing bounded attribution");
        if (status != "good" && status != "bad" && status != "unknown")
            throw std::invalid_argument("Unsupported QC status");
        decisions.emplace(key, status == "good"  ? Truth::True
                               : status == "bad" ? Truth::False
                                                 : Truth::Unknown);
    }
    const auto qcFingerprint = contentHash(qc);
    const auto fingerprint = source.at("fingerprint").get<std::string>();
    RevisionKey revision{fingerprint,
                         fingerprint,
                         graph.plan.revision,
                         contentHash(request.value("application", Json(nullptr))),
                         "none",
                         source.at("parameterFingerprint").get<std::string>(),
                         qcFingerprint,
                         "source-authority",
                         "native-graph/1"};
    Membership universe{revision, {}}, eligibility{revision, {}};
    uint64_t qcGood = 0, qcBad = 0, qcUnknown = 0;
    MeasurementColumn column{revision, feature, units.at(feature).get<std::string>(), {}};
    std::map<std::string, Membership> predicates;
    for (const auto& [id, gate] : graph.gates)
        predicates.emplace(id, Membership{revision, {}});
    for (const auto& [id, gate] : graph.geometries)
        predicates.emplace(id, Membership{revision, {}});
    for (const auto& row : source.at("rows")) {
        const auto key = row.at("key").get<std::string>();
        universe.rows.push_back({key, Truth::True});
        const auto quality = decisions.count(key) ? decisions.at(key) : Truth::Unknown;
        if (quality == Truth::True)
            ++qcGood;
        else if (quality == Truth::False)
            ++qcBad;
        else
            ++qcUnknown;
        eligibility.rows.push_back({key, qcPolicy == "all" ? Truth::True : quality});
        column.rows.push_back({key, measurement(row, feature)});
        for (const auto& [id, gate] : graph.gates)
            predicates.at(id).rows.push_back(
                {key, evaluateInterval(measurement(row, gate.first), gate.second)});
        for (const auto& [id, gate] : graph.geometries) {
            const auto x = measurement(row, gate.at("x")), y = measurement(row, gate.at("y"));
            Truth truth = Truth::Unknown;
            if (x && y) {
                if (gate.at("kind") == "quadrant") {
                    const bool right = *x >= gate.at("thresholdX").get<double>();
                    const bool upper = *y >= gate.at("thresholdY").get<double>();
                    const auto name = gate.at("quadrant").get<std::string>();
                    truth = (right == (name == "lower-right" || name == "upper-right") &&
                             upper == (name == "upper-left" || name == "upper-right")) ? Truth::True : Truth::False;
                } else {
                const long double dx = (static_cast<long double>(*x) - gate.at("centerX").get<double>()) / gate.at("radiusX").get<double>();
                const long double dy = (static_cast<long double>(*y) - gate.at("centerY").get<double>()) / gate.at("radiusY").get<double>();
                truth = std::hypot(dx, dy) <= 1 ? Truth::True : Truth::False;
                }
            }
            predicates.at(id).rows.push_back({key, truth});
        }
    }
    const auto result =
        evaluatePlan(graph.plan, universe, eligibility, predicates).at(graph.output);
    const auto stats = populationStatistics(result, column);
    const auto number = [](std::optional<double> v) -> Json {
        return v ? Json(*v) : Json(nullptr);
    };
    Json rows = Json::array();
    for (const auto& row : result.rows)
        rows.push_back({{"key", row.key},
                        {"membership", row.value == Truth::True    ? "selected"
                                       : row.value == Truth::False ? "excluded"
                                                                   : "unknown"}});
    return {
        {"schema", "F-evaluation/1"},
        {"sourceFingerprint", fingerprint},
        {"parameterFingerprint", source.at("parameterFingerprint")},
        {"transforms", source.at("transforms")},
        {"sourceKey", source.at("sourceKey")},
        {"graph", request.at("graph")},
        {"graphFingerprint", graph.plan.revision},
        {"feature", feature},
        {"unit", column.unit},
        {"selected", std::to_string(stats.selected)},
        {"excluded", std::to_string(stats.excluded)},
        {"unknown", std::to_string(stats.unknownMembership)},
        {"measured", std::to_string(stats.measured)},
        {"missing", std::to_string(stats.missing)},
        {"mean", number(stats.mean)},
        {"median", number(stats.median)},
        {"p05", number(stats.p05)},
        {"p25", number(stats.p25)},
        {"p75", number(stats.p75)},
        {"p95", number(stats.p95)},
        {"sampleStandardDeviation", number(stats.sampleStandardDeviation)},
        {"cvPercent", number(stats.cvPercent)},
        {"geometricMean", number(stats.geometricMean)},
        {"estimators",
         {{"quantile", "Hyndman-Fan type 7"},
          {"standardDeviation", "sample; n-1"},
          {"cvPercent", "100 * sample SD / absolute mean; null for zero mean"},
          {"geometricMean", "exp(mean(log(x))); null if any measured x <= 0"},
          {"nonrepresentable", "null"},
          {"input", "finite raw selected values; not histogram estimates"}}},
        {"minimum", number(stats.minimum)},
        {"maximum", number(stats.maximum)},
        {"rows", rows},
        {"qcPolicy", qcPolicy},
        {"qcFingerprint", qcFingerprint},
        {"qcCounts",
         {{"good", std::to_string(qcGood)},
          {"bad", std::to_string(qcBad)},
          {"unknown", std::to_string(qcUnknown)}}},
        {"qcAuthority",
         "operator assertions; not inferred from processing validity or independently qualified"},
        {"populationComplete", source.at("populationComplete")},
        {"denominator", source.at("populationComplete").get<bool>()
                            ? Json(std::to_string(source.at("rows").size()))
                            : Json(nullptr)}};
}
} // namespace backend::analysis
