// F-1b (F01/F04/F05/F08/F12/F16): the analysis workspace path at full scale,
// headless. Two MIB experiment files of 255,000 and 260,000 events (production
// writer, real compound schema) go through the AnalysisHost exactly as the
// desktop and Review hosts drive it: stored sources resolve to the F-source/2
// columnar handle and the host lends chunked RecordingColumns.
//
//   1. Parity: on a small file the columnar handle and the F-source/1 rows give
//      the same evaluation, histogram and scatter (one evaluator, two sources).
//   2. A saved strategy (interval + polygon + Boolean, table, histogram) on
//      file A: report rows, plot and evaluation equal an independent oracle
//      computed here from the generator (exact counts, n, mean, median,
//      p05/p95 type 7, histogram counts).
//   3. The strategy applied to file B (pinned template), both sources
//      retained as file-pinned handles, and a descriptive comparison of the
//      two recordings; file A is no longer open, so its columns come from a
//      short-lived reader verified by full-file SHA-256. Endpoint values and
//      the difference equal the oracle.
//   4. Memory: ReviewSession open of a 255k-event file stays under 24 MiB of
//      RSS growth (browse metadata is paged), and the whole save + report +
//      plot + retain + apply + compare sequence stays under 64 MiB of peak
//      RSS growth (VmHWM after clear_refs).
#include "backend/analysis/AnalysisHost.h"
#include "backend/analysis/SourceAnalysis.h"
#include "backend/database/SqliteService.h"
#include "backend/processing/ProcessingService.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/review/ReviewSession.h"
#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <malloc.h>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using backend::services::Hdf5Service;
using backend::services::ProcessedFrame;
using Json = nlohmann::json;

namespace {
std::uint64_t mix(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}
struct Event {
    double area, deformability, youngsModulus;
};
// Independent of MIB code. `salt` makes the two recordings differ.
Event generate(std::uint64_t salt, std::uint64_t i) {
    const auto k = i * 0x100000001B3ULL + salt;
    Event e;
    e.area = 40.0 + static_cast<double>(mix(k) % 12000) / 50.0 + static_cast<double>(salt % 7);
    e.deformability = static_cast<double>(mix(k ^ 0xABCDEF) % 3000) / 10000.0;
    e.youngsModulus = 0.5 + static_cast<double>(mix(k * 7 + 1) % 400) / 100.0;
    if (i % 89 == 3) e.deformability = std::numeric_limits<double>::quiet_NaN();
    if (i % 11 == 4) e.youngsModulus = 0.0; // not computed -> missing
    return e;
}
enum class T { F, U, Tr };
T tAnd(T a, T b) { return a == T::F || b == T::F ? T::F : a == T::U || b == T::U ? T::U : T::Tr; }
const std::array<std::array<double, 2>, 4> kPolygon{
    {{{100.0, 0.05}}, {{180.0, 0.08}}, {{160.0, 0.25}}, {{90.0, 0.20}}}};
T polygon(double x, double y) {
    if (!std::isfinite(x) || !std::isfinite(y)) return T::U;
    for (std::size_t k = 0; k < kPolygon.size(); ++k) {
        const auto& a = kPolygon[k];
        const auto& b = kPolygon[(k + 1) % kPolygon.size()];
        if ((b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0]) < 0) return T::F;
    }
    return T::Tr;
}
T interval(double v, double lo, double hi) {
    if (!std::isfinite(v)) return T::U;
    return v >= lo && v <= hi ? T::Tr : T::F;
}
struct Recording {
    std::uint64_t salt, valid, invalid;
    std::uint64_t events() const { return valid + invalid; }
};
// Oracle: per population, tri-state counts and the sorted selected areas.
struct PopulationOracle {
    std::uint64_t selected{0}, unknown{0}, excluded{0};
    std::vector<double> areas;
};
std::map<std::string, PopulationOracle> oracle(const Recording& r) {
    std::map<std::string, PopulationOracle> o;
    for (std::uint64_t i = 0; i < r.events(); ++i) {
        const auto e = generate(r.salt, i);
        const double ym = e.youngsModulus > 0 ? e.youngsModulus : std::numeric_limits<double>::quiet_NaN();
        const T size = interval(e.area, 80.0, 200.0);
        const T shape = tAnd(size, polygon(e.area, e.deformability));
        const T stiff = interval(ym, 1.0, 3.0);
        const T core = tAnd(shape, stiff);
        for (const auto& [id, t] : std::vector<std::pair<std::string, T>>{
                 {"size", size}, {"shape", shape}, {"stiff", stiff}, {"core", core}}) {
            auto& p = o[id];
            if (t == T::Tr) {
                ++p.selected;
                p.areas.push_back(e.area);
            } else if (t == T::U) {
                ++p.unknown;
            } else {
                ++p.excluded;
            }
        }
    }
    for (auto& [id, p] : o) std::sort(p.areas.begin(), p.areas.end());
    return o;
}
double meanOf(const std::vector<double>& v) {
    long double sum = 0;
    for (const auto x : v) sum += x;
    return static_cast<double>(sum / v.size());
}
double quantile(const std::vector<double>& v, double p) {
    const double pos = (v.size() - 1) * p;
    const auto lo = static_cast<std::size_t>(pos);
    const auto hi = std::min(lo + 1, v.size() - 1);
    return v[lo] + (pos - lo) * (v[hi] - v[lo]);
}
bool near(const Json& value, double expected) {
    return value.is_number() && std::abs(value.get<double>() - expected) <= 1e-9 * std::max(1.0, std::abs(expected));
}
void writeFile(const std::string& path, const Recording& r) {
    Hdf5Service hdf5;
    MIB_REQUIRE(hdf5.openFile(path), "create experiment file");
    MIB_REQUIRE(hdf5.initializeDatasets(), "initialize datasets");
    constexpr std::uint64_t kBatch = 20000;
    const auto frame = [&](std::uint64_t i, bool valid) {
        ProcessedFrame f;
        const auto e = generate(r.salt, i);
        f.index = i;
        f.timestampNs = (i + 1) * 1000;
        f.validation.isValid = valid;
        f.validation.objectId = 0;
        f.validation.objectCount = 1;
        f.validation.area = e.area;
        f.validation.deformability = e.deformability;
        f.validation.youngsModulus = e.youngsModulus;
        return f;
    };
    for (std::uint64_t begin = 0; begin < r.valid; begin += kBatch) {
        std::vector<ProcessedFrame> valid;
        for (auto i = begin; i < std::min(r.valid, begin + kBatch); ++i) valid.push_back(frame(i, true));
        MIB_REQUIRE(hdf5.appendFrames(valid, {}), "append valid batch");
    }
    for (std::uint64_t begin = 0; begin < r.invalid; begin += kBatch) {
        std::vector<ProcessedFrame> invalid;
        for (auto i = begin; i < std::min(r.invalid, begin + kBatch); ++i)
            invalid.push_back(frame(r.valid + i, false));
        MIB_REQUIRE(hdf5.appendFrames({}, invalid), "append invalid batch");
    }
    backend::services::ProcessingConfig cfg;
    backend::services::ProcessingService::Roi roi{0, 0, 512, 96};
    MIB_REQUIRE(hdf5.writeExperimentInfo(1000, 2000, r.valid, r.invalid, cfg, roi), "experiment info");
    hdf5.closeFile();
}
Json graph() {
    Json vertices = Json::array();
    for (const auto& v : kPolygon) vertices.push_back(Json::array({v[0], v[1]}));
    return Json{{"id", "scale"},
                {"revision", "1"},
                {"output", "core"},
                {"nodes",
                 Json::array({Json{{"id", "size"}, {"parent", ""}, {"kind", "interval"}, {"feature", "area"},
                                   {"minimum", 80.0}, {"maximum", 200.0}},
                              Json{{"id", "shape"}, {"parent", "size"}, {"kind", "polygon"}, {"x", "area"},
                                   {"y", "deformability"}, {"vertices", vertices}},
                              Json{{"id", "stiff"}, {"parent", ""}, {"kind", "interval"},
                                   {"feature", "youngsModulus"}, {"minimum", 1.0}, {"maximum", 3.0}},
                              Json{{"id", "core"}, {"parent", ""}, {"kind", "and"},
                                   {"operands", {"shape", "stiff"}}}})}};
}
long statusKb(const char* field) {
    std::ifstream status("/proc/self/status");
    std::string line;
    const std::string prefix = std::string(field) + ":";
    while (std::getline(status, line))
        if (line.rfind(prefix, 0) == 0) return std::stol(line.substr(prefix.size()));
    return -1;
}
bool resetPeak() {
    std::ofstream clear("/proc/self/clear_refs");
    clear << "5";
    clear.flush();
    return static_cast<bool>(clear);
}
// Fields that legitimately differ between the row and the columnar form.
Json withoutTransport(Json value) {
    for (const char* key : {"rows", "membershipStorage", "events"}) value.erase(key);
    return value;
}
} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    mib::test::Watchdog wd(280);
    mib::test::TempDir td("f1b_workspace");
    backend::review::ReviewSession session;
    backend::services::SqliteService db;
    MIB_REQUIRE(db.initialize((td.path() / "app.sqlite3").string()), "metadata authority");
    // The hosts' resolvers (BackendFacade / review_shim), on the session.
    auto source = [&](const Json& descriptor) -> Json {
        Json lookup{{"expectedPath", descriptor.at("path")}, {"feature", "area"}, {"minimum", nullptr},
                    {"maximum", nullptr}, {"snapshot", true}, {"sourceKind", "stored"}, {"columnar", true}};
        if (descriptor.at("kind") == "stored-gallery") {
            lookup.erase("columnar");
            lookup["galleryKeys"] = descriptor.at("keys");
            lookup["expectedFingerprint"] = descriptor.at("fingerprint");
        }
        const auto reply = Json::parse(session.analyzePopulation(lookup.dump()));
        if (!reply.value("ok", false)) throw std::invalid_argument(reply.value("message", "refused"));
        return reply.at("source");
    };
    std::uint64_t lends = 0;
    auto columns = [&](const Json& handle, const backend::analysis::ColumnVisitor& use) {
        ++lends;
        session.withRecordingColumns(handle.at("path").get<std::string>(),
                                     handle.at("fingerprint").get<std::string>(), use);
    };
    auto call = [&](const Json& q, bool write) {
        return Json::parse(backend::analysis::analysisRequest(db, q.dump(), write, source, {}, columns));
    };

    // ---- 1. parity: rows vs columnar handle on a small file ----------------
    {
        const Recording small{3, 1800, 400};
        const auto path = (td.path() / "small.h5").string();
        writeFile(path, small);
        MIB_REQUIRE(session.open(path), "open small file");
        const auto rows = Json::parse(session.analyzePopulation(
            Json{{"expectedPath", path}, {"feature", "area"}, {"minimum", nullptr}, {"maximum", nullptr},
                 {"snapshot", true}}.dump()));
        MIB_REQUIRE(rows.value("ok", false) && rows.at("source").at("schema") == "F-source/1", "row form");
        const auto handle = source(Json{{"kind", "stored"}, {"path", path}});
        MIB_EXPECT(handle.at("schema") == "F-source/2" && !handle.contains("rows") &&
                       handle.at("events") == std::to_string(small.events()),
                   "stored source resolves to a columnar handle without rows");
        for (const char* key : {"fingerprint", "sourceKey", "parameters", "scope"})
            MIB_EXPECT(handle.at(key) == rows.at("source").at(key),
                       std::string("binding field identical in both forms: ") + key);
        Json strategy{{"graph", graph()},
                      {"feature", "area"},
                      {"transforms", Json::array({Json{{"input", "area"}, {"inputUnit", "pixel2"}, {"output", "logArea"},
                                                       {"kind", "log10"}, {"scale", 1.0}, {"offset", 0.0}}})},
                      {"qc", {{"policy", "good-only"},
                              {"decisions", Json::array({Json{{"key", backend::analysis::columnarEventKey(
                                                                          handle.at("sourceKey").get<std::string>(),
                                                                          "valid", 5)},
                                                              {"status", "good"}, {"reason", "checked"},
                                                              {"actor", "test"}}})}}}};
        for (const auto& q : {strategy, [&] { auto all = strategy; all.erase("qc"); return all; }()}) {
            const auto a = backend::analysis::evaluateSource(rows.at("source"), q);
            const auto b = backend::analysis::evaluateSource(handle, q, columns);
            MIB_EXPECT(withoutTransport(a) == withoutTransport(b) && a.contains("rows") && !b.contains("rows"),
                       "evaluation identical for rows and columnar handle");
            auto plot = q;
            plot["plot"] = {{"kind", "histogram"}, {"feature", "logArea"}, {"minimum", 1.8}, {"maximum", 2.5},
                            {"bins", 40u}, {"normalization", "density"},
                            {"layers", Json::array({Json{{"population", "size"}, {"displayOffset", 0}},
                                                    Json{{"population", "core"}, {"displayOffset", 1}}})}};
            MIB_EXPECT(backend::analysis::plotSource(rows.at("source"), plot) ==
                           backend::analysis::plotSource(handle, plot, columns),
                       "histogram identical for rows and columnar handle");
            plot["plot"] = {{"kind", "scatter"}, {"x", "area"}, {"y", "deformability"}, {"seed", 7u},
                            {"maxPoints", 50u}, {"layers", Json::array({Json{{"population", "shape"}}})}};
            MIB_EXPECT(backend::analysis::plotSource(rows.at("source"), plot) ==
                           backend::analysis::plotSource(handle, plot, columns),
                       "seeded scatter sample identical for rows and columnar handle");
        }
        session.close();
    }
    wd.mark("parity");

    // ---- 2. saved strategy + report + plot on recording A -----------------
    const Recording recA{11, 200000, 55000}, recB{29, 210000, 50000};
    const auto pathA = (td.path() / "a.h5").string(), pathB = (td.path() / "b.h5").string();
    writeFile(pathA, recA);
    writeFile(pathB, recB);
    const auto oracleA = oracle(recA), oracleB = oracle(recB);
    wd.mark("files + oracle");
    std::printf("F-1b: A %llu events, B %llu events, %.1f / %.1f MiB\n",
                static_cast<unsigned long long>(recA.events()), static_cast<unsigned long long>(recB.events()),
                std::filesystem::file_size(pathA) / 1048576.0, std::filesystem::file_size(pathB) / 1048576.0);

    malloc_trim(0); // freed writer/oracle memory back to the OS: growth is real growth
    const long beforeOpen = statusKb("VmRSS");
    MIB_REQUIRE(session.open(pathA), "open recording A");
    const double openMiB = (statusKb("VmRSS") - beforeOpen) / 1024.0;
    std::printf("F-1b: ReviewSession open of %llu events: RSS +%.1f MiB (paged browse metadata)\n",
                static_cast<unsigned long long>(recA.events()), openMiB);
    MIB_EXPECT(openMiB < 24.0, "open stays under 24 MiB: browse metadata is paged, not held");
    // Browse features still work over the pages.
    std::vector<backend::review::MetricRow> page;
    std::uint64_t total = 0;
    MIB_EXPECT(session.metricsPage(true, 150000, 5, page, total) && total == recA.valid && page.size() == 5 &&
                   page[0].frameIndex == 150000,
               "metrics page deep in the valid set");
    backend::services::ProcessedFrame row;
    MIB_EXPECT(session.frameMeta(false, recA.invalid - 1, row) && row.index == recA.events() - 1 &&
                   row.validation.area == generate(recA.salt, recA.events() - 1).area,
               "last invalid row read on demand");
    MIB_EXPECT(session.scatter().frameIndex.size() == recA.valid, "valid-set scatter streamed page by page");

    malloc_trim(0);
    const bool peak = resetPeak();
    const long baseline = statusKb(peak ? "VmHWM" : "VmRSS");
    const Json descriptorA{{"kind", "stored"}, {"path", pathA}};
    const auto snapshot = call(Json{{"action", "snapshot"}, {"source", descriptorA}}, false);
    MIB_REQUIRE(snapshot.at("status") == "ok", snapshot.dump().substr(0, 300));
    Json binding = Json::object();
    for (const char* k : {"fingerprint", "sourceKey", "parameters", "scope"}) binding[k] = snapshot["source"][k];
    auto strategyGraph = graph();
    strategyGraph["id"] = "planA";
    const Json save{{"action", "save"},
                    {"planId", "planA"},
                    {"expectedRevision", nullptr},
                    {"commandId", "save-a"},
                    {"feature", "area"},
                    {"graph", strategyGraph},
                    {"source", descriptorA},
                    {"sourceBinding", binding},
                    {"table", {{"populations", {"size", "shape", "stiff", "core"}},
                               {"columns", {"population", "feature", "unit", "selected", "excluded", "unknown",
                                            "measured", "mean", "median", "p05", "p95", "denominator"}}}},
                    {"plot", {{"kind", "histogram"}, {"feature", "area"}, {"minimum", 80.0}, {"maximum", 200.0},
                              {"bins", 48u}, {"normalization", "count"},
                              {"layers", Json::array({Json{{"population", "size"}, {"displayOffset", 0}},
                                                      Json{{"population", "core"}, {"displayOffset", 0}}})}}}};
    const auto saved = call(save, true);
    MIB_REQUIRE(saved.value("revision", std::string{}) == "1", saved.dump().substr(0, 400));
    const Json pinA{{"planId", "planA"}, {"revision", "1"}, {"source", descriptorA}};
    auto request = pinA;
    request["action"] = "report";
    const auto report = call(request, false);
    MIB_REQUIRE(report.at("status") == "ok", report.dump().substr(0, 400));
    bool reportOk = report.at("report").at("table").size() == 4;
    for (const auto& r : report.at("report").at("table")) {
        const auto& o = oracleA.at(r.at("population").get<std::string>());
        reportOk = reportOk && r.at("selected") == std::to_string(o.selected) &&
                   r.at("unknown") == std::to_string(o.unknown) && r.at("excluded") == std::to_string(o.excluded) &&
                   r.at("measured") == std::to_string(o.areas.size()) && near(r.at("mean"), meanOf(o.areas)) &&
                   near(r.at("median"), quantile(o.areas, 0.5)) && near(r.at("p05"), quantile(o.areas, 0.05)) &&
                   near(r.at("p95"), quantile(o.areas, 0.95)) &&
                   r.at("denominator") == std::to_string(recA.events()) && r.at("unit") == "pixel2";
        std::printf("  A %-6s selected %7s unknown %6s mean %s\n", r.at("population").get<std::string>().c_str(),
                    r.at("selected").get<std::string>().c_str(), r.at("unknown").get<std::string>().c_str(),
                    r.at("mean").dump().c_str());
    }
    MIB_EXPECT(reportOk, "report over 255k events equals the oracle (counts, n, mean, median, p05, p95)");
    request["action"] = "plot";
    const auto plot = call(request, false);
    MIB_REQUIRE(plot.at("status") == "ok", plot.dump().substr(0, 400));
    bool plotOk = true;
    for (const auto& layer : plot.at("plot").at("layers")) {
        const auto& o = oracleA.at(layer.at("population").get<std::string>());
        std::vector<std::uint64_t> expected(48, 0);
        const auto& edges = plot.at("plot").at("edges");
        for (const auto a : o.areas) {
            if (a < 80.0 || a > 200.0) continue;
            std::size_t bin = 0;
            while (bin + 1 < 48 && a >= edges[bin + 1].get<double>()) ++bin;
            ++expected[bin];
        }
        for (std::size_t b = 0; b < 48; ++b)
            plotOk = plotOk && layer.at("counts")[b] == std::to_string(expected[b]);
        plotOk = plotOk && layer.at("measured") == std::to_string(o.areas.size());
    }
    MIB_EXPECT(plotOk, "saved histogram bins equal the oracle's bins for every layer");
    request["action"] = "gallery";
    request["offset"] = oracleA.at("core").selected - 3;
    const auto gallery = call(request, false);
    MIB_EXPECT(gallery.value("status", std::string{}) == "ok" && gallery.at("images").size() == 3,
               "gallery page at the last selected ranks of 255k events");
    wd.mark("strategy A");

    // ---- 3. retain A, apply to B, retain B, compare (A no longer open) ----
    const auto retainedA = call(Json{{"action", "retainSource"}, {"source", descriptorA}, {"commandId", "keep-a"}}, true);
    MIB_REQUIRE(retainedA.value("status", std::string{}) == "ok", retainedA.dump().substr(0, 300));
    MIB_EXPECT(retainedA.at("source").at("schema") == "F-source/2" &&
                   retainedA.at("source").dump().size() < 4096,
               "a retained recording is a small file-pinned handle, not copied rows");
    MIB_REQUIRE(session.open(pathB), "open recording B");
    const Json descriptorB{{"kind", "stored"}, {"path", pathB}};
    const auto snapshotB = call(Json{{"action", "snapshot"}, {"source", descriptorB}}, false);
    Json bindingB = Json::object();
    for (const char* k : {"fingerprint", "sourceKey", "parameters", "scope"}) bindingB[k] = snapshotB["source"][k];
    const auto applied = call(Json{{"action", "apply"},
                                   {"planId", "planB"},
                                   {"expectedRevision", nullptr},
                                   {"commandId", "apply-b"},
                                   {"application", {{"planId", "planA"}, {"revision", "1"}, {"overrides", Json::array()}}},
                                   {"source", descriptorB},
                                   {"sourceBinding", bindingB}},
                              true);
    MIB_REQUIRE(applied.value("revision", std::string{}) == "1", applied.dump().substr(0, 400));
    const auto retainedB = call(Json{{"action", "retainSource"}, {"source", descriptorB}, {"commandId", "keep-b"}}, true);
    MIB_REQUIRE(retainedB.value("status", std::string{}) == "ok", retainedB.dump().substr(0, 300));
    const Json spec{{"conditions", {"A", "B"}},
                    {"design", "independent"},
                    {"weighting", "equal-experimental-unit"},
                    {"descriptiveOnly", true},
                    {"endpoint", {{"feature", "area"}, {"statistic", "mean"}, {"unit", "pixel2"}}},
                    {"samples", Json::array({Json{{"condition", "A"}, {"experimentalUnit", "run-a"},
                                                  {"planId", "planA"}, {"revision", "1"},
                                                  {"source", retainedA.at("descriptor")}},
                                             Json{{"condition", "B"}, {"experimentalUnit", "run-b"},
                                                  {"planId", "planB"}, {"revision", "1"},
                                                  {"source", retainedB.at("descriptor")}}})}};
    const auto lendsBefore = lends;
    const auto compared = call(Json{{"action", "compare"}, {"spec", spec}}, false);
    MIB_REQUIRE(compared.value("status", std::string{}) == "ok", compared.dump().substr(0, 500));
    const auto& samples = compared.at("comparison").at("samples");
    const double meanA = meanOf(oracleA.at("core").areas), meanB = meanOf(oracleB.at("core").areas);
    MIB_EXPECT(samples.size() == 2 && near(samples[0].at("value"), meanA) && near(samples[1].at("value"), meanB) &&
                   samples[0].at("selected") == std::to_string(oracleA.at("core").selected) &&
                   samples[1].at("selected") == std::to_string(oracleB.at("core").selected),
               "comparison endpoints over two full recordings equal the oracle");
    MIB_EXPECT(near(compared.at("comparison").at("differenceRightMinusLeft"),
                    static_cast<double>(static_cast<long double>(meanB) - meanA)),
               "comparison difference equals the oracle");
    MIB_EXPECT(lends - lendsBefore == 2, "each recording lent once per comparison, no rows materialized");
    std::printf("F-1b: comparison mean(core area) A %.9f, B %.9f\n", meanA, meanB);
    // The retained handle refuses a file that changed (fingerprint re-verified).
    session.close();
    const double peakMiB = (statusKb(peak ? "VmHWM" : "VmRSS") - baseline) / 1024.0;
    std::printf("F-1b: save+report+plot+gallery+retain+apply+compare peak RSS growth %.1f MiB (%s)\n", peakMiB,
                peak ? "VmHWM after clear_refs" : "VmRSS delta");
    MIB_EXPECT(peakMiB < 64.0, "the whole workspace sequence over 2 x 255k+ events stays under 64 MiB peak growth");
    {
        std::ofstream append(pathA, std::ios::binary | std::ios::app);
        append << "tamper";
    }
    const auto stale = call(Json{{"action", "evaluate"}, {"planId", "planA"}, {"revision", "1"},
                                 {"source", retainedA.at("descriptor")}},
                            false);
    MIB_EXPECT(stale.value("status", std::string{}) == "invalid" &&
                   stale.value("message", std::string{}).find("fingerprint") != std::string::npos,
               "a retained file-pinned handle refuses a changed file, never reuses results");
    wd.mark("compare");
    return mib::test::exitCode();
}
