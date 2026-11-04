#include "backend/app/DestinationWriteRate.h"

#include "backend/app/RecordingTarget.h"
#include "backend/app/Tools.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace backend::app {
namespace {

std::filesystem::path nearestExistingDirectory(const std::string& outputPath)
{
    std::error_code ec;
    auto dir = utf8ToPath(outputPath).parent_path();
    if (dir.empty()) dir = std::filesystem::current_path(ec);
    while (!dir.empty() && !std::filesystem::exists(dir, ec)) {
        const auto parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return dir;
}

// A file opened for raw writes with a flush-to-device call.
class RawFile {
public:
    explicit RawFile(const std::filesystem::path& path)
    {
#ifdef _WIN32
        fd_ = _wopen(path.c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
        fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
#endif
    }
    ~RawFile()
    {
        if (fd_ >= 0) {
#ifdef _WIN32
            _close(fd_);
#else
            ::close(fd_);
#endif
        }
    }
    RawFile(const RawFile&) = delete;
    RawFile& operator=(const RawFile&) = delete;
    bool ok() const { return fd_ >= 0; }
    bool write(const uint8_t* data, size_t size)
    {
        while (size > 0) {
#ifdef _WIN32
            const unsigned part = static_cast<unsigned>(std::min<size_t>(size, 1u << 30));
            const int n = _write(fd_, data, part);
#else
            const auto n = ::write(fd_, data, size);
#endif
            if (n <= 0) return false;
            data += n;
            size -= static_cast<size_t>(n);
        }
        return true;
    }
    bool flushToDevice()
    {
#ifdef _WIN32
        return _commit(fd_) == 0;
#else
        return ::fsync(fd_) == 0;
#endif
    }

private:
    int fd_{-1};
};

} // namespace

std::string writeRateDirectory(const std::string& outputPath)
{
    return pathToUtf8(nearestExistingDirectory(outputPath));
}

WriteRateMeasurement measureDestinationWriteRate(const std::string& outputPath, uint64_t bytesPerFrame)
{
    using W = DestinationWriteRate;
    WriteRateMeasurement m;
    const auto dir = nearestExistingDirectory(outputPath);
    m.directory = pathToUtf8(dir);
    if (dir.empty()) {
        m.note = "no existing directory for " + outputPath;
        return m;
    }
    const auto path = dir / (".mib_write_rate_probe_" + std::to_string(Tools::getTimestamp()) + ".bin");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup()
        {
            std::error_code e;
            std::filesystem::remove(path, e);
        }
    } cleanup{path};
    try {
        const uint64_t frame = std::max<uint64_t>(1, bytesPerFrame);
        const uint64_t framesPerChunk = std::max<uint64_t>(1, W::kChunkBytes / frame);
        const uint64_t chunkBytes = std::min<uint64_t>(frame * framesPerChunk, W::kMaxBytes);
        // A varied pattern, so a compressing or deduplicating filesystem
        // cannot shrink the write.
        std::vector<uint8_t> chunk(static_cast<size_t>(chunkBytes));
        uint32_t x = 2463534242u;
        for (auto& b : chunk) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            b = static_cast<uint8_t>(x);
        }
        RawFile file(path);
        if (!file.ok()) {
            m.note = "cannot create a probe file in " + m.directory;
            return m;
        }
        const auto start = std::chrono::steady_clock::now();
        double elapsed = 0.0;
        while (m.bytes < W::kMaxBytes && elapsed < W::kMaxSeconds) {
            if (!file.write(chunk.data(), chunk.size()) || !file.flushToDevice()) {
                m.note = "writing the probe file in " + m.directory + " failed";
                return m;
            }
            m.bytes += chunk.size();
            elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        }
        m.seconds = std::max(elapsed, 1e-6);
        m.bytesPerSecond = static_cast<double>(m.bytes) / m.seconds;
        m.ok = true;
        char note[160];
        std::snprintf(note, sizeof note, "measured: %.1f MiB of %llu-byte frames in %.2f s", m.bytes / 1048576.0,
                      static_cast<unsigned long long>(frame), m.seconds);
        m.note = note;
        SPDLOG_INFO("Destination write rate at {}: {:.0f} MB/s ({})", m.directory, m.bytesPerSecond / 1e6, m.note);
        return m;
    } catch (const std::exception& e) {
        m.ok = false;
        m.note = std::string("write-rate probe failed: ") + e.what();
        return m;
    }
}

} // namespace backend::app
