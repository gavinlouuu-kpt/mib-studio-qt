// pybind11 module over the Qt-free mib_processing core library. See
// docs/gold_standard_metrics.md ("Portable Processing Contract") for the
// field-name contract this module speaks, and
// bindings/python/python/mib_processing/__init__.py for the thin Python
// wrapper layer that re-exports these bindings.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "backend/processing/IProcessingKernel.h"
#include "backend/processing/ProcessingScience.h"

#include "backend/processing/BatchMaskSources.h"
#include "backend/processing/EModulusLut.h"
#include "backend/processing/ProcessingService.h"
#include "backend/recording/Hdf5Service.h"

#include "config_convert.h"
#include "filter_result_convert.h"
#include "numpy_cv_interop.h"

namespace py = pybind11;

namespace {

using backend::EModulusLut;
using backend::services::Hdf5Service;
using backend::services::ProcessedFrame;
using backend::services::ProcessingConfig;
using backend::services::ProcessingService;
using mib_processing_bindings::configFromDict;
using mib_processing_bindings::configToDict;
using mib_processing_bindings::matToNumpy;
using mib_processing_bindings::numpyToGrayMat;
using mib_processing_bindings::processedFrameFromDict;
using mib_processing_bindings::processedFrameToDict;

ProcessingService::Roi roiFromTuple(const py::tuple& t) {
    if (t.size() != 4) {
        throw std::invalid_argument("roi must be a 4-tuple (x, y, w, h)");
    }
    ProcessingService::Roi roi;
    roi.x = t[0].cast<int>();
    roi.y = t[1].cast<int>();
    roi.w = t[2].cast<int>();
    roi.h = t[3].cast<int>();
    return roi;
}

cv::Mat backgroundFromObject(const py::object& background) {
    if (background.is_none()) {
        return cv::Mat{};
    }
    return numpyToGrayMat(background.cast<py::array>());
}

py::dict processedFrameToPyDict(const ProcessedFrame& frame, double pixelToMicron, bool includeMask,
                                bool includeSeriesImages) {
    py::dict d = processedFrameToDict(frame, pixelToMicron);
    if (includeMask) {
        d["mask"] = matToNumpy(frame.processedImage);
    }
    if (includeSeriesImages) {
        py::list series;
        for (const auto& img : frame.seriesImages) {
            series.append(matToNumpy(img));
        }
        d["series_images"] = series;
    }
    return d;
}

py::list processBatch(const std::vector<py::array>& frames, const py::dict& configDict,
                      const py::object& background, const py::tuple& roiTuple, double pixelToMicron,
                      bool includeMasks, bool includeSeriesImages) {
    std::vector<cv::Mat> grayImages;
    grayImages.reserve(frames.size());
    for (const auto& arr : frames) {
        grayImages.push_back(numpyToGrayMat(arr));
    }

    const ProcessingConfig config = configFromDict(configDict);
    const cv::Mat bg = backgroundFromObject(background);
    const ProcessingService::Roi roi = roiFromTuple(roiTuple);

    ProcessingService service;
    std::vector<ProcessedFrame> results;
    {
        py::gil_scoped_release release;
        results = service.processBatch(grayImages, config, bg, roi);
    }

    py::list out;
    for (const auto& frame : results) {
        out.append(processedFrameToPyDict(frame, pixelToMicron, includeMasks, includeSeriesImages));
    }
    return out;
}

py::dict computeProcessedFrame(const py::array& grayInput, const py::object& background,
                               const py::dict& configDict, const py::tuple& roiTuple,
                               uint64_t index, uint64_t timestampNs, double pixelToMicron,
                               bool includeMask) {
    const cv::Mat gray = numpyToGrayMat(grayInput);
    const cv::Mat bg = backgroundFromObject(background);
    const ProcessingConfig config = configFromDict(configDict);
    const ProcessingService::Roi roi = roiFromTuple(roiTuple);

    ProcessingService service;
    ProcessedFrame frame;
    {
        py::gil_scoped_release release;
        frame = service.computeProcessedFrame(gray, bg, config, roi, index, timestampNs);
    }
    return processedFrameToPyDict(frame, pixelToMicron, includeMask, /*includeSeriesImages=*/false);
}

py::tuple loadFromFolder(const std::string& folderPath) {
    std::vector<cv::Mat> outGray;
    std::vector<std::string> outFilenames;
    std::vector<std::string> errors;
    const bool ok =
        backend::services::batch_masks::loadFromFolder(folderPath, outGray, outFilenames, errors);
    py::list images;
    for (const auto& mat : outGray)
        images.append(matToNumpy(mat));
    return py::make_tuple(ok, images, outFilenames, errors);
}

py::tuple loadFromAvi(const std::string& aviPath) {
    std::vector<cv::Mat> outGray;
    std::vector<std::string> outFilenames;
    std::vector<std::string> errors;
    const bool ok =
        backend::services::batch_masks::loadFromAvi(aviPath, outGray, outFilenames, errors);
    py::list images;
    for (const auto& mat : outGray)
        images.append(matToNumpy(mat));
    return py::make_tuple(ok, images, outFilenames, errors);
}

py::tuple loadImagesFromHdf5(const std::string& hdf5Path, const std::string& datasetPath,
                             size_t startIndex, size_t count) {
    Hdf5Service hdf5;
    if (!hdf5.loadFile(hdf5Path)) {
        return py::make_tuple(false, py::list());
    }
    std::vector<cv::Mat> outGray;
    const bool ok =
        backend::services::batch_masks::loadFromHdf5(hdf5, datasetPath, startIndex, count, outGray);
    py::list images;
    for (const auto& mat : outGray)
        images.append(matToNumpy(mat));
    return py::make_tuple(ok, images);
}

bool saveMasksToHdf5(const std::vector<py::dict>& frameDicts, const std::vector<py::array>& images,
                     const std::vector<py::array>& masks, const std::string& outputPath,
                     const py::dict& configDict, const py::tuple& roiTuple,
                     const py::object& background, bool useFrameTimestamps) {
    if (frameDicts.size() != images.size() || frameDicts.size() != masks.size()) {
        throw std::invalid_argument("frame_dicts, images, and masks must be the same length");
    }
    // Hdf5Service::saveFrames writes originalImage to <group>/images and
    // processedImage to <group>/masks separately -- both are required, or
    // HDF5 rejects the empty-Mat dataset with a zero chunk dimension.
    std::vector<ProcessedFrame> frames;
    frames.reserve(frameDicts.size());
    for (size_t i = 0; i < frameDicts.size(); ++i) {
        ProcessedFrame f = processedFrameFromDict(frameDicts[i]);
        f.originalImage = numpyToGrayMat(images[i]);
        f.processedImage = numpyToGrayMat(masks[i]);
        frames.push_back(std::move(f));
    }

    const ProcessingConfig config = configFromDict(configDict);
    const ProcessingService::Roi roi = roiFromTuple(roiTuple);
    const cv::Mat bg = backgroundFromObject(background);

    py::gil_scoped_release release;
    return backend::services::batch_masks::saveMasksToHdf5(frames, outputPath, config, roi.x, roi.y,
                                                           roi.w, roi.h, bg, useFrameTimestamps);
}

// Additive offline API: no desktop profile, result contract, or autofocus change.
py::dict benchmarkFrame(const py::array& image, const py::array& background,
                        const py::dict& configDict, const std::string& difference,
                        bool withLaplacian) {
    if (difference != "subtract" && difference != "absdiff") {
        throw std::invalid_argument("difference must be subtract or absdiff");
    }
    const cv::Mat gray = numpyToGrayMat(image);
    const cv::Mat bg = numpyToGrayMat(background);
    if (gray.empty() || gray.size() != bg.size()) {
        throw std::invalid_argument("nonempty image and matching background required");
    }
    const auto config = configFromDict(configDict);
    const backend::processing::KernelConfig kernelConfig{
        config.gaussian_blur_size, config.bg_subtract_threshold, config.morph_kernel_size,
        config.morph_iterations, config.empty_frame_pixel_threshold};
    auto kernel = backend::processing::makeDifferenceBenchmarkKernel(difference == "absdiff");
    cv::Mat mask;
    std::vector<backend::services::FilterResult> objects;
    std::vector<double> scores;
    double elapsedUs = 0.0;
    {
        py::gil_scoped_release release;
        const auto start = std::chrono::steady_clock::now();
        std::string error;
        if (!kernel->processMask(gray, bg, kernelConfig, {}, mask, &error)) {
            throw std::runtime_error(error);
        }
        objects = backend::processing::science::filterProcessedObjects(
            mask, cv::Rect(0, 0, gray.cols, gray.rows), config, gray, 1.0, nullptr,
            withLaplacian ? &scores : nullptr);
        elapsedUs =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
                .count();
    }
    py::list rows;
    for (size_t i = 0; i < objects.size(); ++i) {
        const auto& object = objects[i];
        if (object.objectId < 0) continue; // not a detected object
        ProcessedFrame frame;
        frame.validation = object;
        py::dict row = mib_processing_bindings::processedFrameToDict(frame, 1.0);
        // These fields are not measured by this untracked, uncalibrated API.
        for (const char* key :
             {"area_um2", "index", "timestamp_ns", "track_id", "track_first_frame",
              "track_last_frame", "track_observation_count", "youngs_modulus", "is_target_group"}) {
            row.attr("pop")(key, py::none());
        }
        if (object.area <= 0.0) { // e.g. border early exit, not a measured zero
            row["area"] = py::none();
            row["deformability"] = py::none();
            row["area_ratio"] = py::none();
        }
        row["bbox"] =
            py::make_tuple(object.bboxX, object.bboxY, object.bboxWidth, object.bboxHeight);
        if (object.innerContourCount == 0 || object.ringRatio <= 0.0) {
            row["ring_ratio"] = py::none();
        }
        row["laplacian_variance"] =
            withLaplacian && std::isfinite(scores[i]) ? py::cast(scores[i]) : py::none();
        rows.append(row);
    }
    py::dict output;
    output["objects"] = rows;
    output["mask"] = matToNumpy(mask);
    output["pipeline_us"] = elapsedUs;
    output["core_build_id"] = kernel->identity().buildId;
    output["opencv_version"] = CV_VERSION;
    output["opencv_threads"] = cv::getNumThreads();
    return output;
}

} // namespace

PYBIND11_MODULE(_mib_processing, m) {
    m.doc() = "pybind11 bindings over the Qt-free mib_processing core "
              "(deformability-cytometry processing pipeline). See "
              "docs/gold_standard_metrics.md in mib-studio-qt for the "
              "field-name contract.";

    m.def("benchmark_frame", &benchmarkFrame, py::arg("image"), py::arg("background"),
          py::arg("config"), py::arg("difference") = "subtract", py::arg("with_laplacian") = true);
    m.def("laplacian_variance",
          [](const py::array& image, const std::vector<std::pair<int, int>>& points) {
              std::vector<cv::Point> contour;
              for (const auto& point : points)
                  contour.emplace_back(point.first, point.second);
              return backend::processing::science::calculateLaplacianVariance(numpyToGrayMat(image),
                                                                              contour);
          });
    m.def("set_opencv_threads", [](int count) {
        if (count < 1) throw std::invalid_argument("thread count must be positive");
        cv::setNumThreads(count);
    });

    m.def("process_batch", &processBatch, py::arg("frames"), py::arg("config"),
          py::arg("background") = py::none(), py::arg("roi") = py::make_tuple(0, 0, 0, 0),
          py::arg("pixel_to_micron") = 0.4886, py::arg("include_masks") = false,
          py::arg("include_series_images") = false,
          "Run the batch pipeline over a list of 2D uint8 grayscale numpy "
          "arrays. Returns a list of dicts (one per detected object "
          "candidate, gold-standard-metrics shaped); pass include_masks=True "
          "to add a 'mask' numpy array to each dict. With multi-image mode "
          "enabled in config, include_series_images=True adds the trigger "
          "image and available following frames to each retained valid record.");

    m.def("compute_processed_frame", &computeProcessedFrame, py::arg("gray"),
          py::arg("background") = py::none(), py::arg("config") = py::dict(),
          py::arg("roi") = py::make_tuple(0, 0, 0, 0), py::arg("index") = 0,
          py::arg("timestamp_ns") = 0, py::arg("pixel_to_micron") = 0.4886,
          py::arg("include_mask") = false,
          "Pure single-frame pipeline (blur -> optional background subtract "
          "-> threshold -> morphology -> filter). Returns one dict for the "
          "frame's first selected object (see docs/gold_standard_metrics.md "
          "on has_single_inner_contour / multi-object semantics).");

    m.def(
        "config_from_dict", [](const py::dict& d) { return configToDict(configFromDict(d)); },
        py::arg("config"),
        "Round-trip a ProcessingConfig dict through the C++ struct, filling in "
        "any missing fields with the struct's own defaults.");

    m.def("load_from_folder", &loadFromFolder, py::arg("folder_path"),
          "Load all TIFF/PNG/JPEG/BMP files in folder_path as grayscale "
          "images. Returns (ok, images, filenames, errors).");

    m.def("load_from_avi", &loadFromAvi, py::arg("avi_path"),
          "Decode all frames from an AVI file as grayscale images. Returns "
          "(ok, images, filenames, errors).");

    m.def("load_images_from_hdf5", &loadImagesFromHdf5, py::arg("hdf5_path"),
          py::arg("dataset_path"), py::arg("start_index") = 0, py::arg("count") = 0,
          "Load a contiguous range of images from an HDF5 dataset (e.g. "
          "'/valid_frames/images'). Returns (ok, images).");

    m.def("save_masks_to_hdf5", &saveMasksToHdf5, py::arg("frame_dicts"), py::arg("images"),
          py::arg("masks"), py::arg("output_path"), py::arg("config"),
          py::arg("roi") = py::make_tuple(0, 0, 0, 0), py::arg("background") = py::none(),
          py::arg("use_frame_timestamps") = false,
          "Write frame_dicts (as returned by process_batch) plus their "
          "source images and masks to a fresh HDF5 file, partitioned into "
          "valid/invalid frames. images and masks must be the same length "
          "as frame_dicts (source grayscale frame and detection mask, "
          "respectively, for each record).");

    py::class_<EModulusLut>(m, "EModulusLut")
        .def(py::init<>())
        .def("load_from_file", &EModulusLut::loadFromFile, py::arg("base_path"),
             "Load a tab-separated LUT file (area_um, deform, emodulus). "
             "Returns True on success.")
        .def("is_loaded", &EModulusLut::isLoaded)
        .def("lookup", &EModulusLut::lookup, py::arg("area_um"), py::arg("deformability"),
             "Bilinear-interpolated Young's modulus (kPa) lookup. Returns "
             "NaN if the query point is outside LUT coverage.");

    m.attr("CONTRACT_VERSION") = 1;
}
