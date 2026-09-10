#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace backend::processing {
// Shared, Qt/OpenCV-free streaming SHA-256 implementation.
std::string processingCoreFileSha256(const std::filesystem::path& path,
                                     std::string* error = nullptr);
std::string processingCoreBytesSha256(const uint8_t* bytes, size_t count);
} // namespace backend::processing
