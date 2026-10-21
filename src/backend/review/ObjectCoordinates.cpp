#include "backend/review/ObjectCoordinates.h"

#include <opencv2/core.hpp>
#if __has_include(<opencv2/geometry.hpp>)
#include <opencv2/geometry.hpp> // OpenCV 5 moved contour geometry out of imgproc.hpp
#endif
#include <opencv2/imgproc.hpp>

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
        bool finiteBox(const cv::Rect2d &box)
        {
            return std::isfinite(box.x) && std::isfinite(box.y) && std::isfinite(box.width) &&
                   std::isfinite(box.height) && box.width > 0 && box.height > 0;
        }

        double iou(const cv::Rect2d &a, const cv::Rect2d &b)
        {
            const double overlap = (a & b).area();
            const double joint = a.area() + b.area() - overlap;
            return joint > 0.0 ? overlap / joint : 0.0;
        }

        // Whether a component (its pixels set in `component`, cropped to its
        // box) is the stored object by its contours: a hole for a hole
        // object, and the hull area of that hole (or of the outline) within
        // the tolerance of the stored area when it is checked.
        bool contoursMatch(const cv::Mat &component, const StoredObject &object)
        {
            const bool checkArea = std::isfinite(object.areaPx) && object.areaPx > 0.0;
            if (!checkArea && !object.hole) return true;
            std::vector<std::vector<cv::Point>> contours;
            std::vector<cv::Vec4i> hierarchy;
            cv::findContours(component, contours, hierarchy, cv::RETR_CCOMP, cv::CHAIN_APPROX_NONE);
            const double tolerance = object.areaPx * kObjectSpaceMaskCheckAreaTolerancePercent / 100.0;
            for (std::size_t i = 0; i < contours.size(); ++i)
            {
                const bool isHole = hierarchy[i][3] >= 0;
                if (isHole != object.hole) continue;
                if (!checkArea) return true;
                std::vector<cv::Point> hull;
                cv::convexHull(contours[i], hull);
                if (std::abs(cv::contourArea(hull) - object.areaPx) <= tolerance) return true;
            }
            return false;
        }
    } // namespace

    bool storedObjectAt(const cv::Mat &mask, const cv::Rect2d &placedBox, const StoredObject &object)
    {
        if (mask.empty() || mask.channels() != 1 || !finiteBox(placedBox)) return false;
        const cv::Rect2d frame(0, 0, mask.cols, mask.rows);
        const cv::Rect2d visible = placedBox & frame;
        if (visible.area() <= 0.0) return false;
        cv::Mat binary;
        cv::compare(mask, 0, binary, cv::CMP_NE);
        cv::Mat labels, stats, centroids;
        cv::connectedComponentsWithStats(binary, labels, stats, centroids, 8, CV_32S);
        const cv::Rect window(static_cast<int>(std::floor(visible.x)), static_cast<int>(std::floor(visible.y)),
                              std::max(1, static_cast<int>(std::ceil(visible.x + visible.width)) -
                                              static_cast<int>(std::floor(visible.x))),
                              std::max(1, static_cast<int>(std::ceil(visible.y + visible.height)) -
                                              static_cast<int>(std::floor(visible.y))));
        const cv::Rect inside = window & cv::Rect(0, 0, mask.cols, mask.rows);
        std::vector<bool> seen(static_cast<std::size_t>(stats.rows), false);
        for (int y = inside.y; y < inside.y + inside.height; ++y)
        {
            const int *row = labels.ptr<int>(y);
            for (int x = inside.x; x < inside.x + inside.width; ++x)
            {
                const int label = row[x];
                if (label <= 0 || seen[static_cast<std::size_t>(label)]) continue;
                seen[static_cast<std::size_t>(label)] = true;
                const cv::Rect box(stats.at<int>(label, cv::CC_STAT_LEFT), stats.at<int>(label, cv::CC_STAT_TOP),
                                   stats.at<int>(label, cv::CC_STAT_WIDTH), stats.at<int>(label, cv::CC_STAT_HEIGHT));
                if (iou(cv::Rect2d(box), placedBox) * 100.0 < kObjectSpaceMaskCheckMinBoxIouPercent) continue;
                cv::Mat component;
                cv::compare(labels(box), label, component, cv::CMP_EQ);
                if (contoursMatch(component, object)) return true;
            }
        }
        return false;
    }

    std::optional<ObjectSpace> maskPlacementVote(const cv::Mat &mask, const StoredObject &object, int originX,
                                                 int originY)
    {
        if (originX == 0 && originY == 0) return std::nullopt; // both placements are the same
        cv::Rect2d shifted = object.box;
        shifted.x += originX;
        shifted.y += originY;
        const bool image = storedObjectAt(mask, object.box, object);
        const bool roiLocal = storedObjectAt(mask, shifted, object);
        if (image == roiLocal) return std::nullopt; // both fit, or neither
        return image ? ObjectSpace::Image : ObjectSpace::RoiLocal;
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

    std::vector<std::size_t> maskCheckSample(std::size_t candidates, std::size_t limit)
    {
        const std::size_t n = std::min(candidates, limit);
        std::vector<std::size_t> out;
        out.reserve(n);
        for (std::size_t k = 0; k < n; ++k) out.push_back(k * candidates / n);
        return out;
    }

} // namespace backend::review
