#pragma once

// Whole-frame Align previews from the PZ7035 results bridge (#501 P1; pz7035-imx426
// docs/YOFO_HOST_INTERFACE.md "Preview (pzres preview)", reference tools/pzres/pzres.c `preview`,
// images results8 on). The bridge's preview writer puts every DECIMATION-th frame of the ingress
// geometry into one of two DDR slots; the host takes the newest READY slot under PREVIEW_HOLD.
// The bridge must be armed after the sensor mode (ROI, timing, ingress geometry, receiver reset)
// is applied: it only publishes frames whose length matches the ingress geometry.
//
// Pure sequencing over a small register/memory interface so tests run without the board; the
// /dev/mem implementation is PzDevMemExecutionProvider's mapping.

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace backend::processing::pz {

class IPzBridgeIo {
public:
    virtual ~IPzBridgeIo() = default;
    virtual uint32_t reg(uint32_t offset) = 0;               // bridge register (0x40101000 + offset)
    virtual void setReg(uint32_t offset, uint32_t value) = 0;
    virtual void readPreview(size_t offset, uint8_t* dst, size_t n) = 0; // from the preview slots
    virtual void sleepUs(unsigned us);
};

struct BridgePreviewConfig {
    uint32_t width{816};
    uint32_t height{624};
    uint32_t decimation{40};          // 400 fps / 40 = 10 previews/s
    uint64_t previewBase{0x3F100000}; // after the 1 MiB result ring, inside the mem=1008M hole
    uint64_t ringBase{0x3F000000};
    uint32_t slots{2};
    uint32_t slotBytes() const { return width * height; }
};

struct BridgePreviewImage {
    uint64_t frameId{0};
    uint32_t width{0}, height{0};
    std::vector<uint8_t> pixels; // Mono8, width*height
};

struct BridgePreviewCounters {
    uint32_t produced{0}, dropped{0}, framesLost{0}, state{0}, fault{0};
};

// STOP (20 ms drain), geometry + Mono8 + decimation, CONFIG_COMMIT, preview slots, result ring
// (tail = head), run id, ARM. False with `error` on an invalid geometry.
bool armBridgePreview(IPzBridgeIo& io, const BridgePreviewConfig& config, std::string* error);

// One poll: releases the result ring, then copies the newest READY slot if its frame id differs
// from `lastFrameId` and the HOLD stuck. Returns true with `out` filled for a new image; false
// otherwise (no new slot yet, or the writer was overwriting it: try again). `bridgeLost` is set
// when the bridge left ARMED/RUNNING.
bool pollBridgePreview(IPzBridgeIo& io, const BridgePreviewConfig& config, uint64_t lastFrameId,
                       BridgePreviewImage& out, bool* bridgeLost);

// Polls every 2 ms until a new image or `timeout` (about 3-5 decimation periods). On timeout,
// `error` names PREVIEW_PRODUCED/DROPPED and FRAMES_LOST so a wrong HMAX (tap overflow) or a
// slot/geometry mismatch shows.
bool waitBridgePreview(IPzBridgeIo& io, const BridgePreviewConfig& config, uint64_t lastFrameId,
                       std::chrono::milliseconds timeout, BridgePreviewImage& out, std::string* error);

BridgePreviewCounters readBridgeCounters(IPzBridgeIo& io);

void stopBridgePreview(IPzBridgeIo& io);

} // namespace backend::processing::pz
