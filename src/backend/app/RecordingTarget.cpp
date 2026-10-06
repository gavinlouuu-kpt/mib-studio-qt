#include "backend/app/RecordingTarget.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

#if defined(__linux__)
#include <sys/vfs.h>
#endif

namespace backend::app {

namespace {
// linux/magic.h
constexpr long kTmpfsMagic = 0x01021994;
constexpr long kRamfsMagic = static_cast<long>(0x858458f6);
constexpr long kExt4Magic = 0xEF53;
} // namespace

RecordingTarget recordingTarget(const std::string& pathOrDir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p = fs::u8path(pathOrDir.empty() ? "." : pathOrDir);
    // An output file that does not exist yet: look at the directory it goes into, or the
    // nearest existing ancestor (the HDF5 writer creates parents).
    if (!fs::is_directory(p, ec)) p = p.has_parent_path() ? p.parent_path() : fs::path(".");
    while (!p.empty() && !fs::exists(p, ec) && p.has_parent_path() && p != p.parent_path()) p = p.parent_path();

    RecordingTarget t;
    t.path = p.string();
    t.exists = fs::is_directory(p, ec);
    if (!t.exists) return t;
    const auto probe = p / ".mib_recording_target_probe";
    {
        std::ofstream f(probe, std::ios::binary);
        t.writable = f.good();
    }
    fs::remove(probe, ec);
    const auto space = fs::space(p, ec);
    if (!ec) t.freeBytes = space.available;
#if defined(__linux__)
    struct statfs st {};
    if (statfs(p.c_str(), &st) == 0) {
        const long type = static_cast<long>(st.f_type);
        t.ram = type == kTmpfsMagic || type == kRamfsMagic;
        t.filesystem = type == kTmpfsMagic ? "tmpfs" : type == kRamfsMagic ? "ramfs" : type == kExt4Magic ? "ext4" : "";
    }
#endif
    return t;
}

std::string recordingTargetWarning(const RecordingTarget& t) {
    if (!t.ram) return {};
    char free[32];
    std::snprintf(free, sizeof(free), "%.1f", t.freeBytes / 1e9);
    return std::string("Recording to RAM: lost on power-off. Copy data off before shutdown. ") + free + " GB free.";
}

} // namespace backend::app
