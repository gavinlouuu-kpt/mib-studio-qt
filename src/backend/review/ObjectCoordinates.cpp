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

    namespace
    {
        struct Component
        {
            cv::Rect box;
            cv::Mat pixels; // 255 on the component's pixels, cropped to `box`
        };

        // The 8-connected components of `binary` (CV_8U, non-zero is
        // foreground) with a pixel inside `window`, each traced in full. Only
        // those components are visited, on this thread: OpenCV's parallel
        // labelling hands buffers between TBB threads, which the thread
        // sanitizer cannot follow.
        std::vector<Component> componentsMeeting(const cv::Mat &binary, const cv::Rect &window)
        {
            std::vector<Component> out;
            cv::Mat visited = cv::Mat::zeros(binary.size(), CV_8U);
            std::vector<cv::Point> stack;
            std::vector<cv::Point> members;
            for (int y = window.y; y < window.y + window.height; ++y)
            {
                for (int x = window.x; x < window.x + window.width; ++x)
                {
                    if (!binary.at<std::uint8_t>(y, x) || visited.at<std::uint8_t>(y, x)) continue;
                    members.clear();
                    stack.assign(1, cv::Point(x, y));
                    visited.at<std::uint8_t>(y, x) = 1;
                    int x0 = x, y0 = y, x1 = x, y1 = y;
                    while (!stack.empty())
                    {
                        const cv::Point p = stack.back();
                        stack.pop_back();
                        members.push_back(p);
                        x0 = std::min(x0, p.x);
                        y0 = std::min(y0, p.y);
                        x1 = std::max(x1, p.x);
                        y1 = std::max(y1, p.y);
                        for (int dy = -1; dy <= 1; ++dy)
                        {
                            for (int dx = -1; dx <= 1; ++dx)
                            {
                                const int nx = p.x + dx, ny = p.y + dy;
                                if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= binary.cols || ny >= binary.rows)
                                    continue;
                                if (!binary.at<std::uint8_t>(ny, nx) || visited.at<std::uint8_t>(ny, nx)) continue;
                                visited.at<std::uint8_t>(ny, nx) = 1;
                                stack.emplace_back(nx, ny);
                            }
                        }
                    }
                    Component c;
                    c.box = cv::Rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
                    c.pixels = cv::Mat::zeros(c.box.size(), CV_8U);
                    for (const cv::Point &m : members) c.pixels.at<std::uint8_t>(m.y - y0, m.x - x0) = 255;
                    out.push_back(std::move(c));
                }
            }
            return out;
        }
    } // namespace

    bool storedObjectAt(const cv::Mat &mask, const cv::Rect2d &placedBox, const StoredObject &object)
    {
        if (mask.empty() || mask.channels() != 1 || !finiteBox(placedBox)) return false;
        const cv::Rect2d visible = placedBox & cv::Rect2d(0, 0, mask.cols, mask.rows);
        if (visible.area() <= 0.0) return false;
        const int x0 = static_cast<int>(std::floor(visible.x));
        const int y0 = static_cast<int>(std::floor(visible.y));
        const int x1 = static_cast<int>(std::ceil(visible.x + visible.width));
        const int y1 = static_cast<int>(std::ceil(visible.y + visible.height));
        const cv::Rect window = cv::Rect(x0, y0, std::max(1, x1 - x0), std::max(1, y1 - y0)) &
                                cv::Rect(0, 0, mask.cols, mask.rows);
        cv::Mat binary;
        cv::compare(mask, 0, binary, cv::CMP_NE);
        for (const Component &c : componentsMeeting(binary, window))
        {
            if (iou(cv::Rect2d(c.box), placedBox) * 100.0 < kObjectSpaceMaskCheckMinBoxIouPercent) continue;
            if (contoursMatch(c.pixels, object)) return true;
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
