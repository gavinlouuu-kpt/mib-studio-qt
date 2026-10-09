#include "backend/processing/pz/ExecutionProviderFactory.h"

#include "backend/processing/pz/PzExecutionProviders.h"

#include <cerrno>
#include <cstdlib>

#include <spdlog/spdlog.h>

// mib_processing defines MIB_PL_SCIENCE (0/1); a file compiled outside CMake
// (e.g. a tool build) gets the desktop default.
#ifndef MIB_PL_SCIENCE
#define MIB_PL_SCIENCE 0
#endif
#include <fstream>
#include <iterator>

namespace backend::processing::pz {

std::unique_ptr<IExecutionProvider> makeExecutionProvider(const std::string& spec, std::string* error) {
    if (spec.empty() || spec == "host" || spec == "none") return nullptr;
    if (spec == "pz") {
#if defined(__linux__)
        PzDevMemExecutionProvider::Layout layout;
        // The every-frame ring (results13): frames to keep; 0 or unset = none. The standing unit sets it when the bundle's
        // kernel leaves room for it (mem=).
        // Digits only, 1 to 1,000,000 frames; anything else is logged and means no ring (never a silently wrong size).
        if (const char* frames = std::getenv("MIB_PZ_RING_FRAMES"); frames && *frames) {
            char* endp = nullptr;
            errno = 0;
            const unsigned long value = std::strtoul(frames, &endp, 10);
            if (errno != 0 || endp == frames || *endp != '\0' || value == 0 || value > 1000000ul)
                spdlog::error("MIB_PZ_RING_FRAMES='{}' is not a frame count from 1 to 1000000: no frame ring", frames);
            else
                layout.ringFrames = static_cast<uint32_t>(value);
        }
        return std::make_unique<PzDevMemExecutionProvider>(layout);
#else
        if (error) *error = "MIB_EXECUTION_PROVIDER=pz needs Linux on the PZ7035 PS";
        return nullptr;
#endif
    }
    if (spec.rfind("replay:", 0) == 0) {
        std::string path = spec.substr(7);
        double fps = 0.0;
        if (const auto at = path.rfind('@'); at != std::string::npos) {
            fps = std::atof(path.c_str() + at + 1);
            path.resize(at);
        }
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            if (error) *error = "MIB_EXECUTION_PROVIDER replay file not readable: " + path;
            return nullptr;
        }
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return std::make_unique<ReplayExecutionProvider>(std::move(bytes), fps);
    }
    if (error) *error = "unknown MIB_EXECUTION_PROVIDER '" + spec + "' (pz | replay:<file>[@fps] | none)";
    return nullptr;
}

std::string executionProviderSpec(const char* env, bool plScienceBuild) {
    if (env) return env;
    return plScienceBuild ? "pz" : "";
}

std::unique_ptr<IExecutionProvider> makeExecutionProviderFromEnv(std::string* error) {
    return makeExecutionProvider(executionProviderSpec(std::getenv("MIB_EXECUTION_PROVIDER"), MIB_PL_SCIENCE != 0),
                                 error);
}

} // namespace backend::processing::pz
