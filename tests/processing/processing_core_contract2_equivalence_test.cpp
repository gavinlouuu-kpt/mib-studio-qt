// ADR 0007 / Contract 2 rollout T1.1a: the absdiff-laplacian (Contract 2)
// native core owns its object science and must reproduce the host's bundled
// Contract-2 science exactly. The core is loaded through the real loader
// (engine ABI v2 negotiation, descriptor + capability checks) and run through
// ProcessingService next to a bundled Contract-2 kernel on the same frames and
// configs; every result field must match.
#include "backend/processing/IProcessingKernel.h"
#include "backend/processing/ProcessingCoreAbi.h"
#include "backend/processing/ProcessingCoreLoader.h"
#include "backend/processing/ProcessingService.h"
#include "support/assert.h"

#include <cmath>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace {

using backend::services::FilterResult;
using backend::services::ProcessedFrame;
using backend::services::ProcessingConfig;
using backend::services::ProcessingService;

bool sameDouble(double a, double b) {
    return (std::isnan(a) && std::isnan(b)) || a == b;
}

std::string describe(const FilterResult& a, const FilterResult& b) {
    return "object " + std::to_string(a.objectId) + "/" + std::to_string(b.objectId) +
           " area " + std::to_string(a.area) + "/" + std::to_string(b.area) + " valid " +
           std::to_string(a.isValid) + "/" + std::to_string(b.isValid);
}

bool sameResult(const FilterResult& a, const FilterResult& b) {
    return a.isValid == b.isValid && a.touchesBorder == b.touchesBorder &&
           a.inRange == b.inRange && a.inChannel == b.inChannel &&
           a.hasSingleInnerContour == b.hasSingleInnerContour &&
           a.innerContourCount == b.innerContourCount && a.objectId == b.objectId &&
           a.objectCount == b.objectCount && a.trackId == b.trackId &&
           sameDouble(a.bboxX, b.bboxX) && sameDouble(a.bboxY, b.bboxY) &&
           sameDouble(a.bboxWidth, b.bboxWidth) && sameDouble(a.bboxHeight, b.bboxHeight) &&
           sameDouble(a.centroidX, b.centroidX) && sameDouble(a.centroidY, b.centroidY) &&
           sameDouble(a.deformability, b.deformability) && sameDouble(a.area, b.area) &&
           sameDouble(a.areaRatio, b.areaRatio) && sameDouble(a.ringRatio, b.ringRatio) &&
           sameDouble(a.laplacianVariance, b.laplacianVariance) &&
           sameDouble(a.youngsModulus, b.youngsModulus) &&
           sameDouble(a.brightness.q1, b.brightness.q1) &&
           sameDouble(a.brightness.q2, b.brightness.q2) &&
           sameDouble(a.brightness.q3, b.brightness.q3) &&
           sameDouble(a.brightness.q4, b.brightness.q4) && a.isTargetGroup == b.isTargetGroup &&
           (a.allContours ? a.allContours->size() : 0) == (b.allContours ? b.allContours->size() : 0);
}

void expectSameBatch(ProcessingService& plugin, ProcessingService& bundled,
                     const std::vector<cv::Mat>& frames, const cv::Mat& background,
                     const ProcessingConfig& config, const ProcessingService::Roi& roi,
                     const std::string& scenario) {
    const auto a = plugin.processBatch(frames, config, background, roi);
    const auto b = bundled.processBatch(frames, config, background, roi);
    MIB_EXPECT(!a.empty(), scenario + ": plugin produced records");
    MIB_EXPECT(a.size() == b.size(), scenario + ": same record count (" +
                                         std::to_string(a.size()) + " vs " +
                                         std::to_string(b.size()) + ")");
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
        MIB_EXPECT(sameResult(a[i].validation, b[i].validation),
                   scenario + ": record " + std::to_string(i) + " matches (" +
                       describe(a[i].validation, b[i].validation) + ")");
        MIB_EXPECT(a[i].processedImage.size() == b[i].processedImage.size() &&
                       cv::countNonZero(a[i].processedImage != b[i].processedImage) == 0,
                   scenario + ": record " + std::to_string(i) + " mask matches");
    }
}

cv::Mat noisyFrame(const cv::Mat& background, unsigned seed, int amplitude) {
    cv::Mat frame = background.clone();
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> noise(-amplitude, amplitude);
    for (int y = 0; y < frame.rows; ++y) {
        for (int x = 0; x < frame.cols; ++x) {
            frame.at<uint8_t>(y, x) = cv::saturate_cast<uint8_t>(frame.at<uint8_t>(y, x) + noise(rng));
        }
    }
    return frame;
}

} // namespace

int main(int argc, char** argv) {
    namespace proc = backend::processing;
    MIB_REQUIRE(argc == 2, "Contract-2 plugin path argument provided");
    const std::filesystem::path pluginPath = std::filesystem::absolute(argv[1]);
    const auto host = proc::bundledProcessingCoreIdentity();

    proc::ProcessingCoreLoadRequirements requirements;
    requirements.expectedVersion = host.version;
    requirements.expectedContractVersion = MIB_PROCESSING_CONTRACT_VERSION_2;
    requirements.expectedEngineAbiVersion = MIB_PROCESSING_ENGINE_ABI_VERSION_2;
    requirements.expectedRuntimeFingerprint = host.runtimeFingerprint;
    std::string hashError;
    requirements.artifactSha256 = proc::processingCoreFileSha256(pluginPath, &hashError);
    MIB_REQUIRE(requirements.artifactSha256.size() == 64, hashError);
    requirements.releaseTag = "mib-processing-absdiff-laplacian-v" + host.version;
    requirements.manifestSha256 = std::string(64, 'b');
    requirements.trustVerifier = [&](const std::filesystem::path& candidate, std::string&) {
        return candidate == pluginPath;
    };

    const auto loaded = proc::loadProcessingCorePlugin(pluginPath, requirements);
    MIB_REQUIRE(loaded, "load Contract-2 core: " + loaded.error);
    MIB_EXPECT(loaded.kernel->identity().contractVersion == 2 &&
                   loaded.kernel->identity().engineAbiVersion == MIB_PROCESSING_ENGINE_ABI_VERSION_2,
               "loaded core declares Contract 2 / ABI v2");
    MIB_EXPECT(loaded.kernel->servesContract(2) && !loaded.kernel->servesContract(1),
               "Contract-2 core serves Contract 2 only");

    // The same module cannot be loaded as a Contract-1 (ABI v1) core.
    auto asV1 = requirements;
    asV1.expectedContractVersion = MIB_PROCESSING_CONTRACT_VERSION;
    asV1.expectedEngineAbiVersion = MIB_PROCESSING_ENGINE_ABI_VERSION;
    MIB_EXPECT(!proc::loadProcessingCorePlugin(pluginPath, asV1),
               "Contract-2 module is refused as a Contract-1 core");
    // Mismatched ABI/contract pairs are refused before the module is opened.
    auto mixed = requirements;
    mixed.expectedEngineAbiVersion = MIB_PROCESSING_ENGINE_ABI_VERSION;
    MIB_EXPECT(!proc::loadProcessingCorePlugin(pluginPath, mixed),
               "Contract 2 over engine ABI v1 is refused");

    ProcessingService plugin;
    MIB_REQUIRE(plugin.activateProcessingKernel(loaded.kernel), "activate Contract-2 core");
    ProcessingService bundled;
    MIB_REQUIRE(bundled.activateProcessingKernel(proc::makeBundledProcessingKernel(2)),
                "activate bundled Contract-2 kernel");

    const cv::Mat background(96, 160, CV_8UC1, cv::Scalar(128));
    ProcessingConfig config;
    config.processing_contract_version = 2;
    config.bg_subtract_threshold = 12;
    config.enable_area_range_check = false;
    config.enable_ring_ratio_check = true; // ignored under Contract 2
    const ProcessingService::Roi full{0, 0, 0, 0};

    // 1. Dark and bright objects (absdiff sees both), one with a hole.
    cv::Mat objects = background.clone();
    cv::rectangle(objects, cv::Rect(20, 30, 24, 24), cv::Scalar(80), cv::FILLED);
    cv::rectangle(objects, cv::Rect(28, 38, 6, 6), cv::Scalar(128), cv::FILLED); // hole
    cv::circle(objects, cv::Point(100, 48), 14, cv::Scalar(190), cv::FILLED);
    expectSameBatch(plugin, bundled, {objects}, background, config, full, "dark+bright+hole");

    // 2. An object on the frame edge (border check) and an ROI sub-rectangle.
    cv::Mat edge = objects.clone();
    cv::rectangle(edge, cv::Rect(140, 10, 20, 20), cv::Scalar(60), cv::FILLED);
    expectSameBatch(plugin, bundled, {edge}, background, config, full, "border object");
    expectSameBatch(plugin, bundled, {edge}, background, config,
                    ProcessingService::Roi{10, 5, 120, 80}, "ROI sub-rectangle");

    // 3. Noise at a low threshold (many small blobs), with and without the area gate.
    ProcessingConfig noisy = config;
    noisy.bg_subtract_threshold = 4;
    const std::vector<cv::Mat> noiseFrames{noisyFrame(objects, 7u, 6), noisyFrame(background, 11u, 6)};
    expectSameBatch(plugin, bundled, noiseFrames, background, noisy, full, "noise, area gate off");
    noisy.enable_area_range_check = true;
    noisy.area_threshold_min = 60;
    noisy.area_threshold_max = 290;
    expectSameBatch(plugin, bundled, noiseFrames, background, noisy, full, "noise, area gate on");

    // 4. Channel band and Laplacian gate travel in the science config.
    ProcessingConfig gated = config;
    gated.channel_band_y = 40;
    gated.channel_band_h = 30;
    gated.enable_laplacian_variance_check = true;
    gated.laplacian_variance_min = 1.0;
    gated.laplacian_variance_max = 1.0e6;
    expectSameBatch(plugin, bundled, {objects}, background, gated, full, "band + Laplacian gate");

    // 5. Target group (no LUT): same gating on both sides.
    ProcessingConfig target = config;
    target.enable_target_group = true;
    target.target_group_area_min = 0;
    target.target_group_area_max = 100000;
    expectSameBatch(plugin, bundled, {objects}, background, target, full, "target group");

    // 6. No object at all: the placeholder record matches.
    expectSameBatch(plugin, bundled, {background.clone()}, background, config, full, "empty frame");

    // 7. A Contract-1 config is refused by the Contract-2 core (no mask).
    ProcessingConfig contract1 = config;
    contract1.processing_contract_version = 1;
    const ProcessedFrame refused = plugin.computeProcessedFrame(objects, background, contract1, full, 0, 0);
    MIB_EXPECT(refused.processedImage.empty(), "Contract-1 config refused by the Contract-2 core");

    return mib::test::exitCode();
}
