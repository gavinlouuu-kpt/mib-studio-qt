#include "backend/recording/HdfExportService.h"

#include <iostream>
#include <atomic>
#include <csignal>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>

namespace {
std::atomic<bool> g_signalCancelled{false};
static_assert(std::atomic<bool>::is_always_lock_free,
              "SIGINT cancellation requires a lock-free atomic<bool>");
void handleSignal(int) {
    g_signalCancelled.store(true, std::memory_order_release);
}

void usage() {
    std::cerr << "usage: hdf_export_cli --input FILE --output DIR --format fcs "
                 "[--fcs-event-mode detection] [--frame-type valid|invalid|both] "
                 "[--pixel-to-micron VALUE]\n";
}
} // namespace

int main(int argc, char** argv) {
    std::string input, output, format, eventMode = "detection", frameType = "valid";
    double pixelToMicron = 0.4886;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (i + 1 >= argc) {
            usage();
            return 2;
        }
        if (arg == "--input" || arg == "-i")
            input = argv[++i];
        else if (arg == "--output" || arg == "-o")
            output = argv[++i];
        else if (arg == "--format" || arg == "-f")
            format = argv[++i];
        else if (arg == "--fcs-event-mode")
            eventMode = argv[++i];
        else if (arg == "--frame-type")
            frameType = argv[++i];
        else if (arg == "--pixel-to-micron" || arg == "-p") {
            const char* value = argv[++i];
            char* end = nullptr;
            errno = 0;
            pixelToMicron = std::strtod(value, &end);
            if (errno == ERANGE || end == value || *end != '\0' ||
                !std::isfinite(pixelToMicron) || pixelToMicron <= 0.0) {
                std::cerr << "ERROR: --pixel-to-micron must be a finite positive number\n";
                return 2;
            }
        }
        else {
            usage();
            return 2;
        }
    }
    if (input.empty() || output.empty() || format != "fcs" || eventMode != "detection") {
        usage();
        if (format != "fcs")
            std::cerr << "ERROR: native CLI currently supports --format fcs only\n";
        if (eventMode != "detection")
            std::cerr << "ERROR: only --fcs-event-mode detection is supported\n";
        return 2;
    }
    backend::recording::HdfExportRequest request;
    request.sourcePath = input;
    request.outputRoot = output;
    request.format = backend::recording::HdfExportFormat::Fcs;
    request.conversionFactor = pixelToMicron;
    if (frameType == "valid")
        request.fcsFrames = backend::recording::HdfExportFrames::Valid;
    else if (frameType == "invalid")
        request.fcsFrames = backend::recording::HdfExportFrames::Invalid;
    else if (frameType == "both")
        request.fcsFrames = backend::recording::HdfExportFrames::Both;
    else {
        std::cerr << "ERROR: --frame-type must be valid, invalid, or both\n";
        return 2;
    }
    backend::recording::HdfExportService service;
    g_signalCancelled.store(false, std::memory_order_release);
    auto signalFlag = std::shared_ptr<std::atomic<bool>>(
        &g_signalCancelled, [](std::atomic<bool>*) {});
    backend::recording::HdfExportCancelToken token(std::move(signalFlag));
    std::signal(SIGINT, handleSignal);
    const auto result = service.run(request, token);
    std::signal(SIGINT, SIG_DFL);
    if (!result.completed()) {
        std::cerr << "ERROR: " << result.error << '\n';
        return 1;
    }
    std::cout << result.finalPath << '\n';
    return 0;
}
