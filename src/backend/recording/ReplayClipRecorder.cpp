#include "backend/recording/ReplayClipRecorder.h"

#include "backend/app/Tools.h"
#include "backend/playback/FrameStore.h"
#include "backend/processing/ProcessingCoreLoader.h"

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>
#include <vector>

namespace backend::recording {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

constexpr uint64_t kPfncMono8 = 0x01080001;
constexpr int kSchemaVersion = 1;

std::string sha256Of(const void* bytes, size_t count)
{
    if (count == 0) return {};
    return backend::processing::processingCoreBytesSha256(static_cast<const uint8_t*>(bytes), count);
}

std::string sha256Of(const std::string& s) { return sha256Of(s.data(), s.size()); }

// Same bytes ProcessingService::backgroundSha256() hashes.
std::string backgroundSha256(const cv::Mat& bg)
{
    if (bg.empty()) return {};
    const cv::Mat contiguous = bg.isContinuous() ? bg : bg.clone();
    return sha256Of(contiguous.data, contiguous.total() * contiguous.elemSize());
}

std::string utcStamp(uint64_t wallClockNs)
{
    const std::time_t secs = static_cast<std::time_t>(wallClockNs / 1'000'000'000ULL);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &secs);
#else
    gmtime_r(&secs, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tm);
    return buf;
}

std::string frameFileName(uint64_t offset)
{
    std::ostringstream o;
    o << std::setw(6) << std::setfill('0') << offset << ".png";
    return o.str();
}

bool writeTextFile(const fs::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    return static_cast<bool>(out);
}

// Replace the file atomically so a reader never sees a half-written manifest.
bool writeAtomically(const fs::path& path, const std::string& text)
{
    fs::path tmp = path;
    tmp += ".tmp";
    if (!writeTextFile(tmp, text)) return false;
    std::error_code ec;
    fs::rename(tmp, path, ec);
    return !ec;
}

bool imwriteNoThrow(const fs::path& path, const cv::Mat& image)
{
    try {
        return cv::imwrite(path.string(), image);
    } catch (const cv::Exception& e) {
        SPDLOG_WARN("ReplayClipRecorder: encode failed for {}: {}", path.string(), e.what());
        return false;
    }
}

struct CopiedFrame {
    uint64_t offset{0};
    playback::Frame frame;
};

struct Gap {
    uint64_t offset{0};
    const char* reason{""};
};

} // namespace

const char* toString(ReplayClipState s)
{
    switch (s) {
    case ReplayClipState::Idle: return "idle";
    case ReplayClipState::Capturing: return "capturing";
    case ReplayClipState::Writing: return "writing";
    case ReplayClipState::Complete: return "complete";
    case ReplayClipState::Incomplete: return "incomplete";
    case ReplayClipState::Skipped: return "skipped";
    case ReplayClipState::Failed: return "failed";
    }
    return "unknown";
}

ReplayClipRecorder::ReplayClipRecorder(std::filesystem::path rootDir, ReplayClipOptions options)
    : rootDir_(std::move(rootDir)), options_(options)
{
}

ReplayClipRecorder::~ReplayClipRecorder() { shutdown(); }

void ReplayClipRecorder::setOptions(const ReplayClipOptions& options)
{
    std::scoped_lock lock(mutex_);
    options_ = options;
}

ReplayClipOptions ReplayClipRecorder::options() const
{
    std::scoped_lock lock(mutex_);
    return options_;
}

ReplayClipStatus ReplayClipRecorder::lastStatus() const
{
    std::scoped_lock lock(mutex_);
    return status_;
}

void ReplayClipRecorder::setStatus(const ReplayClipStatus& status)
{
    std::scoped_lock lock(mutex_);
    status_ = status;
}

diagnostics::MemoryOwnerStats ReplayClipRecorder::memoryStats() const
{
    const auto opts = options();
    return memory_.snapshot("recording.replayClip", diagnostics::MemoryKnowledge::Measured, opts.maxBytes,
                            opts.maxFrames,
                            "start-of-run replay clip frames copied from the FrameStore and not yet written");
}

bool ReplayClipRecorder::arm(ReplayClipArm arm)
{
    std::scoped_lock lock(mutex_);
    const uint64_t generation = arm.run.startGeneration;
    auto skip = [&](const std::string& why) {
        SPDLOG_INFO("ReplayClipRecorder: clip for run {} skipped — {}", generation, why);
        status_ = ReplayClipStatus{};
        status_.state = ReplayClipState::Skipped;
        status_.startGeneration = generation;
        status_.message = why;
        return false;
    };
    if (shutdown_) return false;
    if (busy_) {
        // Never touch the in-flight clip's status; the log is the record.
        SPDLOG_WARN("ReplayClipRecorder: clip for run {} skipped — clip for run {} is still being written",
                    generation, activeGeneration_);
        return false;
    }
    if (!options_.enabled) return skip("disabled");
    if (!arm.store) return skip("no frame store");
    if (worker_.joinable()) worker_.join(); // previous worker has finished (busy_ == false)

    busy_ = true;
    closeWindow_ = false;
    closeReason_.clear();
    abortWrite_ = false;
    activeGeneration_ = generation;
    status_ = ReplayClipStatus{};
    status_.state = ReplayClipState::Capturing;
    status_.startGeneration = generation;
    status_.firstWriteIndex = arm.firstWriteIndex;
    worker_ = std::thread([this, a = std::move(arm)]() mutable { run(std::move(a)); });
    return true;
}

void ReplayClipRecorder::notifyRunEnded(uint64_t startGeneration)
{
    std::scoped_lock lock(mutex_);
    if (busy_ && activeGeneration_ == startGeneration && !closeWindow_) {
        closeWindow_ = true;
        closeReason_ = "run_ended";
    }
}

bool ReplayClipRecorder::waitIdle(std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex_);
    return idleCv_.wait_for(lock, timeout, [this] { return !busy_; });
}

void ReplayClipRecorder::shutdown()
{
    std::unique_lock lock(mutex_);
    shutdown_ = true;
    if (busy_) {
        if (!closeWindow_) {
            closeWindow_ = true;
            closeReason_ = "shutdown";
        }
        idleCv_.notify_all(); // wakes a throttled writer; it stops throttling on shutdown
        if (!idleCv_.wait_for(lock, options_.shutdownDrainTimeout, [this] { return !busy_; })) {
            SPDLOG_WARN("ReplayClipRecorder: shutdown drain expired; abandoning the rest of clip {}",
                        activeGeneration_);
            abortWrite_ = true;
            idleCv_.notify_all();
        }
    }
    lock.unlock();
    if (worker_.joinable()) worker_.join();
}

void ReplayClipRecorder::run(ReplayClipArm arm)
{
    const ReplayClipOptions opt = options();
    const auto& runSnap = arm.run;
    ReplayClipStatus st;
    st.state = ReplayClipState::Capturing;
    st.startGeneration = runSnap.startGeneration;
    st.firstWriteIndex = arm.firstWriteIndex;

    std::vector<CopiedFrame> frames;
    std::vector<Gap> gaps;

    auto finish = [&](ReplayClipState state, const std::string& message) {
        for (auto& f : frames) {
            memory_.remove(f.frame.data.size());
        }
        frames.clear();
        st.state = state;
        if (!message.empty()) st.message = message;
        if (state == ReplayClipState::Skipped || state == ReplayClipState::Failed) {
            SPDLOG_WARN("ReplayClipRecorder: clip for run {} {} — {}", st.startGeneration, toString(state),
                        st.message);
        } else {
            SPDLOG_INFO("ReplayClipRecorder: clip for run {} {} ({} frames, {} gaps, end {}) -> {}",
                        st.startGeneration, toString(state), st.framesWritten, st.gaps, st.endReason,
                        st.clipDir);
        }
        std::scoped_lock lock(mutex_);
        status_ = st;
        busy_ = false;
        idleCv_.notify_all();
    };
    auto windowClosed = [&](std::string& reason) {
        std::scoped_lock lock(mutex_);
        if (closeWindow_) reason = closeReason_;
        return closeWindow_;
    };

    // ---- Free-space preflight: a clip must never starve experiment output.
    std::error_code ec;
    fs::create_directories(rootDir_, ec);
    if (ec) {
        finish(ReplayClipState::Failed, "cannot create " + rootDir_.string() + ": " + ec.message());
        return;
    }
    {
        uint64_t estimate = opt.maxBytes;
        if (runSnap.frameGeometryKnown && runSnap.frameWidth > 0 && runSnap.frameHeight > 0) {
            estimate = std::min<uint64_t>(opt.maxBytes, opt.maxFrames * runSnap.frameWidth * runSnap.frameHeight);
        }
        const auto space = fs::space(rootDir_, ec);
        if (!ec && space.available < estimate + opt.freeSpaceReserveBytes) {
            finish(ReplayClipState::Skipped, "insufficient free space at " + rootDir_.string());
            return;
        }
    }

    // ---- Capture: copy the window by write index.
    auto& store = *arm.store;
    const uint64_t first = arm.firstWriteIndex;
    uint64_t next = first;
    uint64_t firstTs = 0;
    uint64_t lastTs = 0;
    bool haveFirst = false;
    uint64_t bytes = 0;
    std::string endReason;
    const auto firstFrameDeadline = std::chrono::steady_clock::now() + opt.firstFrameTimeout;
    frames.reserve(static_cast<size_t>(std::min<uint64_t>(opt.maxFrames, 100000)));

    while (endReason.empty()) {
        if (windowClosed(endReason)) break;
        if (next - first >= opt.maxFrames) {
            endReason = "frame_limit";
            break;
        }
        if (store.committedCount() <= next) {
            if (!haveFirst && gaps.empty() && std::chrono::steady_clock::now() > firstFrameDeadline) {
                endReason = "no_frames";
                break;
            }
            // Frames stalled: the duration bound still closes the window.
            if (haveFirst) {
                const uint64_t now = Tools::getTimestamp();
                if (now >= firstTs && now - firstTs >= opt.maxDurationUs) {
                    endReason = "duration_limit";
                    break;
                }
            }
            store.waitForFrame(next, std::chrono::milliseconds(20));
            continue;
        }
        playback::Frame f;
        switch (store.readByWriteIndex(next, f)) {
        case playback::FrameReadOutcome::Available: {
            const uint64_t ts = f.hostTimestampUs != 0 ? f.hostTimestampUs : Tools::getTimestamp();
            if (!haveFirst) {
                firstTs = ts;
                haveFirst = true;
            } else if (ts >= firstTs && ts - firstTs >= opt.maxDurationUs) {
                endReason = "duration_limit";
                break;
            }
            if (bytes + f.data.size() > opt.maxBytes) {
                endReason = "byte_limit";
                break;
            }
            bytes += f.data.size();
            memory_.add(f.data.size());
            lastTs = ts;
            frames.push_back(CopiedFrame{next - first, std::move(f)});
            ++next;
            break;
        }
        case playback::FrameReadOutcome::Overwritten: {
            // The ring lapped the reader: account for every lost index and
            // resume at the oldest retained frame, inside the window.
            const uint64_t resume = std::min(std::max(next + 1, store.earliestAvailableIndex()),
                                             first + opt.maxFrames);
            for (; next < resume; ++next) gaps.push_back(Gap{next - first, "overwritten"});
            break;
        }
        case playback::FrameReadOutcome::Malformed:
            gaps.push_back(Gap{next - first, "malformed"});
            ++next;
            break;
        case playback::FrameReadOutcome::NotYetCommitted:
            std::this_thread::yield();
            break;
        case playback::FrameReadOutcome::OutOfRange:
            endReason = "store_unavailable";
            break;
        }
    }

    st.framesCopied = frames.size();
    st.gaps = gaps.size();
    st.bytesCopied = bytes;
    st.contiguous = gaps.empty();
    st.endReason = endReason;
    if (frames.empty()) {
        finish(ReplayClipState::Skipped, "no frames captured (" + endReason + ")");
        return;
    }

    // ---- Write.
    st.state = ReplayClipState::Writing;
    setStatus(st);

    std::string dirName = utcStamp(runSnap.startWallClockNs) + "-g" + std::to_string(runSnap.startGeneration);
    fs::path dir = rootDir_ / dirName;
    for (int n = 1; fs::exists(dir, ec); ++n) dir = rootDir_ / (dirName + "-" + std::to_string(n));
    fs::create_directories(dir / "frames", ec);
    if (ec) {
        finish(ReplayClipState::Failed, "cannot create " + dir.string() + ": " + ec.message());
        return;
    }
    st.clipDir = dir.string();

    // Reprocessing inputs, verified against the hashes frozen in the run.
    const bool haveBackground = arm.background && !arm.background->empty();
    const std::string processingSha = sha256Of(arm.canonicalProcessingConfig);
    const std::string configJsonSha = sha256Of(arm.configJson);
    const std::string bgSha = haveBackground ? backgroundSha256(*arm.background) : std::string{};
    st.configVerified = processingSha == runSnap.processingConfigSha256 &&
                        configJsonSha == runSnap.configJsonSha256 && bgSha == runSnap.backgroundSha256;
    bool inputsOk = writeTextFile(dir / "run_snapshot.json", arm.runSnapshotJson) &&
                    writeTextFile(dir / "processing_config.txt", arm.canonicalProcessingConfig);
    if (!arm.configJson.empty()) inputsOk = writeTextFile(dir / "config.json", arm.configJson) && inputsOk;
    if (haveBackground) inputsOk = imwriteNoThrow(dir / "background.png", *arm.background) && inputsOk;

    auto manifest = [&](const char* state) {
        json m;
        m["schema_version"] = kSchemaVersion;
        m["kind"] = "mib.replay_clip";
        m["state"] = state;
        m["end_reason"] = st.endReason;
        m["message"] = st.message;
        m["contiguous"] = st.contiguous;
        m["config_verified"] = st.configVerified;
        m["capture_rule"] = {{"max_frames", opt.maxFrames},
                             {"max_duration_us", opt.maxDurationUs},
                             {"max_bytes", opt.maxBytes}};
        m["run"] = {{"start_generation", runSnap.startGeneration},
                    {"start_wall_clock_ns", runSnap.startWallClockNs},
                    {"start_host_time_us", runSnap.startHostTimeUs},
                    {"capture_generation", runSnap.captureGeneration},
                    {"output_path", runSnap.outputPath},
                    {"camera", runSnap.camera.effective},
                    {"simulated", runSnap.camera.simulated},
                    {"delivery_mode", runSnap.deliveryModeActive},
                    {"realtime_mode", runSnap.realtimeMode},
                    {"timestamp_descriptor", runSnap.timestampDescriptor},
                    {"processing_core_version", runSnap.processingCore.version},
                    {"processing_contract_version", runSnap.processingCore.contractVersion},
                    {"application_version", runSnap.applicationVersion},
                    {"build_id", runSnap.buildId}};
        m["hashes"] = {{"processing_config_sha256", processingSha},
                       {"expected_processing_config_sha256", runSnap.processingConfigSha256},
                       {"config_json_sha256", configJsonSha},
                       {"expected_config_json_sha256", runSnap.configJsonSha256},
                       {"background_sha256", bgSha},
                       {"expected_background_sha256", runSnap.backgroundSha256}};
        m["first_write_index"] = first;
        m["first_host_timestamp_us"] = firstTs;
        m["last_host_timestamp_us"] = lastTs;
        m["frames"] = {{"window", st.framesCopied + st.gaps},
                       {"copied", st.framesCopied},
                       {"gaps", st.gaps},
                       {"written", st.framesWritten},
                       {"write_failures", st.writeFailures},
                       {"bytes", st.bytesCopied}};
        m["encoding"] = {{"format", "png"}, {"pixel_format", "Mono8"}, {"lossless", true}};
        m["files"] = {{"frames_dir", "frames"},
                      {"frames_index", "frames.jsonl"},
                      {"run_snapshot", "run_snapshot.json"},
                      {"processing_config", "processing_config.txt"},
                      {"config_json", arm.configJson.empty() ? json() : json("config.json")},
                      {"background", haveBackground ? json("background.png") : json()}};
        return m.dump(2);
    };
    // A manifest left in "writing" after a crash tells readers the clip is
    // incomplete.
    if (!writeAtomically(dir / "manifest.json", manifest("writing"))) {
        finish(ReplayClipState::Failed, "cannot write manifest in " + dir.string());
        return;
    }

    std::vector<std::pair<uint64_t, json>> index;
    index.reserve(frames.size() + gaps.size());
    for (const auto& g : gaps) index.emplace_back(g.offset, json{{"offset", g.offset},
                                                                  {"write_index", first + g.offset},
                                                                  {"status", g.reason}});
    const auto writeStart = std::chrono::steady_clock::now();
    uint64_t bytesWritten = 0;
    bool abandoned = false;
    for (auto& cf : frames) {
        {
            std::unique_lock lock(mutex_);
            if (abortWrite_) {
                abandoned = true;
                break;
            }
            // Throttle so the clip does not compete with the experiment's
            // HDF5 I/O; a shutdown drains at full speed.
            if (opt.maxWriteBytesPerSec > 0 && !shutdown_) {
                const auto due = writeStart + std::chrono::microseconds(bytesWritten * 1'000'000ULL /
                                                                        opt.maxWriteBytesPerSec);
                idleCv_.wait_until(lock, due, [this] { return abortWrite_ || shutdown_; });
            }
        }
        const auto& f = cf.frame;
        json rec{{"offset", cf.offset},
                 {"write_index", first + cf.offset},
                 {"device_timestamp", f.timestamp},
                 {"host_timestamp_us", f.hostTimestampUs},
                 {"width", f.width},
                 {"height", f.height},
                 {"line_pitch", f.linePitch},
                 {"pixel_format", f.pixelFormat}};
        const std::string name = frameFileName(cf.offset);
        bool ok = false;
        if (f.pixelFormat != kPfncMono8) {
            rec["status"] = "unsupported_pixel_format";
        } else if (f.width == 0 || f.height == 0 || f.linePitch < f.width ||
                   f.data.size() < f.linePitch * f.height) {
            rec["status"] = "malformed";
        } else {
            const cv::Mat image(static_cast<int>(f.height), static_cast<int>(f.width), CV_8UC1,
                                const_cast<uint8_t*>(f.data.data()), f.linePitch);
            ok = imwriteNoThrow(dir / "frames" / name, image);
            rec["status"] = ok ? "written" : "write_failed";
        }
        if (ok) {
            rec["file"] = "frames/" + name;
            ++st.framesWritten;
        } else {
            ++st.writeFailures;
        }
        index.emplace_back(cf.offset, std::move(rec));
        bytesWritten += cf.frame.data.size();
        memory_.remove(cf.frame.data.size());
        cf.frame.data = {};
    }
    const uint64_t abandonedCount = st.framesCopied - st.framesWritten - st.writeFailures;
    for (const auto& cf : frames) {
        if (cf.frame.data.empty()) continue; // written or failed above
        index.emplace_back(cf.offset, json{{"offset", cf.offset},
                                           {"write_index", first + cf.offset},
                                           {"host_timestamp_us", cf.frame.hostTimestampUs},
                                           {"status", "abandoned"}});
    }

    std::sort(index.begin(), index.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::string jsonl;
    for (const auto& [offset, rec] : index) jsonl += rec.dump() + "\n";
    inputsOk = writeTextFile(dir / "frames.jsonl", jsonl) && inputsOk;

    ReplayClipState state;
    if (st.framesWritten == 0) {
        state = ReplayClipState::Failed;
        st.message = "no frame could be written";
    } else if (abandoned || st.writeFailures > 0 || !inputsOk || !st.contiguous ||
               st.endReason == "run_ended" || st.endReason == "shutdown") {
        state = ReplayClipState::Incomplete;
        if (abandoned) {
            st.endReason = "shutdown";
            st.message = std::to_string(abandonedCount) + " copied frames abandoned at shutdown";
        } else if (!inputsOk) {
            st.message = "a reprocessing input file could not be written";
        } else if (st.writeFailures > 0) {
            st.message = std::to_string(st.writeFailures) + " frames could not be written";
        }
    } else {
        state = ReplayClipState::Complete;
    }
    if (!writeAtomically(dir / "manifest.json", manifest(toString(state)))) {
        finish(ReplayClipState::Failed, "cannot finalize manifest in " + dir.string());
        return;
    }
    finish(state, {});
}

} // namespace backend::recording
