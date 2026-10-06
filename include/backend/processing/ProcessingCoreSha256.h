#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace backend::processing {
// Shared, Qt/OpenCV-free streaming SHA-256 implementation.
std::string processingCoreFileSha256(const std::filesystem::path& path,
                                     std::string* error = nullptr);
std::string processingCoreBytesSha256(const uint8_t* bytes, size_t count);
// Same, polling `cancelled` between 64 KiB chunks; returns empty with
// `error` = "cancelled" when it fires (large run files hashed off the UI).
std::string fileSha256(const std::filesystem::path& path, std::string* error,
                       const std::function<bool()>& cancelled);
} // namespace backend::processing
