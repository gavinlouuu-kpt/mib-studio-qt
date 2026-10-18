// review_charts_json_test (#512 slice 4, plan
// docs/exec-plans/active/2026-10-18-one-review-implementation.md)
//
// The chart snapshot JSON and the saved-image overlay PNG
// (backend/review/ReviewCharts.h) moved out of BackendFacade into
// mib_review_core. The pre-slice facade code is kept below as the oracle
// (`oracle::`, verbatim but for the free-function signatures), and the
// builders must reproduce it byte for byte:
//  - reviewChartsJson == the pre-slice JSON for empty, single, all-invalid,
//    random (NaN / Inf / extreme values, both classes) and bin-edge row
//    sets under several factors and ring ranges; a small fixture's JSON is
//    also pinned literally (curves aside), and the density conserves every
//    finite point; the error JSON is pinned;
//  - renderSavedOverlayPng(…, RoiStyle::Facade) == the pre-slice PNG bytes,
//    or throws the same exception type and message, over files with an
//    in-bounds ROI, an ROI past the image, no masks, invalid frames only and
//    a recording; valid / invalid × indices × modes −1..5 × ROI, plus
//    malformed requests;
//  - RoiStyle::Review draws the Qt tab's / ReviewSession's ROI: its pixels
//    equal ReviewSession::fetchImage with the ROI for modes 0..4.
// The facade surface is covered by backend.export_bridge_facade and, in the
// Qt build, frontend.review_parity. Watchdog guarded.

#include "backend/processing/ProcessingService.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/recording/ProcessingOverlay.h"
#include "backend/recording/ReviewChartData.h"
#include "backend/review/ReviewCharts.h"
#include "backend/review/ReviewSession.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <typeinfo>
#include <vector>

namespace fs = std::filesystem;
using backend::services::Hdf5Service;
using backend::services::ProcessedFrame;
using nlohmann::json;
namespace services = backend::services;
namespace recording = backend::recording;

// ---- the pre-slice implementation (src/backend/app/ReviewCharts.cpp, develop 992d22d1) ----
namespace oracle
{
    std::vector<uint8_t> renderReviewOverlayJson(const std::string &text)
    {
        using nlohmann::json;
        const auto request=json::parse(text);
        const auto& value=request.at("index");
        if(!value.is_number_integer() || value<0)throw std::invalid_argument("Image index must be nonnegative integer");
        const auto index=value.get<size_t>();
        const auto mode=request.value("mode",0);
        if(mode<0 || mode>4)throw std::invalid_argument("Invalid overlay mode");
        const bool valid=request.value("valid",true);
        services::Hdf5Service reader;
        if(!reader.loadFile(request.at("source_path").get<std::string>()))throw std::runtime_error("Cannot open overlay source");
        const auto path=valid?"/valid_frames/images":"/invalid_frames/images";
        size_t count=0;int height=0,width=0,channels=0;
        if(!reader.getDatasetInfo(path,count,height,width,channels) || index>=count || height<=0 || width<=0 || channels!=1 || static_cast<uint64_t>(height)*width>64*1024*1024ULL)
            throw std::runtime_error("Overlay source frame unavailable or exceeds 64 MiB");
        cv::Mat image,mask;
        if(!reader.readImageByIndex(path,index,image))throw std::runtime_error("Cannot read overlay source image");
        if(mode && (!reader.readImageByIndex(valid?"/valid_frames/masks":"/invalid_frames/masks",index,mask) || mask.size()!=image.size()))
            throw std::runtime_error("Matching saved mask unavailable");
        std::vector<services::ProcessedFrame> rows;
        services::FilterResult classification;
        classification.isValid=valid;
        const bool metadataOk=valid?reader.readValidMetadata(rows):reader.readInvalidMetadata(rows);
        if(metadataOk && index<rows.size())classification=rows[index].validation;
        auto rgb=recording::renderProcessingOverlay(image,mask,&classification,static_cast<recording::OverlayMode>(mode));
        if(request.value("roi",false)) {
            uint64_t first=0,last=0;size_t nvalid=0,ninvalid=0;services::ProcessingService::Roi roi;
            if(reader.readExperimentInfo(first,last,nvalid,ninvalid,&roi) && roi.w>0 && roi.h>0)
                cv::rectangle(rgb,cv::Rect(roi.x,roi.y,roi.w,roi.h)&cv::Rect(0,0,rgb.cols,rgb.rows),cv::Scalar(255,210,0),1);
        }
        cv::Mat bgr;cv::cvtColor(rgb,bgr,cv::COLOR_RGB2BGR);std::vector<uint8_t> bytes;
        if(!cv::imencode(".png",bgr,bytes))throw std::runtime_error("Could not encode overlay image");
        return bytes;
    }

    // fetchReviewChartsJson after the reader: `rows`, `source`, `factor`, `cfg` as the facade had them.
    std::string chartsJson(const std::vector<services::ProcessedFrame> &rows, const std::string &source, double factor,
                           double ringMin, double ringMax)
    {
        using nlohmann::json;
        const auto data=recording::makeReviewChartData(rows,factor,ringMin,ringMax);
        constexpr int resolution=128;
        std::array<uint64_t,resolution*resolution> density{};
        const auto bin=[](double value,double min,double max) {return std::clamp(static_cast<int>((static_cast<long double>(value)-min)/(static_cast<long double>(max)-min)*resolution),0,resolution-1);};
        for(const auto& [area,deform]:data.points)++density[bin(deform,data.deformMin,data.deformMax)*resolution+bin(area,data.areaMin,data.areaMax)];
        json cells=json::array();for(int y=0;y<resolution;++y)for(int x=0;x<resolution;++x)if(density[y*resolution+x])cells.push_back({x,y,std::to_string(density[y*resolution+x])});
        json bins=json::array();for(const auto count:data.bins)bins.push_back(std::to_string(count));
        json curves=json::array();for(const auto& [modulus,points]:recording::bundledIsoelasticCurves())curves.push_back({{"modulus",modulus},{"points",points}});
        return json{{"valid",true},{"source_path",source},{"pixel_to_micron",factor},{"rows",std::to_string(rows.size())},
            {"finite_points",std::to_string(data.points.size())},{"excluded_nonfinite",std::to_string(data.excludedNonfinite)},
            {"area_range",{data.areaMin,data.areaMax}},{"deform_range",{data.deformMin,data.deformMax}},
            {"resolution",resolution},{"density",cells},{"histogram",bins},{"ring_range",{data.ringMin,data.ringMax}},
            {"histogram_samples",std::to_string(data.ringRatios.size())},{"curves",curves},
            {"curve_source","Bundled scaled_isoelastic_data_6.16-4.24; channel width 30 um, flow rate 0.25, fluid viscosity 4.24"}}.dump();
    }

    std::string chartsErrorJson(const std::string &error)
    {
        return json{{"valid",false},{"error",error}}.dump();
    }
} // namespace oracle

namespace
{
    ProcessedFrame row(bool valid, double area, double deform, double ring)
    {
        ProcessedFrame f;
        f.validation.isValid = valid;
        f.validation.area = area;
        f.validation.deformability = deform;
        f.validation.ringRatio = ring;
        return f;
    }

    cv::Mat pattern(int i, int w, int h)
    {
        cv::Mat m(h, w, CV_8UC1);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) m.at<uint8_t>(y, x) = static_cast<uint8_t>((x * 5 + y * 3 + i * 11) % 230);
        return m;
    }

    cv::Mat ringMask(int w, int h, int r)
    {
        cv::Mat m = cv::Mat::zeros(h, w, CV_8UC1);
        cv::circle(m, {w / 2, h / 2}, r, cv::Scalar(255), -1);
        cv::circle(m, {w / 2, h / 2}, r / 2, cv::Scalar(0), -1);
        return m;
    }

    void writeExperiment(const fs::path &path, int nValid, int nInvalid, services::ProcessingService::Roi roi, bool masks)
    {
        std::vector<ProcessedFrame> valid, invalid;
        for (int i = 0; i < nValid + nInvalid; ++i)
        {
            ProcessedFrame f;
            f.index = static_cast<std::uint64_t>(i) * 3;
            f.timestampNs = 100 + static_cast<std::uint64_t>(i);
            f.originalImage = pattern(i, 40, 30);
            if (masks) f.processedImage = ringMask(40, 30, 6 + i % 5);
            f.validation.isValid = i < nValid;
            f.validation.objectCount = 1;
            f.validation.objectId = i;
            f.validation.area = 400 + 31 * i;
            f.validation.deformability = 0.01 * (i % 7);
            f.validation.ringRatio = 1.0 + 0.4 * (i % 6);
            f.validation.hasSingleInnerContour = i % 2 == 1;
            (i < nValid ? valid : invalid).push_back(f);
        }
        Hdf5Service writer;
        MIB_REQUIRE(writer.openFile(path.string()) && writer.initializeDatasets(), "fixture open");
        // Without masks appendFrames refuses the batch part-way: the file keeps
        // images but no mask dataset (the "saved mask unavailable" case).
        const bool appended = writer.appendFrames(valid, invalid);
        MIB_REQUIRE(appended != !masks, "fixture frames: " + path.filename().string());
        MIB_REQUIRE(writer.writeExperimentInfo(1, 2, valid.size(), invalid.size(), {}, roi, nullptr),
                    "fixture info: " + path.filename().string());
        writer.closeFile();
    }

    void writeRecording(const fs::path &path)
    {
        Hdf5Service writer;
        MIB_REQUIRE(writer.openFile(path.string()) && writer.initializeRecordingDatasets(), "recording open");
        std::vector<cv::Mat> images;
        std::vector<Hdf5Service::RecordingFrameMeta> meta;
        for (int i = 0; i < 4; ++i)
        {
            images.push_back(pattern(i, 32, 24));
            meta.push_back({static_cast<std::uint64_t>(i), static_cast<std::uint64_t>(i) * 10, 32, 24});
        }
        MIB_REQUIRE(writer.appendRecordingFrames(images, meta), "recording frames");
        MIB_REQUIRE(writer.writeRecordingInfo(1, 2, 4, 0, false, 1), "recording info");
        writer.closeFile();
    }

    // "PNG <bytes>" or "<exception type>: <message>", for a like-for-like compare.
    struct Rendered
    {
        std::vector<std::uint8_t> bytes;
        std::string thrown;
        bool operator==(const Rendered &o) const { return bytes == o.bytes && thrown == o.thrown; }
    };

    Rendered render(const std::function<std::vector<std::uint8_t>()> &call)
    {
        Rendered r;
        try
        {
            r.bytes = call();
        }
        catch (const std::exception &e)
        {
            r.thrown = std::string(typeid(e).name()) + ": " + e.what();
        }
        return r;
    }
} // namespace

int main()
{
    mib::test::Watchdog wd(120);
    mib::test::TempDir td("review_charts_json");

    // ---- charts JSON == the pre-slice JSON ---------------------------------------------
    {
        std::vector<std::vector<ProcessedFrame>> sets;
        sets.push_back({});
        sets.push_back({row(true, 250, 0.05, 2.0)});
        sets.push_back({row(false, 250, 0.05, 2.0), row(false, 900, 0.2, 3.0)});
        {
            std::mt19937 rng(23);
            std::normal_distribution<double> area(900, 300), deform(0.08, 0.05), ring(2.0, 1.2);
            std::vector<ProcessedFrame> mixed;
            for (int i = 0; i < 600; ++i) mixed.push_back(row(i % 5 != 0, area(rng), deform(rng), ring(rng)));
            mixed[1].validation.area = std::numeric_limits<double>::quiet_NaN();
            mixed[2].validation.deformability = std::numeric_limits<double>::infinity();
            mixed[3].validation.ringRatio = 1e9;
            mixed[4].validation.ringRatio = -5;
            mixed[6].validation.area = 1e12;
            mixed[7].validation.ringRatio = std::numeric_limits<double>::quiet_NaN();
            mixed[8].validation.deformability = -std::numeric_limits<double>::infinity();
            sets.push_back(mixed);
        }
        {
            std::vector<ProcessedFrame> edges; // values on the range ends and bin edges
            for (int i = 0; i <= 40; ++i) edges.push_back(row(true, 100.0 + 25.0 * i, 0.005 * i, 0.25 * i));
            edges.push_back(row(true, 100.0, 0.0, 10.0));
            edges.push_back(row(true, 1100.0, 0.2, 0.0));
            sets.push_back(edges);
        }
        const std::vector<std::array<double, 3>> settings{
            {0.4886, 0.0, 10.0}, {0.75, 0.5, 6.0}, {1.0, 1.0, 3.0}, {0.25, 2.0, 2.0}};
        std::size_t cases = 0, throwing = 0;
        for (const auto &rows : sets)
        {
            for (const auto &[factor, ringMin, ringMax] : settings)
            {
                // The facade turns a throw into reviewChartsErrorJson(what()).
                const auto json_or_throw = [&](const auto &build) {
                    try
                    {
                        return build(rows, "/data/run.h5", factor, ringMin, ringMax);
                    }
                    catch (const std::exception &e)
                    {
                        return std::string("throw ") + typeid(e).name() + ": " + e.what();
                    }
                };
                const auto got = json_or_throw(backend::review::reviewChartsJson);
                const auto want = json_or_throw(oracle::chartsJson);
                MIB_EXPECT(got == want, "charts JSON equals the pre-slice output (" + std::to_string(rows.size()) +
                                            " rows, factor " + std::to_string(factor) + "): " + got.substr(0, 120));
                ++cases;
                if (got.rfind("throw ", 0) == 0)
                {
                    ++throwing;
                    continue;
                }
                const auto parsed = json::parse(got);
                std::uint64_t cells = 0;
                for (const auto &cell : parsed["density"]) cells += std::stoull(cell[2].get<std::string>());
                MIB_EXPECT(std::to_string(cells) == parsed["finite_points"].get<std::string>(),
                           "density conserves every finite point");
            }
        }
        MIB_EXPECT(cases == 20 && throwing == 5, "20 chart cases, the 5 with an empty ring range refused");

        // A literal pin of the shape (curves aside), independent of the oracle.
        // One (area, deform) point so the ranges are the exact defaults on every
        // platform (the padding uses long double, which differs on MSVC).
        const std::vector<ProcessedFrame> small{row(true, 400, 0.25, 1.0), row(true, 400, 0.25, 2.0),
                                                row(true, 400, 0.25, 3.0), row(false, 50, 0.5, 9.0)};
        auto pinned = json::parse(backend::review::reviewChartsJson(small, "/data/small.h5", 0.5, 1.0, 3.0));
        json curves = json::array();
        for (const auto &[modulus, points] : recording::bundledIsoelasticCurves())
            curves.push_back({{"modulus", modulus}, {"points", points}});
        MIB_EXPECT(!curves.empty() && pinned["curves"] == curves, "the bundled isoelastic curves");
        pinned.erase("curves");
        const std::string expected =
            R"({"area_range":[0.0,1000.0],"curve_source":"Bundled scaled_isoelastic_data_6.16-4.24; channel width 30 um, )"
            R"(flow rate 0.25, fluid viscosity 4.24","deform_range":[0.0,1.0],"density":[[12,32,"3"]],)"
            R"("excluded_nonfinite":"0","finite_points":"3","histogram":["1","0","1","1"],"histogram_samples":"3",)"
            R"("pixel_to_micron":0.5,"resolution":128,"ring_range":[1.0,3.0],"rows":"4","source_path":"/data/small.h5",)"
            R"("valid":true})";
        MIB_EXPECT(pinned.dump() == expected, "small fixture JSON: " + pinned.dump());
    }
    MIB_EXPECT(backend::review::reviewChartsErrorJson("No HDF file open") == R"({"error":"No HDF file open","valid":false})",
               "error JSON");
    MIB_EXPECT(backend::review::reviewChartsErrorJson("x") == oracle::chartsErrorJson("x"), "error JSON = pre-slice");
    wd.mark("charts");

    // ---- overlay PNG == the pre-slice PNG -----------------------------------------------
    const fs::path inBounds = td.path() / "in-bounds.h5";
    const fs::path pastEdge = td.path() / "past-edge.h5";
    const fs::path noMasks = td.path() / "no-masks.h5";
    const fs::path invalidOnly = td.path() / "invalid-only.h5";
    const fs::path recordingFile = td.path() / "recording.h5";
    writeExperiment(inBounds, 6, 4, {3, 2, 20, 15}, true);
    writeExperiment(pastEdge, 5, 2, {20, 10, 50, 50}, true);
    writeExperiment(noMasks, 4, 3, {0, 0, 0, 0}, false);
    writeExperiment(invalidOnly, 0, 4, {1, 1, 10, 10}, true);
    writeRecording(recordingFile);
    {
        std::size_t cases = 0, pngs = 0;
        for (const auto &file : {inBounds, pastEdge, noMasks, invalidOnly, recordingFile})
            for (const bool valid : {true, false})
                for (const int index : {0, 1, 3, 5, 6, 100})
                    for (int mode = -1; mode <= 5; ++mode)
                        for (const bool roi : {false, true})
                        {
                            const auto request =
                                json{{"source_path", file.string()}, {"valid", valid}, {"index", index}, {"mode", mode}, {"roi", roi}}
                                    .dump();
                            const auto got = render([&] {
                                return backend::review::renderSavedOverlayPng(request, backend::review::RoiStyle::Facade);
                            });
                            const auto want = render([&] { return oracle::renderReviewOverlayJson(request); });
                            MIB_EXPECT(got == want, "overlay equals the pre-slice output: " + request + " -> " +
                                                        (want.thrown.empty() ? "PNG" : want.thrown));
                            ++cases;
                            if (!want.bytes.empty()) ++pngs;
                        }
        MIB_EXPECT(cases == 840 && pngs > 100, "840 overlay cases, " + std::to_string(pngs) + " of them PNGs");
        const std::vector<std::string> malformed{
            "{bad",
            json{{"source_path", inBounds.string()}}.dump(),
            json{{"source_path", inBounds.string()}, {"index", -1}}.dump(),
            json{{"source_path", inBounds.string()}, {"index", 1.5}}.dump(),
            json{{"source_path", inBounds.string()}, {"index", "1"}}.dump(),
            json{{"index", 1}}.dump(),
            json{{"source_path", (td.path() / "missing.h5").string()}, {"index", 1}}.dump(),
            json{{"source_path", inBounds.string()}, {"index", 1}, {"mode", "x"}}.dump(),
            json{{"source_path", inBounds.string()}, {"index", 1}, {"valid", 1}}.dump(),
            json{{"source_path", inBounds.string()}, {"index", 1}, {"roi", "y"}}.dump(),
            json("x").dump(),
            json{{"source_path", inBounds.string()}, {"index", 2}}.dump(), // defaults: valid, mode 0, no ROI
        };
        for (const auto &request : malformed)
        {
            const auto got = render([&] {
                return backend::review::renderSavedOverlayPng(request, backend::review::RoiStyle::Facade);
            });
            const auto want = render([&] { return oracle::renderReviewOverlayJson(request); });
            MIB_EXPECT(got == want, "request handled as before: " + request + " -> " + want.thrown);
        }
        const auto missing = render([&] {
            return backend::review::renderSavedOverlayPng(json{{"source_path", inBounds.string()}, {"index", 1}, {"mode", 5}}.dump(),
                                                          backend::review::RoiStyle::Facade);
        });
        MIB_EXPECT(missing.thrown.find("Invalid overlay mode") != std::string::npos, "the bridge's message");
    }
    wd.mark("overlay");

    // ---- RoiStyle::Review draws ReviewSession's ROI --------------------------------------
    {
        backend::review::ReviewSession session;
        MIB_REQUIRE(session.open(inBounds.string()), "session opens the fixture");
        for (int mode = 0; mode <= 4; ++mode)
        {
            for (const int index : {0, 3})
            {
                const auto png = backend::review::renderSavedOverlayPng(
                    json{{"source_path", inBounds.string()}, {"valid", true}, {"index", index}, {"mode", mode}, {"roi", true}}.dump(),
                    backend::review::RoiStyle::Review);
                cv::Mat rgb;
                cv::cvtColor(cv::imdecode(png, cv::IMREAD_COLOR), rgb, cv::COLOR_BGR2RGB);
                backend::review::ReviewImage image;
                MIB_REQUIRE(session.fetchImage(backend::review::ReviewDataset::ValidImage, static_cast<std::uint64_t>(index),
                                               static_cast<backend::review::OverlayMode>(mode), true, image),
                            "session image");
                MIB_REQUIRE(image.channels == 3 && image.width == static_cast<std::uint64_t>(rgb.cols) &&
                                image.height == static_cast<std::uint64_t>(rgb.rows) && rgb.isContinuous(),
                            "session image is RGB of the same size");
                const std::vector<std::uint8_t> pixels(rgb.data, rgb.data + rgb.total() * rgb.elemSize());
                MIB_EXPECT(pixels == image.data, "Review ROI style = ReviewSession pixels (mode " + std::to_string(mode) +
                                                     ", index " + std::to_string(index) + ")");
            }
            // Without the ROI the style does not matter.
            const auto plain = json{{"source_path", inBounds.string()}, {"index", 1}, {"mode", mode}}.dump();
            MIB_EXPECT(backend::review::renderSavedOverlayPng(plain, backend::review::RoiStyle::Review) ==
                           backend::review::renderSavedOverlayPng(plain, backend::review::RoiStyle::Facade),
                       "no ROI: both styles give the same PNG");
        }
        session.close();
    }
    wd.mark("roi style");

    return mib::test::exitCode();
}
