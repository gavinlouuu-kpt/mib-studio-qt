// The whole PZ7035 cell chain, host against PL, on live IMX426 frames: for
// every frame captured during a board run (raw Mono8), the host computes the
// U-Net mask (UnetC4) and the Contract 3 cells with the page and E-modulus
// table the PL ran; the PL's RESULT records for the same frame id (from the
// result ring the run captured, decoded by backend::pz) must describe the
// same cells.
//
// argv[1]: scripts/conformance/unet-cells-v2-pl-vectors.json (page field table)
// MIB_PZ_BOARD_RUN=<run dir>: board.log ("frame i id N ..."), c<i>.raw,
//   page.bin, lut.bin, ring.bin (e.g. results-hw-20261003/retrain-lit-run3)
// MIB_UNET_C4_PARAMS=<the model release .npz the run used>
// Without them the test reports SKIP (77).
#include "backend/processing/ProcessingScience.h"
#include "backend/processing/UnetC4.h"
#include "backend/pz/PzRecords.h"

#include "support/assert.h"
#include "support/unet_cells_page.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <string>
#include <vector>

namespace pz = backend::pz;
namespace science = backend::processing::science;
using namespace mib::test::unet_cells;

namespace {
std::vector<uint8_t> readBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    MIB_REQUIRE(static_cast<bool>(in), "cannot open " + path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
} // namespace

int main(int argc, char** argv) {
    MIB_REQUIRE(argc > 1, "usage: pz_board_run_host_test <unet-cells-v2-pl-vectors.json>");
    const char* run = std::getenv("MIB_PZ_BOARD_RUN");
    const char* params = std::getenv("MIB_UNET_C4_PARAMS");
    if (!run || !*run || !params || !*params) {
        std::printf("SKIP: set MIB_PZ_BOARD_RUN and MIB_UNET_C4_PARAMS\n");
        return 77;
    }
    const std::string dir(run);
    std::string error;
    const auto model = backend::processing::UnetC4::loadNpz(params, &error);
    MIB_REQUIRE(model.has_value(), "model: " + error);

    // The page and table the PL ran.
    const auto pageBytes = readBytes(dir + "/page.bin");
    MIB_REQUIRE(pageBytes.size() == 128, "page.bin is 32 words");
    nlohmann::json pageWords = nlohmann::json::array();
    for (int i = 0; i < 32; ++i) {
        uint32_t w = 0;
        std::memcpy(&w, &pageBytes[static_cast<size_t>(4 * i)], 4);
        pageWords.push_back(w);
    }
    const auto page = decodePage(readJson(argv[1]).at("page_fields"), pageWords);
    const ProcessingConfig cfg = configFromPage(page);
    const double p2m = page.at("pixel_to_micron");
    backend::EModulusLut lut;
    MIB_REQUIRE(loadTableBytes(lut, readBytes(dir + "/lut.bin"), page), "lut.bin");

    // The PL's frames from the ring capture, by frame id.
    const auto ring = readBytes(dir + "/ring.bin");
    std::map<uint64_t, pz::FrameResults> plFrames;
    pz::FrameAssembler assembler;
    for (const auto& [off, len] : pz::splitRecords(ring.data(), ring.size())) {
        const auto d = pz::decodeRecord(ring.data() + off, len);
        MIB_REQUIRE(d.ok(), std::string("ring record: ") + pz::decodeErrorName(d.error));
        for (auto& fr : assembler.add(*d.record)) plFrames.emplace(fr.frame.frameId, std::move(fr));
    }

    // Captured frames: "frame <i> id <frame_id> ..." in board.log.
    std::ifstream log(dir + "/board.log");
    const std::regex line(R"(^frame (\d+) id (\d+) )");
    std::string text;
    int frames = 0, cells = 0, emod = 0;
    while (std::getline(log, text)) {
        std::smatch m;
        if (!std::regex_search(text, m, line)) continue;
        const std::string idx = m[1].str();
        const uint64_t frameId = std::stoull(m[2].str());
        const auto it = plFrames.find(frameId);
        MIB_REQUIRE(it != plFrames.end(), "captured frame " + std::to_string(frameId) + " is in the ring");
        const auto raw = readBytes(dir + "/c" + idx + ".raw");
        const cv::Mat gray(96, 512, CV_8UC1, const_cast<uint8_t*>(raw.data()));
        const cv::Mat mask = model->foregroundMask(gray);
        const auto host = science::filterProcessedObjects(mask, cv::Rect(0, 0, 512, 96), cfg, gray, p2m, &lut);
        const auto& pl = it->second;
        const std::string where = "frame " + std::to_string(frameId);
        const bool hostEmpty = host.size() == 1 && host.front().objectCount == 0;
        MIB_EXPECT((pl.frame.flags & pz::kFrameEmpty) != 0 == hostEmpty, where + " EMPTY");
        MIB_EXPECT(hostEmpty ? pl.results.empty() : pl.results.size() == host.size(), where + " cell count");
        for (size_t k = 0; k < pl.results.size() && !hostEmpty && k < host.size(); ++k) {
            const auto c = pz::decodeUnetCellsV2(pl.results[k]);
            MIB_REQUIRE(c.has_value(), where + " profile payload");
            const auto& h = host[k];
            const std::string w = where + " cell " + std::to_string(k + 1);
            MIB_EXPECT(c->objectId == h.objectId && c->cellCount == h.objectCount &&
                           c->blemishCount == h.blemishCount && c->pixelCount == h.pixelCount,
                       w + " ids and counts");
            MIB_EXPECT(c->bboxX == h.bboxX && c->bboxY == h.bboxY && c->bboxWidth == h.bboxWidth &&
                           c->bboxHeight == h.bboxHeight && c->cutOff == h.touchesBorder,
                       w + " bbox and cut-off");
            MIB_EXPECT(c->contourArea == h.contourArea && std::abs(c->centroidX - h.centroidX) <= 1.0 / 256 &&
                           std::abs(c->centroidY - h.centroidY) <= 1.0 / 256,
                       w + " contour area and centroid");
            MIB_EXPECT(std::abs(c->brightnessMean - h.brightnessMean) <= 1.0 / 65536 &&
                           std::abs(c->brightnessVariance - h.brightnessVariance) <= 1.0 / 256 &&
                           std::abs(c->laplacianVariance - h.laplacianVariance) <=
                               1.0 / 256 + 1e-7 * std::abs(h.laplacianVariance),
                       w + " brightness and Laplacian");
            if (!c->cutOff && !c->degenerate()) {
                MIB_EXPECT(c->hullArea == h.area && std::abs(c->deformability - h.deformability) <= 1e-4 &&
                               std::abs(c->areaRatio - h.areaRatio) <= 1e-4,
                           w + " shape");
                MIB_EXPECT(std::isnan(c->youngsModulusKpa) == std::isnan(h.youngsModulus) &&
                               (std::isnan(h.youngsModulus) || std::abs(c->youngsModulusKpa - h.youngsModulus) <= 0.01),
                           w + " E-modulus");
                emod += std::isfinite(h.youngsModulus) ? 1 : 0;
            }
            MIB_EXPECT(c->valid() == h.isValid && c->target == h.isTargetGroup, w + " validity and target");
            ++cells;
        }
        ++frames;
    }
    std::printf("board run %s: %d captured frames, %d cells equal host vs PL (%d with an E-modulus)\n", run, frames,
                cells, emod);
    MIB_EXPECT(frames > 0, "captured frames found");
    return mib::test::exitCode();
}
