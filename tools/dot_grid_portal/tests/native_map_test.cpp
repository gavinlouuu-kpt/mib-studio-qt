// Loads the actual portal-exported registry through the unmodified native MIB codec.
#include "backend/processing/DotGridCodec.h"
#include "backend/processing/DotGridRegistry.h"
#include <spdlog/spdlog.h>
#include <cmath>
#include <memory>

int main(int argc, char** argv) {
    using namespace backend::dotgrid;
    if (argc != 2) return 2;
    auto registry = std::make_shared<Registry>();
    std::string error;
    if (!Registry::loadFile(argv[1], *registry, &error) || registry->size() != 1) {
        spdlog::error("Registry load failed: {}", error);
        return 1;
    }
    const auto& design = registry->designs().front();
    ViewPose truth;
    truth.centreXUm = 2500;
    truth.centreYUm = 2500;
    truth.thetaDeg = 12;
    truth.mirrored = true;
    RenderOptions options;
    options.seed = 42;
    auto image = renderView(*design.codebook, truth, options);
    DesignDecoder decoder(CodecSet::bundled(), registry);
    auto decoded = decoder.decode(image, DecoderConfig{});
    if (!decoded.ok || decoded.designId != design.id || decoded.codecContract != 1 ||
        std::abs(decoded.centreXUm - truth.centreXUm) > 2 ||
        std::abs(decoded.centreYUm - truth.centreYUm) > 2) {
        spdlog::error("Native map decode failed: {}", decoded.reason);
        return 1;
    }
    spdlog::info("Native registry + synthetic decode passed: {} at ({:.3f}, {:.3f}) um", design.id,
                 decoded.centreXUm, decoded.centreYUm);
}
