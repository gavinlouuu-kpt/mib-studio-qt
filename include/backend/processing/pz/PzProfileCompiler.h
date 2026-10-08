#pragma once

// Profile compiler for the PZ7035 U-Net cell profile (YOFO impl spec S2,
// pz7035-imx426 abi/profiles/unet_cells_v2.json): the operator's settings ->
// the 32-word profile page and the E-modulus table (PROFILE_TABLE0) the PL
// cell stage runs on. Values out of a field's fixed-point range are compile
// errors (readiness gate processing.profileCompile), never silently clamped,
// except deformability 1.0 -> 65535 (Q0.16 cannot hold 1.0; the PL's
// deformability is <= 65535 by construction).

#include "backend/processing/EModulusLut.h"
#include "backend/processing/ProcessingTypes.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace backend::processing::pz {

struct UnetCellsProfileInputs {
    services::ProcessingConfig config; // gates, target group, cell parameters, channel band
    double pixelToMicron{0.0};         // um per px
    // Store policy (page word 18): invalid frames every N (0 = never), the
    // multi-image count from config.multi_image_*, and the store flags.
    uint32_t storeInvalidEveryN{0};
    bool storeValidAnchored{false};
    bool storeMask{false};
    bool storeAllFrames{false};
    const EModulusLut* lut{nullptr}; // null or unloaded: no table, LUT axes 0, E-modulus gates impossible
};

struct CompiledProfile {
    uint16_t scienceProfile{2};
    uint16_t profileVersion{2};
    std::array<uint32_t, 32> page{};
    std::vector<uint8_t> table0; // 200 x 200 u16 LE, Q8.8 kPa, 0xFFFF = no value; empty without a LUT
    std::vector<std::string> errors;
    // Settings the operator changed from their defaults that the PL does not implement (no effect on this
    // instrument): the profile still compiles, ok() stays true (#651 G7).
    std::vector<std::string> warnings;
    bool ok() const { return errors.empty(); }
};

// One page field as the profile JSON defines it (exposed for the test that
// checks this table against the vendored profile).
struct PageField {
    const char* name;
    int word;
    int lo;
    int hi;
    const char* format; // bool, u4, u8, u16, q16_16, q0_16, q24_8
};
const std::vector<PageField>& unetCellsV2PageFields();

// The names of the settings the PL does not implement that differ from their defaults (the content of
// CompiledProfile::warnings).
std::vector<std::string> plIgnoredSettingsChanged(const ProcessingConfig& config);

CompiledProfile compileUnetCellsV2(const UnetCellsProfileInputs& in);

} // namespace backend::processing::pz
