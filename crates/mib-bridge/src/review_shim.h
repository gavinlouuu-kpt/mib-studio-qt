// C++ side of the review bridge (plan 2026-10-01-standalone-review-app,
// ADR 0008). Wraps backend::review::ReviewSession behind an opaque
// ReviewBridge that cxx owns via UniquePtr. Links mib_review_core +
// mib_processing only — no AppBackend, so the review-only product carries no
// camera, serial, SQLite or Sentry code. No Qt, no exceptions cross the
// boundary.
#pragma once

#include "rust/cxx.h"

#include <cstdint>
#include <memory>

namespace mib_review_bridge {

struct ReviewResult;
struct ReviewFrame;
struct ReviewInfo;
struct ReviewRows;
struct ReviewScatter;
struct ReviewEvent;
struct ReviewChartSnapshot;
struct ReviewDensity;

class ReviewBridge {
public:
    ReviewBridge();
    ~ReviewBridge();
    ReviewBridge(const ReviewBridge&) = delete;
    ReviewBridge& operator=(const ReviewBridge&) = delete;

    bool initialize(rust::Str data_dir);
    void shutdown();
    bool is_initialized() const;

    ReviewResult review_open(rust::Str path);
    ReviewResult review_close();
    void set_fallback_pixel_to_micron(double factor);

    ReviewInfo fetch_review_info();
    ReviewRows fetch_review_rows(bool valid, std::uint64_t offset, std::uint64_t count);
    ReviewFrame fetch_review_frame(std::uint32_t dataset, std::uint64_t index, std::uint32_t overlay,
                                   bool roi_overlay);
    std::uint64_t fetch_review_series_count(std::uint64_t index);
    ReviewFrame fetch_review_series_frame(std::uint64_t index, std::uint64_t k, std::uint32_t overlay,
                                          bool roi_overlay);
    ReviewFrame fetch_review_thumbnails(bool valid, std::uint64_t offset, std::uint64_t count,
                                        std::uint32_t size, std::uint32_t overlay, bool roi_overlay);
    ReviewScatter fetch_review_scatter();
    ReviewResult review_save_core_record(rust::Str json, bool overwrite);

    ReviewResult review_export_metrics(rust::Str output_path);
    ReviewResult review_export_all(rust::Str output_root, bool export_series, std::uint64_t series_start,
                                   std::uint64_t series_end, rust::Vec<ReviewChartSnapshot> charts);
    ReviewResult review_export_charts(rust::Str output_dir, rust::Vec<ReviewChartSnapshot> charts);
    ReviewResult review_batch_export(rust::Vec<rust::String> sources, rust::Str output_root, bool metrics_only,
                                     bool export_series, std::uint64_t series_start, std::uint64_t series_end);
    ReviewResult review_regenerate_masks(std::uint32_t source, rust::Str source_path, std::uint64_t start_index,
                                         std::uint64_t count, rust::Str output_path, bool use_recorded_config,
                                         bool synthesize_background);
    ReviewResult review_compute_core(double core_fraction);
    rust::String fetch_review_computed_core_json();
    ReviewResult review_request_density(double bandwidth_factor, double core_fraction, std::uint32_t levels,
                                        bool want_core_record);
    ReviewDensity fetch_review_density();
    bool review_jobs_busy() const;

    rust::Vec<ReviewEvent> poll_review_events();
    ReviewResult cancel_review_operation(std::uint64_t operation_id);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<ReviewBridge> new_review_bridge();
std::uint32_t review_bridge_abi_version();
bool review_fixture_write_experiment(rust::Str path);
bool review_fixture_write_population(rust::Str path, std::uint32_t cells, std::uint64_t seed);

} // namespace mib_review_bridge
