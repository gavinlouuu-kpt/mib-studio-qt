#include "backend/review/ObjectCoordinates.h"

#include <opencv2/core.hpp>

#include <algorithm>
#include <cmath>

namespace backend::review
{

    const char *toString(ObjectSpace space)
    {
        switch (space)
        {
        case ObjectSpace::Image: return "image";
        case ObjectSpace::RoiLocal: return "roi_local";
        case ObjectSpace::Unknown: break;
        }
        return "unknown";
    }

    const char *toString(ObjectSpaceSource source)
    {
        switch (source)
        {
        case ObjectSpaceSource::Attribute: return "attribute";
        case ObjectSpaceSource::Snapshot: return "snapshot";
        case ObjectSpaceSource::MaskCheck: return "mask_check";
        case ObjectSpaceSource::RoiOrigin: return "roi_origin";
        case ObjectSpaceSource::None: break;
        }
        return "";
    }

    std::optional<ObjectSpace> parseObjectSpace(std::string_view value)
    {
        if (value == "roi_local") return ObjectSpace::RoiLocal;
        if (value == "image") return ObjectSpace::Image;
        return std::nullopt;
    }

    ObjectCoordinates objectCoordinatesFor(ObjectSpace space, ObjectSpaceSource source,
                                           const services::ProcessingService::Roi &recordedRoi)
    {
        ObjectCoordinates out;
        out.space = space;
        out.source = space == ObjectSpace::Unknown ? ObjectSpaceSource::None : source;
        if (space == ObjectSpace::RoiLocal)
        {
            out.originX = recordedRoi.x;
            out.originY = recordedRoi.y;
        }
        return out;
    }

    bool toImageSpace(const ObjectCoordinates &coordinates, cv::Rect2d &bbox, cv::Point2d &centroid)
    {
        if (coordinates.space == ObjectSpace::Unknown) return false;
        bbox.x += coordinates.originX;
        bbox.y += coordinates.originY;
        centroid.x += coordinates.originX;
        centroid.y += coordinates.originY;
        return true;
    }

    namespace
    {
        // Mask pixels (non-zero) inside `box`, clipped to the mask.
        std::size_t maskPixelsIn(const cv::Mat &mask, const cv::Rect2d &box)
        {
            if (!std::isfinite(box.x) || !std::isfinite(box.y) || !std::isfinite(box.width) || !std::isfinite(box.height))
                return 0;
            const int x0 = std::max(0, static_cast<int>(std::floor(box.x)));
            const int y0 = std::max(0, static_cast<int>(std::floor(box.y)));
            const int x1 = std::min(mask.cols, static_cast<int>(std::ceil(box.x + box.width)));
            const int y1 = std::min(mask.rows, static_cast<int>(std::ceil(box.y + box.height)));
            if (x1 <= x0 || y1 <= y0) return 0;
            return static_cast<std::size_t>(cv::countNonZero(mask(cv::Rect(x0, y0, x1 - x0, y1 - y0))));
        }
    } // namespace

    std::optional<ObjectSpace> maskPlacementVote(const cv::Mat &mask, const cv::Rect2d &storedBox, int originX,
                                                 int originY)
    {
        if (mask.empty() || mask.channels() != 1 || storedBox.width <= 0 || storedBox.height <= 0) return std::nullopt;
        if (originX == 0 && originY == 0) return std::nullopt; // both placements are the same
        const std::size_t image = maskPixelsIn(mask, storedBox);
        cv::Rect2d shifted = storedBox;
        shifted.x += originX;
        shifted.y += originY;
        const std::size_t roiLocal = maskPixelsIn(mask, shifted);
        const std::size_t more = std::max(image, roiLocal);
        const std::size_t less = std::min(image, roiLocal);
        if (more == 0 || more < 2 * less) return std::nullopt;
        return image > roiLocal ? ObjectSpace::Image : ObjectSpace::RoiLocal;
    }

    std::optional<ObjectSpace> maskCheckVerdict(const std::vector<std::optional<ObjectSpace>> &votes)
    {
        std::size_t image = 0;
        std::size_t roiLocal = 0;
        for (const auto &vote : votes)
        {
            if (!vote) continue;
            (*vote == ObjectSpace::Image ? image : roiLocal) += 1;
        }
        const std::size_t decisive = image + roiLocal;
        if (decisive < kObjectSpaceMaskCheckMinDecisive) return std::nullopt;
        const std::size_t winner = std::max(image, roiLocal);
        if (winner * 100 < decisive * static_cast<std::size_t>(kObjectSpaceMaskCheckMinAgreementPercent))
            return std::nullopt;
        return image > roiLocal ? ObjectSpace::Image : ObjectSpace::RoiLocal;
    }

    std::vector<std::size_t> maskCheckSample(std::size_t candidates)
    {
        const std::size_t n = std::min(candidates, kObjectSpaceMaskCheckMaxRecords);
        std::vector<std::size_t> out;
        out.reserve(n);
        for (std::size_t k = 0; k < n; ++k) out.push_back(k * candidates / n);
        return out;
    }

} // namespace backend::review
