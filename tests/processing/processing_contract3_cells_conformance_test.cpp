// Contract 3 (U-Net cells) gold: the host science equals the PZ7035 PL cell
// stage. Reads scripts/conformance/unet-cells-v2-pl-vectors.json (path in
// argv[1]): per case the profile page (unet_cells_v2) and E-modulus table, per
// frame the raw gray, the U-Net mask and the RESULT payload words the PL emits
// (from the bit-exact reference model the PL equals on hardware). Every frame
// runs through science::filterProcessedObjects with
// processing_contract_version 3 and is compared within the profile's
// conformance tolerances. MIB_UNET_CELLS_VECTORS=<full vectors .json> checks
// the full set instead of the committed subset (the page layout still comes
// from argv[1]).
#include "backend/processing/EModulusLut.h"
#include "backend/processing/ProcessingContract.h"
#include "backend/processing/ProcessingScience.h"
#include "backend/processing/ProcessingTypes.h"
#include "support/assert.h"

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace science = backend::processing::science;
using backend::services::FilterResult;
using backend::services::ProcessingConfig;

namespace {

constexpr int kWidth = 512;
constexpr int kHeight = 96;

std::vector<uint8_t> base64Decode(const std::string& in) {
    std::vector<int> map(256, -1);
    const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; ++i) map[static_cast<unsigned char>(alphabet[i])] = i;
    std::vector<uint8_t> out;
    out.reserve(in.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (const char ch : in) {
        const int v = map[static_cast<unsigned char>(ch)];
        if (v < 0) continue; // '=' padding
        acc = ((acc << 6) | static_cast<uint32_t>(v)) & 0xFFFFFFu; // keep 24 bits: no overflow
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

nlohmann::json readJson(const std::string& path) {
    std::ifstream in(path);
    MIB_REQUIRE(static_cast<bool>(in), "cannot open " + path);
    return nlohmann::json::parse(in);
}

// Page field values decoded to host units (bool / integer / Q formats).
std::map<std::string, double> decodePage(const nlohmann::json& fields, const nlohmann::json& page) {
    std::map<std::string, double> v;
    for (const auto& f : fields) {
        const uint32_t word = page.at(f.at("word").get<int>()).get<uint32_t>();
        const int lo = f.at("lo").get<int>();
        const int hi = f.at("hi").get<int>();
        const uint64_t mask = (uint64_t{1} << (hi - lo + 1)) - 1;
        const double raw = static_cast<double>((word >> lo) & mask);
        const std::string fmt = f.at("format").get<std::string>();
        double value = raw;
        if (fmt == "q16_16" || fmt == "q0_16") value = raw / 65536.0;
        else if (fmt == "q24_8") value = raw / 256.0;
        v[f.at("name").get<std::string>()] = value;
    }
    return v;
}

ProcessingConfig configFromPage(const std::map<std::string, double>& p) {
    ProcessingConfig c;
    c.processing_contract_version = backend::processing::contract::kProcessingContractVersionV3;
    c.enable_area_range_check = p.at("enable_area_range_check") != 0;
    c.enable_deformability_range_check = p.at("enable_deformability_range_check") != 0;
    c.enable_area_ratio_check = p.at("enable_area_ratio_check") != 0;
    c.enable_laplacian_variance_check = p.at("enable_laplacian_variance_check") != 0;
    c.enable_target_group = p.at("target_group_enabled") != 0;
    c.enable_target_group_emodulus = p.at("target_group_emodulus_enabled") != 0;
    c.channel_band_y = static_cast<int>(p.at("channel_band_y"));
    c.channel_band_h = static_cast<int>(p.at("channel_band_h"));
    c.min_cell_area_px = static_cast<int>(p.at("min_cell_area_px"));
    c.laplacian_kernel_size = static_cast<int>(p.at("laplacian_ksize"));
    // The host keeps whole um2 area thresholds; fractional page values are
    // rounded here, and gate decisions within 1 um2 of them are tolerated.
    c.area_threshold_min = static_cast<int>(std::lround(p.at("area_min_um2")));
    c.area_threshold_max = static_cast<int>(std::lround(p.at("area_max_um2")));
    c.deformability_threshold_min = p.at("deformability_min");
    c.deformability_threshold_max = p.at("deformability_max");
    c.area_ratio_threshold_max = p.at("area_ratio_max");
    c.laplacian_variance_min = p.at("laplacian_variance_min");
    c.laplacian_variance_max = p.at("laplacian_variance_max");
    c.target_group_area_min = static_cast<int>(std::lround(p.at("target_area_min_um2")));
    c.target_group_area_max = static_cast<int>(std::lround(p.at("target_area_max_um2")));
    c.target_group_deformability_min = p.at("target_deformability_min");
    c.target_group_deformability_max = p.at("target_deformability_max");
    c.target_group_emodulus_min = p.at("target_emodulus_min_kpa");
    c.target_group_emodulus_max = p.at("target_emodulus_max_kpa");
    return c;
}

bool loadTable(backend::EModulusLut& lut, const std::string& b64,
               const std::map<std::string, double>& p) {
    const std::vector<uint8_t> bytes = base64Decode(b64);
    constexpr size_t n = 200;
    if (bytes.size() != n * n * 2) return false;
    std::vector<double> grid(n * n);
    for (size_t i = 0; i < n * n; ++i) {
        const unsigned v = bytes[2 * i] | (bytes[2 * i + 1] << 8);
        grid[i] = v == 0xFFFF ? std::numeric_limits<double>::quiet_NaN() : v / 256.0; // Q8.8 kPa
    }
    return lut.loadGrid(p.at("lut_area_min_um2"), p.at("lut_area_step_um2"), n,
                        p.at("lut_deformability_min"), p.at("lut_deformability_step"), n,
                        std::move(grid));
}

// PL reason code: the host's first reason + 1 (NONE = 0 for a valid cell).
int hostReason(const FilterResult& r, const ProcessingConfig& c, double p2m) {
    const auto reasons = science::classifyInvalidReasons(r, c, p2m);
    return reasons.empty() ? 0 : static_cast<int>(reasons.front()) + 1;
}

struct Stats {
    int frames = 0;
    int cells = 0;
    int words = 0;
    int toleratedGates = 0;
    std::map<int, int> reasons;
};

bool near(double a, double b, double tol) { return std::abs(a - b) <= tol; }

void checkCell(const std::string& where, const FilterResult& r, const nlohmann::json& e,
               const ProcessingConfig& c, const std::map<std::string, double>& page, double p2m,
               Stats& st) {
    const auto& w = e.at("payload");
    auto word = [&](int i) { return w.at(i).get<uint32_t>(); };
    const uint32_t validity = e.at("validity").get<uint32_t>();
    auto valid = [&](int i) { return (validity >> i & 1u) != 0; };
    auto q16 = [&](int i) { return static_cast<int32_t>(word(i)) / 65536.0; };

    MIB_EXPECT(r.objectId == e.at("object_id").get<int>(), where + " object id");
    const auto& box = e.at("bbox");
    MIB_EXPECT(r.bboxX == box.at("x").get<double>() && r.bboxY == box.at("y").get<double>() &&
                   r.bboxWidth == box.at("w").get<double>() &&
                   r.bboxHeight == box.at("h").get<double>(),
               where + " bbox");
    MIB_EXPECT(r.touchesBorder == e.at("cut_off").get<bool>(), where + " cut-off");

    // always present: contour area, brightness, centroid, Laplacian, counts
    MIB_EXPECT(r.contourArea * 65536.0 == static_cast<double>(word(1)), where + " contour area (exact)");
    MIB_EXPECT(near(r.brightnessMean, word(6) / 65536.0, 1.0 / 65536.0), where + " brightness mean");
    MIB_EXPECT(near(r.brightnessVariance, word(14) / 256.0, 1.0 / 256.0), where + " brightness variance");
    MIB_EXPECT(near(r.centroidX, q16(7), 1.0 / 256.0) && near(r.centroidY, q16(8), 1.0 / 256.0),
               where + " centroid");
    MIB_EXPECT(near(r.laplacianVariance, word(11) / 256.0, 1.0 / 256.0 + 1e-7 * std::abs(r.laplacianVariance)),
               where + " Laplacian variance");
    MIB_EXPECT(r.pixelCount == static_cast<int>(word(13) & 0xFFFF) &&
                   r.blemishCount == static_cast<int>(word(13) >> 16),
               where + " pixel and blemish counts");
    MIB_EXPECT(r.objectCount == static_cast<int>(word(5) >> 24), where + " cell count");
    st.words += 9;

    if (valid(2)) { // metrics of a measured (not cut-off, not degenerate) cell
        MIB_EXPECT(r.area * 65536.0 == static_cast<double>(word(2)), where + " hull area (exact)");
        MIB_EXPECT(near(r.areaRatio, word(4) / 65536.0, 1e-4), where + " area ratio");
        MIB_EXPECT(near(r.deformability, (word(5) & 0xFFFF) / 65536.0, 1e-4), where + " deformability");
        MIB_EXPECT(near(r.area * p2m * p2m, word(9) / 65536.0, 1e-4), where + " area um2");
        const bool outOfCoverage = (word(0) >> 23 & 1u) != 0;
        MIB_EXPECT(std::isnan(r.youngsModulus) == outOfCoverage, where + " E-modulus coverage");
        if (valid(10)) {
            // one LUT cell: the bilinear value moves by at most the step of the
            // grid, far above this bound for the fixed-point coordinate error
            MIB_EXPECT(near(r.youngsModulus, word(10) / 65536.0, 0.01), where + " E-modulus");
        }
        st.words += 6;
    } else {
        MIB_EXPECT(r.area == 0.0 && (r.touchesBorder || r.degenerateContour),
                   where + " no metrics only when cut off or degenerate");
    }

    // gate decisions: reason and target group, exact unless an area sits
    // within the host's whole-um2 threshold quantisation
    const int reason = static_cast<int>(word(0) >> 16 & 15u);
    const bool target = (word(0) >> 24 & 1u) != 0;
    const int host = hostReason(r, c, p2m);
    if (host != reason || r.isTargetGroup != target) {
        const double a = r.area * p2m * p2m;
        const bool quantised =
            near(a, page.at("area_min_um2"), 1.0) || near(a, page.at("area_max_um2"), 1.0) ||
            near(a, page.at("target_area_min_um2"), 1.0) || near(a, page.at("target_area_max_um2"), 1.0);
        MIB_EXPECT(quantised, where + " reason " + std::to_string(host) + " vs PL " +
                                  std::to_string(reason) + ", target " +
                                  std::to_string(r.isTargetGroup) + " vs PL " + std::to_string(target));
        if (quantised) ++st.toleratedGates;
    }
    MIB_EXPECT(r.isValid == (reason == 0) || host != reason, where + " valid iff reason NONE");
    ++st.reasons[reason];
    ++st.cells;
}

void checkCase(const nlohmann::json& kase, const nlohmann::json& fields, Stats& st) {
    const auto page = decodePage(fields, kase.at("page"));
    const ProcessingConfig cfg = configFromPage(page);
    const double p2m = page.at("pixel_to_micron");
    backend::EModulusLut lut;
    MIB_REQUIRE(loadTable(lut, kase.at("table0_b64").get<std::string>(), page), "E-modulus table");
    const std::string name = kase.at("name").get<std::string>();

    for (const auto& f : kase.at("frames")) {
        const std::string where = name + "/" + f.at("name").get<std::string>();
        const std::vector<uint8_t> gray = base64Decode(f.at("gray_b64").get<std::string>());
        const std::vector<uint8_t> bits = base64Decode(f.at("mask_b64").get<std::string>());
        MIB_REQUIRE(gray.size() == kWidth * kHeight && bits.size() * 8 >= kWidth * kHeight, where);
        cv::Mat image(kHeight, kWidth, CV_8UC1);
        std::copy(gray.begin(), gray.end(), image.data);
        cv::Mat mask(kHeight, kWidth, CV_8UC1);
        for (int i = 0; i < kWidth * kHeight; ++i) {
            mask.data[i] = (bits[i / 8] >> (i % 8) & 1) ? 255 : 0; // packbits, little bit order
        }

        const auto results = science::filterProcessedObjects(mask, cv::Rect(0, 0, kWidth, kHeight), cfg,
                                                             image, p2m, &lut);
        ++st.frames;
        const int cells = f.at("cells").get<int>();
        MIB_REQUIRE(!results.empty(), where + " at least one result");
        MIB_EXPECT(results.front().blemishCount == f.at("blemishes").get<int>(), where + " blemishes");
        if (cells == 0) {
            MIB_EXPECT(results.size() == 1 && results.front().objectCount == 0 && !results.front().isValid,
                       where + " empty frame");
            continue;
        }
        MIB_EXPECT(static_cast<int>(results.size()) == cells, where + " cell count");
        if (f.at("truncated").get<bool>()) continue; // > 16 cells: the PL lists another order
        const auto& expected = f.at("results");
        MIB_REQUIRE(results.size() == expected.size(), where + " result count");
        for (size_t i = 0; i < expected.size(); ++i) {
            checkCell(where + "#" + std::to_string(i + 1), results[i], expected[i], cfg, page, p2m, st);
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    MIB_REQUIRE(argc > 1, "usage: processing_contract3_cells_conformance_test <unet-cells-v2-pl-vectors.json>");
    const nlohmann::json gold = readJson(argv[1]);
    nlohmann::json cases = gold.at("cases");
    if (const char* full = std::getenv("MIB_UNET_CELLS_VECTORS"); full && *full) {
        cases = readJson(full).at("cases");
        std::printf("full vectors: %s\n", full);
    }
    Stats st;
    for (const auto& kase : cases) {
        checkCase(kase, gold.at("page_fields"), st);
    }
    std::printf("contract 3 vs PL: %d frames, %d cells, %d words compared, %d gate decisions within "
                "the um2 threshold rounding; reasons:",
                st.frames, st.cells, st.words, st.toleratedGates);
    for (const auto& [code, n] : st.reasons) std::printf(" %d:%d", code, n);
    std::printf("\n");
    MIB_EXPECT(st.cells > 0, "compared at least one cell");
    return mib::test::exitCode();
}
