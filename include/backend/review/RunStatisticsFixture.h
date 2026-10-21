// The run quick look's parity fixture (#716 A7a, ruling 9): small HDF5 run
// files whose numbers are known by hand, shared by the C++ tests, the Rust
// parity test (`run_quick_look` == `review_run_statistics`, through the
// review bridge's `review_fixture_write_quick_look`) and track B. Test
// support compiled into mib_review_core so every consumer writes the same
// bytes; nothing in the product calls it.
#pragma once

#include <cstdint>
#include <string>

namespace backend::review
{

    enum class RunStatisticsFixture : std::uint32_t
    {
        // Snapshot v3 with a loaded E-LUT, factor 0.25 µm/px, a declared
        // target group, a declared clock (1 MHz), accounting with declared
        // drops (intentionally partial). Valid set: 25 rows, 21 isValid; of
        // those one has a NaN area and one a NaN deformability (19 finite);
        // E finite > 0 on 14, NaN (out of domain) on 4, 0 on 3; 8 rows in the
        // target group. Odd valid n (21); the area and deformability medians
        // are over 20 values each (even), E over 14.
        V3WithE = 0,
        // No snapshot, no accounting, no config document, no clock: legacy.
        Legacy = 1,
        // Snapshot v3 whose E-LUT record says none was loaded (E stays 0).
        LutNotLoaded = 2,
        // As V3WithE with one more isValid row (even valid n, 22; the medians
        // over 21 values and E over 15, odd), another LUT sha256, another
        // profile and run index: the reference for the deltas.
        V3WithEEven = 3,
        // Snapshot v3, only invalid frames: an empty valid set.
        EmptyValidSet = 4,
        // A raw frame recording (/recorded_frames): not available.
        RawRecording = 5,
    };

    // The run_id the v3 variants record (nothing for Legacy and RawRecording).
    std::string runStatisticsFixtureRunId(RunStatisticsFixture variant);

    // Writes `variant` to `path` (replacing it). False with `error` on failure.
    bool writeRunStatisticsFixture(const std::string &path, RunStatisticsFixture variant, std::string *error = nullptr);

} // namespace backend::review
