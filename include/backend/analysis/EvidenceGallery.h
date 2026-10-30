#pragma once
#include "backend/evidence/RetainedImage.h"
#include <nlohmann/json.hpp>
#include <opencv2/imgproc.hpp>
#include <functional>
#include <algorithm>
#include <set>

namespace backend::analysis {
// Presentation-only copies. Event identity is resolved by the producer, never an image index.
inline nlohmann::json evidenceGallery(
    const nlohmann::json& source, const nlohmann::json& keys, bool mask,
    const std::function<evidence::RetainedImage(const std::string&, const std::string&)>& read) {
    using Json = nlohmann::json;
    if (!keys.is_array() || keys.size() > 12)
        throw std::invalid_argument("Gallery requires at most 12 exact keys");
    std::set<std::string> known, seen;
    for (const auto& row : source.at("rows"))
        known.insert(row.at("key").get<std::string>());
    Json images = Json::array();
    for (const auto& value : keys) {
        const auto key = value.get<std::string>();
        const auto ref = Json::parse(key);
        if (!known.count(key) || !seen.insert(key).second || !ref.is_array() || ref.size() != 3 ||
            ref[0] != source.at("sourceKey"))
            throw std::invalid_argument("Foreign or duplicate evidence gallery key");
        const auto set = ref[1].get<std::string>(), ordinal = ref[2].get<std::string>();
        uint64_t number = 0;
        if ((set != "valid" && set != "invalid") || !evidence::canonicalOrdinal(ordinal, number))
            throw std::invalid_argument("Invalid event identity");
        auto retained = read(set, ordinal);
        if (retained.sourceKey != ref[0] || retained.set != set || retained.ordinal != ordinal)
            throw std::invalid_argument("Image provider changed identity");
        Json item{{"key", key},
                  {"set", set},
                  {"ordinal", ordinal},
                  {"pixels", nullptr},
                  {"status", retained.status},
                  {"reason", retained.reason},
                  {"mask", mask}};
        const auto& image = retained.pixels;
        if (retained.status == "available") {
            if (image.empty() || image.depth() != CV_8U ||
                (image.channels() != 1 && image.channels() != 3 && image.channels() != 4)) {
                item["status"] = "refused";
                item["reason"] = "Thumbnail requires nonempty 8-bit gray/BGR/BGRA pixels";
            } else {
                cv::Mat gray;
                if (image.channels() == 1)
                    gray = image;
                else
                    cv::cvtColor(image, gray,
                                 image.channels() == 3 ? cv::COLOR_BGR2GRAY : cv::COLOR_BGRA2GRAY);
                const double scale = std::min(64.0 / gray.cols, 64.0 / gray.rows);
                cv::Mat resized, tile(64, 64, CV_8UC1, cv::Scalar(0));
                cv::resize(gray, resized,
                           cv::Size(std::max(1, int(gray.cols * scale)),
                                    std::max(1, int(gray.rows * scale))),
                           0, 0, mask ? cv::INTER_NEAREST : cv::INTER_AREA);
                resized.copyTo(tile(cv::Rect((64 - resized.cols) / 2, (64 - resized.rows) / 2,
                                             resized.cols, resized.rows)));
                item["pixels"] = std::vector<unsigned char>(tile.data, tile.data + tile.total());
                item["width"] = image.cols;
                item["height"] = image.rows;
                item["channels"] = image.channels();
                item["thumbnailSemantics"] = "64x64 letterboxed grayscale display copy";
            }
        }
        images.push_back(item);
    }
    return Json{
        {"schema", "F-gallery/1"}, {"fingerprint", source.at("fingerprint")}, {"images", images}};
}
} // namespace backend::analysis
