// Where recordings land and whether they survive power-off (#501). The PZ7035 boots a JTAG RAM
// root today, so a recording under the data directory lives in RAM and is lost at power-off.
// This is the one place that decides it: once a SATA disk is mounted and configured as the
// recording target, `ram` turns false and every warning built on it clears.
#pragma once

#include <cstdint>
#include <string>

namespace backend::app {

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

} // namespace backend::app
