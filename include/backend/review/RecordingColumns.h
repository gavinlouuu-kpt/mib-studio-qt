#pragma once

#include "backend/analysis/ColumnarEvaluation.h"

#include <string>
#include <vector>

namespace backend::services {
class Hdf5Service;
}

namespace backend::review {
// The stored object measurements of an open MIB experiment file as chunked
// analysis columns (F-1): /valid_frames/metadata then /invalid_frames/metadata,
// read member by member in bounded chunks through
// Hdf5Service::readMetadataColumns (never whole rows, never every row at once).
//
// Columns: area (px²), deformability (dimensionless), youngsModulus (kPa).
// A member the file's compound type lacks is missing (NaN) for that set, never
// the reader's default 0; nonfinite values are missing; youngsModulus <= 0 is
// missing (no modulus computed). The caller holds the review session's lock
// for every scan (HDF5 is not thread-safe, #722) and states the row counts it
// opened; a scan that sees another count throws (the file changed).
class RecordingColumns final : public analysis::EventColumns {
public:
    RecordingColumns(const services::Hdf5Service& reader, std::uint64_t validRows,
                     std::uint64_t invalidRows, std::vector<std::string> validFields,
                     std::vector<std::string> invalidFields);
    const std::vector<analysis::EventSegment>& segments() const override { return segments_; }
    const std::map<std::string, std::string>& parameters() const override { return parameters_; }
    bool scan(const std::vector<std::string>& columns, std::size_t chunkEvents,
              const ChunkSink& sink) const override;
    // Evidence of the bounded read: HDF5 chunk reads and the largest one.
    std::uint64_t readsIssued() const { return reads_; }
    std::uint64_t largestRead() const { return largest_; }

private:
    const services::Hdf5Service& reader_;
    std::vector<analysis::EventSegment> segments_;
    std::map<std::string, std::string> parameters_;
    std::vector<std::string> fields_[2];
    mutable std::uint64_t reads_{0};
    mutable std::uint64_t largest_{0};
};
} // namespace backend::review
