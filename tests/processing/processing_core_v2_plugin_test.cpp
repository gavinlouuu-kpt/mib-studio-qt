// V2-5 follow-on: the absdiff-laplacian (Contract 2) processing-core module
// exports a working engine ABI v2 and nothing else (ADR 0007). It negotiates get_api_v2, advertises the Contract-2 capabilities,
// passes its v2 self-test, and process_objects returns per-object metrics
// (including a finite Laplacian variance) with deterministic BUFFER_TOO_SMALL
// handling — all across the plain-C boundary via dlopen.
#include "backend/processing/ProcessingCoreAbi.h"
#include "support/assert.h"

#include <dlfcn.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

mib_processing_image_view viewOf(std::vector<uint8_t>& buf, uint32_t w, uint32_t h) {
    mib_processing_image_view v{};
    v.struct_size = sizeof(v);
    v.width = w;
    v.height = h;
    v.stride_bytes = w;
    v.data = buf.data();
    v.data_size_bytes = buf.size();
    return v;
}

} // namespace

int main(int argc, char** argv) {
    MIB_REQUIRE(argc == 2, "plugin path argument provided");
    void* handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    MIB_REQUIRE(handle != nullptr, std::string("dlopen: ") + (dlerror() ? dlerror() : ""));

    auto getApiV2 = reinterpret_cast<mib_processing_get_api_v2_fn>(
        dlsym(handle, MIB_PROCESSING_GET_API_V2_SYMBOL));
    MIB_REQUIRE(getApiV2 != nullptr, "module exports mib_processing_get_api_v2");
    // ADR 0007: the absdiff-laplacian (Contract 2) line implements only
    // Contract 2, so it must not offer the Contract-1 (ABI v1) entry point.
    MIB_EXPECT(dlsym(handle, MIB_PROCESSING_GET_API_SYMBOL) == nullptr,
               "Contract-2 module does not export mib_processing_get_api");

    // A Contract-1 request against the v2 entry point is rejected.
    mib_processing_api_v2 api{};
    char err[256] = {0};
    MIB_EXPECT(getApiV2(1u, sizeof(api), &api, err, sizeof(err)) == MIB_PROCESSING_STATUS_ABI_MISMATCH,
               "v2 entry rejects an ABI-v1 request");

    std::memset(&api, 0, sizeof(api));
    MIB_REQUIRE(getApiV2(MIB_PROCESSING_ENGINE_ABI_VERSION_2, sizeof(api), &api, err, sizeof(err)) ==
                    MIB_PROCESSING_STATUS_OK,
                std::string("get_api_v2: ") + err);
    MIB_REQUIRE(api.descriptor && api.create_context && api.destroy_context && api.process_objects &&
                    api.self_test,
                "v2 API table is complete");
    MIB_EXPECT(api.engine_abi_version == MIB_PROCESSING_ENGINE_ABI_VERSION_2, "reports ABI v2");

    const auto* desc = api.descriptor();
    MIB_REQUIRE(desc != nullptr, "descriptor present");
    MIB_EXPECT(desc->contract_version == MIB_PROCESSING_CONTRACT_VERSION_2, "descriptor is Contract 2");
    MIB_EXPECT((desc->capabilities & MIB_PROCESSING_CAP_CONTRACT2_REQUIRED) ==
                   MIB_PROCESSING_CAP_CONTRACT2_REQUIRED,
               "descriptor advertises the required capabilities");

    std::memset(err, 0, sizeof(err));
    MIB_EXPECT(api.self_test(err, sizeof(err)) == MIB_PROCESSING_STATUS_OK,
               std::string("v2 self-test: ") + err);

    // Synthetic frame: a bright textured block on a flat background.
    const uint32_t W = 40, H = 40;
    std::vector<uint8_t> image(static_cast<size_t>(W) * H, 128);
    std::vector<uint8_t> background(static_cast<size_t>(W) * H, 128);
    for (uint32_t y = 12; y < 28; ++y) {
        for (uint32_t x = 12; x < 28; ++x) {
            image[y * W + x] = ((x + y) % 2 == 0) ? 180 : 220;
        }
    }
    mib_processing_image_view input = viewOf(image, W, H);
    mib_processing_image_view bg = viewOf(background, W, H);

    mib_processing_kernel_config_v2 config{};
    config.struct_size = sizeof(config);
    config.gaussian_blur_size = 3;
    config.difference_threshold = 8;
    config.morphology_kernel_size = 3;
    config.morphology_iterations = 1;
    config.empty_frame_pixel_threshold = 1;
    config.laplacian_kernel_size = 3;
    config.filters = nullptr; // identity preprocessing

    mib_processing_roi roi{};
    roi.struct_size = sizeof(roi);
    roi.x = 0;
    roi.y = 0;
    roi.width = static_cast<int32_t>(W);
    roi.height = static_cast<int32_t>(H);

    mib_processing_context ctx = nullptr;
    std::memset(err, 0, sizeof(err));
    MIB_REQUIRE(api.create_context(&ctx, err, sizeof(err)) == MIB_PROCESSING_STATUS_OK && ctx,
                std::string("create_context: ") + err);

    // First call with a zero-capacity buffer: deterministic BUFFER_TOO_SMALL,
    // reporting the required count without any allocation.
    mib_processing_object_buffer probe{};
    probe.struct_size = sizeof(probe);
    probe.capacity = 0;
    probe.objects = nullptr;
    std::memset(err, 0, sizeof(err));
    const auto probeStatus = api.process_objects(ctx, &input, &bg, &config, &roi, 0.5, nullptr,
                                                 &probe, err, sizeof(err));
    MIB_EXPECT(probeStatus == MIB_PROCESSING_STATUS_BUFFER_TOO_SMALL,
               "zero-capacity buffer reports BUFFER_TOO_SMALL");
    MIB_REQUIRE(probe.required >= 1, "at least one object is required");
    MIB_EXPECT(probe.count == 0, "nothing written into a zero-capacity buffer");

    // Second call with an adequate buffer: metrics are filled.
    std::vector<mib_processing_object_metrics> slots(probe.required);
    mib_processing_object_buffer out{};
    out.struct_size = sizeof(out);
    out.capacity = probe.required;
    out.objects = slots.data();
    std::memset(err, 0, sizeof(err));
    const auto status =
        api.process_objects(ctx, &input, &bg, &config, &roi, 0.5, nullptr, &out, err, sizeof(err));
    MIB_REQUIRE(status == MIB_PROCESSING_STATUS_OK, std::string("process_objects: ") + err);
    MIB_EXPECT(out.count == probe.required && out.required == probe.required, "buffer fully written");
    MIB_EXPECT(out.objects[0].object_id >= 1, "object has a one-based id");
    MIB_EXPECT(std::isfinite(out.objects[0].laplacian_variance) &&
                   out.objects[0].laplacian_variance > 0.0,
               "per-object Laplacian variance crosses the ABI as a finite focus score");
    MIB_EXPECT(out.objects[0].area > 0.0, "object area is reported");

    // Handing back this core's own process_mask output gives the same objects
    // without rebuilding the mask (single mask computation per frame).
    std::vector<uint8_t> maskBytes(static_cast<size_t>(W) * H, 0);
    mib_processing_mutable_image_view maskOut{};
    maskOut.struct_size = sizeof(maskOut);
    maskOut.width = W;
    maskOut.height = H;
    maskOut.stride_bytes = W;
    maskOut.data = maskBytes.data();
    maskOut.data_size_bytes = maskBytes.size();
    mib_processing_kernel_config maskConfig{};
    maskConfig.struct_size = sizeof(maskConfig);
    maskConfig.gaussian_blur_size = config.gaussian_blur_size;
    maskConfig.background_subtract_threshold = config.difference_threshold;
    maskConfig.morphology_kernel_size = config.morphology_kernel_size;
    maskConfig.morphology_iterations = config.morphology_iterations;
    maskConfig.empty_frame_pixel_threshold = config.empty_frame_pixel_threshold;
    maskConfig.flags = MIB_PROCESSING_KERNEL_FLAG_ABSOLUTE_BACKGROUND_DIFFERENCE;
    std::memset(err, 0, sizeof(err));
    MIB_REQUIRE(api.process_mask(ctx, &input, &bg, &maskConfig, &roi, &maskOut, err, sizeof(err)) ==
                    MIB_PROCESSING_STATUS_OK,
                std::string("process_mask: ") + err);
    mib_processing_image_view maskIn = viewOf(maskBytes, W, H);
    auto withMask = config;
    withMask.precomputed_mask = &maskIn;
    std::vector<mib_processing_object_metrics> reused(probe.required);
    mib_processing_object_buffer reusedOut{};
    reusedOut.struct_size = sizeof(reusedOut);
    reusedOut.capacity = probe.required;
    reusedOut.objects = reused.data();
    std::memset(err, 0, sizeof(err));
    MIB_REQUIRE(api.process_objects(ctx, &input, &bg, &withMask, &roi, 0.5, nullptr, &reusedOut,
                                    err, sizeof(err)) == MIB_PROCESSING_STATUS_OK,
                std::string("process_objects with precomputed mask: ") + err);
    MIB_EXPECT(reusedOut.count == out.count, "same object count with a precomputed mask");
    for (uint32_t i = 0; i < std::min(reusedOut.count, out.count); ++i) {
        MIB_EXPECT(reused[i].area == slots[i].area &&
                       reused[i].laplacian_variance == slots[i].laplacian_variance &&
                       reused[i].centroid_x == slots[i].centroid_x &&
                       reused[i].is_valid == slots[i].is_valid,
                   "object " + std::to_string(i) + " identical with a precomputed mask");
    }
    std::vector<uint8_t> wrongSize(4, 0);
    mib_processing_image_view badMask = viewOf(wrongSize, 2, 2);
    withMask.precomputed_mask = &badMask;
    MIB_EXPECT(api.process_objects(ctx, &input, &bg, &withMask, &roi, 0.5, nullptr, &reusedOut, err,
                                   sizeof(err)) == MIB_PROCESSING_STATUS_INVALID_ARGUMENT,
               "a precomputed mask of the wrong size is rejected");

    api.destroy_context(ctx);
    dlclose(handle);
    return mib::test::exitCode();
}
