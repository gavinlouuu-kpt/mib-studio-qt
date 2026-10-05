#include "backend/processing/pz/PzExecutionProviders.h"

#include "pz_mib_abi.h" // vendored bundle (register offsets, control bits)

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdio>
#include <limits>

#if defined(__linux__)
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace backend::processing {

services::FilterResult filterResultFromUnetCell(const backend::pz::UnetCell& c) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    services::FilterResult f;
    f.objectId = c.objectId;
    f.objectCount = c.cellCount;
    f.isValid = c.valid();
    f.inRange = c.valid();
    f.touchesBorder = c.cutOff;
    f.isTargetGroup = c.target;
    f.bboxX = c.bboxX;
    f.bboxY = c.bboxY;
    f.bboxWidth = c.bboxWidth;
    f.bboxHeight = c.bboxHeight;
    f.centroidX = c.centroidX;
    f.centroidY = c.centroidY;
    f.area = c.hullArea;
    f.areaRatio = std::isfinite(c.areaRatio) ? c.areaRatio : 0.0;
    f.deformability = std::isfinite(c.deformability) ? c.deformability : 0.0;
    f.ringRatio = nan; // no ring width in the U-Net cell profile
    f.youngsModulus = c.youngsModulusKpa;
    f.brightness = {nan, nan, nan, nan}; // mean and variance replace the quartiles
    f.laplacianVariance = c.laplacianVariance;
    f.brightnessMean = c.brightnessMean;
    f.brightnessVariance = c.brightnessVariance;
    f.contourArea = c.contourArea;
    f.pixelCount = c.pixelCount;
    f.blemishCount = c.blemishCount;
    f.degenerateContour = c.degenerate();
    return f;
}

namespace pz {

namespace bpz = backend::pz;

PzRecordPipeline::PzRecordPipeline(uint32_t timestampHz) : timestampHz_(timestampHz ? timestampHz : 1) {}

void PzRecordPipeline::reset(uint64_t runId, uint32_t epoch, uint32_t generation) {
    runId_ = runId;
    stream_ = bpz::StreamDecoder(epoch, generation);
    assembler_ = bpz::FrameAssembler();
    std::scoped_lock lk(statusMutex_);
    const bool committed = status_.profileCommitted;
    const uint32_t committedEpoch = status_.epoch;
    status_ = ProviderStatus{};
    status_.profileCommitted = committed; // a profile outlives runs
    status_.epoch = committedEpoch;
}

void PzRecordPipeline::feed(const uint8_t* data, size_t size, const IExecutionProvider::Sink& sink) {
    for (const auto& [offset, length] : bpz::splitRecords(data, size)) {
        const bpz::Decoded d = stream_.feed(data + offset, length);
        if (d.error == bpz::DecodeError::SequenceGap) {
            std::scoped_lock lk(statusMutex_);
            ++status_.sequenceGaps;
            status_.lastError = std::string("SEQUENCE_GAP: ") + d.detail;
        } else if (!d.ok()) {
            std::scoped_lock lk(statusMutex_);
            ++status_.decodeErrors;
            status_.lastError = std::string(bpz::decodeErrorName(d.error)) + ": " + d.detail;
            continue;
        }
        if (!d.record) continue;
        for (auto& fr : assembler_.add(*d.record)) deliver(std::move(fr), sink);
    }
    std::scoped_lock lk(statusMutex_);
    status_.orphanResults = assembler_.orphanResults();
}

void PzRecordPipeline::flush(const IExecutionProvider::Sink& sink) {
    if (auto fr = assembler_.flush()) deliver(std::move(*fr), sink);
}

void PzRecordPipeline::noteOverrun() {
    std::scoped_lock lk(statusMutex_);
    ++status_.overruns;
    status_.lastError = "result ring overrun: the consumer fell behind";
}

void PzRecordPipeline::noteProfile(uint32_t epoch) {
    std::scoped_lock lk(statusMutex_);
    status_.profileCommitted = true;
    status_.epoch = epoch;
}

ProviderStatus PzRecordPipeline::status() const {
    std::scoped_lock lk(statusMutex_);
    return status_;
}

void PzRecordPipeline::deliver(bpz::FrameResults&& fr, const IExecutionProvider::Sink& sink) {
    ProviderFrame out;
    out.runId = runId_;
    out.frameId = fr.frame.frameId;
    out.timestampTicks = fr.frame.timestamp;
    out.timestampNs = static_cast<uint64_t>(static_cast<long double>(fr.frame.timestamp) * 1e9L / timestampHz_);
    out.epoch = fr.frame.epoch;
    out.flags = fr.frame.flags;
    out.width = fr.frame.width;
    out.height = fr.frame.height;
    out.scienceProfile = fr.frame.scienceProfile;
    out.profileVersion = fr.frame.profileVersion;
    out.resultCount = fr.frame.resultCount;
    out.incomplete = fr.incomplete;
    uint64_t unknown = 0;
    for (const auto& r : fr.results) {
        if (auto cell = bpz::decodeUnetCellsV2(r)) {
            out.objects.push_back(filterResultFromUnetCell(*cell));
            out.cells.push_back(std::move(*cell));
        } else {
            ++unknown;
        }
    }
    {
        std::scoped_lock lk(statusMutex_);
        ++status_.frames;
        status_.results += fr.results.size();
        status_.incompleteFrames += fr.incomplete ? 1 : 0;
        status_.unknownProfileResults += unknown;
    }
    if (sink) sink(std::move(out));
}

// ---- replay ----------------------------------------------------------------

ReplayExecutionProvider::ReplayExecutionProvider(std::vector<uint8_t> records, double framesPerSecond)
    : records_(std::move(records)), framesPerSecond_(framesPerSecond) {}

ReplayExecutionProvider::~ReplayExecutionProvider() { stop(); }

bool ReplayExecutionProvider::configure(const CompiledProfile& profile, std::string* error) {
    if (running_.load()) {
        if (error) *error = "configure while running";
        return false;
    }
    profile_ = profile;
    pipeline_.noteProfile(++epoch_);
    return true;
}

bool ReplayExecutionProvider::start(uint64_t runId, std::string* error) {
    if (running_.load()) {
        if (error) *error = "replay provider already running";
        return false;
    }
    if (thread_.joinable()) thread_.join();
    pipeline_.reset(runId);
    stopRequested_.store(false);
    finished_.store(false);
    running_.store(true);
    thread_ = std::thread(&ReplayExecutionProvider::run, this);
    return true;
}

void ReplayExecutionProvider::run() {
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t frames = 0;
    for (const auto& [offset, length] : backend::pz::splitRecords(records_.data(), records_.size())) {
        if (stopRequested_.load()) break;
        const bool frame = length > 4 && records_[offset + 4] == PZ_MIB_RECORD_TYPE_FRAME;
        if (frame && framesPerSecond_ > 0) {
            const auto due = t0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                      std::chrono::duration<double>(static_cast<double>(frames) / framesPerSecond_));
            std::this_thread::sleep_until(due);
        }
        frames += frame ? 1 : 0;
        pipeline_.feed(records_.data() + offset, length, sink_);
    }
    pipeline_.flush(sink_);
    finished_.store(true);
    running_.store(false);
}

void ReplayExecutionProvider::stop() {
    stopRequested_.store(true);
    if (thread_.joinable()) thread_.join();
    running_.store(false);
}

ProviderStatus ReplayExecutionProvider::status() const {
    ProviderStatus s = pipeline_.status();
    s.running = running_.load();
    return s;
}

bool ReplayExecutionProvider::waitFinished(std::chrono::milliseconds timeout) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    while (!finished_.load()) {
        if (std::chrono::steady_clock::now() > until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

std::string formatCoreId(const uint32_t words[4]) {
    if ((words[0] | words[1] | words[2] | words[3]) == 0) return {};
    char hex[33];
    std::snprintf(hex, sizeof(hex), "%08x%08x%08x%08x", words[3], words[2], words[1], words[0]);
    return hex;
}

// ---- /dev/mem (PZ7035 PS) --------------------------------------------------

#if defined(__linux__)

class PzDevMemExecutionProvider::Mapping final : public backend::pz::RingMemory {
public:
    ~Mapping() override {
        if (ring_ && ring_ != MAP_FAILED) munmap(const_cast<uint8_t*>(ring_), ringBytes_);
        if (regs_ && regs_ != MAP_FAILED) munmap(const_cast<uint32_t*>(regs_), 0x1000);
        if (fd_ >= 0) close(fd_);
    }
    bool open(const Layout& layout, std::string* error) {
        ringBytes_ = layout.ringBytes;
        fd_ = ::open("/dev/mem", O_RDWR | O_SYNC);
        if (fd_ < 0) return fail(error, std::string("/dev/mem: ") + std::strerror(errno));
        void* r = mmap(nullptr, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd_,
                       static_cast<off_t>(layout.bridgeBase));
        if (r == MAP_FAILED) return fail(error, "mmap bridge registers");
        regs_ = static_cast<volatile uint32_t*>(r);
        if (reg(PZ_MIB_REG_IDENTITY) != PZ_MIB_IDENTITY_MAGIC) {
            return fail(error, "no PZ-MIB platform bridge at the bridge base (identity)");
        }
        void* m = mmap(nullptr, ringBytes_, PROT_READ, MAP_SHARED, fd_, static_cast<off_t>(layout.ringBase));
        if (m == MAP_FAILED) return fail(error, "mmap result ring (boot Linux with mem=1008M)");
        ring_ = static_cast<volatile uint8_t*>(m);
        return true;
    }
    uint32_t reg(uint32_t off) const { return regs_[off / 4]; }
    void setReg(uint32_t off, uint32_t v) { regs_[off / 4] = v; }

    size_t sizeBytes() const override { return ringBytes_; }
    uint32_t head() override { return reg(PZ_MIB_REG_RESULT_RING_HEAD); }
    uint32_t tail() override { return reg(PZ_MIB_REG_RESULT_RING_TAIL); }
    void setTail(uint32_t t) override { setReg(PZ_MIB_REG_RESULT_RING_TAIL, t); }
    void read(size_t offset, uint8_t* dst, size_t n) override {
        std::memcpy(dst, const_cast<const uint8_t*>(ring_ + offset), n);
    }

private:
    static bool fail(std::string* error, std::string msg) {
        if (error) *error = std::move(msg);
        return false;
    }
    int fd_{-1};
    size_t ringBytes_{0};
    volatile uint32_t* regs_{nullptr};
    volatile uint8_t* ring_{nullptr};
};

PzDevMemExecutionProvider::PzDevMemExecutionProvider(Layout layout) : layout_(layout) {}

PzDevMemExecutionProvider::~PzDevMemExecutionProvider() { stop(); }

bool PzDevMemExecutionProvider::ensureMapped(std::string* error) {
    if (map_) return true;
    auto map = std::make_unique<Mapping>();
    if (!map->open(layout_, error)) return false;
    map_ = std::move(map);
    return true;
}

bool PzDevMemExecutionProvider::configure(const CompiledProfile& profile, std::string* error) {
    if (running_.load()) {
        if (error) *error = "configure while running";
        return false;
    }
    if (!ensureMapped(error)) return false;
    auto& m = *map_;
    m.setReg(PZ_MIB_REG_CONFIG_SHADOW + 4 * PZ_MIB_CONFIG_GEOMETRY, (96u << 16) | 512u);
    m.setReg(PZ_MIB_REG_CONFIG_SHADOW + 4 * PZ_MIB_CONFIG_PIXEL_FORMAT, PZ_MIB_PIXEL_FORMAT_MONO8);
    m.setReg(PZ_MIB_REG_CONFIG_SHADOW + 4 * PZ_MIB_CONFIG_PREVIEW_DECIMATION, 0); // no preview DMA
    for (unsigned i = 0; i < PZ_MIB_PROFILE_PAGE_WORDS; ++i) {
        m.setReg(PZ_MIB_REG_PROFILE_PAGE_SHADOW + 4 * i, profile.page[i]);
    }
    const uint32_t epoch = m.reg(PZ_MIB_REG_EPOCH) + 1;
    m.setReg(PZ_MIB_REG_CONFIG_COMMIT, epoch);
    uint32_t status = PZ_MIB_COMMIT_STATUS_PENDING;
    for (int i = 0; i < 200 && status == PZ_MIB_COMMIT_STATUS_PENDING; ++i) {
        std::this_thread::sleep_for(std::chrono::microseconds(500));
        status = m.reg(PZ_MIB_REG_COMMIT_STATUS);
    }
    if (status != PZ_MIB_COMMIT_STATUS_ACKED) {
        if (error) *error = "profile commit not acknowledged (COMMIT_STATUS " + std::to_string(status) + ")";
        return false;
    }
    if (!profile.table0.empty()) {
        m.setReg(PZ_MIB_REG_TABLE_SELECT, PZ_MIB_TABLE_ID_PROFILE_TABLE0);
        m.setReg(PZ_MIB_REG_TABLE_ADDR, 0);
        for (size_t i = 0; i + 3 < profile.table0.size(); i += 4) {
            const uint32_t word = profile.table0[i] | profile.table0[i + 1] << 8 |
                                  static_cast<uint32_t>(profile.table0[i + 2]) << 16 |
                                  static_cast<uint32_t>(profile.table0[i + 3]) << 24;
            m.setReg(PZ_MIB_REG_TABLE_DATA, word);
        }
    }
    SPDLOG_INFO("PzDevMemExecutionProvider: profile committed at epoch {} (table {} B, status 0x{:08x}, "
                "crc32 0x{:08x})",
                m.reg(PZ_MIB_REG_EPOCH), profile.table0.size(), m.reg(PZ_MIB_REG_TABLE_STATUS),
                m.reg(PZ_MIB_REG_TABLE_CRC32));
    pipeline_.noteProfile(m.reg(PZ_MIB_REG_EPOCH));
    return true;
}

bool PzDevMemExecutionProvider::start(uint64_t runId, std::string* error) {
    if (running_.load()) {
        if (error) *error = "provider already running";
        return false;
    }
    if (!ensureMapped(error)) return false;
    auto* map = map_.get();
    const uint32_t hz = map->reg(PZ_MIB_REG_TIMESTAMP_HZ);
    pipeline_.setTimestampHz(hz ? hz : PZ_MIB_TIMESTAMP_HZ_DEFAULT);
    pipeline_.reset(runId, map->reg(PZ_MIB_REG_EPOCH),
                    map->reg(PZ_MIB_REG_GENERATION));
    // As pzres start: ring base, tail = head (both free-running), run id, ARM.
    map->setReg(PZ_MIB_REG_RESULT_RING_BASE_LO, static_cast<uint32_t>(layout_.ringBase));
    map->setReg(PZ_MIB_REG_RESULT_RING_BASE_HI, static_cast<uint32_t>(layout_.ringBase >> 32));
    map->setReg(PZ_MIB_REG_RESULT_RING_TAIL, map->reg(PZ_MIB_REG_RESULT_RING_HEAD));
    map->setReg(PZ_MIB_REG_RUN_ID_LO, static_cast<uint32_t>(runId));
    map->setReg(PZ_MIB_REG_RUN_ID_HI, static_cast<uint32_t>(runId >> 32));
    map->setReg(PZ_MIB_REG_CONTROL, PZ_MIB_CONTROL_ARM);
    SPDLOG_INFO("PzDevMemExecutionProvider: armed run {} (state {}, fault 0x{:x}, timestamp {} Hz)", runId,
                map->reg(PZ_MIB_REG_STATE), map->reg(PZ_MIB_REG_FAULT), hz);
    stopRequested_.store(false);
    running_.store(true);
    thread_ = std::thread(&PzDevMemExecutionProvider::run, this);
    return true;
}

void PzDevMemExecutionProvider::run() {
    backend::pz::ResultRingReader reader(*map_);
    std::vector<uint8_t> buffer;
    buffer.reserve(layout_.ringBytes);
    while (!stopRequested_.load()) {
        buffer.clear();
        const auto drain = reader.drain(buffer);
        if (drain.overrun) {
            pipeline_.noteOverrun();
            SPDLOG_ERROR("PzDevMemExecutionProvider: result ring overrun; resynchronised at head");
        }
        if (!buffer.empty()) pipeline_.feed(buffer.data(), buffer.size(), sink_);
        std::this_thread::sleep_for(layout_.poll);
    }
    buffer.clear();
    reader.drain(buffer); // what the device wrote before STOP
    if (!buffer.empty()) pipeline_.feed(buffer.data(), buffer.size(), sink_);
    pipeline_.flush(sink_);
    running_.store(false);
}

void PzDevMemExecutionProvider::stop() {
    if (!map_ || !running_.load()) {
        if (thread_.joinable()) thread_.join();
        return;
    }
    map_->setReg(PZ_MIB_REG_CONTROL, PZ_MIB_CONTROL_STOP);
    stopRequested_.store(true);
    if (thread_.joinable()) thread_.join();
    running_.store(false);
}

ProviderStatus PzDevMemExecutionProvider::status() const {
    ProviderStatus s = pipeline_.status();
    s.running = running_.load();
    return s;
}

ProviderIdentity PzDevMemExecutionProvider::identity() {
    ProviderIdentity id;
    std::string error;
    if (!running_.load() && !ensureMapped(&error)) {
        SPDLOG_WARN("PzDevMemExecutionProvider: identity unavailable: {}", error);
        return id;
    }
    const auto& m = *map_;
    const uint32_t build[4] = {m.reg(PZ_MIB_REG_BUILD_ID0), m.reg(PZ_MIB_REG_BUILD_ID1),
                               m.reg(PZ_MIB_REG_BUILD_ID2), m.reg(PZ_MIB_REG_BUILD_ID3)};
    const uint32_t profile[4] = {m.reg(PZ_MIB_REG_PROFILE_ID0), m.reg(PZ_MIB_REG_PROFILE_ID1),
                                 m.reg(PZ_MIB_REG_PROFILE_ID2), m.reg(PZ_MIB_REG_PROFILE_ID3)};
    const uint32_t science = m.reg(PZ_MIB_REG_SCIENCE_PROFILE);
    id.valid = true;
    id.abiVersion = m.reg(PZ_MIB_REG_ABI_VERSION);
    id.scienceProfile = static_cast<uint16_t>(science & 0xFFFFu);
    id.profileVersion = static_cast<uint16_t>(science >> 16);
    id.buildId = formatCoreId(build);
    id.profileId = formatCoreId(profile);
    return id;
}

#endif

} // namespace pz
} // namespace backend::processing
