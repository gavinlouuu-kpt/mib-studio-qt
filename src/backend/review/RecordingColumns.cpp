#include "backend/review/RecordingColumns.h"

#include "backend/recording/Hdf5Service.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <stdexcept>

namespace backend::review {
namespace {
constexpr double kMissing = std::numeric_limits<double>::quiet_NaN();
enum class Column { Area, Deformability, YoungsModulus };
} // namespace

RecordingColumns::RecordingColumns(const services::Hdf5Service& reader, std::uint64_t validRows,
                                   std::uint64_t invalidRows, std::vector<std::string> validFields,
                                   std::vector<std::string> invalidFields)
    : reader_(reader),
      segments_{{"valid", validRows}, {"invalid", invalidRows}},
      parameters_{{"area", "px²"}, {"deformability", "dimensionless"}, {"youngsModulus", "kPa"}} {
    fields_[0] = std::move(validFields);
    fields_[1] = std::move(invalidFields);
}

bool RecordingColumns::scan(const std::vector<std::string>& columns, std::size_t chunkEvents,
                            const ChunkSink& sink) const {
    if (chunkEvents == 0) throw std::invalid_argument("Chunk size must be positive");
    std::vector<Column> kinds;
    for (const auto& id : columns) {
        if (id == "area")
            kinds.push_back(Column::Area);
        else if (id == "deformability")
            kinds.push_back(Column::Deformability);
        else if (id == "youngsModulus")
            kinds.push_back(Column::YoungsModulus);
        else
            throw std::invalid_argument("Column unavailable in recording: " + id);
    }
    std::vector<std::vector<double>> buffers(columns.size());
    std::vector<const double*> pointers(columns.size());
    std::uint64_t base = 0;
    for (int set = 0; set < 2; ++set) {
        const auto expected = segments_[set].count;
        if (expected == 0) continue;
        std::vector<bool> present;
        for (const auto& id : columns)
            present.push_back(std::find(fields_[set].begin(), fields_[set].end(), id) !=
                              fields_[set].end());
        std::uint64_t delivered = 0;
        bool stopped = false;
        // Never throw through the reader: it must close its HDF5 handles.
        std::exception_ptr failure;
        const bool ok = reader_.readMetadataColumns(
            set == 0, chunkEvents,
            [&](const services::Hdf5Service::MetadataColumnRow* rows, std::size_t count) {
              try {
                ++reads_;
                largest_ = std::max<std::uint64_t>(largest_, count);
                if (delivered + count > expected)
                    throw std::invalid_argument("Recording rows changed since open");
                for (std::size_t c = 0; c < kinds.size(); ++c) {
                    auto& out = buffers[c];
                    out.resize(count);
                    for (std::size_t i = 0; i < count; ++i) {
                        double v = kMissing;
                        if (present[c])
                            v = kinds[c] == Column::Area            ? rows[i].area
                                : kinds[c] == Column::Deformability ? rows[i].deformability
                                                                    : rows[i].youngsModulus;
                        if (!std::isfinite(v) || (kinds[c] == Column::YoungsModulus && v <= 0))
                            v = kMissing;
                        out[i] = v;
                    }
                    pointers[c] = out.data();
                }
                if (!sink(base + delivered, count, pointers)) {
                    stopped = true;
                    return false;
                }
                delivered += count;
                return true;
              } catch (...) {
                failure = std::current_exception();
                return false;
              }
            });
        if (failure) std::rethrow_exception(failure);
        if (stopped) return false;
        if (!ok || delivered != expected)
            throw std::invalid_argument(std::string("Stored ") + segments_[set].set +
                                        " metadata could not be read completely in chunks");
        base += expected;
    }
    return true;
}
} // namespace backend::review
