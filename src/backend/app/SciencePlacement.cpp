#include "backend/app/SciencePlacement.h"

#include <cstdlib>
#include <cstring>

#ifndef MIB_PL_SCIENCE
#define MIB_PL_SCIENCE 0
#endif

namespace backend::app {

bool hostProcessingAvailable()
{
    static const bool available = [] {
        if (MIB_PL_SCIENCE) return false;
        const char* env = std::getenv("MIB_PL_SCIENCE");
        return !(env && (std::strcmp(env, "1") == 0 || std::strcmp(env, "true") == 0));
    }();
    return available;
}

const char* sciencePlacement()
{
    return hostProcessingAvailable() ? "host" : "pl";
}

} // namespace backend::app
