// hdf5_exit_teardown_test
//
// Regression guard for the installed-app crash of 2026-09-07 (WER bucket
// INVALID_POINTER_READ_c0000005_hdf5.dll, stack: ucrtbase exit ->
// LdrShutdownProcess -> hdf5 DLL_PROCESS_DETACH -> H5_term_library ->
// H5D_top_term_package -> H5D_close -> H5FL_blk_free): the process exited
// while an HDF5 file was still open and a writer was mid-append, and the
// library's own atexit teardown then closed a dataset whose chunk cache was
// already gone. The process must exit with code 0 even when it leaves a
// file open after writing — the backend disables HDF5's atexit teardown
// (H5dont_atexit) and closes what it can in shutdown; leaked ids are
// leaked, never torn down by the library at exit.

#include "backend/recording/Hdf5Service.h"

#include <opencv2/core.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {
backend::services::ProcessedFrame makeFrame(uint64_t index)
{
    backend::services::ProcessedFrame f;
    f.index = index;
    f.timestampNs = index * 1000;
    f.originalImage = cv::Mat(96, 128, CV_8UC1, cv::Scalar(static_cast<int>(index % 251)));
    f.processedImage = cv::Mat(96, 128, CV_8UC1, cv::Scalar(255));
    f.validation.isValid = true;
    return f;
}
} // namespace

int main()
{
    const auto dir = std::filesystem::temp_directory_path() /
                     ("hdf5_exit_teardown_" + std::to_string(static_cast<long long>(::time(nullptr))));
    std::filesystem::create_directories(dir);
    const std::string path = (dir / "left_open.h5").string();

    // Deliberately leaked: destructors never run because the process exits
    // from main() below while the writer is still appending.
    auto* hdf5 = new backend::services::Hdf5Service();
    if (!hdf5->openFile(path) || !hdf5->initializeDatasets()) {
        std::fprintf(stderr, "could not open %s\n", path.c_str());
        return 2;
    }
    std::atomic<bool> run{true};
    std::atomic<uint64_t> appended{0};
    std::thread writer([&] {
        uint64_t idx = 0;
        while (run.load(std::memory_order_relaxed)) {
            std::vector<backend::services::ProcessedFrame> valid;
            for (int i = 0; i < 8; ++i) valid.push_back(makeFrame(idx++));
            if (hdf5->appendFrames(valid, {})) appended.fetch_add(valid.size(), std::memory_order_relaxed);
        }
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
    while (appended.load(std::memory_order_relaxed) < 64 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // Stop the writer the way the app's shutdown does (the flush queue joins
    // its thread). What stays behind is the *open file* with its datasets and
    // chunk cache: the state the installed app exited in. A thread still
    // running through exit() would be undefined behaviour on every platform
    // (Linux CI segfaulted in static destruction), not a test of HDF5.
    run.store(false);
    writer.join();
    std::printf("exiting with %llu frames appended and the file still open\n",
                static_cast<unsigned long long>(appended.load()));
    std::fflush(stdout);
    // Exit like a Qt app does after QApplication::exec() returns: normal CRT
    // exit, static destructors and DLL detach included, file never closed.
    std::exit(0);
}
