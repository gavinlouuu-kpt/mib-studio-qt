// run_quick_look_memory_test (#716 A7a): the run quick look's cost on a large synthetic run.
//
// Writes a run of N valid-set rows (default 100000; MIB_QUICK_LOOK_ROWS overrides, e.g. 1000000)
// and measures, in this process:
//  - the column path (readRunStatistics: what the coordinator's pass at close and run_quick_look
//    run): wall time and peak resident memory above the baseline;
//  - for comparison, ReviewSession's open (the row path: every metadata row as a ProcessedFrame).
// Gate (a ratio, never an absolute time): the column path's peak memory stays well below the row
// path's (it must not load whole rows; the PZ7035 has little RAM), and both give the same numbers.
// Linux only for the memory figures (/proc/self/status VmHWM, reset through clear_refs); elsewhere
// it only checks the numbers. The figures are printed for the PR. The heap is trimmed before each
// measurement (glibc malloc_trim) so the peak is the call's own.

#include "backend/review/ReviewSession.h"
#include "backend/review/RunStatistics.h"
#include "backend/recording/Hdf5Service.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <opencv2/core.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

namespace fs = std::filesystem;
using namespace backend::review;

namespace
{
    // kB from /proc/self/status, -1 when unavailable.
    long statusKb(const char *key)
    {
        std::ifstream in("/proc/self/status");
        std::string line;
        const std::string prefix = std::string(key) + ":";
        while (std::getline(in, line))
            if (line.rfind(prefix, 0) == 0) return std::strtol(line.c_str() + prefix.size(), nullptr, 10);
        return -1;
    }

    // Resets the peak (VmHWM) to the current RSS; false when the kernel does not allow it.
    bool resetPeak()
    {
        std::ofstream out("/proc/self/clear_refs");
        if (!out) return false;
        out << "5";
        out.flush();
        return static_cast<bool>(out);
    }

    struct Cost
    {
        double ms{0};
        long peakAboveBaselineKb{-1};
    };

    template <typename F>
    Cost measure(F &&f)
    {
#if defined(__GLIBC__)
        // Hand freed heap back first, so the peak above the baseline is this call's own memory
        // and not pages an earlier phase (writing the file) left with the allocator.
        malloc_trim(0);
#endif
        const bool peak = resetPeak();
        const long baseline = statusKb("VmRSS");
        const auto t0 = std::chrono::steady_clock::now();
        f();
        Cost c;
        c.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (peak && baseline >= 0) c.peakAboveBaselineKb = statusKb("VmHWM") - baseline;
        return c;
    }
} // namespace

int main()
{
    mib::test::Watchdog wd(600);
    std::size_t rows = 100000;
    if (const char *env = std::getenv("MIB_QUICK_LOOK_ROWS")) rows = std::strtoull(env, nullptr, 10);
    MIB_REQUIRE(rows >= 1000, "at least 1000 rows");
    mib::test::TempDir td("run_quick_look_memory");
    const fs::path file = td.path() / "big.h5";
    {
        wd.mark("write");
        backend::services::Hdf5Service w;
        MIB_REQUIRE(w.openFile(file.string()) && w.initializeDatasets(), "open");
        std::vector<backend::services::ProcessedFrame> batch;
        const cv::Mat tiny(2, 2, CV_8UC1, cv::Scalar(0));
        for (std::size_t i = 0; i < rows; ++i)
        {
            backend::services::ProcessedFrame f;
            f.index = i;
            f.timestampNs = i * 200;
            f.originalImage = tiny;
            f.processedImage = tiny;
            auto &v = f.validation;
            v.isValid = i % 9 != 0;
            v.area = 100.0 + std::fmod(static_cast<double>(i) * 7.31, 97.0);
            v.deformability = std::fmod(static_cast<double>(i) * 0.0137, 0.4);
            v.youngsModulus = i % 13 == 0 ? std::nan("") : 0.5 + std::fmod(static_cast<double>(i) * 0.071, 5.0);
            v.isTargetGroup = i % 5 == 0;
            batch.push_back(std::move(f));
            if (batch.size() == 20000 || i + 1 == rows)
            {
                MIB_REQUIRE(w.appendFrames(batch, {}), "append");
                batch.clear();
                wd.mark("write");
            }
        }
        MIB_REQUIRE(w.writeRunSnapshotJson(R"({"schema_version":3,"pixel_to_micron":0.25,"emodulus_lut":{"loaded":true,"sha256":"aa"}})", "{}"),
                    "snapshot");
        w.closeFile();
    }

    wd.mark("column path");
    RunStatisticsResult column;
    const auto columnCost = measure([&] { column = readRunStatistics(file.string(), "close"); });
    MIB_REQUIRE(column.status == RunStatisticsStatus::Computed, column.message);

    wd.mark("row path");
    std::string rowJson;
    const auto rowCost = measure([&] {
        ReviewSession s;
        MIB_REQUIRE(s.open(file.string()), "open");
        backend::services::Hdf5Service reader;
        MIB_REQUIRE(reader.loadFile(file.string()), "header");
        const auto header = readRunStatisticsHeader(reader);
        reader.closeFile();
        rowJson = runStatisticsJson(computeRunStatistics(runStatisticsRowsFromFrames(s.validSet().frames), header));
    });
    MIB_EXPECT(runStatisticsJson(*column.stats) == rowJson, "the column path and the row path agree");

    std::printf("run_quick_look_memory: rows=%zu file_bytes=%llu valid=%llu\n", rows,
                static_cast<unsigned long long>(fs::file_size(file)),
                static_cast<unsigned long long>(column.stats->validRows));
    std::printf("run_quick_look_memory: column path (close pass / run_quick_look): %.1f ms, peak +%ld kB\n",
                columnCost.ms, columnCost.peakAboveBaselineKb);
    std::printf("run_quick_look_memory: row path (ReviewSession open, comparison): %.1f ms, peak +%ld kB\n", rowCost.ms,
                rowCost.peakAboveBaselineKb);
    std::fflush(stdout);
    if (columnCost.peakAboveBaselineKb >= 0 && rowCost.peakAboveBaselineKb > 0)
        MIB_EXPECT(columnCost.peakAboveBaselineKb * 4 < rowCost.peakAboveBaselineKb,
                   "the column path holds well under a quarter of the row path's memory");
    return mib::test::exitCode();
}
