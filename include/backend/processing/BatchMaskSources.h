#pragma once

#include <string>
#include <functional>
#include <limits>
#include <cstddef>
#include <vector>

namespace cv { class Mat; }
namespace backend::processing { struct ProcessingCoreIdentity; }

namespace backend::services {

class Hdf5Service;
struct ProcessedFrame;
struct ProcessingConfig;
class ProcessingService;

} // namespace backend::services

namespace backend::services::batch_masks {

struct LoadOptions {
    size_t start{0};
    size_t count{0}; // zero selects the remaining source
    size_t maxFrames{std::numeric_limits<size_t>::max()};
    size_t maxBytes{std::numeric_limits<size_t>::max()};
    std::function<bool()> cancelled;
};

// Shared Qt/Tauri synthetic background: quietest temporal tiles, unchanged
// from BatchMaskDialog's original algorithm. Cancellation is checked per tile.
cv::Mat buildSyntheticBackground(const std::vector<cv::Mat>& frames,
                                 const std::function<bool()>& cancelled = {});

// Load one source frame without changing any live reader or processing state.
bool loadPreview(const std::string& kind,const std::string& path,const std::string& dataset,
                 size_t index,cv::Mat& image);

// Load a contiguous range of images from an open Hdf5Service dataset.
// `datasetPath` is typically one of:
//   "/valid_frames/images", "/invalid_frames/images", "/recorded_frames/images"
// Returned matrices are CV_8UC1. Delegates to Hdf5Service::readImagesRange().
bool loadFromHdf5(Hdf5Service& hdf5,
                  const std::string& datasetPath,
                  size_t startIndex,
                  size_t count,
                  std::vector<cv::Mat>& outGray);

// Load all TIFF/PNG/JPEG files in `folderPath`, sorted by filename, converted
// to CV_8UC1. Returns true if the folder was scanned successfully; per-file
// read errors are recorded in `errors` and the offending file is skipped.
// `outFilenames` contains the basenames (no path) aligned with `outGray`.
bool loadFromFolder(const std::string& folderPath,
                    std::vector<cv::Mat>& outGray,
                    std::vector<std::string>& outFilenames,
                    std::vector<std::string>& errors,
                    const LoadOptions& options = {});

// Load all frames from an AVI file (written by FrameStore::saveFramesToAvi or
// any AVI readable by cv::VideoCapture). Frames are decoded sequentially and
// converted to CV_8UC1. Returns true if the file was opened and at least one
// frame was read; per-frame decode errors are appended to `errors`.
// `outFilenames` is populated with synthetic names `frame_00000`, ...
bool loadFromAvi(const std::string& aviPath,
                 std::vector<cv::Mat>& outGray,
                 std::vector<std::string>& outFilenames,
                 std::vector<std::string>& errors,
                    const LoadOptions& options = {});

// Write one mask PNG per processed frame into `outputDir`. Filenames are
// `<basename>_mask.png` (basename taken from `filenames[i]` if provided,
// stripped of extension; otherwise `mask_00000.png`, `mask_00001.png`, ...).
// Returns the number of masks written. `outputDir` is created if missing.
size_t saveMaskImages(const std::vector<ProcessedFrame>& frames,
                      const std::string& outputDir,
                      const std::vector<std::string>& filenames = {});

// Partition `frames` into valid/invalid based on `validation.isValid` and
// write them to a new HDF5 file at `outputPath` via Hdf5Service::saveFrames().
// Also writes `experiment_info` so the file round-trips through HdfReviewTab.
// Returns true on success.
bool saveMasksToHdf5(const std::vector<ProcessedFrame>& frames,
                     const std::string& outputPath,
                     const ProcessingConfig& config,
                     int roiX, int roiY, int roiW, int roiH,
                     const cv::Mat& background,
                     bool useFrameTimestamps = false,
                     const backend::processing::ProcessingCoreIdentity* processingCore = nullptr);

} // namespace backend::services::batch_masks
