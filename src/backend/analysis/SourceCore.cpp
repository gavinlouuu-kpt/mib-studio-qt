// F-1b: the one evaluation core behind evaluateSource / plotSource /
// scatterSource for inline row sources (F-source/1) and columnar source
// handles (F-source/2). See SourceCore.h.
#include "SourceCore.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace backend::analysis {
namespace detail {
namespace {
constexpr double kMissing = std::numeric_limits<double>::quiet_NaN();

std::string unitText(const Json& unit) {
    return unit.is_string() ? unit.get<std::string>() : unit.dump();
}

// Raw columns over F-source/1 rows, read in place (no copy of the rows).
// A null, absent, non-numeric or nonfinite measurement is missing (NaN).
class RowColumns final : public EventColumns {
public:
    RowColumns(const Json& rows, std::map<std::string, std::string> units)
        : rows_(rows), segments_{{"rows", static_cast<std::uint64_t>(rows.size())}},
          units_(std::move(units)) {}
    const std::vector<EventSegment>& segments() const override { return segments_; }
    const std::map<std::string, std::string>& parameters() const override { return units_; }
    bool scan(const std::vector<std::string>& columns, std::size_t chunkEvents,
              const ChunkSink& sink) const override {
        if (chunkEvents == 0) throw std::invalid_argument("Chunk size must be positive");
        for (const auto& id : columns)
            if (!units_.count(id)) throw std::invalid_argument("Column unavailable: " + id);
        std::vector<std::vector<double>> buffers(columns.size());
        std::vector<const double*> pointers(columns.size());
        const std::uint64_t total = rows_.size();
        for (std::uint64_t begin = 0; begin < total;) {
            const auto count =
                static_cast<std::size_t>(std::min<std::uint64_t>(chunkEvents, total - begin));
            for (std::size_t c = 0; c < columns.size(); ++c) {
                auto& out = buffers[c];
                out.resize(count);
                for (std::size_t j = 0; j < count; ++j) {
                    const auto& values = rows_[static_cast<std::size_t>(begin + j)].at("measurements");
                    const auto found = values.find(columns[c]);
                    double v = kMissing;
                    if (found != values.end() && found->is_number()) v = found->get<double>();
                    out[j] = std::isfinite(v) ? v : kMissing;
                }
                pointers[c] = out.data();
            }
            if (!sink(begin, count, pointers)) return false;
            begin += count;
        }
        return true;
    }

private:
    const Json& rows_;
    std::vector<EventSegment> segments_;
    std::map<std::string, std::string> units_;
};

std::optional<std::uint64_t> canonicalOrdinal(const std::string& text) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos ||
        (text.size() > 1 && text[0] == '0'))
        return std::nullopt;
    std::uint64_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return std::nullopt;
    return value;
}
} // namespace

void withSource(const Json& source, const ColumnResolver& columns,
                const std::function<void(const SourceView&)>& use) {
    if (isColumnarSource(source)) {
        const auto segments = columnarSegments(source);
        if (!columns)
            throw std::invalid_argument(
                "Columnar source requires a host that lends its columns; none is available");
        const auto sourceKey = source.at("sourceKey").get<std::string>();
        const auto& units = source.at("parameters");
        if (!units.is_object() || units.empty() || units.size() > kMaxColumnarParameters)
            throw std::invalid_argument("Columnar source parameters must be 1.." +
                                        std::to_string(kMaxColumnarParameters) + " units");
        bool lent = false;
        columns(source, [&](const EventColumns& raw) {
            lent = true;
            const auto& actual = raw.segments();
            if (actual.size() != segments.size())
                throw std::invalid_argument("Columnar source changed: segments differ from its descriptor");
            for (std::size_t i = 0; i < segments.size(); ++i)
                if (actual[i].set != segments[i].set || actual[i].count != segments[i].count)
                    throw std::invalid_argument(
                        "Columnar source changed: segments differ from its descriptor");
            for (auto it = units.begin(); it != units.end(); ++it)
                if (!raw.parameters().count(it.key()))
                    throw std::invalid_argument("Columnar source lacks parameter " + it.key());
            SourceView view;
            view.source = &source;
            view.raw = &raw;
            view.events = raw.eventCount();
            view.inlineRows = false;
            view.key = [&segments, sourceKey](std::uint64_t i) {
                const auto ref = eventReference(segments, i);
                return columnarEventKey(sourceKey, ref.first, ref.second);
            };
            view.index = [&segments, sourceKey](const std::string& key) -> std::optional<std::uint64_t> {
                const auto parsed = Json::parse(key, nullptr, false);
                if (!parsed.is_array() || parsed.size() != 3 || !parsed[0].is_string() ||
                    !parsed[1].is_string() || !parsed[2].is_string() ||
                    parsed[0].get<std::string>() != sourceKey)
                    return std::nullopt;
                const auto set = parsed[1].get<std::string>();
                const auto ordinal = canonicalOrdinal(parsed[2].get<std::string>());
                if (!ordinal || columnarEventKey(sourceKey, set, *ordinal) != key) return std::nullopt;
                std::uint64_t base = 0;
                for (const auto& s : segments) {
                    if (s.set == set) {
                        if (*ordinal >= s.count) return std::nullopt;
                        return base + *ordinal;
                    }
                    base += s.count;
                }
                return std::nullopt;
            };
            view.value = [](std::uint64_t, const std::string&, double v) { return Json(v); };
            use(view);
        });
        if (!lent) throw std::invalid_argument("Host did not lend the columnar source");
        return;
    }
    if (!source.is_object() || source.value("schema", std::string{}) != "F-source/1")
        throw std::invalid_argument("Unsupported analysis source");
    const auto& rows = source.at("rows");
    if (!rows.is_array()) throw std::invalid_argument("Unsupported analysis source");
    if (rows.size() > kMaxJsonSourceEvents) throw std::invalid_argument(jsonSourceBoundMessage());
    std::map<std::string, std::string> units;
    const auto& parameters = source.at("parameters");
    for (auto it = parameters.begin(); it != parameters.end(); ++it)
        units.emplace(it.key(), unitText(it.value()));
    std::map<std::string, std::uint64_t> keys;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto key = rows[i].at("key").get<std::string>();
        if (key.empty() || !keys.emplace(key, i).second)
            throw std::invalid_argument("Empty or duplicate source key");
    }
    RowColumns raw(rows, units);
    SourceView view;
    view.source = &source;
    view.raw = &raw;
    view.events = rows.size();
    view.inlineRows = true;
    view.key = [&rows](std::uint64_t i) {
        return rows[static_cast<std::size_t>(i)].at("key").get<std::string>();
    };
    view.index = [&keys](const std::string& key) -> std::optional<std::uint64_t> {
        const auto found = keys.find(key);
        return found == keys.end() ? std::nullopt : std::optional<std::uint64_t>(found->second);
    };
    view.value = [&rows, &units](std::uint64_t i, const std::string& feature, double v) {
        if (units.count(feature)) {
            const auto& values = rows[static_cast<std::size_t>(i)].at("measurements");
            const auto found = values.find(feature);
            if (found != values.end() && found->is_number()) return *found;
        }
        return Json(v);
    };
    use(view);
}

TransformedColumns::TransformedColumns(const EventColumns& raw,
                                       std::map<std::string, std::string> units,
                                       std::vector<TransformStage> stages)
    : raw_(raw), units_(std::move(units)), stages_(std::move(stages)) {}

bool TransformedColumns::derived(const std::string& id) const {
    return std::any_of(stages_.begin(), stages_.end(),
                       [&](const TransformStage& s) { return s.output == id; });
}

double transformValue(const TransformStage& stage, double raw) {
    if (!std::isfinite(raw)) return kMissing;
    long double value = (static_cast<long double>(raw) - stage.offset) / stage.scale;
    if (stage.kind == "asinh") value = std::asinh(value);
    if (stage.kind == "log10")
        value = value > 0 ? std::log10(value) : std::numeric_limits<long double>::quiet_NaN();
    const double number = static_cast<double>(value);
    return std::isfinite(number) ? number : kMissing;
}

bool TransformedColumns::scan(const std::vector<std::string>& columns, std::size_t chunkEvents,
                              const ChunkSink& sink) const {
    // Stages needed by the request, transitively; raw inputs they read.
    std::vector<bool> needed(stages_.size(), false);
    std::function<void(const std::string&)> need = [&](const std::string& id) {
        for (std::size_t k = stages_.size(); k-- > 0;)
            if (stages_[k].output == id) {
                if (!needed[k]) {
                    needed[k] = true;
                    need(stages_[k].input);
                }
                return;
            }
    };
    for (const auto& id : columns) {
        if (!units_.count(id)) throw std::invalid_argument("Column unavailable: " + id);
        need(id);
    }
    std::vector<std::string> rawColumns;
    const auto addRaw = [&](const std::string& id) {
        if (!derived(id) && std::find(rawColumns.begin(), rawColumns.end(), id) == rawColumns.end())
            rawColumns.push_back(id);
    };
    for (const auto& id : columns) addRaw(id);
    for (std::size_t k = 0; k < stages_.size(); ++k)
        if (needed[k]) addRaw(stages_[k].input);
    std::vector<std::vector<double>> outputs(stages_.size());
    std::vector<const double*> pointers(columns.size());
    return raw_.scan(rawColumns, chunkEvents,
                     [&](std::uint64_t begin, std::size_t count, const std::vector<const double*>& values) {
                         std::map<std::string, const double*> at;
                         for (std::size_t c = 0; c < rawColumns.size(); ++c) at[rawColumns[c]] = values[c];
                         for (std::size_t k = 0; k < stages_.size(); ++k) {
                             if (!needed[k]) continue;
                             const double* in = at.at(stages_[k].input);
                             auto& out = outputs[k];
                             out.resize(count);
                             for (std::size_t j = 0; j < count; ++j) out[j] = transformValue(stages_[k], in[j]);
                             at[stages_[k].output] = out.data();
                         }
                         for (std::size_t c = 0; c < columns.size(); ++c) pointers[c] = at.at(columns[c]);
                         return sink(begin, count, pointers);
                     });
}

Prepared prepare(const SourceView& view, const Json& strategy) {
    const auto& source = *view.source;
    Prepared result;
    result.units = source.at("parameters");
    const auto stages = strategy.value("transforms", Json::array());
    if (!stages.is_array() || stages.size() > 8)
        throw std::invalid_argument("Expected at most eight ordered transforms");
    std::vector<TransformStage> parsed;
    for (const auto& stage : stages) {
        const std::set<std::string> names{"input", "inputUnit", "output", "kind", "scale", "offset"};
        if (!stage.is_object() || stage.size() != names.size())
            throw std::invalid_argument("Incomplete transform fields");
        for (auto i = stage.begin(); i != stage.end(); ++i)
            if (!names.count(i.key())) throw std::invalid_argument("Unknown transform field");
        TransformStage s;
        s.input = stage.at("input").get<std::string>();
        s.output = stage.at("output").get<std::string>();
        s.kind = stage.at("kind").get<std::string>();
        auto& units = result.units;
        if (!units.contains(s.input) || units.at(s.input) != stage.at("inputUnit"))
            throw std::invalid_argument("Transform input/unit unavailable or mismatched");
        if (s.output.empty() || s.output.size() > 64 ||
            s.output.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
                std::string::npos ||
            units.contains(s.output))
            throw std::invalid_argument("Transform output must be a new unique parameter");
        s.scale = stage.at("scale").get<double>();
        s.offset = stage.at("offset").get<double>();
        if (!std::isfinite(s.scale) || s.scale <= 0 || !std::isfinite(s.offset) ||
            (s.kind != "linear" && s.kind != "asinh" && s.kind != "log10"))
            throw std::invalid_argument("Unsupported transform or invalid scale/offset");
        units[s.output] = "dimensionless";
        parsed.push_back(std::move(s));
    }
    result.transforms = stages;
    result.parameterFingerprint =
        contentHash(Json{{"rawParameters", source.at("parameters")}, {"transforms", stages}});
    std::map<std::string, std::string> units;
    for (auto it = result.units.begin(); it != result.units.end(); ++it)
        units.emplace(it.key(), unitText(it.value()));
    result.columns = std::make_unique<TransformedColumns>(*view.raw, std::move(units), std::move(parsed));
    return result;
}

Json graphFor(const Json& request, const std::string& population) {
    auto graph = request.at("graph");
    graph["output"] = population;
    (void)compileGraph(graph);
    return graph;
}

GraphRun runGraph(const SourceView& view, const Prepared& prepared, const Json& request) {
    const auto& source = *view.source;
    GraphRun run;
    run.graph = compileGraph(request.at("graph"));
    run.graph.plan.revision = contentHash(request.at("graph"));
    const auto& units = prepared.units;
    for (const auto& [id, gate] : run.graph.gates) {
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
    run.qcPolicy = qc.at("policy").get<std::string>();
    if (run.qcPolicy != "all" && run.qcPolicy != "good-only")
        throw std::invalid_argument("Unsupported QC policy");
    std::map<std::uint64_t, Truth> decisions;
    for (const auto& decision : qc.at("decisions")) {
        if (!decision.is_object() || decision.size() != 4)
            throw std::invalid_argument("QC decision requires key, status, reason and actor");
        const auto key = decision.at("key").get<std::string>();
        const auto status = decision.at("status").get<std::string>();
        const auto reason = decision.at("reason").get<std::string>(),
                   actor = decision.at("actor").get<std::string>();
        const auto index = view.index(key);
        if (!index || decisions.count(*index) || reason.empty() || reason.size() > 512 ||
            actor.empty() || actor.size() > 128)
            throw std::invalid_argument(
                "QC decision has foreign/duplicate key or missing bounded attribution");
        if (status != "good" && status != "bad" && status != "unknown")
            throw std::invalid_argument("Unsupported QC status");
        decisions.emplace(*index, status == "good"  ? Truth::True
                                  : status == "bad" ? Truth::False
                                                    : Truth::Unknown);
    }
    run.qcFingerprint = contentHash(qc);
    const auto fingerprint = source.at("fingerprint").get<std::string>();
    run.revision = RevisionKey{fingerprint,
                               fingerprint,
                               run.graph.plan.revision,
                               contentHash(request.value("application", Json(nullptr))),
                               "none",
                               prepared.parameterFingerprint,
                               run.qcFingerprint,
                               "source-authority",
                               "native-graph/1"};
    for (const auto* field : {&run.revision.snapshot, &run.revision.sourceFingerprint,
                              &run.revision.plan, &run.revision.application,
                              &run.revision.parameters, &run.revision.qc})
        if (field->empty()) throw std::invalid_argument("Unpinned analysis revision");
    for (const auto& [index, quality] : decisions) {
        if (quality == Truth::True)
            ++run.qcGood;
        else if (quality == Truth::False)
            ++run.qcBad;
    }
    run.qcUnknown = view.events - run.qcGood - run.qcBad;
    std::unique_ptr<TriStateBits> eligibility;
    if (run.qcPolicy == "good-only") {
        // Every event Unknown unless an operator decision says otherwise.
        eligibility = std::make_unique<TriStateBits>(view.events);
        for (std::uint64_t i = 0; i < view.events; ++i) eligibility->set(i, Truth::Unknown);
        for (const auto& [index, quality] : decisions) eligibility->set(index, quality);
    }
    run.membership = evaluateColumns(run.graph, *prepared.columns, run.revision, eligibility.get());
    return run;
}

Json evaluation(const SourceView& view, const Prepared& prepared, const GraphRun& run,
                const Json& request, const std::string& population) {
    const auto& source = *view.source;
    const auto feature = request.at("feature").get<std::string>();
    const auto& units = prepared.units;
    if (!units.contains(feature)) throw std::invalid_argument("Reduction feature unavailable in source");
    const auto graph = population == run.graph.output ? request.at("graph") : graphFor(request, population);
    auto revision = run.revision;
    revision.plan = contentHash(graph);
    const auto unit = units.at(feature).get<std::string>();
    if (unit.empty()) throw std::invalid_argument("Unqualified measurement column");
    const auto& result = run.membership->population(population);
    auto stats = columnStatistics(result, *prepared.columns, feature, revision);
    stats.unit = unit;
    const auto number = [](std::optional<double> v) -> Json { return v ? Json(*v) : Json(nullptr); };
    const bool complete = source.at("populationComplete").get<bool>();
    Json out{
        {"schema", "F-evaluation/1"},
        {"sourceFingerprint", source.at("fingerprint")},
        {"parameterFingerprint", prepared.parameterFingerprint},
        {"transforms", prepared.transforms},
        {"sourceKey", source.at("sourceKey")},
        {"graph", graph},
        {"graphFingerprint", revision.plan},
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
        {"qcPolicy", run.qcPolicy},
        {"qcFingerprint", run.qcFingerprint},
        {"qcCounts",
         {{"good", std::to_string(run.qcGood)},
          {"bad", std::to_string(run.qcBad)},
          {"unknown", std::to_string(run.qcUnknown)}}},
        {"qcAuthority",
         "operator assertions; not inferred from processing validity or independently qualified"},
        {"populationComplete", source.at("populationComplete")},
        {"denominator", complete ? Json(std::to_string(view.events)) : Json(nullptr)}};
    if (view.inlineRows) {
        Json rows = Json::array();
        for (std::uint64_t i = 0; i < view.events; ++i) {
            const auto value = result.get(i);
            rows.push_back({{"key", view.key(i)},
                            {"membership", value == Truth::True    ? "selected"
                                           : value == Truth::False ? "excluded"
                                                                   : "unknown"}});
        }
        out["rows"] = std::move(rows);
    } else {
        // Exact membership stays native (two bits per event); only bounded
        // results cross. Pages of keys: selectedEventKeys / gallery.
        out["membershipStorage"] = "native-bitset";
        out["events"] = std::to_string(view.events);
    }
    return out;
}
} // namespace detail

using Json = nlohmann::json;

bool isColumnarSource(const Json& source) {
    return source.is_object() && source.value("schema", std::string{}) == kColumnarSourceSchema;
}

std::vector<EventSegment> columnarSegments(const Json& source) {
    if (!isColumnarSource(source) || source.value("storage", std::string{}) != "columnar")
        throw std::invalid_argument("Expected an F-source/2 columnar descriptor");
    for (const char* key : {"sourceKey", "fingerprint", "scope"})
        if (!source.contains(key) || !source.at(key).is_string() ||
            source.at(key).get<std::string>().empty())
            throw std::invalid_argument(std::string("Columnar descriptor lacks ") + key);
    const auto& segments = source.at("segments");
    if (!segments.is_array() || segments.empty() || segments.size() > 16)
        throw std::invalid_argument("Columnar descriptor needs 1..16 segments");
    std::vector<EventSegment> out;
    std::set<std::string> names;
    std::uint64_t total = 0;
    for (const auto& s : segments) {
        if (!s.is_object() || s.size() != 2 || !s.at("set").is_string() || !s.at("count").is_string())
            throw std::invalid_argument("Columnar segment requires set and decimal count");
        EventSegment segment;
        segment.set = s.at("set").get<std::string>();
        const auto count = s.at("count").get<std::string>();
        std::uint64_t value{};
        const auto parsed = std::from_chars(count.data(), count.data() + count.size(), value);
        if (segment.set.empty() || !names.insert(segment.set).second || count.empty() ||
            parsed.ec != std::errc{} || parsed.ptr != count.data() + count.size() ||
            std::to_string(value) != count)
            throw std::invalid_argument("Invalid or duplicate columnar segment");
        segment.count = value;
        total += value;
        out.push_back(std::move(segment));
    }
    if (source.at("events") != std::to_string(total))
        throw std::invalid_argument("Columnar descriptor event count disagrees with its segments");
    return out;
}

std::string columnarEventKey(const std::string& sourceKey, const std::string& set,
                             std::uint64_t ordinal) {
    return Json::array({sourceKey, set, std::to_string(ordinal)}).dump();
}

Json materializeColumnarSource(const Json& source, const EventColumns& columns) {
    const auto segments = columnarSegments(source);
    const auto events = columns.eventCount();
    if (events > kMaxJsonSourceEvents) throw std::invalid_argument(jsonSourceBoundMessage());
    const auto sourceKey = source.at("sourceKey").get<std::string>();
    const auto fingerprint = source.at("fingerprint").get<std::string>();
    const bool stored = source.at("scope") == "stored";
    std::vector<std::string> ids;
    for (auto it = source.at("parameters").begin(); it != source.at("parameters").end(); ++it)
        ids.push_back(it.key());
    if (static_cast<std::uint64_t>(events) * ids.size() > kMaxJsonSourceCells)
        throw std::invalid_argument("F-source/1 JSON rows are bounded at " + std::to_string(kMaxJsonSourceCells) +
                                    " cells (events x parameters); this source has " + std::to_string(events) +
                                    " events x " + std::to_string(ids.size()) +
                                    " parameters. Use the columnar evaluator; nothing was sampled");
    Json rows = Json::array();
    columns.scan(ids, 65536, [&](std::uint64_t begin, std::size_t count, const std::vector<const double*>& v) {
        for (std::size_t j = 0; j < count; ++j) {
            const auto ref = eventReference(segments, begin + j);
            const auto ordinal = std::to_string(ref.second);
            Json measures = Json::object();
            for (std::size_t c = 0; c < ids.size(); ++c)
                measures[ids[c]] = std::isfinite(v[c][j]) ? Json(v[c][j]) : Json(nullptr);
            Json image = stored ? Json{{"available", nullptr},
                                       {"sourceFingerprint", fingerprint},
                                       {"set", ref.first},
                                       {"ordinal", ordinal}}
                                : Json{{"available", false}, {"reason", "This source carries no event images"}};
            rows.push_back({{"key", columnarEventKey(sourceKey, ref.first, ref.second)},
                            {"set", ref.first},
                            {"ordinal", ordinal},
                            {"measurements", std::move(measures)},
                            {"scientificQc", "unknown"},
                            {"image", std::move(image)}});
        }
        return true;
    });
    Json result = source;
    for (const char* key : {"storage", "segments", "events", "retention", "columnUnits", "import"})
        result.erase(key);
    result["schema"] = "F-source/1";
    result["rows"] = std::move(rows);
    return result;
}

Json evaluateSource(const Json& source, const Json& request, const ColumnResolver& columns) {
    Json result;
    detail::withSource(source, columns, [&](const detail::SourceView& view) {
        const auto prepared = detail::prepare(view, request);
        const auto feature = request.at("feature").get<std::string>();
        if (!prepared.units.contains(feature))
            throw std::invalid_argument("Reduction feature unavailable in source");
        const auto run = detail::runGraph(view, prepared, request);
        result = detail::evaluation(view, prepared, run, request, run.graph.output);
    });
    return result;
}

std::vector<Json> evaluatePopulations(const Json& source, const Json& request,
                                      const std::vector<std::string>& populations,
                                      const ColumnResolver& columns) {
    std::vector<Json> out;
    detail::withSource(source, columns, [&](const detail::SourceView& view) {
        const auto prepared = detail::prepare(view, request);
        if (!prepared.units.contains(request.at("feature").get<std::string>()))
            throw std::invalid_argument("Reduction feature unavailable in source");
        const auto run = detail::runGraph(view, prepared, request);
        for (const auto& population : populations) {
            auto row = detail::evaluation(view, prepared, run, request, population);
            row.erase("rows");
            out.push_back(std::move(row));
        }
    });
    return out;
}

Json selectedEventKeys(const Json& source, const Json& request, std::uint64_t offset,
                       std::size_t limit, const ColumnResolver& columns) {
    Json result;
    detail::withSource(source, columns, [&](const detail::SourceView& view) {
        const auto prepared = detail::prepare(view, request);
        const auto run = detail::runGraph(view, prepared, request);
        const auto& bits = run.membership->population(run.graph.output);
        Json keys = Json::array();
        if (limit > 0)
            if (const auto first = bits.find(Truth::True, offset))
                for (std::uint64_t i = *first; i < view.events && keys.size() < limit; ++i)
                    if (bits.get(i) == Truth::True) keys.push_back(view.key(i));
        result = Json{{"selected", std::to_string(bits.count(Truth::True))}, {"keys", keys}};
    });
    return result;
}
} // namespace backend::analysis
