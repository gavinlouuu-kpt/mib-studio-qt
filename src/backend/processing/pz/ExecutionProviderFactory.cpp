#include "backend/processing/pz/ExecutionProviderFactory.h"

#include "backend/processing/pz/PzExecutionProviders.h"

#include <cstdlib>
#include <fstream>
#include <iterator>

namespace backend::processing::pz {

std::unique_ptr<IExecutionProvider> makeExecutionProvider(const std::string& spec, std::string* error) {
    if (spec.empty() || spec == "host") return nullptr;
    if (spec == "pz") {
#if defined(__linux__)
        return std::make_unique<PzDevMemExecutionProvider>(PzDevMemExecutionProvider::Layout{});
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
    if (error) *error = "unknown MIB_EXECUTION_PROVIDER '" + spec + "' (pz | replay:<file>[@fps] | host)";
    return nullptr;
}

std::unique_ptr<IExecutionProvider> makeExecutionProviderFromEnv(std::string* error) {
    const char* spec = std::getenv("MIB_EXECUTION_PROVIDER");
    return makeExecutionProvider(spec ? spec : "", error);
}

} // namespace backend::processing::pz
