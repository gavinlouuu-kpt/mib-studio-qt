#pragma once
#include <string>
namespace backend {
class AppBackend;
}
namespace backend::app {
std::string processingCoreCommand(AppBackend& backend, const std::string& cacheRoot,
                                  const std::string& request);
}
