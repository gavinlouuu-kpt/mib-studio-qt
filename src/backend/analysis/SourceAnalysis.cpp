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
// evaluateSource, evaluatePopulations, selectedEventKeys: SourceCore.cpp (F-1b).
} // namespace backend::analysis
