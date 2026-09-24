// pybind11 module over the Qt-free mib_processing core library. See
// docs/gold_standard_metrics.md ("Portable Processing Contract") for the
// field-name contract this module speaks, and
// bindings/python/python/mib_processing/__init__.py for the thin Python
// wrapper layer that re-exports these bindings.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <limits>
#include <stdexcept>

#include "backend/processing/BatchMaskSources.h"
#include "backend/processing/EModulusLut.h"
#include "backend/processing/ProcessingScience.h"
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
        py::dict d = processedFrameToPyDict(frame, pixelToMicron, includeMasks, includeSeriesImages);
        d["processing_contract_version"] = config.processing_contract_version;
        out.append(d);
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
    py::dict out =
        processedFrameToPyDict(frame, pixelToMicron, includeMask, /*includeSeriesImages=*/false);
    out["processing_contract_version"] = config.processing_contract_version;
    return out;
}

// Per-object results for one frame/ROI: the same science expansion processBatch
// performs (science::filterProcessedObjects), without tracking. This is the
// YOLO-guided pattern (one ROI per detection) and the Contract-2 "full
// per-object results" surface of the wheel. Returns a list with one dict per
// detected object; a frame with no detection returns a single empty record
// (object_id -1). `mask` (when requested) is the full-frame mask, shared by
// every record of the frame.
py::list computeProcessedObjects(const py::array& grayInput, const py::object& background,
                                 const py::dict& configDict, const py::tuple& roiTuple,
                                 uint64_t index, uint64_t timestampNs, double pixelToMicron,
                                 bool includeMask) {
    const cv::Mat gray = numpyToGrayMat(grayInput);
    const cv::Mat bg = backgroundFromObject(background);
    const ProcessingConfig config = configFromDict(configDict);
    const ProcessingService::Roi roiIn = roiFromTuple(roiTuple);

    ProcessingService service;
    ProcessedFrame frame;
    std::vector<backend::services::FilterResult> objects;
    {
        py::gil_scoped_release release;
        frame = service.computeProcessedFrame(gray, bg, config, roiIn, index, timestampNs);
        if (!frame.processedImage.empty()) {
            // Same ROI normalisation as computeProcessedFrame.
            ProcessingService::Roi roi = roiIn;
            if (roi.w <= 0 || roi.h <= 0) {
                roi = ProcessingService::Roi{0, 0, frame.processedImage.cols,
                                             frame.processedImage.rows};
            }
            roi.x = std::max(0, std::min(roi.x, frame.processedImage.cols - 1));
            roi.y = std::max(0, std::min(roi.y, frame.processedImage.rows - 1));
            roi.w = std::max(1, std::min(roi.w, frame.processedImage.cols - roi.x));
            roi.h = std::max(1, std::min(roi.h, frame.processedImage.rows - roi.y));
            const cv::Rect cvRoi(roi.x, roi.y, roi.w, roi.h);
            objects = backend::processing::science::filterProcessedObjects(
                frame.processedImage, cvRoi, config, frame.originalImage, pixelToMicron,
                nullptr);
        }
    }
    py::list out;
    if (objects.empty()) {
        py::dict d = processedFrameToPyDict(frame, pixelToMicron, includeMask, false);
        d["processing_contract_version"] = config.processing_contract_version;
        out.append(d);
        return out;
    }
    py::object sharedMask;
    if (includeMask) {
        sharedMask = matToNumpy(frame.processedImage);
    }
    for (auto& object : objects) {
        ProcessedFrame objectFrame = frame; // cv::Mat headers share the frame's pixels
        objectFrame.validation = std::move(object);
        py::dict d = processedFrameToPyDict(objectFrame, pixelToMicron, false, false);
        if (includeMask) {
            d["mask"] = sharedMask;
        }
        d["processing_contract_version"] = config.processing_contract_version;
        out.append(d);
    }
    return out;
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

} // namespace

PYBIND11_MODULE(_mib_processing, m) {
    m.doc() = "pybind11 bindings over the Qt-free mib_processing core "
              "(deformability-cytometry processing pipeline). See "
              "docs/gold_standard_metrics.md in mib-studio-qt for the "
              "field-name contract.";

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

    m.def("compute_processed_objects", &computeProcessedObjects, py::arg("gray"),
          py::arg("background"), py::arg("config"), py::arg("roi"), py::arg("index") = 0,
          py::arg("timestamp_ns") = 0, py::arg("pixel_to_micron") = 0.4886,
          py::arg("include_mask") = false,
          "Per-object variant of compute_processed_frame: returns a list with one "
          "gold-standard-shaped dict per detected object in the ROI (the same "
          "science expansion process_batch performs, without tracking). Honors "
          "config['processing_contract_version'] (1 or 2). A frame with no "
          "detection returns a single empty record (object_id -1).");

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

    // Default (Contract 1) science contract executed when a config omits
    // processing_contract_version; the wheel also executes Contract 2.
    m.attr("CONTRACT_VERSION") = 1;
    m.attr("SUPPORTED_CONTRACT_VERSIONS") = py::make_tuple(1, 2);
}
