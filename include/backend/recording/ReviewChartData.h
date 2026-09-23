#pragma once
#include "backend/processing/ProcessingTypes.h"
#include <map>
#include <string>
#include <utility>
#include <vector>
#include <opencv2/core.hpp>

namespace backend::recording {
struct ReviewChartData {
    std::vector<std::pair<double,double>> points;
    std::vector<double> ringRatios;
    std::vector<uint64_t> bins;
    double areaMin{0},areaMax{1000},deformMin{0},deformMax{1};
    double ringMin{0},ringMax{1};
    uint64_t excludedNonfinite{0};
};
// Shared Qt/Tauri chart preparation; no processing pipeline or modulus inference.
ReviewChartData makeReviewChartData(const std::vector<services::ProcessedFrame>& frames,
                                    double pixelToMicron,double ringMin,double ringMax);
using IsoelasticCurves=std::map<double,std::vector<std::pair<double,double>>>;
// The same repository curve resource used by Qt, embedded for reproducible packaging.
const IsoelasticCurves& bundledIsoelasticCurves();
std::map<std::string,cv::Mat> renderReviewCharts(const ReviewChartData& data,bool overlays=true);
} // namespace backend::recording
