#pragma once
#include <string>
namespace backend::app {
// Bounded unauthenticated public catalog transport; no backend state/locks.
std::string fetchProfileUrl(const std::string& url);
} // namespace backend::app
