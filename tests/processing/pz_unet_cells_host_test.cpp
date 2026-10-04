// Contract 3 closes the loop with the PZ7035 PL: the PL's RESULT payloads
// (conformance vectors), encoded and decoded through the record path
// (backend::pz), describe the same cells as the host Contract 3 science on the
// same frames: ids, bbox, cut-off, counts, centroid, brightness and shape.
//
// argv[1]: scripts/conformance/unet-cells-v2-pl-vectors.json
#include "backend/pz/PzRecords.h"

#include "backend/processing/ProcessingContract.h"
#include "backend/processing/ProcessingScience.h"
#include "support/assert.h"

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace pz = backend::pz;
namespace science = backend::processing::science;
using backend::services::FilterResult;
using backend::services::ProcessingConfig;

namespace {

nlohmann::json readJson(const std::string& path) {
    std::ifstream in(path);
    MIB_REQUIRE(static_cast<bool>(in), "cannot open " + path);
    return nlohmann::json::parse(in);
}

std::vector<uint8_t> base64Decode(const std::string& in) {
    std::vector<int> map(256, -1);
    const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; ++i) map[static_cast<unsigned char>(a[i])] = i;
    std::vector<uint8_t> out;
    int acc = 0, bits = 0;
    for (char ch : in) {
        const int v = map[static_cast<unsigned char>(ch)];
        if (v < 0) continue;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

void testProfileAgainstHostScience(const std::string& vectorsPath) {
    const auto doc = readJson(vectorsPath);
    int cells = 0;
    for (const auto& kase : doc.at("cases")) {
        // Page values that matter here: p2m, ksize, min px, band, gates off.
        std::map<std::string, double> page;
        for (const auto& fld : doc.at("page_fields")) {
            const uint32_t word = kase.at("page").at(fld.at("word").get<int>()).get<uint32_t>();
            const int lo = fld.at("lo").get<int>(), hi = fld.at("hi").get<int>();
            double v = static_cast<double>((word >> lo) & ((uint64_t{1} << (hi - lo + 1)) - 1));
            const auto fmt = fld.at("format").get<std::string>();
            if (fmt == "q16_16" || fmt == "q0_16") v /= 65536.0;
            if (fmt == "q24_8") v /= 256.0;
            page[fld.at("name").get<std::string>()] = v;
        }
        ProcessingConfig cfg;
        cfg.processing_contract_version = backend::processing::contract::kProcessingContractVersionV3;
        cfg.min_cell_area_px = static_cast<int>(page["min_cell_area_px"]);
        cfg.laplacian_kernel_size = static_cast<int>(page["laplacian_ksize"]);
        cfg.channel_band_y = static_cast<int>(page["channel_band_y"]);
        cfg.channel_band_h = static_cast<int>(page["channel_band_h"]);
        cfg.enable_area_range_check = cfg.enable_deformability_range_check = false;
        cfg.enable_area_ratio_check = cfg.enable_laplacian_variance_check = false;
        const double p2m = page["pixel_to_micron"];

        uint32_t seq = 1;
        for (const auto& frame : kase.at("frames")) {
            const auto gray = base64Decode(frame.at("gray_b64").get<std::string>());
            const auto bits = base64Decode(frame.at("mask_b64").get<std::string>());
            cv::Mat image(96, 512, CV_8UC1);
            std::copy(gray.begin(), gray.end(), image.data);
            cv::Mat mask(96, 512, CV_8UC1);
            for (int i = 0; i < 512 * 96; ++i) mask.data[i] = (bits[i / 8] >> (i % 8) & 1) ? 255 : 0;
            const auto host = science::filterProcessedObjects(mask, cv::Rect(0, 0, 512, 96), cfg, image, p2m, nullptr);
            const auto& expected = frame.at("results");
            for (size_t i = 0; i < expected.size() && i < host.size(); ++i) {
                const auto& e = expected[i];
                pz::ResultRecord r;
                r.frameId = 42;
                r.resultIndex = static_cast<uint16_t>(i);
                r.flags = e.at("target").get<bool>() ? pz::kResultTarget : 0;
                r.scienceProfile = pz::kScienceProfileUnetCells;
                r.profileVersion = pz::kUnetCellsProfileVersion;
                r.bboxX = e.at("bbox").at("x").get<uint16_t>();
                r.bboxY = e.at("bbox").at("y").get<uint16_t>();
                r.bboxW = e.at("bbox").at("w").get<uint16_t>();
                r.bboxH = e.at("bbox").at("h").get<uint16_t>();
                r.payloadValidity = e.at("validity").get<uint32_t>();
                r.payload = e.at("payload").get<std::vector<uint32_t>>();
                const auto bytes = pz::encodeResultRecord(r, seq++);
                const auto d = pz::decodeRecord(bytes.data(), bytes.size());
                MIB_REQUIRE(d.ok(), "vector RESULT decodes");
                const auto cell = pz::decodeUnetCellsV2(std::get<pz::ResultRecord>(d.record->body));
                MIB_REQUIRE(cell.has_value(), "unet_cells_v2 payload decodes");
                const pz::UnetCell& pl = *cell;
                const FilterResult& h = host[i];
                const std::string where = kase.at("name").get<std::string>() + "/" +
                                          frame.at("name").get<std::string>() + "#" + std::to_string(i + 1);
                MIB_EXPECT(pl.objectId == h.objectId && pl.cellCount == h.objectCount, where + " ids");
                MIB_EXPECT(pl.bboxX == h.bboxX && pl.bboxY == h.bboxY && pl.bboxWidth == h.bboxWidth &&
                               pl.bboxHeight == h.bboxHeight,
                           where + " bbox");
                MIB_EXPECT(pl.cutOff == h.touchesBorder && pl.degenerate() == h.degenerateContour,
                           where + " cut-off / degenerate");
                MIB_EXPECT(pl.contourArea == h.contourArea && pl.pixelCount == h.pixelCount &&
                               pl.blemishCount == h.blemishCount,
                           where + " areas and counts");
                MIB_EXPECT(std::abs(pl.centroidX - h.centroidX) <= 1.0 / 256 &&
                               std::abs(pl.centroidY - h.centroidY) <= 1.0 / 256,
                           where + " centroid");
                MIB_EXPECT(std::abs(pl.brightnessMean - h.brightnessMean) <= 1.0 / 65536 &&
                               std::abs(pl.brightnessVariance - h.brightnessVariance) <= 1.0 / 256,
                           where + " brightness");
                if (!pl.cutOff && !pl.degenerate()) {
                    MIB_EXPECT(pl.hullArea == h.area && std::abs(pl.areaRatio - h.areaRatio) <= 1e-4 &&
                                   std::abs(pl.deformability - h.deformability) <= 1e-4,
                               where + " shape metrics");
                    MIB_EXPECT(std::abs(pl.areaUm2 - h.area * p2m * p2m) <= 1e-4, where + " area um2");
                }
                ++cells;
            }
        }
    }
    std::printf("unet_cells_v2: %d PL results decoded through the record path equal the host science\n", cells);
    MIB_EXPECT(cells > 0, "compared cells");

}

} // namespace

int main(int argc, char** argv) {
    MIB_REQUIRE(argc > 1, "usage: pz_unet_cells_host_test <unet-cells-v2-pl-vectors.json>");
    testProfileAgainstHostScience(argv[1]);
    return mib::test::exitCode();
}
