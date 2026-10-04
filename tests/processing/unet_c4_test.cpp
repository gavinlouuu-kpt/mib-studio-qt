// W3.D: the host C4 U-Net (UnetC4) is bit-exact with the PZ7035 PL network.
// Every fixture of the model release (fresh QONNX executions of the exported
// network, the goldens the PL engine equals on hardware) must give the same
// 96x512 output codes, and the foreground masks follow.
//
// MIB_UNET_C4_PARAMS=<release>/c4_multiline_params.npz
// MIB_UNET_C4_FIXTURES=<release>/fixtures   (manifest.json + *.input.i8 / *.output.i8)
// MIB_UNET_C4_BOARD_CAPTURES=<dir> (optional): c<N>.raw Mono8 frames and the
// c<N>.mask the PL produced for them on the board (packed bits, little order),
// e.g. results-hw-20261003/retrain-lit-run3: the host mask must equal the PL's.
// CTest points both at the provisioned asset unet-c4-multiline-v1
// (env/assets.json, private Hub repo, token); without it the test reports
// SKIP (77).
#include "backend/processing/UnetC4.h"

#include "support/assert.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using backend::processing::UnetC4;

namespace {
std::vector<char> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
} // namespace

int main() {
    std::string error;
    MIB_EXPECT(!UnetC4::loadNpz("/nonexistent/params.npz", &error) && !error.empty(), "missing file is an error");

    const char* params = std::getenv("MIB_UNET_C4_PARAMS");
    const char* fixtures = std::getenv("MIB_UNET_C4_FIXTURES");
    if (!params || !*params || !fixtures || !*fixtures || !std::filesystem::exists(params) ||
        !std::filesystem::exists(std::string(fixtures) + "/manifest.json")) {
        std::printf("SKIP: provision the model (scripts/provision-assets.py --asset unet-c4-multiline-v1, "
                    "token) or set MIB_UNET_C4_PARAMS and MIB_UNET_C4_FIXTURES\n");
        return mib::test::exitCode() == 0 ? 77 : 1;
    }
    const auto model = UnetC4::loadNpz(params, &error);
    MIB_REQUIRE(model.has_value(), "params load: " + error);

    std::ifstream mf(std::string(fixtures) + "/manifest.json");
    MIB_REQUIRE(static_cast<bool>(mf), "fixture manifest");
    const auto manifest = nlohmann::json::parse(mf);
    int exact = 0, total = 0;
    long long fgPixels = 0;
    double ms = 0;
    for (const auto& f : manifest.at("frames")) {
        const auto in = readFile(std::string(fixtures) + "/" + f.at("input").get<std::string>());
        const auto want = readFile(std::string(fixtures) + "/" + f.at("output").get<std::string>());
        MIB_REQUIRE(in.size() == 96 * 512 && want.size() == 96 * 512, "fixture sizes");
        cv::Mat codes(96, 512, CV_8SC1, const_cast<char*>(in.data()));
        const auto t0 = std::chrono::steady_clock::now();
        const cv::Mat out = model->run(codes);
        ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        int diff = 0;
        for (int i = 0; i < 96 * 512; ++i) {
            diff += out.ptr<int8_t>()[i] != static_cast<int8_t>(want[static_cast<size_t>(i)]);
            fgPixels += out.ptr<int8_t>()[i] > 0;
        }
        if (diff) std::printf("frame %s: %d codes differ\n", f.at("original_frame_id").dump().c_str(), diff);
        exact += diff == 0;
        ++total;
    }
    std::printf("UnetC4: %d/%d fixtures bit-exact, %lld foreground px, %.1f ms/frame\n", exact, total, fgPixels,
                total ? ms / total : 0.0);
    MIB_EXPECT(total > 0 && exact == total, "every fixture bit-exact");

    // foregroundMask(raw) == run(raw XOR 0x80) > 0
    const auto& f0 = manifest.at("frames").at(0);
    const auto raw = readFile(std::string(fixtures) + "/" + f0.at("raw").get<std::string>());
    const auto want = readFile(std::string(fixtures) + "/" + f0.at("output").get<std::string>());
    const cv::Mat mask = model->foregroundMask(cv::Mat(96, 512, CV_8UC1, const_cast<char*>(raw.data())));
    int bad = 0;
    for (int i = 0; i < 96 * 512; ++i)
        bad += (mask.ptr<uint8_t>()[i] != 0) != (static_cast<int8_t>(want[static_cast<size_t>(i)]) > 0);
    MIB_EXPECT(bad == 0, "foreground mask from raw Mono8 equals code > 0");

    if (const char* captures = std::getenv("MIB_UNET_C4_BOARD_CAPTURES"); captures && *captures) {
        int frames = 0, equal = 0;
        for (const auto& entry : std::filesystem::directory_iterator(captures)) {
            const auto path = entry.path();
            if (path.extension() != ".raw") continue;
            auto maskPath = path;
            maskPath.replace_extension(".mask");
            if (!std::filesystem::exists(maskPath)) continue;
            const auto gray = readFile(path.string());
            const auto packed = readFile(maskPath.string());
            MIB_REQUIRE(gray.size() == 96 * 512 && packed.size() == 96 * 512 / 8, "capture sizes " + path.string());
            const cv::Mat host = model->foregroundMask(cv::Mat(96, 512, CV_8UC1, const_cast<char*>(gray.data())));
            int differ = 0;
            for (int i = 0; i < 96 * 512; ++i) {
                const bool pl = (static_cast<uint8_t>(packed[static_cast<size_t>(i / 8)]) >> (i % 8)) & 1;
                differ += pl != (host.ptr<uint8_t>()[i] != 0);
            }
            if (differ) std::printf("%s: %d mask pixels differ from the PL\n", path.filename().c_str(), differ);
            equal += differ == 0;
            ++frames;
        }
        std::printf("board captures: %d/%d host masks equal the PL's\n", equal, frames);
        MIB_EXPECT(frames > 0 && equal == frames, "host masks equal the PL masks captured on the board");
    }
    return mib::test::exitCode();
}
