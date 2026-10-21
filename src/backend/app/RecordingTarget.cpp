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
    fs::path p = utf8ToPath(pathOrDir.empty() ? "." : pathOrDir);
    // An output file that does not exist yet: look at the directory it goes into, or the
    // nearest existing ancestor (the HDF5 writer creates parents).
    if (!fs::is_directory(p, ec)) p = p.has_parent_path() ? p.parent_path() : fs::path(".");
    while (!p.empty() && !fs::exists(p, ec) && p.has_parent_path() && p != p.parent_path()) p = p.parent_path();

    RecordingTarget t;
    t.path = pathToUtf8(p);
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

std::string storageRootOf(const std::string& dataDir) {
    namespace fs = std::filesystem;
    if (dataDir.empty()) return {};
    std::error_code ec;
    fs::path p = fs::absolute(utf8ToPath(dataDir), ec);
    if (ec) p = utf8ToPath(dataDir);
    const fs::path canon = fs::weakly_canonical(p, ec);
    return pathToUtf8(ec ? p.lexically_normal() : canon);
}

namespace {
// `inner` lies in `root` (or is it), compared by path components, never as strings. A trailing
// separator (an empty last component) does not count.
bool insideRoot(const std::filesystem::path& root, const std::filesystem::path& inner) {
    auto r = root.begin();
    auto i = inner.begin();
    for (; r != root.end(); ++r) {
        if (r->empty()) continue;
        while (i != inner.end() && i->empty()) ++i;
        if (i == inner.end() || *i != *r) return false;
        ++i;
    }
    return true;
}
} // namespace

ClientPath resolveClientPath(const std::string& requested, const std::string& storageRoot) {
    namespace fs = std::filesystem;
    ClientPath out;
    if (requested.empty()) {
        out.reason = "no output path given";
        return out;
    }
    const fs::path asked = utf8ToPath(requested);
    // Absolute: today's behaviour (the native dialogs). No root: the desktop without a data
    // directory keeps resolving against the working directory, as before (#762).
    if (asked.is_absolute() || storageRoot.empty()) {
        out.ok = true;
        out.path = requested;
        return out;
    }
    std::error_code ec;
    const fs::path root = utf8ToPath(storageRootOf(storageRoot));
    const fs::path joined = root / asked;
    // A dangling symlink anywhere on the way: weakly_canonical treats it as not existing (and
    // keeps its name), but creating the file would follow it wherever it points.
    {
        fs::path prefix;
        for (const auto& part : joined) {
            prefix /= part;
            std::error_code se;
            const auto st = fs::symlink_status(prefix, se);
            if (!se && fs::is_symlink(st) && !fs::exists(prefix, se)) {
                out.reason = "the output path " + requested + " goes through a symlink that points nowhere (" +
                             pathToUtf8(prefix) + "); choose a path inside the data directory " + pathToUtf8(root);
                return out;
            }
        }
    }
    const fs::path resolved = fs::weakly_canonical(joined, ec);
    if (ec) {
        out.reason = "the output path " + requested + " cannot be resolved: " + ec.message();
        return out;
    }
    if (!insideRoot(root, resolved)) {
        out.reason = "the output path " + requested + " leaves the data directory " + pathToUtf8(root) +
                     " (it resolves to " + pathToUtf8(resolved) + "); choose a path inside it";
        return out;
    }
    out.ok = true;
    out.path = pathToUtf8(resolved);
    return out;
}

} // namespace backend::app
