#include "backend/recording/ReviewExport.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace backend::review
{
    namespace
    {
        // Matches Qt's QString::number(x, 'f', precision).
        std::string fixed(double value, int precision)
        {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.*f", precision, value);
            return buf;
        }
    } // namespace

    bool writeMetricsCsv(
        const std::string &filePath,
        const std::vector<services::ProcessedFrame> &validFrames,
        const std::vector<services::ProcessedFrame> &invalidFrames,
        double pixelToMicronFactor,
        std::string *errorOut,
        const std::function<bool(std::uint64_t, std::uint64_t)> &progress)
    {
        std::ofstream out(filePath, std::ios::out | std::ios::trunc);
        if (!out.is_open())
        {
            if (errorOut)
            {
                *errorOut = "Failed to open file for writing: " + filePath;
            }
            return false;
        }

        auto removePartial = [&filePath, &out]() {
            out.close();
            std::error_code ec;
            std::filesystem::remove(std::filesystem::path(filePath), ec);
        };

        const double areaConversionFactor = pixelToMicronFactor * pixelToMicronFactor;
        const std::uint64_t total = validFrames.size() + invalidFrames.size();
        std::uint64_t done = 0;

        // Column set/precision mirror the Qt HdfReviewTab export exactly.
        out << "Frame Type,Index,Timestamp,Object Id,Object Count,Track Id,Track First,Track Last,Track Observations,"
            << "Deformability,Area,Area (um²),Area Ratio,Ring Ratio,"
            << "Valid,Touches Border,Single Inner,In Range,Inner Count,"
            << "Bright Q1,Bright Q2,Bright Q3,Bright Q4\n";

        auto writeFrame = [&](const char *frameType, const services::ProcessedFrame &frame) -> bool {
            const auto &val = frame.validation;
            const double areaMicrons = val.area * areaConversionFactor;
            out << frameType << ','
                << frame.index << ','
                << frame.timestampNs << ','
                << val.objectId << ','
                << val.objectCount << ','
                << val.trackId << ','
                << val.trackFirstFrame << ','
                << val.trackLastFrame << ','
                << val.trackObservationCount << ','
                << fixed(val.deformability, 3) << ','
                << fixed(val.area, 2) << ','
                << fixed(areaMicrons, 2) << ','
                << fixed(val.areaRatio, 3) << ','
                << fixed(val.ringRatio, 3) << ','
                << (val.isValid ? "Yes" : "No") << ','
                << (val.touchesBorder ? "Yes" : "No") << ','
                << (val.hasSingleInnerContour ? "Yes" : "No") << ','
                << (val.inRange ? "Yes" : "No") << ','
                << val.innerContourCount << ','
                << fixed(val.brightness.q1, 2) << ','
                << fixed(val.brightness.q2, 2) << ','
                << fixed(val.brightness.q3, 2) << ','
                << fixed(val.brightness.q4, 2) << '\n';

            ++done;
            if (progress && (done % 256 == 0 || done == total))
            {
                if (!progress(done, total))
                {
                    return false; // cancelled
                }
            }
            return true;
        };

        for (const auto &frame : validFrames)
        {
            if (!writeFrame("Valid", frame))
            {
                removePartial();
                if (errorOut)
                {
                    *errorOut = "Export cancelled";
                }
                return false;
            }
        }
        for (const auto &frame : invalidFrames)
        {
            if (!writeFrame("Invalid", frame))
            {
                removePartial();
                if (errorOut)
                {
                    *errorOut = "Export cancelled";
                }
                return false;
            }
        }

        out.flush();
        if (!out.good())
        {
            removePartial();
            if (errorOut)
            {
                *errorOut = "Failed while writing CSV: " + filePath;
            }
            return false;
        }
        return true;
    }

} // namespace backend::review
