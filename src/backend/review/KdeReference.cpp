#include "backend/review/KdeReference.h"

#include "backend/processing/KdeCoreRecord.h"
#include "backend/recording/Hdf5Service.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace backend::review {

namespace {

KdeReferenceResult refused(std::string reason) {
    KdeReferenceResult r;
    r.reason = std::move(reason);
    return r;
}

} // namespace

KdeReferenceResult readKdeReference(const std::string& path) {
    try {
        std::error_code ec;
        if (path.empty() || !std::filesystem::exists(std::filesystem::u8path(path), ec) || ec)
            return refused("file not found");
        services::Hdf5Service reader;
        if (!reader.loadFile(path)) {
            reader.closeFile();
            return refused("not a readable HDF5 file");
        }
        // Prefer the authoritative full-run record, fall back to the live one (Qt's order).
        std::string json;
        std::optional<monitoring::KdeCoreRecord> record;
        std::string whyAnalysis, whyLive;
        if (reader.readKdeAnalysisJson(json)) record = monitoring::fromJson(json, &whyAnalysis);
        if (!record && reader.readKdeLiveJson(json)) record = monitoring::fromJson(json, &whyLive);
        reader.closeFile();
        if (!record) {
            if (!whyAnalysis.empty() || !whyLive.empty())
                return refused("stored core contour unreadable (" + (!whyLive.empty() ? whyLive : whyAnalysis) + ")");
            return refused("the experiment has no stored core contour");
        }
        if (record->contours.empty()) return refused("the stored core contour is empty (too few cells)");
        std::size_t vertices = 0;
        for (const auto& loop : record->contours) vertices += loop.size();
        if (vertices > kKdeReferenceMaxVertices)
            return refused("the stored core contour is too large (" + std::to_string(vertices) +
                           " vertices, at most " + std::to_string(kKdeReferenceMaxVertices) + ")");
        KdeReferenceResult r;
        r.ok = true;
        r.source = record->provisional ? "live" : "full-run";
        r.coreFraction = record->coreFraction;
        r.contours = std::move(record->contours);
        return r;
    } catch (const std::exception& e) {
        return refused(std::string("read failed: ") + e.what());
    } catch (...) {
        return refused("read failed");
    }
}

namespace {

std::string fileNameOf(const std::string& path) {
    try {
        return std::filesystem::u8path(path).filename().string();
    } catch (...) {
        return path;
    }
}

std::string errorText(const std::string& path, const std::string& reason) {
    return "No reference contour loaded from " + path + ": " + reason;
}

} // namespace

std::string kdeReferenceRefusedJson(const std::string& requestedPath, const std::string& reason) {
    return nlohmann::json{{"ok", false},
                          {"path", requestedPath},
                          {"file_name", fileNameOf(requestedPath)},
                          {"error", errorText(requestedPath, reason)},
                          {"reason", reason},
                          {"source", nullptr},
                          {"core_fraction", nullptr},
                          {"core_percent", nullptr},
                          {"loops", 0},
                          {"contours", nlohmann::json::array()}}
        .dump();
}

std::string kdeReferenceReplyJson(const std::string& requestedPath, const KdeReferenceResult& result) {
    if (!result.ok) return kdeReferenceRefusedJson(requestedPath, result.reason);
    nlohmann::json loops = nlohmann::json::array();
    for (const auto& loop : result.contours) {
        nlohmann::json pts = nlohmann::json::array();
        for (const auto& p : loop) pts.push_back({p.x, p.y});
        loops.push_back(std::move(pts));
    }
    return nlohmann::json{{"ok", true},
                          {"path", requestedPath},
                          {"file_name", fileNameOf(requestedPath)},
                          {"error", nullptr},
                          {"reason", nullptr},
                          {"source", result.source},
                          {"core_fraction", result.coreFraction},
                          {"core_percent", std::lround(result.coreFraction * 100.0)},
                          {"loops", result.contours.size()},
                          {"contours", std::move(loops)}}
        .dump();
}

} // namespace backend::review
