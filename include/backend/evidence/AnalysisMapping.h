#pragma once
#include "backend/evidence/CommittedEventStore.h"
#include <nlohmann/json.hpp>

namespace backend::evidence {
// Isolated E mapping proposal, NOT a registered transport or F ABI.
// JSON arrays give unambiguous component boundaries, even for arbitrary ':' text.
inline std::string analysisSourceKey(const Identity& id) {
    nlohmann::json parts = {id.source, id.session, id.epoch,
                            std::to_string(id.generation), id.revision};
    parts.push_back(id.run ? nlohmann::json(*id.run) : nlohmann::json(nullptr));
    return parts.dump();
}
inline nlohmann::json analysisRow(const Identity& id, const EventRow& row) {
    using nlohmann::json;
    auto exact = [](const std::optional<std::uint64_t>& v) -> json {
        return v ? json(std::to_string(*v)) : json(nullptr);
    };
    json values = json::array();
    for (const auto& value : row.values)
        values.push_back({{"value", value ? json(*value) : json(nullptr)},
                          {"missingReason", value ? json(nullptr) : json("unavailable")}});
    // Legacy free-form qc is opaque processing metadata. It cannot establish
    // scientific QC or exclude an event. No QC-policy revision was supplied.
    return {{"sourceKey", analysisSourceKey(id)}, {"source", id.source},
            {"run", id.run ? json(*id.run) : json(nullptr)},
            {"set", row.set}, {"ordinal", std::to_string(row.ordinal)},
            {"frame", exact(row.frame)}, {"object", exact(row.object)},
            {"track", exact(row.track)}, {"values", values},
            {"processingStatusRaw", row.qc}, {"scientificQc", "unknown"},
            {"qcPolicyRevision", nullptr}};
}
inline nlohmann::json analysisCoverage(const Snapshot& s) {
    return {{"unit", "batch_sequence"}, {"interval", "half_open"},
            {"requestedBegin", std::to_string(s.requested.begin)},
            {"requestedEndExclusive", std::to_string(s.requested.end)},
            {"observedBegin", std::to_string(s.observed.begin)},
            {"observedEndExclusive", std::to_string(s.observed.end)},
            {"nextBatchExclusive", std::to_string(s.watermark)},
            {"eventCountKnown", s.eventCountKnown},
            {"observedEvents", std::to_string(s.accounting.observedEvents)},
            {"knownMissingEvents", std::to_string(s.accounting.missingEvents)}};
}
} // namespace backend::evidence
