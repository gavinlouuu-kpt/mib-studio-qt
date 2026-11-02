// F-1 (F01/F04/F05/F08/F16): a full-size MIB experiment file through the
// native analysis authority, headless.
//
// The test writes a 300,000-event experiment file with the production writer
// (Hdf5Service: /valid_frames/metadata and /invalid_frames/metadata, the real
// compound schema, imageless PL-style rows), opens it in ReviewSession and
// analyses it through review_analyze_population's C++ entry point. Interval,
// polygon and Boolean populations (with missing values, so Unknown is
// exercised under NOT) are checked against an independent oracle computed
// here from the generator, not from the file or the evaluator:
//   - exact selected/excluded/unknown counts at every population;
//   - exact n, mean, median, p05/p95 of the reduction;
//   - histogram counts conserve n; decimated scatter is bounded;
//   - a page of selected event references and a deep gallery page agree with
//     the oracle's rank order;
//   - the membership handle is reused (cached) for the same source+graph;
//   - the streaming SHA-256 fingerprints a file over 64 MiB and equals an
//     independent whole-buffer hash;
//   - the columnar reader never reads more than its chunk bound, and the
//     analysis' peak RSS growth (VmHWM after clear_refs) stays under 64 MiB.
#include "backend/analysis/ColumnarEvaluation.h"
#include "backend/analysis/PopulationGates.h"
#include "backend/processing/ProcessingCoreSha256.h"
#include "backend/processing/ProcessingService.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/review/RecordingColumns.h"
#include "backend/review/ReviewSession.h"
#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using backend::services::Hdf5Service;
using backend::services::ProcessedFrame;
using Json = nlohmann::json;

namespace {
constexpr std::uint64_t kValid = 240000;
constexpr std::uint64_t kInvalid = 60000;
constexpr std::uint64_t kEvents = kValid + kInvalid;

// Deterministic generator (splitmix64), independent of any MIB code.
std::uint64_t mix(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}
struct Event {
    double area, deformability, youngsModulus;
};
// Global event index: valid set first, then invalid (the evaluator's order).
Event generate(std::uint64_t i) {
    Event e;
    e.area = 40.0 + static_cast<double>(mix(i) % 12000) / 50.0; // 40 .. 279.98
    e.deformability = static_cast<double>(mix(i ^ 0xABCDEF) % 3000) / 10000.0;
    e.youngsModulus = 0.5 + static_cast<double>(mix(i * 7 + 1) % 400) / 100.0;
    if (i % 97 == 5) e.deformability = std::numeric_limits<double>::quiet_NaN();
    if (i % 13 == 2) e.youngsModulus = 0.0; // not computed -> missing
    if (i % 4999 == 17) e.area = 200.0;     // inclusive interval edge
    return e;
}

enum class T { F, U, Tr };
T tAnd(T a, T b) { return a == T::F || b == T::F ? T::F : a == T::U || b == T::U ? T::U : T::Tr; }
T tOr(T a, T b) { return a == T::Tr || b == T::Tr ? T::Tr : a == T::U || b == T::U ? T::U : T::F; }
T tNot(T a) { return a == T::U ? T::U : a == T::Tr ? T::F : T::Tr; }

// The polygon (area, deformability), counter-clockwise and convex: a point is
// inside or on the boundary iff it is left of or on every directed edge.
const std::array<std::array<double, 2>, 4> kPolygon{
    {{{100.0, 0.05}}, {{180.0, 0.08}}, {{160.0, 0.25}}, {{90.0, 0.20}}}};
T oraclePolygon(double x, double y) {
    if (!std::isfinite(x) || !std::isfinite(y)) return T::U;
    for (std::size_t k = 0; k < kPolygon.size(); ++k) {
        const auto& a = kPolygon[k];
        const auto& b = kPolygon[(k + 1) % kPolygon.size()];
        if ((b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0]) < 0) return T::F;
    }
    return T::Tr;
}
T oracleInterval(double v, double lo, double hi) {
    if (!std::isfinite(v)) return T::U;
    return v >= lo && v <= hi ? T::Tr : T::F;
}
struct Oracle {
    std::map<std::string, std::vector<T>> m;
};
Oracle oracle() {
    Oracle o;
    for (const char* id : {"size", "shape", "stiff", "core", "notShape", "output"})
        o.m[id].resize(kEvents);
    for (std::uint64_t i = 0; i < kEvents; ++i) {
        const auto e = generate(i);
        const double ym = e.youngsModulus > 0 ? e.youngsModulus : std::numeric_limits<double>::quiet_NaN();
        const T size = oracleInterval(e.area, 80.0, 200.0);
        const T shape = tAnd(size, oraclePolygon(e.area, e.deformability));
        const T stiff = oracleInterval(ym, 1.0, 3.0);
        const T core = tAnd(shape, stiff);
        const T notShape = tAnd(size, tNot(shape));
        const T output = tOr(core, notShape);
        o.m["size"][i] = size;
        o.m["shape"][i] = shape;
        o.m["stiff"][i] = stiff;
        o.m["core"][i] = core;
        o.m["notShape"][i] = notShape;
        o.m["output"][i] = output;
    }
    return o;
}
Json graph() {
    Json vertices = Json::array();
    for (const auto& v : kPolygon) vertices.push_back(Json::array({v[0], v[1]}));
    return Json{{"id", "scale"},
                {"revision", "1"},
                {"output", "output"},
                {"nodes",
                 Json::array({Json{{"id", "size"}, {"parent", ""}, {"kind", "interval"}, {"feature", "area"},
                                   {"minimum", 80.0}, {"maximum", 200.0}},
                              Json{{"id", "shape"}, {"parent", "size"}, {"kind", "polygon"}, {"x", "area"},
                                   {"y", "deformability"}, {"vertices", vertices}},
                              Json{{"id", "stiff"}, {"parent", ""}, {"kind", "interval"},
                                   {"feature", "youngsModulus"}, {"minimum", 1.0}, {"maximum", 3.0}},
                              Json{{"id", "core"}, {"parent", ""}, {"kind", "and"}, {"operands", {"shape", "stiff"}}},
                              Json{{"id", "notShape"}, {"parent", "size"}, {"kind", "not"}, {"operands", {"shape"}}},
                              Json{{"id", "output"}, {"parent", ""}, {"kind", "or"},
                                   {"operands", {"core", "notShape"}}}})}};
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

void writeFile(const std::string& path) {
    Hdf5Service hdf5;
    MIB_REQUIRE(hdf5.openFile(path), "create experiment file");
    MIB_REQUIRE(hdf5.initializeDatasets(), "initialize datasets");
    constexpr std::uint64_t kBatch = 20000;
    const auto frame = [](std::uint64_t i, bool valid) {
        ProcessedFrame f;
        const auto e = generate(i);
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
    for (std::uint64_t begin = 0; begin < kValid; begin += kBatch) {
        std::vector<ProcessedFrame> valid;
        for (auto i = begin; i < std::min(kValid, begin + kBatch); ++i) valid.push_back(frame(i, true));
        MIB_REQUIRE(hdf5.appendFrames(valid, {}), "append valid batch");
    }
    for (std::uint64_t begin = 0; begin < kInvalid; begin += kBatch) {
        std::vector<ProcessedFrame> invalid;
        for (auto i = begin; i < std::min(kInvalid, begin + kBatch); ++i)
            invalid.push_back(frame(kValid + i, false));
        MIB_REQUIRE(hdf5.appendFrames({}, invalid), "append invalid batch");
    }
    backend::services::ProcessingConfig cfg;
    backend::services::ProcessingService::Roi roi{0, 0, 512, 96};
    MIB_REQUIRE(hdf5.writeExperimentInfo(1000, 2000, kValid, kInvalid, cfg, roi), "experiment info");
    hdf5.closeFile();
}
} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0); // results visible even on a REQUIRE exit
    mib::test::Watchdog wd(240);
    mib::test::TempDir td("f1_scale");
    const auto path = (td.path() / "f1_scale.h5").string();
    writeFile(path);
    wd.mark("file written");
    const auto fileBytes = std::filesystem::file_size(path);
    std::printf("F-1 scale: %llu events (%llu valid, %llu invalid), file %.1f MiB\n",
                static_cast<unsigned long long>(kEvents), static_cast<unsigned long long>(kValid),
                static_cast<unsigned long long>(kInvalid), fileBytes / 1048576.0);
    MIB_EXPECT(fileBytes > 64ULL * 1024 * 1024, "the proof file is above the former 64 MiB cap");

    const auto expected = oracle();
    const auto count = [&](const std::string& id, T value) {
        return static_cast<std::uint64_t>(std::count(expected.m.at(id).begin(), expected.m.at(id).end(), value));
    };
    wd.mark("oracle");

    // 1. The chunked reader + evaluator directly, at a small chunk bound.
    {
        Hdf5Service reader;
        MIB_REQUIRE(reader.loadFile(path), "reader opens the file");
        std::vector<std::string> validFields, invalidFields;
        MIB_REQUIRE(reader.readMetadataFieldNames(true, validFields) &&
                        reader.readMetadataFieldNames(false, invalidFields),
                    "schema field names");
        backend::review::RecordingColumns columns(reader, kValid, kInvalid, validFields, invalidFields);
        backend::analysis::ResourceBounds bounds;
        bounds.chunkEvents = 4096;
        const auto compiled = backend::analysis::compileGraph(graph());
        const backend::analysis::RevisionKey rev{"s", "f", "p", "a", "o", "p", "q", "m", "e"};
        const auto set = backend::analysis::evaluateColumns(compiled, columns, rev, nullptr, bounds);
        MIB_EXPECT(columns.largestRead() <= 4096 && set->largestChunk <= 4096,
                   "no HDF5 read or evaluation step exceeds the 4096-event chunk");
        MIB_EXPECT(columns.readsIssued() == (kValid + 4095) / 4096 + (kInvalid + 4095) / 4096,
                   "one bounded pass per set");
        for (const auto& id : {"size", "shape", "stiff", "core", "notShape", "output"}) {
            const auto& bits = set->population(id);
            MIB_EXPECT(bits.count(backend::analysis::Truth::True) == count(id, T::Tr) &&
                           bits.count(backend::analysis::Truth::Unknown) == count(id, T::U) &&
                           bits.count(backend::analysis::Truth::False) == count(id, T::F),
                       std::string("exact tri-state counts at chunk 4096: ") + id);
        }
        reader.closeFile();
    }
    wd.mark("direct columnar");

    std::vector<std::uint64_t> selected;
    for (std::uint64_t i = 0; i < kEvents; ++i)
        if (expected.m.at("output")[i] == T::Tr) selected.push_back(i);
    MIB_REQUIRE(selected.size() > 20000, "oracle output population is large");
    // A gallery page deep in the selected events, far beyond the former 10000 cap.
    const std::uint64_t deep = selected.size() - 20;

    // 2. The product entry point (review_analyze_population's C++ authority).
    backend::review::ReviewSession session;
    const long beforeOpenKb = statusKb("VmRSS");
    MIB_REQUIRE(session.open(path), "ReviewSession opens the full file");
    const long afterOpenKb = statusKb("VmRSS");
    std::printf("F-1 scale: ReviewSession open RSS +%.1f MiB (browse metadata, not analysis)\n",
                (afterOpenKb - beforeOpenKb) / 1024.0);
    Json request{{"expectedPath", path}, {"feature", "area"}, {"minimum", nullptr}, {"maximum", nullptr},
                 {"graph", graph()},
                 {"histogram", {{"minimum", 80.0}, {"maximum", 200.0}, {"bins", 60}}},
                 {"scatter", {{"x", "area"}, {"y", "deformability"}, {"maxPoints", 2000}}},
                 {"page", {{"offset", 1000}, {"limit", 50}, {"membership", "selected"}}},
                 {"galleryOffset", deep}};
    const bool peakReset = resetPeak();
    const long baselineKb = statusKb(peakReset ? "VmHWM" : "VmRSS");
    const auto reply = Json::parse(session.analyzePopulation(request.dump()));
    const long peakKb = statusKb(peakReset ? "VmHWM" : "VmRSS");
    MIB_REQUIRE(reply.at("ok").get<bool>(), reply.dump().substr(0, 400));
    const double growthMiB = (peakKb - baselineKb) / 1024.0;
    std::printf("F-1 scale: analysis peak RSS growth %.1f MiB (%s), membership %s bytes, %s chunks\n",
                growthMiB, peakReset ? "VmHWM after clear_refs" : "VmRSS delta",
                reply.at("evaluation").at("membershipBytes").get<std::string>().c_str(),
                reply.at("evaluation").at("chunksRead").get<std::string>().c_str());
    MIB_EXPECT(growthMiB < 64.0, "analysis of 300k events stays under 64 MiB peak growth");
    MIB_EXPECT(reply.dump().size() < 512 * 1024, "only bounded results cross the bridge");
    MIB_EXPECT(reply.at("valid") == std::to_string(kValid) && reply.at("invalid") == std::to_string(kInvalid),
               "both sets counted");
    MIB_EXPECT(reply.at("selected") == std::to_string(count("output", T::Tr)) &&
                   reply.at("unknown") == std::to_string(count("output", T::U)) &&
                   reply.at("excluded") == std::to_string(count("output", T::F)),
               "output population exact against the oracle");
    for (const auto& p : reply.at("populations")) {
        const auto id = p.at("id").get<std::string>();
        MIB_EXPECT(p.at("selected") == std::to_string(count(id, T::Tr)) &&
                       p.at("unknown") == std::to_string(count(id, T::U)) &&
                       p.at("excluded") == std::to_string(count(id, T::F)),
                   "population table exact: " + id);
        std::printf("  %-9s selected %7s unknown %6s excluded %7s\n", id.c_str(),
                    p.at("selected").get<std::string>().c_str(), p.at("unknown").get<std::string>().c_str(),
                    p.at("excluded").get<std::string>().c_str());
    }
    // Reduction oracle: every selected area, exact.
    std::vector<double> areas;
    for (std::uint64_t i = 0; i < kEvents; ++i)
        if (expected.m.at("output")[i] == T::Tr) areas.push_back(generate(i).area);
    std::sort(areas.begin(), areas.end());
    long double sum = 0;
    for (const auto a : areas) sum += a;
    const double mean = static_cast<double>(sum / areas.size());
    const auto q = [&](double p) {
        const double pos = (areas.size() - 1) * p;
        const auto lo = static_cast<std::size_t>(pos);
        const auto hi = std::min(lo + 1, areas.size() - 1);
        return areas[lo] + (pos - lo) * (areas[hi] - areas[lo]);
    };
    MIB_EXPECT(reply.at("measured") == std::to_string(areas.size()) && reply.at("missing") == "0",
               "every selected area measured");
    MIB_EXPECT(std::abs(reply.at("mean").get<double>() - mean) <= 1e-9 * std::abs(mean), "exact mean");
    MIB_EXPECT(std::abs(reply.at("median").get<double>() - q(0.5)) <= 1e-9, "exact median");
    MIB_EXPECT(std::abs(reply.at("p05").get<double>() - q(0.05)) <= 1e-9 &&
                   std::abs(reply.at("p95").get<double>() - q(0.95)) <= 1e-9,
               "exact type-7 percentiles");
    std::printf("F-1 scale: output n=%zu mean=%.6f median=%.6f\n", areas.size(), mean, q(0.5));
    // Histogram conserves n; scatter is bounded and display-only.
    std::uint64_t binned = std::stoull(reply.at("histogram").at("underflow").get<std::string>()) +
                           std::stoull(reply.at("histogram").at("overflow").get<std::string>());
    for (const auto& c : reply.at("histogram").at("counts")) binned += std::stoull(c.get<std::string>());
    MIB_EXPECT(binned == areas.size(), "binned counts sum to population n");
    MIB_EXPECT(reply.at("scatter").at("points").size() <= 2000 && reply.at("scatter").at("decimated") == true,
               "decimated scatter bounded");
    // Page and gallery: the oracle's selected rank order.
    const auto refOf = [](std::uint64_t i) {
        return i < kValid ? std::make_pair(std::string("valid"), std::to_string(i))
                          : std::make_pair(std::string("invalid"), std::to_string(i - kValid));
    };
    MIB_REQUIRE(reply.at("rows").size() == 50, "page of 50 selected references");
    bool pageOk = reply.at("rowsTotal") == std::to_string(selected.size());
    for (std::size_t k = 0; k < 50; ++k) {
        const auto ref = refOf(selected[1000 + k]);
        pageOk = pageOk && reply.at("rows")[k].at("set") == ref.first &&
                 reply.at("rows")[k].at("ordinal") == ref.second;
    }
    MIB_EXPECT(pageOk, "selected page = oracle ranks 1000..1049");
    MIB_REQUIRE(reply.at("gallery").size() == 12, "deep gallery page is served");
    MIB_EXPECT(reply.at("gallery")[0].at("ordinal") == refOf(selected[deep]).second &&
                   reply.at("gallery")[0].at("set") == refOf(selected[deep]).first &&
                   reply.at("gallery")[11].at("ordinal") == refOf(selected[deep + 11]).second,
               "gallery offset beyond the former 10000 cap is exact");
    MIB_EXPECT(reply.at("membershipKey").at("cached") == false, "first evaluation computes");
    wd.mark("analysis");

    // Same source + graph: the membership handle is reused.
    request.erase("histogram");
    request.erase("scatter");
    request["feature"] = "youngsModulus";
    const auto again = Json::parse(session.analyzePopulation(request.dump()));
    MIB_EXPECT(again.at("ok").get<bool>() && again.at("membershipKey").at("cached") == true &&
                   again.at("selected") == reply.at("selected"),
               "membership reused across reductions (keyed to source + graph revision)");
    // Streaming fingerprint above 64 MiB, equal to an independent whole-buffer hash.
    request["fingerprint"] = true;
    const auto fingerprinted = Json::parse(session.analyzePopulation(request.dump()));
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto independent = "sha256:" + backend::processing::processingCoreBytesSha256(
                                             reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    MIB_EXPECT(fingerprinted.at("ok").get<bool>() && fingerprinted.at("sourceFingerprint") == independent &&
                   fingerprinted.at("membershipKey").at("sourceFingerprint") == independent,
               "streaming SHA-256 of a >64 MiB file keys the membership");
    std::printf("F-1 scale: fingerprint %s\n", independent.c_str());
    // The JSON F-source transport refuses explicitly above its bound.
    const auto frozen = Json::parse(session.analyzePopulation(
        Json{{"expectedPath", path}, {"feature", "area"}, {"minimum", nullptr}, {"maximum", nullptr},
             {"snapshot", true}}.dump()));
    MIB_EXPECT(!frozen.at("ok").get<bool>() &&
                   frozen.at("message").get<std::string>().find("bounded at 100000") != std::string::npos,
               "JSON row transport refuses with its explicit bound, never samples");
    wd.mark("fingerprint");
    return mib::test::exitCode();
}
