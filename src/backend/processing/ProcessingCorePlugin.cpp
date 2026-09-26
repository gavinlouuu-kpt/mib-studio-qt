#include "backend/processing/ProcessingCoreAbi.h"
#include "backend/processing/IProcessingKernel.h"
#include "backend/processing/ImageFilterPipeline.h"
#include "backend/processing/ProcessingScience.h"
#include "backend/processing/ProcessingTypes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace {

struct PluginContext {
    std::shared_ptr<backend::processing::IProcessingKernel> kernel{
        backend::processing::makeBundledProcessingKernel()};
};

void writeError(char* destination, size_t capacity, const std::string& message) {
    if (!destination || capacity == 0) return;
    std::snprintf(destination, capacity, "%s", message.c_str());
}

bool validImage(const mib_processing_image_view* view, std::string& error) {
    if (!view || view->struct_size < sizeof(mib_processing_image_view) || !view->data ||
        view->width == 0 || view->height == 0 || view->stride_bytes < view->width) {
        error = "invalid Gray8 image view";
        return false;
    }
    const uint64_t required =
        (static_cast<uint64_t>(view->height) - 1u) * view->stride_bytes + view->width;
    if (required > view->data_size_bytes) {
        error = "Gray8 image buffer is shorter than its dimensions and stride";
        return false;
    }
    return true;
}

bool validOutput(const mib_processing_mutable_image_view* view,
                 uint32_t width,
                 uint32_t height,
                 std::string& error) {
    if (!view || view->struct_size < sizeof(mib_processing_mutable_image_view) || !view->data ||
        view->width != width || view->height != height || view->stride_bytes < width) {
        error = "invalid output Gray8 mask view";
        return false;
    }
    const uint64_t required =
        (static_cast<uint64_t>(height) - 1u) * view->stride_bytes + width;
    if (required > view->data_size_bytes) {
        error = "output mask buffer is shorter than its dimensions and stride";
        return false;
    }
    return true;
}

backend::processing::KernelConfig toConfig(const mib_processing_kernel_config& value) {
    return {value.gaussian_blur_size,
            value.background_subtract_threshold,
            value.morphology_kernel_size,
            value.morphology_iterations,
            value.empty_frame_pixel_threshold,
            (value.flags & MIB_PROCESSING_KERNEL_FLAG_ABSOLUTE_BACKGROUND_DIFFERENCE) != 0};
}

backend::processing::KernelRoi toRoi(const mib_processing_roi& value) {
    return {value.x, value.y, value.width, value.height};
}

cv::Mat borrowedMat(const mib_processing_image_view& view) {
    return cv::Mat(static_cast<int>(view.height), static_cast<int>(view.width), CV_8UC1,
                   const_cast<uint8_t*>(view.data), static_cast<size_t>(view.stride_bytes));
}

const mib_processing_core_descriptor* MIB_PROCESSING_CALL descriptor() {
    static const backend::processing::ProcessingCoreIdentity identity =
        backend::processing::bundledProcessingCoreIdentity();
    static const mib_processing_core_descriptor value = {
        sizeof(mib_processing_core_descriptor),
        MIB_PROCESSING_ENGINE_ABI_VERSION,
        MIB_PROCESSING_CONTRACT_VERSION,
        0u,
        identity.version.c_str(),
        identity.buildId.c_str(),
        identity.runtimeFingerprint.c_str(),
        {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u}};
    return &value;
}

mib_processing_status MIB_PROCESSING_CALL createContext(mib_processing_context* output,
                                                        char* error,
                                                        size_t errorCapacity) {
    if (!output) {
        writeError(error, errorCapacity, "out_context is null");
        return MIB_PROCESSING_STATUS_INVALID_ARGUMENT;
    }
    try {
        *output = new PluginContext();
        return MIB_PROCESSING_STATUS_OK;
    } catch (const std::exception& ex) {
        writeError(error, errorCapacity, ex.what());
        return MIB_PROCESSING_STATUS_INTERNAL_ERROR;
    } catch (...) {
        writeError(error, errorCapacity, "unknown context construction failure");
        return MIB_PROCESSING_STATUS_INTERNAL_ERROR;
    }
}

void MIB_PROCESSING_CALL destroyContext(mib_processing_context context) {
    delete static_cast<PluginContext*>(context);
}

mib_processing_status MIB_PROCESSING_CALL resetContext(mib_processing_context context,
                                                       char* error,
                                                       size_t errorCapacity) {
    if (!context) {
        writeError(error, errorCapacity, "context is null");
        return MIB_PROCESSING_STATUS_INVALID_ARGUMENT;
    }
    std::string detail;
    if (!static_cast<PluginContext*>(context)->kernel->reset(&detail)) {
        writeError(error, errorCapacity, detail);
        return MIB_PROCESSING_STATUS_PROCESSING_FAILED;
    }
    return MIB_PROCESSING_STATUS_OK;
}

mib_processing_status MIB_PROCESSING_CALL processMask(
    mib_processing_context context,
    const mib_processing_image_view* input,
    const mib_processing_image_view* background,
    const mib_processing_kernel_config* config,
    const mib_processing_roi* roi,
    mib_processing_mutable_image_view* output,
    char* error,
    size_t errorCapacity) {
    try {
        std::string detail;
        if (!context || !config || config->struct_size < sizeof(*config) || !roi ||
            roi->struct_size < sizeof(*roi) || !validImage(input, detail) ||
            !validOutput(output, input ? input->width : 0, input ? input->height : 0, detail)) {
            writeError(error, errorCapacity, detail.empty() ? "invalid process arguments" : detail);
            return MIB_PROCESSING_STATUS_INVALID_ARGUMENT;
        }
        cv::Mat backgroundMat;
        if (background) {
            if (!validImage(background, detail) || background->width != input->width ||
                background->height != input->height) {
                writeError(error, errorCapacity,
                           detail.empty() ? "background dimensions do not match input" : detail);
                return MIB_PROCESSING_STATUS_INVALID_ARGUMENT;
            }
            backgroundMat = borrowedMat(*background);
        }

        cv::Mat mask;
        if (!static_cast<PluginContext*>(context)->kernel->processMask(
                borrowedMat(*input), backgroundMat, toConfig(*config), toRoi(*roi), mask,
                &detail)) {
            writeError(error, errorCapacity, detail);
            return MIB_PROCESSING_STATUS_PROCESSING_FAILED;
        }
        for (int row = 0; row < mask.rows; ++row) {
            std::memcpy(output->data + static_cast<size_t>(row) * output->stride_bytes,
                        mask.ptr(row), static_cast<size_t>(mask.cols));
        }
        return MIB_PROCESSING_STATUS_OK;
    } catch (const std::exception& ex) {
        writeError(error, errorCapacity, ex.what());
        return MIB_PROCESSING_STATUS_INTERNAL_ERROR;
    } catch (...) {
        writeError(error, errorCapacity, "unknown processing failure");
        return MIB_PROCESSING_STATUS_INTERNAL_ERROR;
    }
}

mib_processing_status MIB_PROCESSING_CALL isEmpty(
    mib_processing_context context,
    const mib_processing_image_view* input,
    const mib_processing_image_view* background,
    const mib_processing_kernel_config* config,
    const mib_processing_roi* roi,
    uint8_t* output,
    char* error,
    size_t errorCapacity) {
    try {
        std::string detail;
        if (!context || !output || !config || config->struct_size < sizeof(*config) || !roi ||
            roi->struct_size < sizeof(*roi) || !validImage(input, detail)) {
            writeError(error, errorCapacity, detail.empty() ? "invalid empty-check arguments" : detail);
            return MIB_PROCESSING_STATUS_INVALID_ARGUMENT;
        }
        cv::Mat backgroundMat;
        if (background) {
            if (!validImage(background, detail) || background->width != input->width ||
                background->height != input->height) {
                writeError(error, errorCapacity,
                           detail.empty() ? "background dimensions do not match input" : detail);
                return MIB_PROCESSING_STATUS_INVALID_ARGUMENT;
            }
            backgroundMat = borrowedMat(*background);
        }
        bool empty = false;
        if (!static_cast<PluginContext*>(context)->kernel->isEmpty(
                borrowedMat(*input), backgroundMat, toConfig(*config), toRoi(*roi), empty,
                &detail)) {
            writeError(error, errorCapacity, detail);
            return MIB_PROCESSING_STATUS_PROCESSING_FAILED;
        }
        *output = empty ? 1u : 0u;
        return MIB_PROCESSING_STATUS_OK;
    } catch (const std::exception& ex) {
        writeError(error, errorCapacity, ex.what());
        return MIB_PROCESSING_STATUS_INTERNAL_ERROR;
    } catch (...) {
        writeError(error, errorCapacity, "unknown empty-check failure");
        return MIB_PROCESSING_STATUS_INTERNAL_ERROR;
    }
}

mib_processing_status MIB_PROCESSING_CALL selfTest(char* error, size_t errorCapacity) {
    try {
        auto kernel = backend::processing::makeBundledProcessingKernel();
        cv::Mat input = cv::Mat::zeros(9, 9, CV_8UC1);
        input.at<uint8_t>(4, 4) = 255;
        cv::Mat output;
        std::string detail;
        const backend::processing::KernelConfig config{3, 8, 3, 1, 1};
        if (!kernel->processMask(input, {}, config, {0, 0, 9, 9}, output, &detail) ||
            output.size() != input.size() || output.type() != CV_8UC1) {
            writeError(error, errorCapacity, detail.empty() ? "self-test output mismatch" : detail);
            return MIB_PROCESSING_STATUS_PROCESSING_FAILED;
        }
        return MIB_PROCESSING_STATUS_OK;
    } catch (const std::exception& ex) {
        writeError(error, errorCapacity, ex.what());
        return MIB_PROCESSING_STATUS_INTERNAL_ERROR;
    }
}

/* ---------------------------------------------------------------------------
 * Engine ABI v2: the full Contract-2 pipeline across the C boundary.
 * ------------------------------------------------------------------------- */

int oddAtLeastOne(int value) {
    value = std::max(1, value);
    return (value % 2 == 0) ? value + 1 : value;
}

cv::Rect clampRegion(const cv::Mat& gray, const mib_processing_roi& roi) {
    if (roi.width <= 0 || roi.height <= 0) {
        return {0, 0, gray.cols, gray.rows};
    }
    int x = std::max(0, std::min(roi.x, gray.cols - 1));
    int y = std::max(0, std::min(roi.y, gray.rows - 1));
    int w = std::max(1, std::min(roi.width, gray.cols - x));
    int h = std::max(1, std::min(roi.height, gray.rows - y));
    return {x, y, w, h};
}

bool compileStages(const mib_processing_filter_stage* stages, uint32_t count,
                   backend::processing::ImageFilterPipeline& out, std::string& error) {
    using backend::processing::ImageFilterStageKind;
    using backend::processing::ImageFilterStageSpec;
    std::vector<ImageFilterStageSpec> specs;
    specs.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (!stages || stages[i].struct_size < sizeof(mib_processing_filter_stage)) {
            error = "invalid filter stage";
            return false;
        }
        ImageFilterStageSpec spec;
        switch (stages[i].kind) {
        case MIB_PROCESSING_FILTER_IDENTITY: spec.kind = ImageFilterStageKind::Identity; break;
        case MIB_PROCESSING_FILTER_INVERT: spec.kind = ImageFilterStageKind::Invert; break;
        case MIB_PROCESSING_FILTER_LINEAR_CONTRAST:
            spec.kind = ImageFilterStageKind::LinearContrast;
            spec.alpha = stages[i].alpha;
            spec.beta = stages[i].beta;
            break;
        case MIB_PROCESSING_FILTER_GAMMA:
            spec.kind = ImageFilterStageKind::Gamma;
            spec.gamma = stages[i].gamma;
            break;
        case MIB_PROCESSING_FILTER_CLAHE:
            spec.kind = ImageFilterStageKind::Clahe;
            spec.clipLimit = stages[i].clip_limit;
            spec.tileGridSize = stages[i].tile_grid_size;
            break;
        default:
            error = "unknown filter stage kind";
            return false;
        }
        specs.push_back(spec);
    }
    auto compiled = backend::processing::ImageFilterPipeline::compile(specs, &error);
    if (!compiled) {
        return false;
    }
    out = *compiled;
    return true;
}

void fillObjectMetrics(mib_processing_object_metrics& dst,
                       const backend::services::FilterResult& src) {
    std::memset(&dst, 0, sizeof(dst));
    dst.struct_size = sizeof(mib_processing_object_metrics);
    dst.object_id = src.objectId;
    dst.object_count = src.objectCount;
    dst.is_valid = src.isValid ? 1 : 0;
    dst.touches_border = src.touchesBorder ? 1 : 0;
    dst.is_target_group = src.isTargetGroup ? 1 : 0;
    dst.track_id = src.trackId;
    dst.area = src.area;
    dst.deformability = src.deformability;
    dst.area_ratio = src.areaRatio;
    dst.laplacian_variance = src.laplacianVariance;
    dst.youngs_modulus = src.youngsModulus;
    dst.centroid_x = src.centroidX;
    dst.centroid_y = src.centroidY;
    dst.bbox_x = src.bboxX;
    dst.bbox_y = src.bboxY;
    dst.bbox_width = src.bboxWidth;
    dst.bbox_height = src.bboxHeight;
    dst.brightness_q1 = src.brightness.q1;
    dst.brightness_q2 = src.brightness.q2;
    dst.brightness_q3 = src.brightness.q3;
    dst.brightness_q4 = src.brightness.q4;
}

// Build the Contract-2 mask (absolute difference + compiled filters) and the
// full per-object metrics. `region` is the clamped ROI; `mask` is full-size.
std::vector<backend::services::FilterResult> runContract2Pipeline(
    const cv::Mat& gray, const cv::Mat& background, const cv::Rect& region,
    const mib_processing_kernel_config_v2& config,
    const backend::processing::ImageFilterPipeline& inputStages,
    const backend::processing::ImageFilterPipeline& differenceStages,
    double pixelToMicronFactor, cv::Mat& mask, std::string& error) {
    cv::Mat difference;
    if (!backend::processing::buildDifferenceImage(gray, background, region, inputStages,
                                                   differenceStages, config.gaussian_blur_size,
                                                   /*absoluteDifference=*/true, difference, &error)) {
        return {};
    }
    mask = cv::Mat::zeros(gray.rows, gray.cols, CV_8UC1);
    cv::Mat thresholded;
    cv::threshold(difference, thresholded, std::max(0, config.difference_threshold), 255,
                  cv::THRESH_BINARY);
    const int ms = oddAtLeastOne(config.morphology_kernel_size);
    const cv::Mat kernel = cv::getStructuringElement(cv::MORPH_CROSS, cv::Size(ms, ms));
    cv::Mat dst = mask(region);
    const int iters = std::max(1, config.morphology_iterations);
    cv::morphologyEx(thresholded, dst, cv::MORPH_CLOSE, kernel, cv::Point(-1, -1), iters);
    cv::morphologyEx(dst, dst, cv::MORPH_OPEN, kernel, cv::Point(-1, -1), iters);

    backend::services::ProcessingConfig science;
    science.require_single_inner_contour = false;
    science.gaussian_blur_size = config.gaussian_blur_size;
    science.bg_subtract_threshold = config.difference_threshold;
    science.morph_kernel_size = config.morphology_kernel_size;
    science.morph_iterations = config.morphology_iterations;
    science.empty_frame_pixel_threshold = config.empty_frame_pixel_threshold;
    return backend::processing::science::filterProcessedObjects(mask, region, science, gray,
                                                                pixelToMicronFactor, nullptr);
}

mib_processing_status MIB_PROCESSING_CALL processObjects(
    mib_processing_context context,
    const mib_processing_image_view* input,
    const mib_processing_image_view* background,
    const mib_processing_kernel_config_v2* config,
    const mib_processing_roi* roi,
    double pixelToMicronFactor,
    mib_processing_mutable_image_view* outputMask,
    mib_processing_object_buffer* outObjects,
    char* error,
    size_t errorCapacity) {
    try {
        std::string detail;
        if (!context || !config || config->struct_size < sizeof(*config) || !roi ||
            roi->struct_size < sizeof(*roi) || !outObjects ||
            outObjects->struct_size < sizeof(*outObjects) || !validImage(input, detail)) {
            writeError(error, errorCapacity,
                       detail.empty() ? "invalid process-objects arguments" : detail);
            return MIB_PROCESSING_STATUS_INVALID_ARGUMENT;
        }
        if (outObjects->capacity > 0 && !outObjects->objects) {
            writeError(error, errorCapacity, "object buffer has capacity but no storage");
            return MIB_PROCESSING_STATUS_INVALID_ARGUMENT;
        }
        cv::Mat gray = borrowedMat(*input);
        cv::Mat backgroundMat;
        if (background) {
            if (!validImage(background, detail) || background->width != input->width ||
                background->height != input->height) {
                writeError(error, errorCapacity,
                           detail.empty() ? "background dimensions do not match input" : detail);
                return MIB_PROCESSING_STATUS_INVALID_ARGUMENT;
            }
            backgroundMat = borrowedMat(*background);
        }

        backend::processing::ImageFilterPipeline inputStages;
        backend::processing::ImageFilterPipeline differenceStages;
        if (config->filters) {
            if (config->filters->struct_size < sizeof(mib_processing_filter_chain) ||
                !compileStages(config->filters->input_stages, config->filters->input_stage_count,
                               inputStages, detail) ||
                !compileStages(config->filters->difference_stages,
                               config->filters->difference_stage_count, differenceStages, detail)) {
                writeError(error, errorCapacity, detail.empty() ? "invalid filter chain" : detail);
                return MIB_PROCESSING_STATUS_INVALID_ARGUMENT;
            }
        }

        const cv::Rect region = clampRegion(gray, *roi);
        cv::Mat mask;
        const auto results = runContract2Pipeline(gray, backgroundMat, region, *config, inputStages,
                                                  differenceStages, pixelToMicronFactor, mask,
                                                  detail);
        if (mask.empty()) {
            writeError(error, errorCapacity, detail.empty() ? "difference build failed" : detail);
            return MIB_PROCESSING_STATUS_PROCESSING_FAILED;
        }

        // Emitted objects have a one-based id; the no-detection placeholder does not.
        std::vector<const backend::services::FilterResult*> objects;
        for (const auto& result : results) {
            if (result.objectId >= 1) {
                objects.push_back(&result);
            }
        }
        const uint32_t required = static_cast<uint32_t>(objects.size());
        const uint32_t writeCount = std::min(outObjects->capacity, required);
        for (uint32_t i = 0; i < writeCount; ++i) {
            fillObjectMetrics(outObjects->objects[i], *objects[i]);
        }
        outObjects->count = writeCount;
        outObjects->required = required;

        if (outputMask) {
            if (!validOutput(outputMask, input->width, input->height, detail)) {
                writeError(error, errorCapacity, detail);
                return MIB_PROCESSING_STATUS_INVALID_ARGUMENT;
            }
            for (int row = 0; row < mask.rows; ++row) {
                std::memcpy(outputMask->data + static_cast<size_t>(row) * outputMask->stride_bytes,
                            mask.ptr(row), static_cast<size_t>(mask.cols));
            }
        }

        if (required > outObjects->capacity) {
            writeError(error, errorCapacity, "object buffer too small");
            return MIB_PROCESSING_STATUS_BUFFER_TOO_SMALL;
        }
        return MIB_PROCESSING_STATUS_OK;
    } catch (const std::exception& ex) {
        writeError(error, errorCapacity, ex.what());
        return MIB_PROCESSING_STATUS_INTERNAL_ERROR;
    } catch (...) {
        writeError(error, errorCapacity, "unknown process-objects failure");
        return MIB_PROCESSING_STATUS_INTERNAL_ERROR;
    }
}

const mib_processing_core_descriptor* MIB_PROCESSING_CALL descriptorV2() {
    static const backend::processing::ProcessingCoreIdentity identity =
        backend::processing::bundledProcessingCoreIdentity();
    static const mib_processing_core_descriptor value = {
        sizeof(mib_processing_core_descriptor),
        MIB_PROCESSING_ENGINE_ABI_VERSION_2,
        MIB_PROCESSING_CONTRACT_VERSION_2,
        MIB_PROCESSING_CAP_CONTRACT2_REQUIRED,
        identity.version.c_str(),
        identity.buildId.c_str(),
        identity.runtimeFingerprint.c_str(),
        {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u}};
    return &value;
}

mib_processing_status MIB_PROCESSING_CALL selfTestV2(char* error, size_t errorCapacity) {
    try {
        // A textured object on a flat background must yield one object with a
        // finite, positive focus score under the full v2 pipeline.
        // A bright textured block: distinct from the background after blur (so
        // it is detected) yet internally textured (so the focus score is > 0).
        cv::Mat gray(40, 40, CV_8UC1, cv::Scalar(128));
        for (int y = 12; y < 28; ++y) {
            for (int x = 12; x < 28; ++x) {
                gray.at<uint8_t>(y, x) = ((x + y) % 2 == 0) ? 180 : 220;
            }
        }
        const cv::Mat background(40, 40, CV_8UC1, cv::Scalar(128));
        mib_processing_kernel_config_v2 config = {};
        config.struct_size = sizeof(config);
        config.gaussian_blur_size = 3;
        config.difference_threshold = 8;
        config.morphology_kernel_size = 3;
        config.morphology_iterations = 1;
        config.empty_frame_pixel_threshold = 1;
        const backend::processing::ImageFilterPipeline identity;
        cv::Mat mask;
        std::string detail;
        const auto results =
            runContract2Pipeline(gray, background, cv::Rect(0, 0, 40, 40), config, identity,
                                  identity, 0.5, mask, detail);
        bool found = false;
        for (const auto& r : results) {
            if (r.objectId >= 1 && std::isfinite(r.laplacianVariance) && r.laplacianVariance > 0.0) {
                found = true;
            }
        }
        if (!found) {
            writeError(error, errorCapacity, "v2 self-test produced no object focus score");
            return MIB_PROCESSING_STATUS_PROCESSING_FAILED;
        }
        return MIB_PROCESSING_STATUS_OK;
    } catch (const std::exception& ex) {
        writeError(error, errorCapacity, ex.what());
        return MIB_PROCESSING_STATUS_INTERNAL_ERROR;
    }
}

} // namespace

extern "C" MIB_PROCESSING_EXPORT mib_processing_status MIB_PROCESSING_CALL
mib_processing_get_api(uint32_t requestedEngineAbi,
                       uint32_t hostApiStructSize,
                       mib_processing_api* output,
                       char* error,
                       size_t errorCapacity) {
    if (requestedEngineAbi != MIB_PROCESSING_ENGINE_ABI_VERSION) {
        writeError(error, errorCapacity, "unsupported processing engine ABI");
        return MIB_PROCESSING_STATUS_ABI_MISMATCH;
    }
    if (!output || hostApiStructSize < sizeof(mib_processing_api)) {
        writeError(error, errorCapacity, "host API table is too small");
        return MIB_PROCESSING_STATUS_BUFFER_TOO_SMALL;
    }
    std::memset(output, 0, hostApiStructSize);
    output->struct_size = sizeof(mib_processing_api);
    output->engine_abi_version = MIB_PROCESSING_ENGINE_ABI_VERSION;
    output->descriptor = &descriptor;
    output->create_context = &createContext;
    output->destroy_context = &destroyContext;
    output->reset_context = &resetContext;
    output->process_mask = &processMask;
    output->is_empty = &isEmpty;
    output->self_test = &selfTest;
    return MIB_PROCESSING_STATUS_OK;
}

extern "C" MIB_PROCESSING_EXPORT mib_processing_status MIB_PROCESSING_CALL
mib_processing_get_api_v2(uint32_t requestedEngineAbi,
                          uint32_t hostApiStructSize,
                          mib_processing_api_v2* output,
                          char* error,
                          size_t errorCapacity) {
    if (requestedEngineAbi != MIB_PROCESSING_ENGINE_ABI_VERSION_2) {
        writeError(error, errorCapacity, "unsupported processing engine ABI (v2)");
        return MIB_PROCESSING_STATUS_ABI_MISMATCH;
    }
    if (!output || hostApiStructSize < sizeof(mib_processing_api_v2)) {
        writeError(error, errorCapacity, "host API v2 table is too small");
        return MIB_PROCESSING_STATUS_BUFFER_TOO_SMALL;
    }
    std::memset(output, 0, hostApiStructSize);
    output->struct_size = sizeof(mib_processing_api_v2);
    output->engine_abi_version = MIB_PROCESSING_ENGINE_ABI_VERSION_2;
    output->descriptor = &descriptorV2;
    output->create_context = &createContext;
    output->destroy_context = &destroyContext;
    output->reset_context = &resetContext;
    output->process_mask = &processMask;
    output->is_empty = &isEmpty;
    output->process_objects = &processObjects;
    output->self_test = &selfTestV2;
    return MIB_PROCESSING_STATUS_OK;
}
