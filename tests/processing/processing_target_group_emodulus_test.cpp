// Target group, Young's modulus layer: the layer filters only when the E range
// is enabled AND an E-modulus table is loaded. Without a table no cell has an E
// (FilterResult::youngsModulus keeps its 0.0 placeholder), so the group is the
// area / deformability box alone ("two-layer filter"), whatever emodulus_min /
// emodulus_max say. With a table a cell whose lookup is NaN (outside it) still
// fails the layer.
//
// Covers science::isTargetGroupMember directly and, for the three object
// paths of filterProcessedObjects (Contract 1 inner contours, Contract 2 outer
// contours, Contract 3 U-Net cells), the decision it makes on a real mask, for
// a positive and a zero emodulus_min.
#include "backend/processing/EModulusLut.h"
#include "backend/processing/ProcessingContract.h"
#include "backend/processing/ProcessingScience.h"
#include "backend/processing/ProcessingTypes.h"
#include "support/assert.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace science = backend::processing::science;
namespace contract = backend::processing::contract;
using backend::EModulusLut;
using backend::services::FilterResult;
using backend::services::ProcessingConfig;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// A table with the same E everywhere in its coverage: area [0, 5000] um^2,
// deformability [0, 1].
EModulusLut flatLut(double e, double areaMax = 5000.0) {
    EModulusLut lut;
    const size_t bins = 11;
    std::vector<double> grid(bins * bins, e);
    MIB_REQUIRE(lut.loadGrid(0.0, areaMax / static_cast<double>(bins - 1), bins, 0.0,
                             1.0 / static_cast<double>(bins - 1), bins, std::move(grid)),
                "flat LUT loads");
    return lut;
}

ProcessingConfig targetConfig(bool eGate, double eMin, double eMax) {
    ProcessingConfig c;
    c.enable_target_group = true;
    c.target_group_area_min = 0;
    c.target_group_area_max = 100000;
    c.target_group_deformability_min = 0.0;
    c.target_group_deformability_max = 1.0;
    c.enable_target_group_emodulus = eGate;
    c.target_group_emodulus_min = eMin;
    c.target_group_emodulus_max = eMax;
    return c;
}

void testMemberRule() {
    const EModulusLut lut2 = flatLut(2.0);
    const EModulusLut unloaded;
    MIB_REQUIRE(!unloaded.isLoaded(), "a default table is not loaded");

    // ---- the box alone: no E gate -------------------------------------------------
    {
        ProcessingConfig c = targetConfig(false, 5.0, 10.0);
        MIB_EXPECT(science::isTargetGroupMember(c, nullptr, 100, 0.1, 0.0), "E gate off, no table: box only");
        MIB_EXPECT(science::isTargetGroupMember(c, &lut2, 100, 0.1, 2.0), "E gate off, table: box only");
        MIB_EXPECT(science::isTargetGroupMember(c, &lut2, 100, 0.1, kNaN), "E gate off: a NaN E is ignored");
        MIB_EXPECT(science::isTargetGroupMember(c, &lut2, 100, 0.1, 99.0), "E gate off: an E outside the range is ignored");
        c.target_group_area_max = 50;
        MIB_EXPECT(!science::isTargetGroupMember(c, &lut2, 100, 0.1, 2.0), "outside the area box: not target");
        c = targetConfig(false, 0.0, 10.0);
        c.target_group_deformability_max = 0.05;
        MIB_EXPECT(!science::isTargetGroupMember(c, nullptr, 100, 0.1, 0.0), "outside the deformability box: not target");
        c = targetConfig(false, 0.0, 10.0);
        c.enable_target_group = false;
        MIB_EXPECT(!science::isTargetGroupMember(c, &lut2, 100, 0.1, 2.0), "target group off: never target");
    }

    // ---- E gate on, no table: the E layer is skipped (min 0 and a positive min) -----
    for (const double eMin : {0.0, 5.0}) {
        const std::string tag = " (emodulus_min " + std::to_string(eMin) + ")";
        ProcessingConfig c = targetConfig(true, eMin, 10.0);
        MIB_EXPECT(science::isTargetGroupMember(c, nullptr, 100, 0.1, 0.0), "no table (null): box only" + tag);
        MIB_EXPECT(science::isTargetGroupMember(c, &unloaded, 100, 0.1, 0.0), "no table (unloaded): box only" + tag);
        MIB_EXPECT(science::isTargetGroupMember(c, nullptr, 100, 0.1, kNaN), "no table: the E is not read, NaN too" + tag);
        MIB_EXPECT(science::isTargetGroupMember(c, nullptr, 100, 0.1, 999.0), "no table: the E is not read, any value" + tag);
        c.target_group_area_min = 200;
        MIB_EXPECT(!science::isTargetGroupMember(c, nullptr, 100, 0.1, 0.0), "no table: a cell outside the box is still out" + tag);
        c = targetConfig(true, eMin, 10.0);
        c.target_group_deformability_min = 0.5;
        MIB_EXPECT(!science::isTargetGroupMember(c, &unloaded, 100, 0.1, 0.0), "no table: outside the deformability box is out" + tag);
    }

    // ---- E gate on, table loaded: the layer filters ---------------------------------
    {
        const ProcessingConfig c = targetConfig(true, 1.0, 3.0);
        MIB_EXPECT(science::isTargetGroupMember(c, &lut2, 100, 0.1, 2.0), "table: E inside the range");
        MIB_EXPECT(science::isTargetGroupMember(c, &lut2, 100, 0.1, 1.0) && science::isTargetGroupMember(c, &lut2, 100, 0.1, 3.0),
                   "table: the range bounds are inclusive");
        MIB_EXPECT(!science::isTargetGroupMember(c, &lut2, 100, 0.1, 0.999), "table: E below the range");
        MIB_EXPECT(!science::isTargetGroupMember(c, &lut2, 100, 0.1, 3.001), "table: E above the range");
        MIB_EXPECT(!science::isTargetGroupMember(c, &lut2, 100, 0.1, kNaN), "table: a NaN E (outside the table) fails the layer");
        MIB_EXPECT(!science::isTargetGroupMember(c, &lut2, 100, 0.1, 0.0), "table: the 0.0 placeholder is not special");
        ProcessingConfig outsideBox = c;
        outsideBox.target_group_area_max = 50;
        MIB_EXPECT(!science::isTargetGroupMember(outsideBox, &lut2, 100, 0.1, 2.0), "table: E fine but outside the box");
        const ProcessingConfig zeroMin = targetConfig(true, 0.0, 10.0);
        MIB_EXPECT(science::isTargetGroupMember(zeroMin, &lut2, 100, 0.1, 2.0), "table, min 0: E inside");
        MIB_EXPECT(!science::isTargetGroupMember(zeroMin, &lut2, 100, 0.1, kNaN), "table, min 0: a NaN E still fails");
    }
}

// ---- the decision on a real mask, for each object path ------------------------------

struct Scene {
    const char* name;
    int contractVersion;
    cv::Mat mask;      // 0 / 255
    cv::Mat original;  // Gray8
};

Scene makeScene(int contractVersion) {
    Scene s;
    s.contractVersion = contractVersion;
    s.original = cv::Mat(96, 160, CV_8UC1, cv::Scalar(128));
    s.mask = cv::Mat(96, 160, CV_8UC1, cv::Scalar(0));
    if (contractVersion == contract::kProcessingContractVersionV1) {
        s.name = "Contract 1 (inner contour)";
        cv::circle(s.mask, cv::Point(48, 48), 22, cv::Scalar(255), cv::FILLED);
        cv::circle(s.mask, cv::Point(48, 48), 11, cv::Scalar(0), cv::FILLED); // the hole is the object
    } else if (contractVersion == contract::kProcessingContractVersionV2) {
        s.name = "Contract 2 (outer contour)";
        cv::circle(s.mask, cv::Point(80, 48), 18, cv::Scalar(255), cv::FILLED);
    } else {
        s.name = "Contract 3 (U-Net cells)";
        cv::circle(s.mask, cv::Point(80, 48), 16, cv::Scalar(255), cv::FILLED);
    }
    return s;
}

ProcessingConfig sceneConfig(const Scene& scene, bool eGate, double eMin, double eMax) {
    ProcessingConfig c = targetConfig(eGate, eMin, eMax);
    c.processing_contract_version = scene.contractVersion;
    c.enable_area_range_check = false;
    c.enable_deformability_range_check = false;
    c.enable_area_ratio_check = false;
    c.enable_ring_ratio_check = false;
    c.enable_border_check = true;
    c.require_single_inner_contour = true;
    c.min_cell_area_px = 250;
    c.laplacian_kernel_size = 3;
    return c;
}

// The valid cells of the scene under `config` and `lut`.
std::vector<FilterResult> validCells(const Scene& scene, const ProcessingConfig& config, const EModulusLut* lut) {
    const cv::Rect roi(0, 0, scene.mask.cols, scene.mask.rows);
    const auto all = science::filterProcessedObjects(scene.mask, roi, config, scene.original, 1.0, lut);
    std::vector<FilterResult> valid;
    for (const auto& r : all) {
        if (r.isValid) valid.push_back(r);
    }
    return valid;
}

void expectTarget(const Scene& scene, const ProcessingConfig& config, const EModulusLut* lut, bool want,
                  const std::string& what) {
    const auto cells = validCells(scene, config, lut);
    MIB_REQUIRE(!cells.empty(), std::string(scene.name) + ": " + what + ": the scene has a valid cell");
    for (const auto& r : cells) {
        MIB_EXPECT(r.isTargetGroup == want, std::string(scene.name) + ": " + what + ": target " +
                                                std::to_string(r.isTargetGroup) + " (area " +
                                                std::to_string(r.area) + ", E " + std::to_string(r.youngsModulus) + ")");
    }
}

void testFilterObjects() {
    const EModulusLut lut2 = flatLut(2.0);
    const EModulusLut lutSmall = flatLut(2.0, 100.0); // covers up to 100 um^2: the scene's cells lie outside it
    const EModulusLut unloaded;

    for (const int version : {contract::kProcessingContractVersionV1, contract::kProcessingContractVersionV2,
                              contract::kProcessingContractVersionV3}) {
        const Scene scene = makeScene(version);

        // A reference: the cells exist, and their lookup in the flat table is that table's E.
        {
            const auto cells = validCells(scene, sceneConfig(scene, false, 0.0, 10.0), &lut2);
            MIB_REQUIRE(!cells.empty(), std::string(scene.name) + ": a valid cell");
            MIB_EXPECT(std::abs(cells.front().youngsModulus - 2.0) < 1e-9, std::string(scene.name) + ": E from the table");
            const auto noLut = validCells(scene, sceneConfig(scene, false, 0.0, 10.0), nullptr);
            MIB_REQUIRE(!noLut.empty(), std::string(scene.name) + ": a valid cell without a table");
            MIB_EXPECT(noLut.front().youngsModulus == 0.0, std::string(scene.name) + ": the E stays 0.0 without a table");
        }

        // The target group off never marks a cell.
        {
            ProcessingConfig off = sceneConfig(scene, true, 0.0, 10.0);
            off.enable_target_group = false;
            expectTarget(scene, off, &lut2, false, "target group off");
        }

        // No E gate: the box alone, with or without a table.
        expectTarget(scene, sceneConfig(scene, false, 5.0, 10.0), nullptr, true, "no E gate, no table");
        expectTarget(scene, sceneConfig(scene, false, 5.0, 10.0), &lut2, true, "no E gate, table (range ignored)");

        // E gate on, no table: the layer is skipped for a zero and a positive minimum.
        for (const double eMin : {0.0, 5.0}) {
            const std::string tag = "E gate, no table, min " + std::to_string(eMin);
            expectTarget(scene, sceneConfig(scene, true, eMin, 10.0), nullptr, true, tag + " (null)");
            expectTarget(scene, sceneConfig(scene, true, eMin, 10.0), &unloaded, true, tag + " (unloaded)");
            ProcessingConfig outside = sceneConfig(scene, true, eMin, 10.0);
            outside.target_group_area_min = 5000; // the box excludes the cell: still not target
            expectTarget(scene, outside, nullptr, false, tag + ", cell outside the box");
        }

        // E gate on, table loaded: the layer filters.
        expectTarget(scene, sceneConfig(scene, true, 1.0, 3.0), &lut2, true, "E gate, table, E in range");
        expectTarget(scene, sceneConfig(scene, true, 3.0, 4.0), &lut2, false, "E gate, table, E below the range");
        expectTarget(scene, sceneConfig(scene, true, 0.0, 1.0), &lut2, false, "E gate, table, E above the range");
        expectTarget(scene, sceneConfig(scene, true, 0.0, 10.0), &lutSmall, false,
                     "E gate, table, lookup outside the table (NaN) fails the layer");
        expectTarget(scene, sceneConfig(scene, false, 0.0, 10.0), &lutSmall, true,
                     "no E gate, lookup outside the table: box only");
    }
}

} // namespace

int main() {
    testMemberRule();
    testFilterObjects();
    return mib::test::exitCode();
}
