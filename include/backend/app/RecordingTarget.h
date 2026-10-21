// Where recordings land and whether they survive power-off (#501). The PZ7035 boots a JTAG RAM
// root today, so a recording under the data directory lives in RAM and is lost at power-off.
// This is the one place that decides it: once a SATA disk is mounted and configured as the
// recording target, `ram` turns false and every warning built on it clears.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace backend::app {

// Path strings in the backend are UTF-8 (the bridge, JSON, HDF5's Windows file driver; #762). On
// MSVC `path(std::string)` and `path::string()` use the ANSI code page instead, so a non-ASCII path
// would be decoded wrongly or recorded as mojibake: convert at the string boundary with these.
inline std::filesystem::path utf8ToPath(const std::string& text) { return std::filesystem::u8path(text); }
inline std::string pathToUtf8(const std::filesystem::path& path)
{
    const auto text = path.u8string();   // std::string in C++17, std::u8string in C++20
    return std::string(text.begin(), text.end());
}

struct RecordingTarget {
    std::string path;        // the directory that was examined (nearest existing ancestor)
    bool exists{false};
    bool writable{false};
    bool ram{false};         // tmpfs / ramfs / initramfs rootfs
    uint64_t freeBytes{0};
    std::string filesystem;  // "tmpfs", "ramfs", "ext4", ... ("" if unknown)
};

// `pathOrDir` may be a file to be created (an .h5 output) or a directory.
RecordingTarget recordingTarget(const std::string& pathOrDir);

// The operator-facing warning, or "" when the target is persistent:
// "Recording to RAM: lost on power-off. Copy data off before shutdown. <N> GB free."
std::string recordingTargetWarning(const RecordingTarget& target);

// The storage root for a data directory: absolute, symlinks resolved (weakly_canonical), so it can
// be compared component by component. "" when `dataDir` is "" (no root configured). This is what
// fetch_instrument_status reports as storage.path, and what resolveClientPath confines to (#762).
std::string storageRootOf(const std::string& dataDir);

// A path a client gave for a file or folder the backend writes (#762): experiment Start (and its
// readiness probes), raw recording, review exports, Regenerate masks, preview buffer saves.
struct ClientPath {
    bool ok{false};
    std::string path;    // what to write: see resolveClientPath
    std::string reason;  // plain reason when !ok
};

// The one rule for client output paths (#762):
//  - empty: refused ("no output path given");
//  - absolute: unchanged (the desktop's native dialogs give absolute paths; today's behaviour);
//  - relative, no storage root configured (`storageRoot` ""): unchanged, so it is relative to the
//    process's working directory as before (an uninitialized backend; every shell initializes one);
//  - relative: joined to the root and resolved with weakly_canonical, so the result is absolute
//    with `..` and existing symlinks resolved. It must stay inside the weakly_canonical root,
//    compared component by component (a sibling "data2" is not inside "data"): `..` that leaves the
//    root, an existing symlink (as the file or a parent) that resolves outside it, and a dangling
//    symlink (whose target could be created anywhere) are refused with a plain reason. The root
//    itself is accepted (a folder writer may write into it). The resolved path is returned: it is
//    the path that was checked.
// All strings in and out are UTF-8 (utf8ToPath / pathToUtf8): an absolute request comes back
// byte for byte.
// Limit (ruling on #767): a local process that swaps a component for a symlink between this check
// and the open is not stopped. The threat is a path a client sends, not a local user who can already
// write to the storage root, and HDF5 opens files by pathname only, so there is no handle-based walk;
// experiment Start checks once more right before its open.
ClientPath resolveClientPath(const std::string& requested, const std::string& storageRoot);

} // namespace backend::app
