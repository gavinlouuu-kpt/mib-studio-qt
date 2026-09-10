#include "backend/processing/ProcessingCoreAbi.h"
#include "backend/processing/IProcessingKernel.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include <opencv2/core.hpp>

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
            (value.flags & MIB_PROCESSING_KERNEL_FLAG_ABSOLUTE_BACKGROUND_DIFFERENCE) != 0
                ? backend::processing::BackgroundDifferenceMode::AbsoluteDifference
                : backend::processing::BackgroundDifferenceMode::DirectionalSubtract};
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
