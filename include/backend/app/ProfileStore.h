#pragma once
#include <string>
namespace backend {
class AppBackend;
}
namespace backend::app {
// Portable local profile files; Qt-compatible config.json/egrabberConfig.js layout.
// Caller serializes mutations. Revision covers all three managed files.
std::string profileStoreCommand(AppBackend& backend, const std::string& base,
                                const std::string& request);
} // namespace backend::app
