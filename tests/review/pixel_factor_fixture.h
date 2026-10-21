// Shared fixture of the #758 tests (review.reanalysis_pixel_factor,
// backend.facade_reanalysis_pixel_factor): experiment files whose one bright
// disc per frame is valid and in the target group at 0.25 µm/px but not at
// the 0.4886 µm/px default.
//
// The disc's hull area A is measured once with processBatch (it depends on
// blur, threshold and contour rounding, not on this file), then the config's
// ranges are set around A × 0.25²:
//   - area range [A·0.25²/2, A·0.25²·2] µm²: valid at 0.25, while
//     A·0.4886² = 3.8 × A·0.25² is above it (invalid at 0.4886);
//   - target group [A·0.25²·0.8, A·0.25²·1.25] µm².
// `targetOnlyConfig` keeps the area range wide (valid at both factors) so
// only the target group flips. Contract 1 outer-contour mode: a filled disc,
// no ring, saturating background subtraction (frames and background black).
#pragma once

#include "backend/processing/ProcessingConfigJson.h"
#include "backend/processing/ProcessingService.h"
#include "backend/processing/ProcessingTypes.h"
#include "backend/recording/Hdf5Service.h"

#include "support/assert.h"

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace pixel_factor_fixture
{
    namespace fs = std::filesystem;
    using backend::services::Hdf5Service;
    using backend::services::ProcessedFrame;
    using backend::services::ProcessingConfig;
    using backend::services::ProcessingService;

    constexpr int kRows = 96, kCols = 128;
    constexpr int kFrames = 4;
    constexpr double kRecorded = 0.25;
    constexpr double kDefault = ProcessingService::kDefaultPixelToMicronFactor;

    // A bright disc on black; frames move it along x so no two are one track.
    inline cv::Mat disc(int frame, int radius = 12, int row = 48)
    {
        cv::Mat m(kRows, kCols, CV_8UC1, cv::Scalar(0));
        cv::circle(m, {20 + 28 * frame, row}, radius, cv::Scalar(200), -1, cv::LINE_8);
        return m;
    }

    inline cv::Mat blackBackground() { return cv::Mat(kRows, kCols, CV_8UC1, cv::Scalar(0)); }

    inline ProcessingConfig baseConfig()
    {
        ProcessingConfig c;
        c.processing_contract_version = 1;
        c.require_single_inner_contour = false; // outer-contour objects: a filled disc
        c.enable_ring_ratio_check = false;
        c.enable_border_check = true;
        c.enable_area_range_check = true;
        c.area_threshold_min = 1;
        c.area_threshold_max = 100000;
        c.enable_target_group = false;
        return c;
    }

    // The disc's hull area in pixels (factor independent).
    inline double discAreaPx()
    {
        ProcessingService probe;
        const auto records = probe.processBatch({disc(0)}, baseConfig(), blackBackground());
        double area = 0.0;
        for (const auto &r : records)
            if (r.validation.isValid) area = r.validation.area;
        MIB_REQUIRE(area > 100.0, "fixture: the disc is one valid object (area " + std::to_string(area) + " px)");
        return area;
    }

    inline ProcessingConfig flipConfig(double areaPx)
    {
        const double at25 = areaPx * kRecorded * kRecorded;
        ProcessingConfig c = baseConfig();
        c.area_threshold_min = static_cast<int>(std::floor(at25 / 2.0));
        c.area_threshold_max = static_cast<int>(std::ceil(at25 * 2.0));
        c.enable_target_group = true;
        c.target_group_area_min = static_cast<int>(std::floor(at25 * 0.8));
        c.target_group_area_max = static_cast<int>(std::ceil(at25 * 1.25));
        c.target_group_deformability_min = 0.0;
        c.target_group_deformability_max = 0.5;
        MIB_REQUIRE(areaPx * kDefault * kDefault > c.area_threshold_max, "fixture: invalid at 0.4886");
        return c;
    }

    inline ProcessingConfig targetOnlyConfig(double areaPx)
    {
        ProcessingConfig c = flipConfig(areaPx);
        c.area_threshold_min = 1;
        c.area_threshold_max = 100000;
        MIB_REQUIRE(areaPx * kDefault * kDefault > c.target_group_area_max, "fixture: no target group at 0.4886");
        return c;
    }

    // An experiment file as a capture writes it: kFrames valid rows (the disc
    // images), the recorded config (attributes + the `config_json` document,
    // which alone carries the target group), a full-frame ROI, a black
    // background; `recordedFactor` > 0 is written to the run snapshot as
    // `pixel_to_micron` (TD-17), 0 writes none. `document` false leaves out
    // `config_json` (a file from before captures wrote it).
    inline void writeSource(const fs::path &path, double recordedFactor, const ProcessingConfig &config,
                            bool document = true)
    {
        Hdf5Service writer;
        MIB_REQUIRE(writer.openFile(path.string()) && writer.initializeDatasets(), "fixture open " + path.string());
        std::vector<ProcessedFrame> valid;
        for (int i = 0; i < kFrames; ++i)
        {
            ProcessedFrame f;
            f.index = 100 + static_cast<std::uint64_t>(i);
            f.timestampNs = 1000 + 10 * static_cast<std::uint64_t>(i);
            f.originalImage = disc(i);
            f.processedImage = cv::Mat::zeros(kRows, kCols, CV_8UC1);
            f.validation.isValid = true;
            f.validation.objectCount = 1;
            valid.push_back(f);
        }
        MIB_REQUIRE(writer.appendFrames(valid, {}), "fixture frames");
        const cv::Mat background = blackBackground();
        MIB_REQUIRE(writer.writeExperimentInfo(1000, 2000, valid.size(), 0, config, {0, 0, kCols, kRows}, &background),
                    "fixture info");
        if (document)
        {
            nlohmann::json doc{{"image_processing", backend::processing::config_json::toJson(config)}};
            if (recordedFactor > 0.0) doc["pixel_to_micron_factor"] = recordedFactor;
            MIB_REQUIRE(writer.writeConfigJson(doc.dump()), "fixture config document");
        }
        if (recordedFactor > 0.0)
            MIB_REQUIRE(writer.writeRunSnapshotJson("{\"pixel_to_micron\":" + std::to_string(recordedFactor) + "}", "{}"),
                        "fixture snapshot");
        writer.closeFile();
    }

    // A folder of the same disc frames (PNG).
    inline void writeFolder(const fs::path &dir)
    {
        fs::create_directories(dir);
        for (int i = 0; i < kFrames; ++i)
            MIB_REQUIRE(cv::imwrite((dir / ("frame_" + std::to_string(i) + ".png")).string(), disc(i)), "folder frame");
    }

    struct Output
    {
        std::vector<ProcessedFrame> rows; // valid then invalid
        std::size_t valid{0}, target{0};
        std::optional<double> recordedFactor; // /experiment_info @pixel_to_micron
        ProcessingConfig config;
    };

    inline Output readOutput(const fs::path &path)
    {
        Output out;
        Hdf5Service reader;
        MIB_REQUIRE(reader.loadFile(path.string()), "output opens " + path.string());
        std::vector<ProcessedFrame> valid, invalid;
        reader.readValidMetadata(valid);
        reader.readInvalidMetadata(invalid);
        out.valid = valid.size();
        out.rows = valid;
        out.rows.insert(out.rows.end(), invalid.begin(), invalid.end());
        for (const auto &r : out.rows)
            if (r.validation.isTargetGroup) ++out.target;
        out.recordedFactor = reader.readAnalysisPixelToMicron();
        reader.readRecordedProcessingConfig(out.config);
        return out;
    }

    // Every record of a frame with an object: all valid (and in the target
    // group when `target`), or none valid.
    inline bool allObjects(const Output &out, bool valid, bool target)
    {
        std::size_t objects = 0;
        bool ok = true;
        for (const auto &r : out.rows)
        {
            if (r.validation.objectCount <= 0) continue;
            ++objects;
            if (r.validation.isValid != valid || r.validation.isTargetGroup != target) ok = false;
        }
        if (!ok || objects == 0)
        {
            std::fprintf(stderr, "allObjects(valid=%d, target=%d): %zu rows\n", valid, target, out.rows.size());
            for (const auto &r : out.rows)
                std::fprintf(stderr, "  index %llu objects %d valid %d target %d area %.1f px\n",
                             static_cast<unsigned long long>(r.index), r.validation.objectCount, r.validation.isValid,
                             r.validation.isTargetGroup, r.validation.area);
        }
        return ok && objects >= 1;
    }
} // namespace pixel_factor_fixture
