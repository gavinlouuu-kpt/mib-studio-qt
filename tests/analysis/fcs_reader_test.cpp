// F-2 (F15 read side, F04): the native FCS 3.0/3.1 reader feeding the
// columnar evaluator, against files this test writes itself
// (fcs_test_support.h, independent of FcsReader) and values it chose:
//   - TEXT tokenizer: doubled-delimiter escapes in keywords and values,
//     non-'/' delimiters, padding, odd/duplicate refusal;
//   - $DATATYPE F big-endian, D little-endian with DATA offsets only in
//     TEXT, I with 8/16/32-bit parameters, $PnR masking, $PnE log decoding and
//     optional $PnG division: every decoded value equals the independent
//     expectation;
//   - refusals (histogram mode, ASCII, packed bits, mixed byte order,
//     truncated DATA, FCS2.0, F with 64 bits) name their reason;
//   - present-but-unapplied constructs are reported, never silent;
//   - a 250,000-event FCS file gated through evaluateSource, PopulationGraph
//     semantics and the AnalysisHost (save + report) equals an oracle, and a
//     changed file is refused by its full-file fingerprint.
// Optional comparator (never the production path): with
// MIB_FCS_COMPARATOR_PYTHON set, tools/flowjo_acceptance/fcs_comparator.py
// (fcsparser and/or flowio) re-reads the float file and must agree; with
// MIB_FCS_COMPARATOR_FLOWKIT also set (F-3), FlowKit gates the same rectangle
// with its own GatingML RectangleGate and must select the native count.
#include "backend/analysis/AnalysisHost.h"
#include "backend/analysis/FcsReader.h"
#include "backend/analysis/SourceAnalysis.h"
#include "backend/database/SqliteService.h"
#include "fcs_test_support.h"
#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace fcs = mib::test::fcs;
using backend::analysis::FcsColumns;
using backend::analysis::FcsError;
using Json = nlohmann::json;

namespace {
std::uint64_t mix(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}
// Every value of `id` in event order, through the reader.
std::vector<double> column(const FcsColumns& c, const std::string& id, std::size_t chunk = 1000) {
    std::vector<double> out;
    c.scan({id}, chunk, [&](std::uint64_t begin, std::size_t count, const std::vector<const double*>& v) {
        MIB_REQUIRE(begin == out.size() && count <= chunk, "chunks in order and bounded");
        out.insert(out.end(), v[0], v[0] + count);
        return true;
    });
    return out;
}
bool hasIssue(const backend::analysis::FcsFile& f, const std::string& keyword) {
    return std::any_of(f.unsupported.begin(), f.unsupported.end(),
                       [&](const backend::analysis::FcsIssue& i) { return i.keyword == keyword; });
}
std::string refusal(const std::string& path) {
    try {
        (void)backend::analysis::readFcsMetadata(path);
    } catch (const FcsError& e) {
        return e.what();
    }
    return {};
}
} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    mib::test::Watchdog wd(120);
    mib::test::TempDir td("fcs_reader");
    const auto file = [&](const char* name) { return (td.path() / name).string(); };

    // ---- TEXT tokenizer ------------------------------------------------------
    {
        const auto t = backend::analysis::parseFcsText("/$A/b//c/$d/e/K//X/v/   ");
        MIB_EXPECT(t.size() == 3 && t.at("$A") == "b/c" && t.at("$D") == "e" && t.at("K/X") == "v",
                   "doubled delimiters are literal in values and keywords; keys uppercased; padding ignored");
        const auto ff = backend::analysis::parseFcsText("\x0c$A\x0c" "1\x0c\x0c" "2\x0c$B\x0c" "x/y\x0c");
        MIB_EXPECT(ff.at("$A") == "1\x0c" "2" && ff.at("$B") == "x/y", "form-feed delimiter with an escape");
        const auto value = backend::analysis::parseFcsText("|$A|ends with bar|||$B|1|");
        MIB_EXPECT(value.at("$A") == "ends with bar|" && value.at("$B") == "1",
                   "a value ending in the delimiter (escaped, then closed)");
        bool odd = false, dup = false;
        try { (void)backend::analysis::parseFcsText("/$A/1/$B/"); } catch (const FcsError&) { odd = true; }
        try { (void)backend::analysis::parseFcsText("/$A/1/$a/2/"); } catch (const FcsError&) { dup = true; }
        MIB_EXPECT(odd && dup, "a keyword without value and a repeated keyword are refused");
    }

    // ---- F, big-endian, escapes, lowercase keys, unsupported reporting ----
    constexpr std::uint64_t kEvents = 250000;
    const auto fscOf = [](std::uint64_t i) { return static_cast<float>(static_cast<double>(mix(i) % 200000) + 0.25); };
    const auto sscOf = [](std::uint64_t i) {
        return static_cast<float>(static_cast<double>(mix(i ^ 0x5555) % 80000) + 0.5);
    };
    const auto pathF = file("float.fcs");
    {
        fcs::Spec s;
        s.dataType = 'F';
        s.bigEndian = true;
        s.lowercaseKeys = true;
        s.columns = {{"FSC-A", "Forward/scatter", 32, "262144"},
                     {"SSC-A", "", 32, "262144"},
                     {"Time", "", 32, "262144"}};
        s.extra = {{"$SPILLOVER", "2,FSC-A,SSC-A,1,0.1,0,1"}, {"$TIMESTEP", "0.01"},
                   {"$P1CALIBRATION", "0.5,MESF"}, {"$CYT", "Synthetic/F-2"}};
        std::vector<unsigned char> data;
        for (std::uint64_t i = 0; i < kEvents; ++i) {
            fcs::putValue(data, s, s.columns[0], fscOf(i), 0);
            fcs::putValue(data, s, s.columns[1], sscOf(i), 0);
            fcs::putValue(data, s, s.columns[2], static_cast<double>(i), 0);
        }
        MIB_REQUIRE(fcs::write(pathF, s, kEvents, data), "write float file");
        const auto meta = backend::analysis::readFcsMetadata(pathF);
        MIB_EXPECT(meta.version == "FCS3.1" && meta.dataType == 'F' && meta.bigEndian && meta.events == kEvents &&
                       meta.parameters.size() == 3 && meta.parameters[0].label == "Forward/scatter" &&
                       meta.keywords.at("$CYT") == "Synthetic/F-2",
                   "F big-endian metadata, escaped label, lowercase keywords");
        MIB_EXPECT(hasIssue(meta, "$SPILLOVER") && hasIssue(meta, "$TIMESTEP") && hasIssue(meta, "$P1CALIBRATION"),
                   "compensation, time step and calibration are reported as not applied");
        FcsColumns c(meta);
        const auto fsc = column(c, "FSC-A", 4096), ssc = column(c, "SSC-A");
        bool exact = fsc.size() == kEvents && ssc.size() == kEvents;
        for (std::uint64_t i = 0; exact && i < kEvents; ++i)
            exact = fsc[i] == static_cast<double>(fscOf(i)) && ssc[i] == static_cast<double>(sscOf(i));
        MIB_EXPECT(exact, "every float value decodes exactly (big-endian)");
        MIB_EXPECT(c.parameters().at("FSC-A") == "a.u. (FCS channel value)", "explicit unit of a linear channel");
    }
    wd.mark("float");

    // ---- D, little-endian, DATA offsets only in TEXT, '|' delimiter -------
    {
        fcs::Spec s;
        s.version = "FCS3.0";
        s.delimiter = '|';
        s.dataType = 'D';
        s.bigEndian = false;
        s.offsetsInText = true;
        s.columns = {{"A|B", "", 64, "1e6"}, {"C", "", 64, "1e6"}};
        std::vector<unsigned char> data;
        std::vector<double> a, b;
        for (std::uint64_t i = 0; i < 3000; ++i) {
            a.push_back(static_cast<double>(mix(i)) / 3.0e13 - 1.0);
            b.push_back(i % 17 == 0 ? std::nan("") : std::ldexp(static_cast<double>(i), -7));
            fcs::putValue(data, s, s.columns[0], a.back(), 0);
            fcs::putValue(data, s, s.columns[1], b.back(), 0);
        }
        const auto path = file("double.fcs");
        MIB_REQUIRE(fcs::write(path, s, 3000, data), "write double file");
        const auto meta = backend::analysis::readFcsMetadata(path);
        MIB_EXPECT(meta.version == "FCS3.0" && !meta.bigEndian && meta.delimiter == '|' &&
                       meta.parameters[0].name == "A|B" && meta.parameters[0].id == "A_B",
                   "D little-endian; escaped name made graph-safe");
        FcsColumns c(meta);
        const auto va = column(c, "A_B"), vb = column(c, "C");
        bool exact = va.size() == 3000;
        for (std::size_t i = 0; exact && i < 3000; ++i)
            exact = va[i] == a[i] && (std::isnan(b[i]) ? std::isnan(vb[i]) : vb[i] == b[i]);
        MIB_EXPECT(exact, "every double decodes exactly; NaN is missing");
    }

    // ---- I: 16/32/8-bit, masking, log decoding, gain -----------------------
    {
        fcs::Spec s;
        s.dataType = 'I';
        s.bigEndian = true;
        s.byteOrder = "4,3,2,1";
        s.columns = {{"masked", "", 16, "1024"},
                     {"gained", "", 32, "100000", "0,0", "2.0"},
                     {"logged", "", 16, "1024", "4,1"},
                     {"small", "", 8, "256"}};
        std::vector<unsigned char> data;
        for (std::uint64_t i = 0; i < 2048; ++i) {
            fcs::putValue(data, s, s.columns[0], 0, (i % 1024) | 0xF000); // high bits above $P1R
            fcs::putValue(data, s, s.columns[1], 0, i * 3);
            fcs::putValue(data, s, s.columns[2], 0, i % 1024);
            fcs::putValue(data, s, s.columns[3], 0, i % 256);
        }
        const auto path = file("int.fcs");
        MIB_REQUIRE(fcs::write(path, s, 2048, data), "write integer file");
        const auto raw = backend::analysis::readFcsMetadata(path);
        MIB_EXPECT(raw.eventBytes == 9 && hasIssue(raw, "$P2G"), "mixed widths; gain reported when not applied");
        FcsColumns c(raw);
        const auto masked = column(c, "masked"), gained = column(c, "gained"), logged = column(c, "logged"),
                   small = column(c, "small");
        bool ok = masked.size() == 2048;
        for (std::uint64_t i = 0; ok && i < 2048; ++i)
            ok = masked[i] == static_cast<double>(i % 1024) && gained[i] == static_cast<double>(i * 3) &&
                 std::abs(logged[i] - std::pow(10.0, 4.0 * static_cast<double>(i % 1024) / 1024.0)) <=
                     1e-12 * logged[i] &&
                 small[i] == static_cast<double>(i % 256);
        MIB_EXPECT(ok, "integer masking by $PnR, log decoding by $PnE, raw gain by default");
        MIB_EXPECT(c.parameters().at("logged") == "a.u. (FCS log-decoded scale value)", "log unit is explicit");
        backend::analysis::FcsOptions withGain;
        withGain.applyGain = true;
        FcsColumns g(backend::analysis::readFcsMetadata(path, withGain));
        const auto halved = column(g, "gained");
        MIB_EXPECT(halved[7] == 10.5 && g.parameters().at("gained") == "a.u. (FCS channel value / $PnG)",
                   "applyGain divides linear values by $PnG and says so in the unit");
    }
    wd.mark("types");

    // ---- refusals and reported constructs ----------------------------------
    {
        const auto variant = [&](const char* name, auto&& edit, std::uint64_t events = 4) {
            fcs::Spec s;
            s.columns = {{"X", "", 32, "1024"}};
            edit(s);
            std::vector<unsigned char> data;
            for (std::uint64_t i = 0; i < events; ++i) fcs::putValue(data, s, s.columns[0], 1.0, 1);
            const auto path = file(name);
            MIB_REQUIRE(fcs::write(path, s, events, data), name);
            return path;
        };
        const auto has = [](const std::string& text, const char* needle) {
            return text.find(needle) != std::string::npos;
        };
        MIB_EXPECT(has(refusal(variant("hist.fcs", [](fcs::Spec& s) { s.mode = "C"; })), "list mode"),
                   "histogram mode refused");
        MIB_EXPECT(has(refusal(variant("ascii.fcs", [](fcs::Spec& s) { s.dataType = 'A'; })), "ASCII"),
                   "ASCII data refused");
        MIB_EXPECT(has(refusal(variant("packed.fcs", [](fcs::Spec& s) { s.dataType = 'I'; s.columns[0].bits = 10; })),
                       "packed"),
                   "packed integer bits refused");
        MIB_EXPECT(has(refusal(variant("pdp.fcs", [](fcs::Spec& s) { s.byteOrder = "3,4,1,2"; })), "mixed byte order"),
                   "mixed byte order refused");
        MIB_EXPECT(has(refusal(variant("short.fcs", [](fcs::Spec& s) { s.totOverride = 9; })), "needs"),
                   "DATA shorter than $TOT x width refused");
        MIB_EXPECT(has(refusal(variant("v2.fcs", [](fcs::Spec& s) { s.version = "FCS2.0"; })), "version"),
                   "FCS2.0 refused");
        MIB_EXPECT(has(refusal(variant("f64.fcs", [](fcs::Spec& s) { s.columns[0].bits = 64; })), "requires"),
                   "F with 64-bit parameter refused");
        const auto reported = backend::analysis::readFcsMetadata(variant("next.fcs", [](fcs::Spec& s) {
            s.extra = {{"$COMP", "1"}, {"$P1D", "Log,0,4"}};
            s.columns[0].amplification = "4,1";
        }));
        MIB_EXPECT(hasIssue(reported, "$COMP") && hasIssue(reported, "$P1D") && hasIssue(reported, "$P1E"),
                   "compensation, display scale and float log flags are reported, not applied");
    }
    wd.mark("refusals");

    // ---- the evaluator and the host over the 250k-event float file --------
    {
        std::uint64_t inRect = 0, inFsc = 0;
        std::vector<double> rectFsc;
        for (std::uint64_t i = 0; i < kEvents; ++i) {
            const double x = fscOf(i), y = sscOf(i);
            const bool fx = x >= 50000 && x <= 150000;
            inFsc += fx;
            if (fx && y >= 10000 && y <= 50000) {
                ++inRect;
                rectFsc.push_back(x);
            }
        }
        std::sort(rectFsc.begin(), rectFsc.end());
        const auto handle = backend::analysis::fcsSourceDescriptor(Json{{"kind", "fcs"}, {"path", pathF}});
        MIB_EXPECT(handle.at("schema") == "F-source/2" && handle.at("events") == std::to_string(kEvents) &&
                       handle.at("parameters").at("SSC-A") == "a.u. (FCS channel value)" &&
                       handle.at("import").at("unsupported").size() >= 3,
                   "FCS columnar handle: identity, explicit units, unsupported constructs listed");
        const Json graph{{"id", "rect"}, {"revision", "1"}, {"output", "rect"},
                         {"nodes", Json::array({Json{{"id", "fsc"}, {"parent", ""}, {"kind", "interval"},
                                                     {"feature", "FSC-A"}, {"minimum", 50000}, {"maximum", 150000}},
                                                Json{{"id", "rect"}, {"parent", "fsc"}, {"kind", "interval"},
                                                     {"feature", "SSC-A"}, {"minimum", 10000}, {"maximum", 50000}}})}};
        const auto result = backend::analysis::evaluateSource(handle, Json{{"graph", graph}, {"feature", "FSC-A"}},
                                                              backend::analysis::lendFcsColumns);
        const double median = rectFsc[(rectFsc.size() - 1) / 2] +
                              0.5 * ((rectFsc.size() - 1) % 2) * (rectFsc[rectFsc.size() / 2] - rectFsc[(rectFsc.size() - 1) / 2]);
        MIB_EXPECT(result.at("selected") == std::to_string(inRect) &&
                       result.at("denominator") == std::to_string(kEvents) &&
                       std::abs(result.at("median").get<double>() - median) <= 1e-9 * median,
                   "rectangle gate over 250k FCS events equals the oracle (count, median)");
        std::printf("F-2: synthetic FCS %llu events, rectangle %llu (FSC gate %llu)\n",
                    static_cast<unsigned long long>(kEvents), static_cast<unsigned long long>(inRect),
                    static_cast<unsigned long long>(inFsc));
        // Through the AnalysisHost exactly as a host wires kind "fcs".
        backend::services::SqliteService db;
        MIB_REQUIRE(db.initialize(file("app.sqlite3")), "metadata authority");
        auto source = [](const Json& d) { return backend::analysis::fcsSourceDescriptor(d); };
        auto call = [&](const Json& q, bool write) {
            return Json::parse(backend::analysis::analysisRequest(db, q.dump(), write, source, {},
                                                                  backend::analysis::lendFcsColumns));
        };
        const Json descriptor{{"kind", "fcs"}, {"path", pathF}};
        Json binding = Json::object();
        for (const char* k : {"fingerprint", "sourceKey", "parameters", "scope"}) binding[k] = handle.at(k);
        auto saveGraph = graph;
        saveGraph["id"] = "fcsPlan";
        const auto saved = call(Json{{"action", "save"}, {"planId", "fcsPlan"}, {"expectedRevision", nullptr},
                                     {"commandId", "fcs-1"}, {"feature", "FSC-A"}, {"graph", saveGraph},
                                     {"source", descriptor}, {"sourceBinding", binding}},
                                true);
        MIB_REQUIRE(saved.value("revision", std::string{}) == "1", saved.dump().substr(0, 300));
        const auto report = call(Json{{"action", "report"}, {"planId", "fcsPlan"}, {"revision", "1"},
                                      {"source", descriptor}},
                                 false);
        MIB_EXPECT(report.value("status", std::string{}) == "ok" &&
                       report.at("report").at("table")[0].at("selected") == std::to_string(inFsc) &&
                       report.at("report").at("table")[1].at("selected") == std::to_string(inRect),
                   "saved strategy report over the FCS source equals the oracle at every population");
        {
            std::ofstream tamper(pathF, std::ios::binary | std::ios::app);
            tamper << "x";
        }
        bool refused = false;
        try {
            backend::analysis::evaluateSource(handle, Json{{"graph", graph}, {"feature", "FSC-A"}},
                                              backend::analysis::lendFcsColumns);
        } catch (const std::invalid_argument& e) {
            refused = std::string(e.what()).find("fingerprint") != std::string::npos;
        }
        MIB_EXPECT(refused, "a changed FCS file is refused by its pinned fingerprint");
    }
    wd.mark("evaluator");

    // ---- optional independent comparator (fcsparser / flowio) -------------
    if (const char* python = std::getenv("MIB_FCS_COMPARATOR_PYTHON")) {
        const auto copy = file("comparator.fcs");
        fcs::Spec s;
        s.columns = {{"FSC-A", "", 32, "262144"}, {"SSC-A", "", 32, "262144"}};
        std::vector<unsigned char> data;
        double sum = 0;
        for (std::uint64_t i = 0; i < 10000; ++i) {
            fcs::putValue(data, s, s.columns[0], fscOf(i), 0);
            fcs::putValue(data, s, s.columns[1], sscOf(i), 0);
            sum += static_cast<double>(fscOf(i));
        }
        MIB_REQUIRE(fcs::write(copy, s, 10000, data), "comparator file");
        const auto script = std::string(MIB_SOURCE_DIR) + "/tools/flowjo_acceptance/fcs_comparator.py";
        // Native rectangle on the comparator file (values sit off the bounds:
        // GatingML max is exclusive, MIB's inclusive).
        const Json graph{{"id", "rect"}, {"revision", "1"}, {"output", "rect"},
                         {"nodes", Json::array({Json{{"id", "fsc"}, {"parent", ""}, {"kind", "interval"},
                                                     {"feature", "FSC-A"}, {"minimum", 50000}, {"maximum", 150000}},
                                                Json{{"id", "rect"}, {"parent", "fsc"}, {"kind", "interval"},
                                                     {"feature", "SSC-A"}, {"minimum", 10000}, {"maximum", 50000}}})}};
        const auto native = backend::analysis::evaluateSource(
            backend::analysis::fcsSourceDescriptor(Json{{"kind", "fcs"}, {"path", copy}}),
            Json{{"graph", graph}, {"feature", "FSC-A"}}, backend::analysis::lendFcsColumns);
        const auto nativeRect = std::stoll(native.at("selected").get<std::string>());
        const bool flowkit = std::getenv("MIB_FCS_COMPARATOR_FLOWKIT") != nullptr;
        const auto command = std::string(python) + " -I " + script + " " + copy +
                             " FSC-A --rect FSC-A 50000 150000 SSC-A 10000 50000" +
                             (flowkit ? " --flowkit" : "");
        FILE* pipe = popen(command.c_str(), "r");
        std::string output;
        char buffer[4096];
        while (pipe && fgets(buffer, sizeof(buffer), pipe)) output += buffer;
        const int status = pipe ? pclose(pipe) : -1;
        const auto parsed = Json::parse(output, nullptr, false);
        MIB_EXPECT(status == 0 && parsed.is_object() && parsed.at("events") == 10000 &&
                       std::abs(parsed.at("sum").get<double>() - sum) <= 1e-9 * sum,
                   "independent comparator agrees with the native reader: " + output);
        MIB_EXPECT(parsed.is_object() && parsed.value("rectangle", Json(nullptr)) == nativeRect,
                   "comparator rectangle count equals the native gate: " + output);
        if (flowkit)
            MIB_EXPECT(parsed.is_object() && parsed.value("flowkitRectangle", Json(nullptr)) == nativeRect,
                       "FlowKit's RectangleGate selects the native population: " + output);
        std::printf("F-2: comparator %s\n", output.c_str());
    } else {
        std::printf("F-2: optional comparator not configured (MIB_FCS_COMPARATOR_PYTHON unset); native checks only\n");
    }
    return mib::test::exitCode();
}
