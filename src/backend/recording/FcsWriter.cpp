#include "backend/recording/FcsWriter.h"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <sstream>
#include <unordered_set>

namespace backend::recording {
namespace {

constexpr uint64_t kHeaderOffsetLimit = 99999999ULL;
constexpr std::size_t kHeaderSize = 58;

bool hasMember(const std::unordered_set<std::string>& members, const char* name) {
    return members.find(name) != members.end();
}

void add(std::vector<FcsField>& fields, const char* member, const char* name, const char* display,
         const char* unit) {
    fields.push_back(FcsField{member, name, display, unit});
}

std::string cleanText(std::string value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char c : value) {
        escaped.push_back(c);
        if (c == '|') escaped.push_back(c);
    }
    return escaped;
}

std::string offsetText(uint64_t value) {
    return std::to_string(value);
}

std::string makeText(const std::vector<FcsField>& fields, uint64_t total, uint64_t beginData,
                     uint64_t endData, const FcsWriteOptions& options) {
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << '|' << "$BEGINANALYSIS|0|$ENDANALYSIS|0|$BEGINSTEXT|0|$ENDSTEXT|0|" << "$TOT|" << total
         << "|$PAR|" << fields.size() << "|$MODE|L|$DATATYPE|F|"
         << "$BYTEORD|1,2,3,4|$NEXTDATA|0|$BEGINDATA|" << offsetText(beginData) << "|$ENDDATA|"
         << offsetText(endData) << "|$TIMESTEP|1|$FIL|"
         << cleanText(std::filesystem::path(options.metadata.source).filename().string()) << '|';
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto& field = fields[i];
        const std::string parameter = "P" + std::to_string(i + 1);
        text << "$" << parameter << "B|32|$" << parameter << "E|0,0|$" << parameter << "N|"
             << cleanText(field.pnn) << "|$" << parameter << "S|" << cleanText(field.pns) << "|$"
             << parameter << "U|" << (field.unit.empty() ? "unitless" : cleanText(field.unit))
             << "|$" << parameter << "R|" << std::setprecision(17) << field.range << '|';
    }
    if (!options.metadata.source.empty())
        text << "$MIB_SOURCE|" << cleanText(options.metadata.source) << '|';
    if (options.contract > 0) text << "$MIB_CONTRACT|" << options.contract << '|';
    if (!options.metadata.exportTime.empty())
        text << "$MIB_EXPORT_TIME|" << cleanText(options.metadata.exportTime) << '|';
    if (!options.metadata.mibVersion.empty())
        text << "$MIB_VERSION|" << cleanText(options.metadata.mibVersion) << '|';
    if (!options.metadata.runId.empty())
        text << "$MIB_RUN_ID|" << cleanText(options.metadata.runId) << '|';
    if (std::isfinite(options.pixelToMicron) && options.pixelToMicron > 0.0)
        text << "$MIB_PIXEL_TO_MICRON|" << std::setprecision(17) << options.pixelToMicron
             << "|$MIB_CALIBRATION|pixel-to-micron=" << std::setprecision(17)
             << options.pixelToMicron << '|';
    return text.str();
}

std::string makeHeader(uint64_t beginText, uint64_t endText, uint64_t beginData, uint64_t endData) {
    auto field = [](uint64_t value) {
        if (value > kHeaderOffsetLimit) return std::string("00000000");
        char buffer[9]{};
        std::snprintf(buffer, sizeof(buffer), "%08llu", static_cast<unsigned long long>(value));
        return std::string(buffer, 8);
    };
    std::string header = "FCS3.1    ";
    const bool dataInRange = beginData <= kHeaderOffsetLimit && endData <= kHeaderOffsetLimit;
    header += field(beginText) + field(endText) + field(dataInRange ? beginData : 0) +
              field(dataInRange ? endData : 0) + field(0) + field(0);
    return header;
}

double valueFor(const FcsField& field, const services::ProcessedFrame& frame, double factor,
                double relativeTime) {
    const auto& v = frame.validation;
    if (field.key == "area") return field.pnn == "Area_px2" ? v.area : v.area * factor * factor;
    if (field.key == "deformability") return v.deformability;
    if (field.key == "areaRatio") return v.areaRatio;
    if (field.key == "ringRatio") return v.ringRatio;
    if (field.key == "laplacianVariance") return v.laplacianVariance;
    if (field.key == "brightness_q1") return v.brightness.q1;
    if (field.key == "brightness_q2") return v.brightness.q2;
    if (field.key == "brightness_q3") return v.brightness.q3;
    if (field.key == "brightness_q4") return v.brightness.q4;
    if (field.key == "brightness_mean") return v.brightnessMean;
    if (field.key == "brightness_variance") return v.brightnessVariance;
    if (field.key == "pixelCount") return static_cast<double>(v.pixelCount);
    if (field.key == "blemishCount") return static_cast<double>(v.blemishCount);
    if (field.key == "__time") return relativeTime;
    return 0.0;
}

} // namespace

FcsLayout planFcsLayout(uint64_t textLength, uint64_t dataBytes)
{
    FcsLayout layout;
    layout.beginText = kHeaderSize;
    if (textLength > std::numeric_limits<uint64_t>::max() - kHeaderSize) {
        layout.overflow = true;
        layout.endText = std::numeric_limits<uint64_t>::max();
        layout.beginData = std::numeric_limits<uint64_t>::max();
        layout.endData = layout.beginData;
        return layout;
    }
    layout.endText = textLength == 0 ? kHeaderSize - 1 : kHeaderSize + textLength - 1;
    layout.beginData = kHeaderSize + textLength;
    if (dataBytes > 0 && layout.beginData > std::numeric_limits<uint64_t>::max() - (dataBytes - 1)) {
        layout.overflow = true;
        layout.endData = std::numeric_limits<uint64_t>::max();
    } else {
        layout.endData = dataBytes == 0 ? layout.beginData - 1 : layout.beginData + dataBytes - 1;
    }
    return layout;
}

std::string formatFcsHeader(const FcsLayout& layout)
{
    return makeHeader(layout.beginText, layout.endText, layout.beginData, layout.endData);
}

std::vector<FcsField> fcsFieldRegistry(int contract,
                                       const std::vector<std::string>& storedMembers) {
    if (contract < 1 || contract > 3) return {};
    std::unordered_set<std::string> members(storedMembers.begin(), storedMembers.end());
    std::vector<FcsField> fields;
    if (hasMember(members, "area")) add(fields, "area", "Area_um2", "Area (um2)", "um^2");
    if (hasMember(members, "area")) add(fields, "area", "Area_px2", "Area (px2)", "px^2");
    if (hasMember(members, "deformability"))
        add(fields, "deformability", "Deformability", "Deformability", "");
    if (hasMember(members, "areaRatio")) add(fields, "areaRatio", "AreaRatio", "Area Ratio", "");
    if (contract == 1 && hasMember(members, "ringRatio"))
        add(fields, "ringRatio", "RingRatio", "Ring Ratio", "");
    if ((contract == 2 || contract == 3) && hasMember(members, "laplacianVariance"))
        add(fields, "laplacianVariance", "LaplacianVar", "Laplacian Variance", "gray^2");
    if ((contract == 1 || contract == 2) && hasMember(members, "brightness_q1"))
        add(fields, "brightness_q1", "BrightQ1", "Bright Q1", "gray");
    if ((contract == 1 || contract == 2) && hasMember(members, "brightness_q2"))
        add(fields, "brightness_q2", "BrightQ2", "Bright Q2", "gray");
    if ((contract == 1 || contract == 2) && hasMember(members, "brightness_q3"))
        add(fields, "brightness_q3", "BrightQ3", "Bright Q3", "gray");
    if ((contract == 1 || contract == 2) && hasMember(members, "brightness_q4"))
        add(fields, "brightness_q4", "BrightQ4", "Bright Q4", "gray");
    if (contract == 3 && hasMember(members, "brightness_mean"))
        add(fields, "brightness_mean", "BrightMean", "Bright Mean", "gray");
    if (contract == 3 && hasMember(members, "brightness_variance"))
        add(fields, "brightness_variance", "BrightVar", "Bright Variance", "gray^2");
    if (contract == 3 && hasMember(members, "pixelCount"))
        add(fields, "pixelCount", "Pixels", "Pixels", "count");
    if (contract == 3 && hasMember(members, "blemishCount"))
        add(fields, "blemishCount", "Blemishes", "Blemishes", "count");
    if (hasMember(members, "timestampNs")) add(fields, "__time", "Time", "Time", "s");
    return fields;
}

FcsWriteResult writeFcs(const std::string& fcsPath, const std::string& eventMapPath,
                        const std::vector<services::ProcessedFrame>& inputFrames,
                        const std::vector<std::string>& storedMembers,
                        const FcsWriteOptions& options) {
    FcsWriteResult result;
    result.fields = fcsFieldRegistry(options.contract, storedMembers);
    // TODO(#520): per-cell event mode requires a verified cross-frame track
    // identity. Keep one event per detection and preserve repeated object IDs
    // until that tracking contract is available.
    if (options.eventMode != FcsEventMode::Detection) {
        result.error = "only per-detection FCS event mode is supported";
        return result;
    }
    if (result.fields.empty()) {
        result.error = "no supported stored metrics for FCS export";
        return result;
    }
    const bool hasAreaUm2 =
        std::any_of(result.fields.begin(), result.fields.end(),
                    [](const FcsField& field) { return field.pnn == "Area_um2"; });
    if (hasAreaUm2 && (!std::isfinite(options.pixelToMicron) || options.pixelToMicron <= 0.0)) {
        result.error = "pixel-to-micron calibration must be finite and positive for Area_um2";
        return result;
    }
    std::vector<std::size_t> order(inputFrames.size());
    for (std::size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&inputFrames](std::size_t a, std::size_t b) {
        return inputFrames[a].timestampNs < inputFrames[b].timestampNs;
    });
    const uint64_t firstNs = order.empty() ? 0 : inputFrames[order.front()].timestampNs;

    if (result.fields.size() > std::numeric_limits<uint64_t>::max() / sizeof(float) ||
        order.size() > std::numeric_limits<uint64_t>::max() / result.fields.size() ||
        order.size() * result.fields.size() > std::numeric_limits<uint64_t>::max() / sizeof(float)) {
        result.error = "FCS event data size overflows the file layout";
        return result;
    }

    // A first pass computes PnR without retaining all events. The second pass
    // writes each event directly after TEXT, avoiding a second event-value buffer.
    std::vector<double> maxima(result.fields.size(), 1.0);
    for (std::size_t sequence = 0; sequence < order.size(); ++sequence) {
        if (options.cancel && options.cancel->load(std::memory_order_acquire)) {
            result.cancelled = true;
            result.error = "export cancelled";
            return result;
        }
        const auto& frame = inputFrames[order[sequence]];
        const double time = static_cast<double>(frame.timestampNs - firstNs) / 1.0e9;
        for (std::size_t fieldIndex = 0; fieldIndex < result.fields.size(); ++fieldIndex) {
            const auto& field = result.fields[fieldIndex];
            const double value = valueFor(field, frame, options.pixelToMicron, time);
            if (!std::isfinite(value)) {
                result.error = "non-finite value in FCS field " + field.key + " at event " +
                               std::to_string(sequence);
                return result;
            }
            const float converted = static_cast<float>(value);
            if (!std::isfinite(converted)) {
                result.error = "value cannot be represented as FCS float in field " + field.key;
                return result;
            }
            maxima[fieldIndex] = std::max(maxima[fieldIndex], std::abs(value));
        }
    }

    for (std::size_t fieldIndex = 0; fieldIndex < result.fields.size(); ++fieldIndex) {
        const double maximum = maxima[fieldIndex];
        result.fields[fieldIndex].range = maximum >= std::numeric_limits<float>::max() - 1.0
                                              ? std::numeric_limits<float>::max()
                                              : std::max(1.0, std::ceil(maximum) + 1.0);
    }

    const uint64_t dataBytes = static_cast<uint64_t>(order.size() * result.fields.size()) * sizeof(float);
    std::string text;
    FcsLayout layout;
    bool stable = false;
    for (unsigned iteration = 0; iteration < 128; ++iteration) {
        if (text.size() > std::numeric_limits<uint64_t>::max() - kHeaderSize) {
            result.error = "FCS TEXT offset overflows the file layout";
            return result;
        }
        layout = planFcsLayout(text.size(), dataBytes);
        if (layout.overflow) {
            result.error = "FCS DATA offset overflows the file layout";
            return result;
        }
        if (layout.endText > kHeaderOffsetLimit) {
            result.error = "FCS TEXT segment exceeds the 8-digit header offset limit";
            return result;
        }
        std::string next = makeText(result.fields, order.size(), layout.beginData, layout.endData, options);
        if (next == text) {
            stable = true;
            break;
        }
        text = std::move(next);
    }
    if (!stable) {
        result.error = "FCS TEXT layout did not converge";
        return result;
    }
    layout = planFcsLayout(text.size(), dataBytes);

    std::ofstream out(fcsPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        result.error = "failed to open FCS output: " + fcsPath;
        return result;
    }
    out.imbue(std::locale::classic());
    out << formatFcsHeader(layout) << text;
    for (std::size_t sequence = 0; sequence < order.size(); ++sequence) {
        if (options.cancel && options.cancel->load(std::memory_order_acquire)) {
            result.cancelled = true;
            result.error = "export cancelled";
            return result;
        }
        const auto& frame = inputFrames[order[sequence]];
        const double time = static_cast<double>(frame.timestampNs - firstNs) / 1.0e9;
        for (const auto& field : result.fields) {
            const double value = valueFor(field, frame, options.pixelToMicron, time);
            if (!std::isfinite(value)) {
                result.error = "non-finite value in FCS field " + field.key + " at event " +
                               std::to_string(sequence);
                return result;
            }
            const float converted = static_cast<float>(value);
            if (!std::isfinite(converted)) {
                result.error = "value cannot be represented as FCS float in field " + field.key;
                return result;
            }
            uint32_t bits = 0;
            std::memcpy(&bits, &converted, sizeof(bits));
            const char bytes[4] = {
                static_cast<char>(bits & 0xffU), static_cast<char>((bits >> 8) & 0xffU),
                static_cast<char>((bits >> 16) & 0xffU), static_cast<char>((bits >> 24) & 0xffU)};
            out.write(bytes, sizeof(bytes));
        }
    }
    out.flush();
    if (!out) {
        result.error = "failed while writing FCS output: " + fcsPath;
        return result;
    }

    std::ofstream map(eventMapPath, std::ios::binary | std::ios::trunc);
    if (!map) {
        result.error = "failed to open FCS event map: " + eventMapPath;
        return result;
    }
    map.imbue(std::locale::classic());
    map << "fcs_event_index,source_frame_index,object_id,timestamp_ns,event_mode\n";
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (options.cancel && options.cancel->load(std::memory_order_acquire)) {
            result.cancelled = true;
            result.error = "export cancelled";
            return result;
        }
        const auto& frame = inputFrames[order[i]];
        map << i << ',' << frame.index << ',' << frame.validation.objectId << ','
            << frame.timestampNs << ",detection\n";
    }
    map.flush();
    if (!map) {
        result.error = "failed while writing FCS event map: " + eventMapPath;
        return result;
    }
    result.events = order.size();
    return result;
}

} // namespace backend::recording
