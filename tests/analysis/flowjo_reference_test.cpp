// F-2 acceptance (F04/F15): the private FlowJo 10.8.1 reference through MIB's
// own code: the native FCS reader -> F-source/2 handle -> the columnar
// population evaluator (PopulationGraph tri-state semantics) -> exact counts
// and the native 4,096-bin histograms, compared with FlowJo's exported TSVs.
//
// The gate and pins are those of tools/flowjo_acceptance/verify_reference.py:
//   FSC-A (parameter 1) in [50000, 150000] AND SSC-A (parameter 3) in
//   [10000, 50000]; 229,585 events, 190,705 selected, zero boundary events;
//   bins round(value / 64) * 64, 4,096 per channel, zero mismatches.
//
// The fixture is private and never bundled. Set MIB_FLOWJO_REFERENCE_ROOT to
// the authorized flowjo-windows-10.8.1 directory (private/fixtures/
// fj-vendor-control.fcs, evidence/{fsc,ssc}-a-histogram.tsv). Without it the
// test prints why and exits 77, which CTest reports as SKIPPED (never as a
// pass). With MIB_FCS_COMPARATOR_PYTHON set, the optional fcsparser/flowio
// comparator (tools/flowjo_acceptance/fcs_comparator.py) recounts the gate
// independently; it is a comparator, never the production path.
#include "backend/analysis/ColumnarEvaluation.h"
#include "backend/analysis/FcsReader.h"
#include "backend/analysis/PopulationGates.h"
#include "backend/analysis/SourceAnalysis.h"
#include "backend/processing/ProcessingCoreSha256.h"
#include "support/assert.h"
#include "support/watchdog.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

using Json = nlohmann::json;

namespace {
constexpr const char* kFcsSha = "6687290a19cfcd2a86e8fe5af7c2077c4db10302af45b71eb6b95f6272706ada";
struct Histogram {
    const char* parameter;
    const char* file;
    const char* sha;
};
constexpr Histogram kHistograms[] = {
    {"FSC-A", "fsc-a-histogram.tsv", "8497ed8a7a1804eaafafe92f6fe17c932f12f74f94304f9151f9e3418b0c2038"},
    {"SSC-A", "ssc-a-histogram.tsv", "dd219c97391968f8565a698cec3a1a1cbcaddace0cf3a6701f56dedf0378ac3d"},
};
std::string sha256(const std::filesystem::path& path) {
    std::string error;
    return backend::processing::fileSha256(path, &error, {});
}
} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const char* rootEnv = std::getenv("MIB_FLOWJO_REFERENCE_ROOT");
    if (!rootEnv || !*rootEnv) {
        std::printf("SKIPPED: MIB_FLOWJO_REFERENCE_ROOT is not set. The FlowJo 10.8.1 reference fixture is "
                    "private (tools/flowjo_acceptance/README.md); this acceptance did not run and is not a pass.\n");
        return 77;
    }
    mib::test::Watchdog wd(120);
    const std::filesystem::path root(rootEnv);
    const auto fcsPath = root / "private/fixtures/fj-vendor-control.fcs";
    if (!std::filesystem::exists(fcsPath)) {
        std::printf("SKIPPED: %s does not exist; the reference fixture is absent. Not a pass.\n",
                    fcsPath.string().c_str());
        return 77;
    }
    MIB_REQUIRE(sha256(fcsPath) == kFcsSha, "reference FCS matches its SHA-256 pin");

    // 1. Native import: F-source/2 handle of the FCS file.
    const auto handle = backend::analysis::fcsSourceDescriptor(Json{{"kind", "fcs"}, {"path", fcsPath.string()}});
    const auto& imported = handle.at("import");
    std::printf("F-2 reference: %s, $DATATYPE %s, %s, %s events, %zu parameters\n",
                imported.at("format").get<std::string>().c_str(), imported.at("dataType").get<std::string>().c_str(),
                imported.at("byteOrder").get<std::string>().c_str(), handle.at("events").get<std::string>().c_str(),
                imported.at("parameters").size());
    for (const auto& issue : imported.at("unsupported"))
        std::printf("  not applied: %s: %s\n", issue.at("keyword").get<std::string>().c_str(),
                    issue.at("message").get<std::string>().c_str());
    MIB_EXPECT(handle.at("events") == "229585", "229,585 events imported natively");
    const auto& params = imported.at("parameters");
    MIB_REQUIRE(params.size() >= 3 && params[0].at("name") == "FSC-A" && params[2].at("name") == "SSC-A",
                "parameter 1 is FSC-A and parameter 3 is SSC-A (the reference gate's channels)");
    const auto x = params[0].at("id").get<std::string>(), y = params[2].at("id").get<std::string>();

    // 2. The documented rectangle through the population graph.
    const Json graph{{"id", "flowjo-reference"},
                     {"revision", "1"},
                     {"output", "rectangle"},
                     {"nodes", Json::array({Json{{"id", "fsc"}, {"parent", ""}, {"kind", "interval"}, {"feature", x},
                                                 {"minimum", 50000}, {"maximum", 150000}},
                                            Json{{"id", "rectangle"}, {"parent", "fsc"}, {"kind", "interval"},
                                                 {"feature", y}, {"minimum", 10000}, {"maximum", 50000}}})}};
    const auto evaluation = backend::analysis::evaluateSource(handle, Json{{"graph", graph}, {"feature", x}},
                                                              backend::analysis::lendFcsColumns);
    std::printf("F-2 reference: selected %s of %s (unknown %s)\n", evaluation.at("selected").get<std::string>().c_str(),
                evaluation.at("denominator").get<std::string>().c_str(),
                evaluation.at("unknown").get<std::string>().c_str());
    MIB_EXPECT(evaluation.at("selected") == "190705" && evaluation.at("unknown") == "0" &&
                   evaluation.at("denominator") == "229585",
               "190,705 of 229,585 inside the rectangle through PopulationGraph (native)");

    // 3. Boundary inventory and 4,096-bin histograms over exact membership.
    backend::analysis::lendFcsColumns(handle, [&](const backend::analysis::EventColumns& columns) {
        const auto compiled = backend::analysis::compileGraph(graph);
        const backend::analysis::RevisionKey rev{"ref", "ref", "graph", "none", "none", "raw", "none", "native", "native-graph/1"};
        const auto membership = backend::analysis::evaluateColumns(compiled, columns, rev);
        const auto& bits = membership->population("rectangle");
        std::uint64_t boundaries = 0;
        columns.scan({x, y}, 65536, [&](std::uint64_t, std::size_t count, const std::vector<const double*>& v) {
            for (std::size_t j = 0; j < count; ++j)
                boundaries += v[0][j] == 50000 || v[0][j] == 150000 || v[1][j] == 10000 || v[1][j] == 50000;
            return true;
        });
        MIB_EXPECT(boundaries == 0, "zero boundary events (the reference inventory)");
        for (std::size_t h = 0; h < 2; ++h) {
            const auto& spec = kHistograms[h];
            const auto feature = h == 0 ? x : y;
            const auto tsv = root / "evidence" / spec.file;
            if (!std::filesystem::exists(tsv)) {
                MIB_EXPECT(false, std::string("histogram export missing: ") + tsv.string());
                continue;
            }
            MIB_REQUIRE(sha256(tsv) == spec.sha, std::string("histogram export matches its pin: ") + spec.file);
            // Bins centred on multiples of 64: [64i - 32, 64i + 32).
            backend::analysis::ResourceBounds fine;
            fine.maxHistogramBins = 4096;
            const auto native =
                backend::analysis::columnHistogram(bits, columns, feature, -32.0, 4096 * 64.0 - 32.0, 4096, fine);
            std::ifstream in(tsv);
            std::string line;
            std::size_t row = 0, mismatches = 0;
            std::uint64_t total = 0;
            bool shape = true;
            while (std::getline(in, line)) {
                if (row == 0 && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF) line = line.substr(3);
                if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
                std::istringstream fields(line);
                double coordinate = 0;
                std::uint64_t count = 0;
                if (!(fields >> coordinate >> count) || row >= 4096 || coordinate != static_cast<double>(row * 64)) {
                    shape = false;
                    break;
                }
                total += count;
                mismatches += native.counts[row] != count;
                ++row;
            }
            MIB_EXPECT(shape && row == 4096, std::string("4,096-row export: ") + spec.file);
            MIB_EXPECT(total == 190705 && native.underflow == 0 && native.overflow == 0 && native.measured == 190705,
                       std::string("histogram conserves the selected count: ") + spec.parameter);
            MIB_EXPECT(mismatches == 0, std::string("zero mismatches against FlowJo's 4,096-bin export: ") + spec.parameter);
            std::printf("F-2 reference: %s 4096 bins, %zu mismatches\n", spec.parameter, mismatches);
        }
    });

    // 4. Optional independent comparator (fcsparser / flowio).
    if (const char* python = std::getenv("MIB_FCS_COMPARATOR_PYTHON")) {
        const auto script = std::string(MIB_SOURCE_DIR) + "/tools/flowjo_acceptance/fcs_comparator.py";
        const auto command = std::string(python) + " -I " + script + " " + fcsPath.string() +
                             " FSC-A --rect FSC-A 50000 150000 SSC-A 10000 50000";
        FILE* pipe = popen(command.c_str(), "r");
        std::string output;
        char buffer[4096];
        while (pipe && fgets(buffer, sizeof(buffer), pipe)) output += buffer;
        const int status = pipe ? pclose(pipe) : -1;
        const auto parsed = Json::parse(output, nullptr, false);
        MIB_EXPECT(status == 0 && parsed.is_object() && parsed.at("events") == 229585 && parsed.at("rectangle") == 190705,
                   "independent comparator recounts the reference: " + output);
    } else {
        std::printf("F-2 reference: optional comparator not configured (MIB_FCS_COMPARATOR_PYTHON unset)\n");
    }
    return mib::test::exitCode();
}
