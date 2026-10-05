#pragma once
// Shared by the Contract 3 / PZ7035 tests: base64, JSON files, the
// unet_cells_v2 page decoded (with the field table the PL vectors carry) into
// a Contract 3 ProcessingConfig, and the E-modulus table.
#include "backend/processing/EModulusLut.h"
#include "backend/processing/ProcessingContract.h"
#include "backend/processing/ProcessingTypes.h"
#include "support/assert.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace mib::test::unet_cells {

using backend::services::ProcessingConfig;

inline std::vector<uint8_t> base64Decode(const std::string& in) {
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

inline nlohmann::json readJson(const std::string& path) {
    std::ifstream in(path);
    MIB_REQUIRE(static_cast<bool>(in), "cannot open " + path);
    return nlohmann::json::parse(in);
}

// Page field values decoded to host units (bool / integer / Q formats).
inline std::map<std::string, double> decodePage(const nlohmann::json& fields, const nlohmann::json& page) {
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

inline ProcessingConfig configFromPage(const std::map<std::string, double>& p) {
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

// PROFILE_TABLE0 bytes (200 x 200 u16 LE, Q8.8 kPa, 0xFFFF = none) with the
// page's LUT axes.
inline bool loadTableBytes(backend::EModulusLut& lut, const std::vector<uint8_t>& bytes,
                    const std::map<std::string, double>& p) {
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

inline bool loadTable(backend::EModulusLut& lut, const std::string& b64, const std::map<std::string, double>& p) {
    return loadTableBytes(lut, base64Decode(b64), p);
}

} // namespace mib::test::unet_cells
