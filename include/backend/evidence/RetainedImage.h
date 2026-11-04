#pragma once
#include "backend/evidence/AnalysisMapping.h"
#include <opencv2/core.hpp>
#include <charconv>

namespace backend::evidence {
// Owned copy, never a borrowed SDK/ring pointer. No durable retention promise.
struct RetainedImage {
    std::string status{"missing"}, reason{"image was not retained"};
    std::string sourceKey, set, ordinal;
    cv::Mat pixels;
};
inline bool canonicalOrdinal(const std::string& value, std::uint64_t& out) {
    if (value.empty() || (value.size() > 1 && value.front() == '0')) return false;
    auto parsed = std::from_chars(value.data(), value.data() + value.size(), out);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}
} // namespace backend::evidence
