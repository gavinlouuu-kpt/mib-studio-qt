#include "backend/processing/pz/PzExecutionProviders.h"

#include "pz_mib_abi.h" // vendored bundle (register offsets, control bits)
#include "backend/pz/PzPlatformMonitor.h" // pzPlConfigured

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>

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
        if (fr.frame.flags & bpz::kFrameEmpty) ++status_.emptyFrames;
        if (fr.frame.flags & bpz::kFrameInvalid) ++status_.invalidFrames;
        if (fr.frame.flags & (bpz::kFrameResultsTruncated | bpz::kFrameResultsOverflow)) ++status_.truncatedFrames;
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

// Bridge registers through the provider's mapping, preview slots through their own read-only map.
class PzDevMemExecutionProvider::PreviewIo final : public IPzBridgeIo {
public:
    PreviewIo(Mapping& map, uint64_t base, size_t bytes) : map_(map), bytes_(bytes) {
        fd_ = ::open("/dev/mem", O_RDONLY | O_SYNC);
        if (fd_ >= 0) {
            void* p = mmap(nullptr, bytes_, PROT_READ, MAP_SHARED, fd_, static_cast<off_t>(base));
            if (p != MAP_FAILED) slots_ = static_cast<volatile uint8_t*>(p);
        }
    }
    ~PreviewIo() override {
        if (slots_) munmap(const_cast<uint8_t*>(slots_), bytes_);
        if (fd_ >= 0) close(fd_);
    }
    bool ok() const { return slots_ != nullptr; }
    uint32_t reg(uint32_t offset) override { return map_.reg(offset); }
    void setReg(uint32_t offset, uint32_t value) override { map_.setReg(offset, value); }
    void readPreview(size_t offset, uint8_t* dst, size_t n) override {
        if (offset + n <= bytes_) std::memcpy(dst, const_cast<const uint8_t*>(slots_ + offset), n);
    }

private:
    Mapping& map_;
    size_t bytes_;
    int fd_{-1};
    volatile uint8_t* slots_{nullptr};
};

// The ring's registers through the provider's mapping, its memory through a read-only map of the whole ring
// (mapped on first use after an ARM; the ring is outside Linux's RAM, so /dev/mem may map it).
class PzDevMemExecutionProvider::RingIo final : public backend::pz::IRingIo {
public:
    explicit RingIo(Mapping& map) : map_(map) { fd_ = ::open("/dev/mem", O_RDONLY | O_SYNC); }
    ~RingIo() override {
        unmap();
        if (fd_ >= 0) close(fd_);
    }
    bool ok() const { return fd_ >= 0; }
    uint32_t reg(uint32_t offset) override { return map_.reg(offset); }
    void setReg(uint32_t offset, uint32_t value) override { map_.setReg(offset, value); }
    bool ensureMapped(uint64_t base, uint64_t bytes) {
        if (mem_ && base == base_ && bytes == bytes_) return true;
        unmap();
        void* p = mmap(nullptr, static_cast<size_t>(bytes), PROT_READ, MAP_SHARED, fd_, static_cast<off_t>(base));
        if (p == MAP_FAILED) return false;
        mem_ = static_cast<volatile uint8_t*>(p);
        base_ = base;
        bytes_ = bytes;
        return true;
    }
    bool read(uint64_t physical, void* dst, size_t bytes) override {
        if (!mem_ || physical < base_ || physical + bytes > base_ + bytes_) return false;
        std::memcpy(dst, const_cast<const uint8_t*>(mem_ + (physical - base_)), bytes);
        return true;
    }

private:
    void unmap() {
        if (mem_) munmap(const_cast<uint8_t*>(mem_), static_cast<size_t>(bytes_));
        mem_ = nullptr;
    }
    Mapping& map_;
    int fd_{-1};
    volatile uint8_t* mem_{nullptr};
    uint64_t base_{0}, bytes_{0};
};

namespace {
// The end of Linux's RAM from /proc/iomem (the `mem=` limit). Unknown (not root, no ranges) means no ring.
std::optional<uint64_t> readLinuxRamEnd(std::string* why) {
    std::ifstream in("/proc/iomem");
    if (!in) {
        if (why) *why = "cannot read /proc/iomem to find where Linux's RAM ends";
        return std::nullopt;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto end = backend::pz::systemRamEnd(text);
    if (!end || *end <= 1) {
        if (why) *why = "/proc/iomem shows no RAM ranges (Studio must run as root on the instrument)";
        return std::nullopt;
    }
    return end;
}
} // namespace

PzDevMemExecutionProvider::PzDevMemExecutionProvider(Layout layout) : layout_(layout) {}

std::string PzDevMemExecutionProvider::ringPlacementProblem() {
    if (layout_.ringFrames == 0) return {};
    std::string why;
    const auto end = readLinuxRamEnd(&why);
    if (!end) return "frame ring: " + why;
    const auto plan = backend::pz::planRing(layout_.ringFrames, *end);
    if (!plan.ok) return "frame ring: " + plan.why;
    return {};
}

backend::pz::RingStatus PzDevMemExecutionProvider::ringStatus() {
    backend::pz::RingStatus none;
    none.why = "no frame ring is armed";
    if (!ringProgrammed_.load()) return none;
    std::string why;
    if (!backend::pz::pzPlConfigured(&why)) {
        none.why = why;
        return none;
    }
    std::lock_guard<std::mutex> lock(ringMutex_);
    if (!ring_) return none;
    auto st = backend::pz::PzFrameRing(*ring_).status();
    st.restoreNeeded = ringRestoreNeeded_.load();
    return st;
}

backend::pz::RingRead PzDevMemExecutionProvider::ringRead(uint64_t seq, backend::pz::RingFrame& out, std::string* why) {
    if (!ringProgrammed_.load()) {
        if (why) *why = "no frame ring is armed";
        return backend::pz::RingRead::Unavailable;
    }
    std::string configured;
    if (!backend::pz::pzPlConfigured(&configured)) {
        if (why) *why = configured;
        return backend::pz::RingRead::Unavailable;
    }
    std::lock_guard<std::mutex> lock(ringMutex_);
    if (!ring_) {
        if (why) *why = "no frame ring is armed";
        return backend::pz::RingRead::Unavailable;
    }
    backend::pz::PzFrameRing ring(*ring_);
    const auto st = ring.status();
    if (!st.valid) {
        if (why) *why = st.why;
        return backend::pz::RingRead::Unavailable;
    }
    if (!ring_->ensureMapped(st.base, static_cast<uint64_t>(st.records) * st.recordBytes)) {
        if (why) *why = "mmap of the frame ring failed (is the ring outside Linux's RAM? boot with a smaller mem=)";
        return backend::pz::RingRead::Unavailable;
    }
    return ring.readFrame(seq, out, why);
}

uint32_t PzDevMemExecutionProvider::ringTickHz() {
    std::string why;
    if (!backend::pz::pzPlConfigured(&why) || !ensureMapped(&why)) return 0;
    const uint32_t hz = map_->reg(PZ_MIB_REG_TIMESTAMP_HZ);
    return hz ? hz : PZ_MIB_TIMESTAMP_HZ_DEFAULT;
}

PzDevMemExecutionProvider::~PzDevMemExecutionProvider() {
    stopPreview();
    stop();
}

bool PzDevMemExecutionProvider::startPreview(const BridgePreviewConfig& config, std::string* error) {
    std::lock_guard<std::mutex> lock(previewMutex_);
    if (running_.load()) {
        if (error) *error = "a run is armed: stop it before Align previews";
        return false;
    }
    if (!backend::pz::pzPlConfigured(error) || !ensureMapped(error)) return false;
    ringProgrammed_.store(false); // the preview arms the bridge again: the ring is gone
    auto io = std::make_unique<PreviewIo>(*map_, config.previewBase, static_cast<size_t>(config.slots) * config.slotBytes());
    if (!io->ok()) {
        if (error) *error = "mmap preview slots (boot Linux with mem=1008M)";
        return false;
    }
    if (!armBridgePreview(*io, config, error)) return false;
    preview_ = std::move(io);
    previewConfig_ = config;
    previewing_.store(true);
    SPDLOG_INFO("PzDevMemExecutionProvider: Align previews {}x{} every {} frames", config.width, config.height,
                config.decimation);
    return true;
}

bool PzDevMemExecutionProvider::fetchPreview(uint64_t lastFrameId, std::chrono::milliseconds timeout,
                                             BridgePreviewImage& out, std::string* error) {
    std::lock_guard<std::mutex> lock(previewMutex_);
    if (!preview_) {
        if (error) *error = "bridge preview is not started";
        return false;
    }
    // The PL may have been reloaded or blanked since: never read it blank.
    if (!backend::pz::pzPlConfigured(error)) return false;
    return waitBridgePreview(*preview_, previewConfig_, lastFrameId, timeout, out, error);
}

void PzDevMemExecutionProvider::stopPreview() {
    std::lock_guard<std::mutex> lock(previewMutex_);
    if (!preview_) return;
    std::string why;
    if (backend::pz::pzPlConfigured(&why)) stopBridgePreview(*preview_);
    preview_.reset();
    previewing_.store(false);
}

bool PzDevMemExecutionProvider::ensureMapped(std::string* error) {
    // identity() runs on the UI's status poll while configure()/start() run on
    // the experiment thread: one mapping, created once.
    std::lock_guard<std::mutex> lock(mapMutex_);
    if (map_) return true;
    if (!backend::pz::pzPlConfigured(error)) return false; // Mapping::open reads IDENTITY
    auto map = std::make_unique<Mapping>();
    if (!map->open(layout_, error)) return false;
    map_ = std::move(map);
    return true;
}

bool PzDevMemExecutionProvider::configure(const CompiledProfile& profile, std::string* error) {
    if (previewing_.load()) {
        if (error) *error = "the bridge is serving Align previews: switch to Run first";
        return false;
    }
    if (running_.load()) {
        if (error) *error = "configure while running";
        return false;
    }
    // A blank PL stalls the AXI bus on any read: check before touching it.
    if (!backend::pz::pzPlConfigured(error) || !ensureMapped(error)) return false;
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
    if (previewing_.load()) {
        if (error) *error = "the bridge is serving Align previews: switch to Run first";
        return false;
    }
    if (running_.load()) {
        if (error) *error = "provider already running";
        return false;
    }
    if (!backend::pz::pzPlConfigured(error) || !ensureMapped(error)) return false;
    auto* map = map_.get();
    // A ring the last STOP did not complete (DRAINING, or the sticky RING_STALLED / STOP_STUCK bits of results13) is left
    // by RESET_GENERATION, then ARM (board owner, pz7035 docs/FRAME_RING.md). Done before the generation is read below,
    // because the reset changes it.
    if (layout_.ringFrames > 0) {
        const uint32_t storeState = map->reg(PZ_MIB_REG_STORE_STATE);
        if ((storeState & backend::pz::kRingStateCodeMask) == PZ_MIB_STORE_STATE_DRAINING ||
            (storeState & (backend::pz::kRingStateStalled | backend::pz::kRingStateStopStuck)) != 0) {
            SPDLOG_WARN("PzDevMemExecutionProvider: the frame ring was left in STORE_STATE 0x{:x}: RESET_GENERATION before ARM", storeState);
            map->setReg(PZ_MIB_REG_CONTROL, PZ_MIB_CONTROL_RESET_GENERATION);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    const uint32_t hz = map->reg(PZ_MIB_REG_TIMESTAMP_HZ);
    pipeline_.setTimestampHz(hz ? hz : PZ_MIB_TIMESTAMP_HZ_DEFAULT);
    pipeline_.reset(runId, map->reg(PZ_MIB_REG_EPOCH),
                    map->reg(PZ_MIB_REG_GENERATION));
    // The every-frame ring is programmed before ARM (STORE_MODE is latched there) and only where it is safe: the
    // PL does not enforce a DDR floor, so a ring that would overlap Linux's RAM refuses the run.
    ringProgrammed_.store(false);
    ringRestoreNeeded_.store(false);
    if (layout_.ringFrames > 0) {
        // A new ring starts with RESET_GENERATION in IDLE only, then ARM (RESET_GENERATION in any other state is refused and sets a
        // sticky fault bit). After a STOP the device reaches IDLE by itself in about 2 ms; one that does not within the bound has a
        // broken tap or clock and needs a restore.
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(backend::pz::kRingFreezeWaitMs);
        uint32_t storeState = map->reg(PZ_MIB_REG_STORE_STATE);
        while ((storeState & backend::pz::kRingStateCodeMask) == PZ_MIB_STORE_STATE_DRAINING && std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            storeState = map->reg(PZ_MIB_REG_STORE_STATE);
        }
        if ((storeState & backend::pz::kRingStateCodeMask) == PZ_MIB_STORE_STATE_DRAINING) {
            ringRestoreNeeded_.store(true);
            if (error) *error = "frame ring: the device is still draining a stopped run (STORE_STATE 0x" +
                                std::string(fmt::format("{:x}", storeState)) + "): restore the PL";
            return false;
        }
        if ((storeState & (backend::pz::kRingStateStalled | backend::pz::kRingStateStopStuck | backend::pz::kRingStateResetRefused)) != 0) {
            SPDLOG_WARN("PzDevMemExecutionProvider: the last frame ring ended with STORE_STATE 0x{:x}: RESET_GENERATION in IDLE, then ARM", storeState);
            map->setReg(PZ_MIB_REG_CONTROL, PZ_MIB_CONTROL_RESET_GENERATION);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    if (layout_.ringFrames > 0) {
        std::string why;
        const auto end = readLinuxRamEnd(&why);
        if (!end) {
            if (error) *error = "frame ring: " + why;
            return false;
        }
        const auto plan = backend::pz::planRing(layout_.ringFrames, *end);
        if (!plan.ok) {
            if (error) *error = "frame ring: " + plan.why;
            return false;
        }
        if ((map->reg(PZ_MIB_REG_CAPABILITIES) & PZ_MIB_CAPABILITIES_FRAME_STORE) == 0) {
            if (error) *error = "frame ring: this PL image has no frame ring (it needs results13)";
            return false;
        }
        std::lock_guard<std::mutex> lock(ringMutex_);
        if (!ring_) ring_ = std::make_unique<RingIo>(*map);
        if (!ring_->ok()) {
            if (error) *error = "frame ring: cannot open /dev/mem";
            return false;
        }
        if (!backend::pz::PzFrameRing(*ring_).program(plan, &why)) {
            if (error) *error = "frame ring: " + why;
            return false;
        }
        ringPlan_ = plan;
        ringProgrammed_.store(true);
        SPDLOG_INFO("PzDevMemExecutionProvider: frame ring {} frames ({:.0f} MiB) at 0x{:08x}", plan.records,
                    static_cast<double>(plan.bytes) / (1024.0 * 1024.0), static_cast<uint32_t>(plan.base));
    }
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
    // The ring freezes only when the device says so: STATE = IDLE with FINAL = HEAD + 1 and neither sticky bit. The
    // frame the device had open at STOP completes first (a watchdog bounds it); anything else is reported, never assumed.
    if (ringProgrammed_.load()) {
        std::lock_guard<std::mutex> lock(ringMutex_);
        if (ring_) {
            const auto st = backend::pz::PzFrameRing(*ring_).awaitFrozen(std::chrono::milliseconds(backend::pz::kRingFreezeWaitMs));
            // No sticky bit and no IDLE within the bound: not a ring fault but a broken tap or clock (the tap closes an open frame in about 2 ms).
            ringRestoreNeeded_.store(st.valid && !st.frozen && !st.fault && !st.stopStuck);
            if (st.frozen)
                SPDLOG_INFO("PzDevMemExecutionProvider: frame ring frozen: {} frames readable (sequences {} to {})", st.count(), st.lo,
                            st.final ? st.final - 1 : 0);
            else
                SPDLOG_WARN("PzDevMemExecutionProvider: frame ring not frozen after STOP ({}, state {}, head {}, final {})",
                            st.valid ? (!st.invalidReason.empty() ? st.invalidReason
                                        : st.stopStuck        ? std::string("stop incomplete (STOP_STUCK): the frames below FINAL stay readable")
                                                              : std::string("still draining: restore needed"))
                                     : st.why,
                            st.state, st.head, st.final);
        }
    }
}

ProviderStatus PzDevMemExecutionProvider::status() const {
    ProviderStatus s = pipeline_.status();
    s.running = running_.load();
    return s;
}

ProviderIdentity PzDevMemExecutionProvider::identity() {
    ProviderIdentity id;
    std::string error;
    // While stopped the PL may have been reloaded or blanked since the mapping
    // was made: check before every read (a running provider implies a
    // configured PL).
    if (!running_.load() && (!backend::pz::pzPlConfigured(&error) || !ensureMapped(&error))) {
        SPDLOG_DEBUG("PzDevMemExecutionProvider: identity unavailable: {}", error);
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
