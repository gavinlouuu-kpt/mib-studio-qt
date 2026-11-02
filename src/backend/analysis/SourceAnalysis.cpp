#include "backend/analysis/SourceAnalysis.h"
#include "backend/analysis/ColumnarEvaluation.h"
#include "backend/analysis/PopulationGates.h"
#include "backend/evidence/AnalysisMapping.h"
#include "backend/processing/ProcessingCoreSha256.h"
#include <cmath>
#include <charconv>
#include <algorithm>
#include <set>
#include <array>
#include <limits>

namespace backend::analysis {
using Json = nlohmann::json;
namespace {
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
    (void)compileGraph(graph);
}
Json freezeEvidence(evidence::LiveEvidence& owner, const std::string& scope,
                    EvidenceLeaseRead read) {
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
        if (rows.size() + page.rows.size() > kMaxJsonSourceEvents)
            throw std::invalid_argument(jsonSourceBoundMessage());
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
    if (read) read(metadata->identity, held.lease, result);
    if (!owner.metadata(metadata->identity, held.lease))
        throw std::invalid_argument("Evidence source changed during leased read");
    return result;
}
Json adaptEvidenceArchive(const Json& archive) {
    if (archive.at("schema") != "E-run-events/1" || !archive.at("available").get<bool>())
        throw std::invalid_argument("Run event archive unavailable or unsupported");
    const auto sourceKey = archive.at("sourceKey").get<std::string>();
    const auto& parameters = archive.at("parameters");
    if (!parameters.is_array() || parameters.size() > 32 || !archive.at("rows").is_array())
        throw std::invalid_argument("Evidence archive budget exceeded");
    if (archive.at("rows").size() > kMaxJsonSourceEvents)
        throw std::invalid_argument(jsonSourceBoundMessage());
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
        rows.push_back({{"key", key},
                        {"set", set},
                        {"ordinal", ordinal},
                        {"measurements", values},
                        {"missingReasons", missing},
                        {"scientificQc", "unknown"},
                        {"image",
                         {{"available", false},
                          {"reason", "Use a fresh fenced run/archive gallery lookup; snapshots do "
                                     "not retain pixels"}}}});
    }
    const auto& coverage = archive.at("coverage");
    if (coverage.at("unit") != "batch_sequence" || coverage.at("interval") != "half_open")
        throw std::invalid_argument("Unsupported evidence watermark unit");
    Json scientific{{"sourceKey", sourceKey},
                    {"coverage", coverage},
                    {"parameters", units},
                    {"rows", rows},
                    {"gaps", archive.at("gaps")}};
    // F-source/1 historically hashed this scalar-only presentation field. Keep its
    // canonical value so enabling a fresh image lookup does not invalidate saved
    // scientific bindings. The returned row explanation is current, not hash input.
    for (auto& row : scientific["rows"])
        row["image"]["reason"] = "E provider has no retained-image locator";
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
    if (source.at("schema") != "F-source/1") throw std::invalid_argument("Unsupported analysis source");
    if (source.at("rows").size() > kMaxJsonSourceEvents)
        throw std::invalid_argument(jsonSourceBoundMessage());
    auto graph = compileGraph(request.at("graph"));
    graph.plan.revision = contentHash(request.at("graph"));
    const auto feature = request.at("feature").get<std::string>();
    const auto& units = source.at("parameters");
    if (!units.contains(feature))
        throw std::invalid_argument("Reduction feature unavailable in source");
    for (const auto& [id, gate] : graph.gates) {
        if (gate.kind == GateSpec::Kind::Interval) {
            if (!units.contains(gate.x))
                throw std::invalid_argument("Gate feature unavailable in source: " + gate.x);
        } else if (!units.contains(gate.x) || !units.contains(gate.y)) {
            throw std::invalid_argument("Geometry parameter unavailable in source");
        }
    }
    const auto qc = request.value("qc", Json{{"policy", "all"}, {"decisions", Json::array()}});
    if (!qc.is_object() || qc.size() != 2 || !qc.at("decisions").is_array() ||
        qc.at("decisions").size() > kMaxJsonSourceEvents)
        throw std::invalid_argument("Expected QC policy and bounded decisions");
    const auto qcPolicy = qc.at("policy").get<std::string>();
    if (qcPolicy != "all" && qcPolicy != "good-only")
        throw std::invalid_argument("Unsupported QC policy");
    std::map<std::string, Truth> decisions;
    std::set<std::string> sourceKeys;
    for (const auto& row : source.at("rows")) {
        const auto key = row.at("key").get<std::string>();
        if (key.empty() || !sourceKeys.insert(key).second)
            throw std::invalid_argument("Empty or duplicate source key");
    }
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
    for (const auto* field : {&revision.snapshot, &revision.sourceFingerprint, &revision.plan,
                              &revision.application, &revision.parameters, &revision.qc})
        if (field->empty()) throw std::invalid_argument("Unpinned analysis revision");
    const auto unit = units.at(feature).get<std::string>();
    if (unit.empty()) throw std::invalid_argument("Unqualified measurement column");
    // The JSON rows become in-memory columns; the one columnar evaluator then
    // decides membership and statistics for this host as for full recordings.
    const auto& sourceRows = source.at("rows");
    const auto events = static_cast<std::uint64_t>(sourceRows.size());
    auto needed = gateFeatures(graph);
    if (std::find(needed.begin(), needed.end(), feature) == needed.end()) needed.push_back(feature);
    std::map<std::string, std::string> parameterUnits;
    for (const auto& f : needed)
        parameterUnits.emplace(f, units.at(f).is_string() ? units.at(f).get<std::string>()
                                                          : units.at(f).dump());
    VectorEventColumns columns({{"rows", events}}, parameterUnits);
    for (const auto& f : needed) {
        std::vector<double> values;
        values.reserve(static_cast<std::size_t>(events));
        for (const auto& row : sourceRows)
            values.push_back(measurement(row, f).value_or(std::numeric_limits<double>::quiet_NaN()));
        columns.setColumn(f, std::move(values));
    }
    TriStateBits eligibility(events);
    uint64_t qcGood = 0, qcBad = 0, qcUnknown = 0;
    for (std::uint64_t i = 0; i < events; ++i) {
        const auto key = sourceRows[static_cast<std::size_t>(i)].at("key").get<std::string>();
        const auto quality = decisions.count(key) ? decisions.at(key) : Truth::Unknown;
        if (quality == Truth::True)
            ++qcGood;
        else if (quality == Truth::False)
            ++qcBad;
        else
            ++qcUnknown;
        eligibility.set(i, qcPolicy == "all" ? Truth::True : quality);
    }
    const auto membership = evaluateColumns(graph, columns, revision, &eligibility);
    const auto& result = membership->population(graph.output);
    auto stats = columnStatistics(result, columns, feature, revision);
    stats.unit = unit;
    const auto number = [](std::optional<double> v) -> Json {
        return v ? Json(*v) : Json(nullptr);
    };
    Json rows = Json::array();
    for (std::uint64_t i = 0; i < events; ++i) {
        const auto value = result.get(i);
        rows.push_back({{"key", sourceRows[static_cast<std::size_t>(i)].at("key")},
                        {"membership", value == Truth::True    ? "selected"
                                       : value == Truth::False ? "excluded"
                                                               : "unknown"}});
    }
    return {
        {"schema", "F-evaluation/1"},
        {"sourceFingerprint", fingerprint},
        {"parameterFingerprint", source.at("parameterFingerprint")},
        {"transforms", source.at("transforms")},
        {"sourceKey", source.at("sourceKey")},
        {"graph", request.at("graph")},
        {"graphFingerprint", graph.plan.revision},
        {"feature", feature},
        {"unit", unit},
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
