// YOFO S2 profile compiler (unet_cells_v2): the page field table equals the
// vendored profile; the host's default settings compile to exactly the page
// the board ran (pz7035-imx426 scripts/pz_results_check.py page.bin,
// committed through pzres config on 2026-10-03); the E-modulus grid becomes
// the Q8.8 table byte for byte; out-of-range settings are compile errors.
//
// argv[1]: third_party/pz7035-abi
// MIB_PZ_BOARD_LUT=<lut.bin>: also compare the table with the board's file.
#include "backend/processing/pz/PzProfileCompiler.h"

#include "support/assert.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

using namespace backend::processing::pz;

namespace {

// page.bin of results-hw-20261003/retrain-lit-run3 (sha256 6f73c8af9e9443b3...).
constexpr std::array<uint32_t, 32> kBoardPage = {
    0x98000000u, 0x00000000u, 0x000000FAu, 0x80030000u, 0x00008000u, 0x003C0000u, 0x01220000u, 0x00000000u,
    0x00008000u, 0x00018000u, 0x00000000u, 0x00000000u, 0x00480000u, 0x00BF0000u, 0x00000000u, 0x00004CCDu,
    0x00000000u, 0x000A0000u, 0x00000000u, 0x00140000u, 0x00018000u, 0x00A40000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u};

// The board's synthetic LUT (pz_cell_check.smooth_lut): Q8.8 of
// 0.5 + 0.03 a + 25 (d/200)^1.5 kPa, no value for d, a >= 190.
backend::EModulusLut boardLut() {
    std::vector<double> g(200 * 200);
    for (int d = 0; d < 200; ++d)
        for (int a = 0; a < 200; ++a)
            g[static_cast<size_t>(d * 200 + a)] = (d >= 190 && a >= 190)
                                                      ? std::numeric_limits<double>::quiet_NaN()
                                                      : 0.5 + 0.03 * a + 25.0 * std::pow(d / 200.0, 1.5);
    backend::EModulusLut lut;
    MIB_REQUIRE(lut.loadGrid(20.0, 1.5, 200, 0.0, 0.0025, 200, std::move(g)), "board LUT grid loads");
    return lut;
}

UnetCellsProfileInputs hostDefaults(const backend::EModulusLut* lut) {
    UnetCellsProfileInputs in;
    auto& c = in.config;
    // resources/defaults gates as the board run used them (no ring in v2)
    c.enable_area_range_check = true;
    c.area_threshold_min = 60;
    c.area_threshold_max = 290;
    c.enable_deformability_range_check = true;
    c.deformability_threshold_min = 0.0;
    c.deformability_threshold_max = 0.5;
    c.enable_area_ratio_check = false;
    c.area_ratio_threshold_max = 1.5;
    c.enable_target_group = true;
    c.target_group_area_min = 72;
    c.target_group_area_max = 191;
    c.target_group_deformability_min = 0.0;
    c.target_group_deformability_max = 0.3;
    c.enable_target_group_emodulus = true;
    c.target_group_emodulus_min = 0.0;
    c.target_group_emodulus_max = 10.0;
    c.min_cell_area_px = 250;
    c.laplacian_kernel_size = 3;
    in.pixelToMicron = 0.5;
    in.lut = lut;
    return in;
}

void testFieldTableMatchesProfile(const std::string& abiDir) {
    std::ifstream f(abiDir + "/profiles/unet_cells_v2.json");
    MIB_REQUIRE(static_cast<bool>(f), "vendored profile");
    const auto profile = nlohmann::json::parse(f);
    const auto& page = profile.at("page");
    const auto& table = unetCellsV2PageFields();
    MIB_REQUIRE(page.size() == table.size(), "same number of page fields");
    for (size_t i = 0; i < table.size(); ++i) {
        const auto& j = page[i];
        MIB_EXPECT(j.at("name") == table[i].name && j.at("word") == table[i].word &&
                       j.at("bits")[0] == table[i].lo && j.at("bits")[1] == table[i].hi &&
                       j.at("format") == table[i].format,
                   std::string("field ") + table[i].name + " matches the profile");
    }
    MIB_EXPECT(profile.at("page_words") == 32, "32 page words");
}

void testHostDefaultsCompileToTheBoardPage() {
    const auto lut = boardLut();
    const auto p = compileUnetCellsV2(hostDefaults(&lut));
    MIB_REQUIRE(p.ok(), "defaults compile");
    for (size_t i = 0; i < 32; ++i) {
        MIB_EXPECT(p.page[i] == kBoardPage[i], "page word " + std::to_string(i));
    }
    MIB_REQUIRE(p.table0.size() == 80000, "table is 200 x 200 u16");
    size_t mismatches = 0;
    for (int d = 0; d < 200; ++d)
        for (int a = 0; a < 200; ++a) {
            const size_t i = static_cast<size_t>(d * 200 + a);
            const unsigned got = p.table0[2 * i] | p.table0[2 * i + 1] << 8;
            const unsigned want = (d >= 190 && a >= 190)
                                      ? 0xFFFFu
                                      : static_cast<unsigned>(std::llround(
                                            (0.5 + 0.03 * a + 25.0 * std::pow(d / 200.0, 1.5)) * 256.0));
            mismatches += got != want;
        }
    MIB_EXPECT(mismatches == 0, "table equals the Q8.8 grid: " + std::to_string(mismatches) + " differ");
    if (const char* path = std::getenv("MIB_PZ_BOARD_LUT"); path && *path) {
        std::ifstream in(path, std::ios::binary);
        std::vector<uint8_t> board((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        MIB_EXPECT(board == p.table0, "table equals the board's lut.bin byte for byte");
        std::printf("board lut.bin compared: %s\n", board == p.table0 ? "equal" : "DIFFERENT");
    }
}

void testOutOfRangeIsAnError() {
    const auto lut = boardLut();
    auto bad = [&](auto mutate, const std::string& what) {
        auto in = hostDefaults(&lut);
        mutate(in);
        const auto p = compileUnetCellsV2(in);
        MIB_EXPECT(!p.ok(), what + " is a compile error");
    };
    bad([](UnetCellsProfileInputs& in) { in.pixelToMicron = 0; }, "p2m 0");
    bad([](UnetCellsProfileInputs& in) { in.config.laplacian_kernel_size = 2; }, "ksize 2");
    bad([](UnetCellsProfileInputs& in) { in.config.min_cell_area_px = 70000; }, "min px over u16");
    bad([](UnetCellsProfileInputs& in) { in.config.area_threshold_max = -1; }, "negative area");
    bad([](UnetCellsProfileInputs& in) { in.config.deformability_threshold_max = 1.5; }, "deformability > 1");
    bad([](UnetCellsProfileInputs& in) { in.config.laplacian_variance_max = 2e7; }, "Laplacian over Q24.8");
    bad([](UnetCellsProfileInputs& in) { in.lut = nullptr; }, "E-modulus target gate without a LUT");
    bad([](UnetCellsProfileInputs& in) {
        in.config.channel_band_y = -3;
        in.config.channel_band_h = 50;
    }, "negative band");

    auto in = hostDefaults(nullptr);
    in.config.enable_target_group_emodulus = false;
    in.config.deformability_threshold_max = 1.0;
    in.config.channel_band_y = 12;
    in.config.channel_band_h = 70;
    in.config.multi_image_enabled = true;
    in.config.multi_image_count = 5;
    in.storeInvalidEveryN = 100;
    const auto p = compileUnetCellsV2(in);
    MIB_EXPECT(p.ok() && p.table0.empty(), "no LUT and no E-modulus gate is fine, without a table");
    MIB_EXPECT((p.page[8] & 0xFFFF) == 65535, "deformability 1.0 is held as 65535");
    MIB_EXPECT(p.page[1] == (70u << 16 | 12u), "channel band word");
    MIB_EXPECT(p.page[18] == (100u | 5u << 16), "store word: invalid every 100, 5 multi-image frames");
    MIB_EXPECT(p.page[19] == 0 && p.page[20] == 0 && p.page[21] == 0, "no LUT axes without a LUT");
}

} // namespace

int main(int argc, char** argv) {
    MIB_REQUIRE(argc > 1, "usage: pz_profile_compiler_test <third_party/pz7035-abi>");
    testFieldTableMatchesProfile(argv[1]);
    testHostDefaultsCompileToTheBoardPage();
    testOutOfRangeIsAnError();
    return mib::test::exitCode();
}
