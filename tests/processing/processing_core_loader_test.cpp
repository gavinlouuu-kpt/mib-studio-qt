#include "backend/processing/IProcessingKernel.h"
#include "backend/processing/ProcessingCoreLoader.h"
#include "support/assert.h"

#include <filesystem>
#include <string>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

int main(int argc, char** argv) {
    MIB_REQUIRE(argc == 2, "plugin path argument provided");
    const std::filesystem::path pluginPath = std::filesystem::absolute(argv[1]);
    const auto bundledIdentity = backend::processing::bundledProcessingCoreIdentity();

    int trustCalls = 0;
    backend::processing::ProcessingCoreLoadRequirements requirements;
    requirements.expectedVersion = bundledIdentity.version;
    requirements.expectedContractVersion = bundledIdentity.contractVersion;
    requirements.expectedEngineAbiVersion = bundledIdentity.engineAbiVersion;
    requirements.expectedRuntimeFingerprint = bundledIdentity.runtimeFingerprint;
    std::string hashError;
    requirements.artifactSha256 =
        backend::processing::processingCoreFileSha256(pluginPath, &hashError);
    MIB_REQUIRE(requirements.artifactSha256.size() == 64, hashError);
    requirements.releaseTag = "mib-processing-v" + bundledIdentity.version;
    requirements.manifestSha256 = std::string(64, 'b');
    requirements.trustVerifier = [&](const std::filesystem::path& candidate, std::string&) {
        ++trustCalls;
        return candidate == pluginPath;
    };

    const auto loaded = backend::processing::loadProcessingCorePlugin(pluginPath, requirements);
    MIB_REQUIRE(loaded, loaded.error);
    MIB_EXPECT(trustCalls == 1, "trust verifier called exactly once");
    MIB_EXPECT(loaded.kernel->identity().version == bundledIdentity.version,
               "descriptor version copied into identity");
    MIB_EXPECT(loaded.kernel->identity().source == "plugin", "plugin source recorded");
    MIB_EXPECT(loaded.kernel->servesContract(1) && !loaded.kernel->servesContract(2),
               "subtract-ring plugin serves Contract 1 only (ADR 0007)");
    MIB_EXPECT(loaded.kernel->identity().artifactSha256 == requirements.artifactSha256,
               "artifact digest propagated");

    cv::Mat input = cv::Mat::zeros(41, 53, CV_8UC1);
    cv::rectangle(input, cv::Rect(12, 10, 20, 18), cv::Scalar(255), cv::FILLED);
    backend::processing::KernelConfig config;
    config.backgroundSubtractThreshold = 8;
    config.emptyFramePixelThreshold = 10;
    const backend::processing::KernelRoi roi{4, 3, 42, 34};
    cv::Mat dynamicMask;
    cv::Mat bundledMask;
    std::string error;
    auto bundled = backend::processing::makeBundledProcessingKernel();
    MIB_REQUIRE(loaded.kernel->processMask(input, {}, config, roi, dynamicMask, &error), error);
    MIB_REQUIRE(bundled->processMask(input, {}, config, roi, bundledMask, &error), error);
    MIB_EXPECT(cv::countNonZero(dynamicMask != bundledMask) == 0,
               "dynamic and bundled masks are bit-identical");

    bool dynamicEmpty = true;
    bool bundledEmpty = true;
    MIB_REQUIRE(loaded.kernel->isEmpty(input, {}, config, roi, dynamicEmpty, &error), error);
    MIB_REQUIRE(bundled->isEmpty(input, {}, config, roi, bundledEmpty, &error), error);
    MIB_EXPECT(dynamicEmpty == bundledEmpty, "dynamic and bundled empty checks agree");

    cv::Mat darker = cv::Mat::zeros(input.size(), CV_8UC1);
    cv::Mat brighter(input.size(), CV_8UC1, cv::Scalar(255));
    config.absoluteBackgroundDifference = true;
    MIB_REQUIRE(loaded.kernel->isEmpty(darker, brighter, config, roi, dynamicEmpty, &error),
                error);
    MIB_REQUIRE(bundled->isEmpty(darker, brighter, config, roi, bundledEmpty, &error), error);
    MIB_EXPECT(!dynamicEmpty && dynamicEmpty == bundledEmpty,
               "dynamic core owns absolute-difference auto-background empty checks");
    config.absoluteBackgroundDifference = false;

    cv::Mat invalidBackground(input.rows, input.cols, CV_16UC1, cv::Scalar(1000));
    MIB_REQUIRE(loaded.kernel->processMask(input, invalidBackground, config, roi,
                                           dynamicMask, &error), error);
    MIB_REQUIRE(bundled->processMask(input, invalidBackground, config, roi,
                                     bundledMask, &error), error);
    MIB_EXPECT(cv::countNonZero(dynamicMask != bundledMask) == 0,
               "invalid background type is consistently ignored before the ABI call");

    auto mismatch = requirements;
    mismatch.expectedVersion = "999.0.0";
    MIB_EXPECT(!backend::processing::loadProcessingCorePlugin(pluginPath, mismatch),
               "exact version mismatch fails closed");

    auto incompatibleAbi = requirements;
    incompatibleAbi.expectedEngineAbiVersion += 1;
    MIB_EXPECT(!backend::processing::loadProcessingCorePlugin(pluginPath, incompatibleAbi),
               "registry metadata cannot opt the host into another engine ABI");
    auto incompatibleContract = requirements;
    incompatibleContract.expectedContractVersion += 1;
    MIB_EXPECT(!backend::processing::loadProcessingCorePlugin(pluginPath, incompatibleContract),
               "registry metadata cannot opt the host into another contract");
    auto incompatibleRuntime = requirements;
    incompatibleRuntime.expectedRuntimeFingerprint = "attacker-selected-runtime";
    MIB_EXPECT(!backend::processing::loadProcessingCorePlugin(pluginPath, incompatibleRuntime),
               "registry metadata must match the host runtime fingerprint");

    auto tamperedDigest = requirements;
    tamperedDigest.artifactSha256 = std::string(64, '0');
    MIB_EXPECT(!backend::processing::loadProcessingCorePlugin(pluginPath, tamperedDigest),
               "artifact SHA mismatch fails before module load");
    auto missingArtifactDigest = requirements;
    missingArtifactDigest.artifactSha256.clear();
    MIB_EXPECT(!backend::processing::loadProcessingCorePlugin(pluginPath,
                                                               missingArtifactDigest),
               "artifact SHA is mandatory at the public loader boundary");
    auto missingManifestDigest = requirements;
    missingManifestDigest.manifestSha256.clear();
    MIB_EXPECT(!backend::processing::loadProcessingCorePlugin(pluginPath,
                                                               missingManifestDigest),
               "immutable manifest SHA is mandatory at the public loader boundary");

    auto rejected = requirements;
    rejected.trustVerifier = [](const std::filesystem::path&, std::string& reason) {
        reason = "fixture signer is not approved";
        return false;
    };
    const auto rejectedResult =
        backend::processing::loadProcessingCorePlugin(pluginPath, rejected);
    MIB_EXPECT(!rejectedResult && rejectedResult.error.find("trust") != std::string::npos,
               "trust rejection fails before activation");

    MIB_EXPECT(!backend::processing::loadProcessingCorePlugin(pluginPath.filename(), requirements),
               "relative plugin paths are rejected");
    return mib::test::exitCode();
}
