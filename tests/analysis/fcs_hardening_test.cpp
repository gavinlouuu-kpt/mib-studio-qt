// F-2 review fixes (F15, F04, F16): FCS reader hardening.
//   - $DATATYPE I, $PnB 64 with $PnR between 2^63 and 2^64 finishes (the
//     former power-of-two loop overflowed to 0 and never ended) and applies
//     no mask; $PnR 2^63 and $PnR 1024 at 64 bits mask exactly;
//   - $PnR 1 is a real mask of 0 (every value reads 0) and $PnR 2 masks to
//     one bit; "no mask" is explicit, never encoded as mask 0;
//   - an FCS file rewritten in place between descriptor and evaluation (same
//     size, write time moved) is refused; a change during the scan is
//     refused by the post-scan fence; the descriptor's retention text claims
//     no full re-hash per evaluation;
//   - an 80-parameter file (above the former 64-unit columnar limit) gates
//     on two parameters through evaluateSource, equals an oracle, and the
//     scan reads only the referenced columns.
#include "backend/analysis/FcsReader.h"
#include "backend/analysis/SourceAnalysis.h"
#include "fcs_test_support.h"
#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

namespace fcs = mib::test::fcs;
using backend::analysis::EventColumns;
using backend::analysis::FcsColumns;
using Json = nlohmann::json;

namespace {
std::vector<double> column(const FcsColumns& c, const std::string& id) {
    std::vector<double> out;
    c.scan({id}, 1000, [&](std::uint64_t, std::size_t count, const std::vector<const double*>& v) {
        out.insert(out.end(), v[0], v[0] + count);
        return true;
    });
    return out;
}

// One integer column of `bits` with the given $PnR and raw values.
backend::analysis::FcsFile integerFile(const std::string& path, unsigned bits, const std::string& range,
                                       const std::vector<std::uint64_t>& values) {
    fcs::Spec s;
    s.dataType = 'I';
    s.bigEndian = false;
    for (unsigned k = 0; k < bits / 8; ++k) s.byteOrder += (k ? "," : "") + std::to_string(k + 1);
    s.columns = {{"X", "", bits, range}};
    std::vector<unsigned char> data;
    for (const auto v : values) fcs::putValue(data, s, s.columns[0], 0, v);
    MIB_REQUIRE(fcs::write(path, s, values.size(), data), "write integer file " + path);
    return backend::analysis::readFcsMetadata(path);
}

// Forwards to the lent columns and records which columns each scan asks for.
class RecordingColumns final : public EventColumns {
public:
    RecordingColumns(const EventColumns& inner, std::set<std::string>& seen) : inner_(inner), seen_(seen) {}
    const std::vector<backend::analysis::EventSegment>& segments() const override { return inner_.segments(); }
    const std::map<std::string, std::string>& parameters() const override { return inner_.parameters(); }
    bool scan(const std::vector<std::string>& columns, std::size_t chunkEvents, const ChunkSink& sink) const override {
        seen_.insert(columns.begin(), columns.end());
        return inner_.scan(columns, chunkEvents, sink);
    }

private:
    const EventColumns& inner_;
    std::set<std::string>& seen_;
};

void rewriteInPlace(const std::string& path, const std::vector<unsigned char>& bytes) {
    const auto before = std::filesystem::last_write_time(path);
    std::fstream out(path, std::ios::binary | std::ios::in | std::ios::out);
    out.seekp(0);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();
    // Coarse-timestamp filesystems: the write time moves visibly.
    std::filesystem::last_write_time(path, before + std::chrono::seconds(2));
}
std::vector<unsigned char> slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // The former reader hung forever on the first case; the watchdog turns a
    // regression into a fast failure instead of a CI timeout.
    mib::test::Watchdog wd(30);
    mib::test::TempDir td("fcs_hardening");
    const auto file = [&](const char* name) { return (td.path() / name).string(); };
    constexpr std::uint64_t kAll = ~std::uint64_t{0};

    // ---- $PnR masks ----------------------------------------------------------
    {
        const auto t0 = std::chrono::steady_clock::now();
        const auto f = integerFile(file("r64high.fcs"), 64, "15000000000000000000", {5, kAll});
        const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        MIB_EXPECT(seconds < 5.0, "64-bit $PnR between 2^63 and 2^64 finishes quickly");
        MIB_EXPECT(!f.parameters[0].mask.has_value(), "$PnR above 2^(bits-1): no mask (all stored bits count)");
        const auto v = column(FcsColumns(f), "X");
        MIB_EXPECT(v.size() == 2 && v[0] == 5.0 && v[1] == static_cast<double>(kAll), "unmasked 64-bit values");
    }
    wd.mark("64-bit high range");
    {
        const auto f = integerFile(file("r64edge.fcs"), 64, "9223372036854775808", {kAll, 7});
        MIB_EXPECT(f.parameters[0].mask == std::optional<std::uint64_t>(0x7FFFFFFFFFFFFFFFULL),
                   "$PnR 2^63 at 64 bits masks to 63 bits");
        const auto v = column(FcsColumns(f), "X");
        MIB_EXPECT(v[0] == static_cast<double>(0x7FFFFFFFFFFFFFFFULL) && v[1] == 7.0, "63-bit masked values");
        const auto small = integerFile(file("r64small.fcs"), 64, "1024", {0xF000000000000401ULL});
        MIB_EXPECT(small.parameters[0].mask == std::optional<std::uint64_t>(1023) &&
                       column(FcsColumns(small), "X")[0] == 1.0,
                   "$PnR 1024 at 64 bits masks to 10 bits");
    }
    {
        const auto one = integerFile(file("r1.fcs"), 16, "1", {0, 1, 0xFFFF});
        MIB_EXPECT(one.parameters[0].mask == std::optional<std::uint64_t>(0),
                   "$PnR 1 is a present mask of 0 (values 0..0), not 'no mask'");
        const auto v1 = column(FcsColumns(one), "X");
        MIB_EXPECT(v1 == std::vector<double>({0.0, 0.0, 0.0}), "$PnR 1: every value reads 0");
        const auto two = integerFile(file("r2.fcs"), 16, "2", {0, 1, 2, 3, 0xFFFE});
        MIB_EXPECT(two.parameters[0].mask == std::optional<std::uint64_t>(1), "$PnR 2 masks to one bit");
        MIB_EXPECT(column(FcsColumns(two), "X") == std::vector<double>({0.0, 1.0, 0.0, 1.0, 0.0}),
                   "$PnR 2: values 0..1");
        const auto frac = integerFile(file("r1000.fcs"), 16, "1000.5", {1001, 0xFFFF});
        MIB_EXPECT(frac.parameters[0].mask == std::optional<std::uint64_t>(1023), "$PnR 1000.5 -> 10 bits");
        const auto full = integerFile(file("r8full.fcs"), 8, "200", {255});
        MIB_EXPECT(!full.parameters[0].mask.has_value() && column(FcsColumns(full), "X")[0] == 255.0,
                   "$PnR above 2^(bits-1) at 8 bits: no mask");
    }
    wd.mark("masks");

    // ---- rewrite between descriptor and evaluation ---------------------------
    const auto writeFloat = [&](const std::string& path, std::size_t parameters, std::uint64_t events,
                                double shift) {
        fcs::Spec s;
        for (std::size_t p = 0; p < parameters; ++p)
            s.columns.push_back({"P" + std::to_string(p + 1), "", 32, "262144"});
        std::vector<unsigned char> data;
        for (std::uint64_t i = 0; i < events; ++i)
            for (std::size_t p = 0; p < parameters; ++p)
                fcs::putValue(data, s, s.columns[p], shift + static_cast<double>((i * (p + 3)) % 1000), 0);
        MIB_REQUIRE(fcs::write(path, s, events, data), "write float file " + path);
    };
    const Json graph{{"id", "g"}, {"revision", "1"}, {"output", "both"},
                     {"nodes", Json::array({Json{{"id", "a"}, {"parent", ""}, {"kind", "interval"},
                                                 {"feature", "P3"}, {"minimum", 100}, {"maximum", 600}},
                                            Json{{"id", "both"}, {"parent", "a"}, {"kind", "interval"},
                                                 {"feature", "P71"}, {"minimum", 0}, {"maximum", 500}}})}};
    {
        const auto path = file("rewrite.fcs"), other = file("other.fcs");
        writeFloat(path, 80, 5000, 0.0);
        writeFloat(other, 80, 5000, 2000.0);
        MIB_REQUIRE(slurp(path).size() == slurp(other).size(), "same shape, same size");
        const auto handle = backend::analysis::fcsSourceDescriptor(Json{{"kind", "fcs"}, {"path", path}});
        const auto retention = handle.at("retention").get<std::string>();
        MIB_EXPECT(retention.find("not detected") != std::string::npos &&
                       retention.find("re-verified on every") == std::string::npos,
                   "retention text states what is verified and claims no per-evaluation re-hash");
        const Json request{{"graph", graph}, {"feature", "P3"}};
        const auto before = backend::analysis::evaluateSource(handle, request, backend::analysis::lendFcsColumns);
        MIB_EXPECT(before.at("selected") != "0", "original content selects events");
        rewriteInPlace(path, slurp(other));
        std::string message;
        try {
            backend::analysis::evaluateSource(handle, request, backend::analysis::lendFcsColumns);
        } catch (const std::invalid_argument& e) {
            message = e.what();
        }
        MIB_EXPECT(message.find("fingerprint") != std::string::npos,
                   "rewrite between descriptor and evaluation is refused: " + message);

        // A change during the scan: the post-scan fence refuses the results.
        const auto fresh = backend::analysis::fcsSourceDescriptor(Json{{"kind", "fcs"}, {"path", path}});
        const auto original = slurp(other);
        auto changedDuringScan = [&](const Json& source, const backend::analysis::ColumnVisitor& use) {
            backend::analysis::lendFcsColumns(source, [&](const EventColumns& columns) {
                use(columns);
                auto bytes = original;
                bytes.back() ^= 0x01;
                rewriteInPlace(path, bytes);
            });
        };
        message.clear();
        try {
            backend::analysis::evaluateSource(fresh, request, changedDuringScan);
        } catch (const std::invalid_argument& e) {
            message = e.what();
        }
        MIB_EXPECT(message.find("during evaluation") != std::string::npos,
                   "a change during the scan is refused by the post-scan fence: " + message);
    }
    wd.mark("rewrite");

    // ---- more than 64 parameters, gating on two of them ----------------------
    {
        const auto path = file("wide.fcs");
        constexpr std::uint64_t kEvents = 20000;
        writeFloat(path, 80, kEvents, 0.0);
        const auto handle = backend::analysis::fcsSourceDescriptor(Json{{"kind", "fcs"}, {"path", path}});
        MIB_EXPECT(handle.at("parameters").size() == 80, "80 parameters imported");
        std::uint64_t inA = 0, inBoth = 0;
        for (std::uint64_t i = 0; i < kEvents; ++i) {
            const double p3 = static_cast<double>((i * 5) % 1000), p71 = static_cast<double>((i * 73) % 1000);
            const bool a = p3 >= 100 && p3 <= 600;
            inA += a;
            inBoth += a && p71 <= 500;
        }
        std::set<std::string> seen;
        auto recording = [&](const Json& source, const backend::analysis::ColumnVisitor& use) {
            backend::analysis::lendFcsColumns(source, [&](const EventColumns& columns) {
                use(RecordingColumns(columns, seen));
            });
        };
        Json result;
        try {
            result = backend::analysis::evaluateSource(handle, Json{{"graph", graph}, {"feature", "P3"}}, recording);
        } catch (const std::exception& e) {
            MIB_REQUIRE(false, std::string("an 80-parameter FCS source evaluates: ") + e.what());
        }
        MIB_EXPECT(result.at("selected") == std::to_string(inBoth) && result.at("denominator") == std::to_string(kEvents),
                   "gate on P3 and P71 of an 80-parameter file equals the oracle");
        const auto populations = backend::analysis::evaluatePopulations(
            handle, Json{{"graph", graph}, {"feature", "P3"}}, {"a"}, backend::analysis::lendFcsColumns);
        MIB_EXPECT(populations.size() == 1 && populations[0].at("selected") == std::to_string(inA),
                   "parent population exact");
        MIB_EXPECT(seen == std::set<std::string>({"P3", "P71"}),
                   "columns stay lazy: only the referenced parameters are read");
        std::printf("F-2 wide: 80 parameters, %llu events, selected %llu, columns read %zu\n",
                    static_cast<unsigned long long>(kEvents), static_cast<unsigned long long>(inBoth), seen.size());
    }
    return mib::test::exitCode();
}
