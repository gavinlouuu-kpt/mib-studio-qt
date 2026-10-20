# BatchMaskSources

> Input/output adapters that pair with [[ProcessingService]]'s `processBatch`
> API. Lets the app re-generate masks from stream images that originated
> outside the live camera path — saved HDF5 experiments or a folder of
> image files.

**Source:** `src/backend/processing/BatchMaskSources.cpp`,
`include/backend/processing/BatchMaskSources.h`
**Namespace:** `backend::services::batch_masks`
**Related:** [[ProcessingService]], [[Hdf5Service]],
[[../frontend/HdfReviewTab]]

**Build target:** compiled into `mib_processing`, the Qt-free static library
(`src/backend/CMakeLists.txt`) `mib_backend` links publicly. Same portable
core as [[ProcessingService]].

## API

### Inputs

- `loadFromHdf5(hdf5, datasetPath, startIndex, count, outGray)` — wraps
  `Hdf5Service::readImagesRange()`. Common `datasetPath` values:
  `/valid_frames/images`, `/invalid_frames/images`,
  `/recorded_frames/images`. Coerces results to `CV_8UC1`.
- `loadFromFolder(folderPath, outGray, outFilenames, errors)` — scans a
  directory for TIFF/PNG/JPEG/BMP files, sorts by filename, loads as
  grayscale via `cv::imread(..., IMREAD_GRAYSCALE)`. Per-file failures are
  recorded in `errors`; the offending file is skipped, not aborted.
- `loadFromAvi(aviPath, outGray, outFilenames, errors)` — opens an AVI
  with `cv::VideoCapture`, decodes every frame, coerces to `CV_8UC1`
  (BGR→GRAY / BGRA→GRAY as needed). `outFilenames` are synthetic
  `frame_00000`, `frame_00001`, ... Pairs naturally with the AVI written
  by `FrameStore::saveFramesToAvi`.

### Outputs

- `saveMaskImages(frames, outputDir, filenames = {})` — writes one
  `<basename>_mask.png` per `ProcessedFrame::processedImage`. If
  `filenames` is empty or shorter than `frames`, falls back to
  `mask_00000.png`, `mask_00001.png`, ...
- `saveMasksToHdf5(frames, outputPath, config, roiX, roiY, roiW, roiH, background, useFrameTimestamps = false)`
  — opens a fresh HDF5 file, writes `experiment_info` (so the file
  round-trips through [[../frontend/HdfReviewTab]]) and
  `object_coordinates = image` (#756 review 2: `computeProcessedFrame`
  validates the full-frame mask, so the rows are image coordinates whatever
  the ROI; refused if it cannot be written), then partitions
  `frames` into valid/invalid by `validation.isValid` and writes them via
  `Hdf5Service::saveFrames()`. When `useFrameTimestamps` is true,
  `experiment_info` start/end timestamps come from the already normalised
  frame timestamps instead of wall-clock write time.

## Typical flow

```cpp
std::vector<cv::Mat> imgs;
std::vector<std::string> names;
std::vector<std::string> errs;
batch_masks::loadFromFolder("/path/to/folder", imgs, names, errs);

const auto frames = backend.processing().processBatch(
    imgs, backend.processing().getProcessingConfig(),
    /*background=*/{}, /*roi=*/{0,0,0,0});

batch_masks::saveMaskImages(frames, "/out/dir", names);
```

## Gotchas

- `loadFromHdf5` opens nothing — pass an `Hdf5Service` you already opened.
- `saveMasksToHdf5` overwrites `outputPath` (calls `Hdf5Service::openFile`
  which uses `H5F_ACC_TRUNC`).
- All loaders force `CV_8UC1`. Anything else is `cvtColor`-converted or
  `convertTo`-coerced.
