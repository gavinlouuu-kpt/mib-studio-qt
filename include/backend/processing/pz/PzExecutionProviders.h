#pragma once

// Execution providers for the PZ7035 result records (YOFO impl spec S1):
// - PzRecordPipeline: record bytes -> stream checks -> FRAME/RESULT assembly
//   -> ProviderFrame (shared by both providers, testable on its own);
// - ReplayExecutionProvider: replays a record stream (a ring capture, e.g.
//   `pzres capture`, or encoded fixtures) at a frame rate; tests never need
//   the board;
// - PzDevMemExecutionProvider (Linux): the PS DDR result ring and bridge
//   registers through /dev/mem, exactly as pz7035-imx426 tools/pzres does
//   (ring at 0x3F000000, 1 MiB, Linux booted with mem=1008M; bridge at
//   0x40101000). A kernel driver (/dev/pz_mib0) replaces this later.

#include "backend/processing/IExecutionProvider.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

namespace backend::processing::pz {

// The 32-hex id of four identity registers (words[0] = ID0 ... words[3] =
// ID3), most significant word first; empty when all four are zero.
std::string formatCoreId(const uint32_t words[4]);

class PzRecordPipeline {
public:
    explicit PzRecordPipeline(uint32_t timestampHz = 59400000u);
    void reset(uint64_t runId, uint32_t epoch = 0, uint32_t generation = 0);
    void setTimestampHz(uint32_t hz) { timestampHz_ = hz ? hz : 1; }
    // Feed whole records (a drained ring span or a capture chunk); completed
    // frames go to `sink`.
    void feed(const uint8_t* data, size_t size, const IExecutionProvider::Sink& sink);
    // Deliver a frame still waiting for RESULTs (at stop).
    void flush(const IExecutionProvider::Sink& sink);
    void noteOverrun();
    void noteProfile(uint32_t epoch);
    ProviderStatus status() const;

private:
    void deliver(backend::pz::FrameResults&& fr, const IExecutionProvider::Sink& sink);

    uint32_t timestampHz_;
    uint64_t runId_{0};
    backend::pz::StreamDecoder stream_{0, 0};
    backend::pz::FrameAssembler assembler_;
    mutable std::mutex statusMutex_;
    ProviderStatus status_;
};

class ReplayExecutionProvider final : public IExecutionProvider {
public:
    // `records`: a byte stream of whole records. `framesPerSecond` <= 0 replays
    // as fast as the sink takes them.
    ReplayExecutionProvider(std::vector<uint8_t> records, double framesPerSecond);
    ~ReplayExecutionProvider() override;
    std::string name() const override { return "replay"; }
    void setSink(Sink sink) override { sink_ = std::move(sink); }
    // Records the profile (a replay has no device to configure).
    bool configure(const CompiledProfile& profile, std::string* error) override;
    const CompiledProfile& lastProfile() const { return profile_; }
    bool start(uint64_t runId, std::string* error) override;
    void stop() override;
    ProviderStatus status() const override;
    // Block until the whole stream was delivered (or stop()).
    bool waitFinished(std::chrono::milliseconds timeout);

private:
    void run();

    std::vector<uint8_t> records_;
    double framesPerSecond_;
    CompiledProfile profile_;
    uint32_t epoch_{0};
    Sink sink_;
    PzRecordPipeline pipeline_;
    std::thread thread_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> finished_{false};
};

#if defined(__linux__)
class PzDevMemExecutionProvider final : public IExecutionProvider {
public:
    struct Layout {
        uint64_t bridgeBase{0x40101000u};
        uint64_t ringBase{0x3F000000u};
        size_t ringBytes{1u << 20};
        std::chrono::microseconds poll{500};
        // The every-frame ring (results13): frames to keep, 0 = none. From MIB_PZ_RING_FRAMES; the placement is
        // validated against Linux's RAM before every ARM and a run is refused when it does not fit.
        uint32_t ringFrames{0};
    };
    explicit PzDevMemExecutionProvider(Layout layout);
    ~PzDevMemExecutionProvider() override;
    std::string name() const override { return "pz-devmem"; }
    void setSink(Sink sink) override { sink_ = std::move(sink); }
    // As pzres config: geometry 512x96 MONO8, preview off, the 32-word page,
    // CONFIG_COMMIT (waits for ACKED), then PROFILE_TABLE0 through the loader.
    bool configure(const CompiledProfile& profile, std::string* error) override;
    bool start(uint64_t runId, std::string* error) override;
    void stop() override;
    ProviderStatus status() const override;
    // BUILD_ID, PROFILE_ID, ABI_VERSION and SCIENCE_PROFILE from the bridge.
    ProviderIdentity identity() override;
    // As pzres preview (Align, images results8 on): refused while a run is armed.
    bool startPreview(const BridgePreviewConfig& config, std::string* error) override;
    bool fetchPreview(uint64_t lastFrameId, std::chrono::milliseconds timeout, BridgePreviewImage& out,
                      std::string* error) override;
    void stopPreview() override;
    uint32_t ringFramesWanted() const override { return layout_.ringFrames; }
    std::string ringPlacementProblem() override;
    backend::pz::RingStatus ringStatus() override;
    backend::pz::RingRead ringRead(uint64_t seq, backend::pz::RingFrame& out, std::string* why) override;
    uint32_t ringTickHz() override;

private:
    class Mapping;
    class PreviewIo;
    class RingIo;
    void run();
    bool ensureMapped(std::string* error);

    Layout layout_;
    Sink sink_;
    PzRecordPipeline pipeline_;
    std::mutex mapMutex_; // guards creating map_ (never reset once made)
    std::unique_ptr<Mapping> map_;
    std::thread thread_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> running_{false};
    std::mutex previewMutex_;              // serialises preview start/fetch/stop
    std::unique_ptr<PreviewIo> preview_;   // set while previewing
    BridgePreviewConfig previewConfig_;
    std::atomic<bool> previewing_{false};
    std::mutex ringMutex_;                 // the ring io, its mapping and the plan
    std::unique_ptr<RingIo> ring_;
    backend::pz::RingPlan ringPlan_;       // the plan the last ARM programmed
    std::atomic<bool> ringProgrammed_{false};
};
#endif

} // namespace backend::processing::pz
