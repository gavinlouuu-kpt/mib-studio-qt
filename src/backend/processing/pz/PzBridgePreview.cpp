#include "backend/processing/pz/PzBridgePreview.h"

#include "pz_mib_abi.h" // vendored bundle (register offsets, control bits)

#include <cstdio>
#include <thread>

namespace backend::processing::pz {

void IPzBridgeIo::sleepUs(unsigned us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }

bool armBridgePreview(IPzBridgeIo& io, const BridgePreviewConfig& c, std::string* error) {
    const uint64_t slot = c.slotBytes();
    if (!c.width || !c.height || c.width % 8 || slot % 128 || slot > (6u << 20) || !c.decimation || !c.slots) {
        if (error) *error = "bridge preview: width a multiple of 8, width*height a multiple of 128, <= 6 MiB";
        return false;
    }
    // Geometry needs a drained bridge.
    io.setReg(PZ_MIB_REG_CONTROL, PZ_MIB_CONTROL_STOP);
    io.sleepUs(20000);
    io.setReg(PZ_MIB_REG_CONFIG_SHADOW + 4 * PZ_MIB_CONFIG_GEOMETRY, (c.height << 16) | c.width);
    io.setReg(PZ_MIB_REG_CONFIG_SHADOW + 4 * PZ_MIB_CONFIG_PIXEL_FORMAT, PZ_MIB_PIXEL_FORMAT_MONO8);
    io.setReg(PZ_MIB_REG_CONFIG_SHADOW + 4 * PZ_MIB_CONFIG_PREVIEW_DECIMATION, c.decimation);
    io.setReg(PZ_MIB_REG_CONFIG_COMMIT, io.reg(PZ_MIB_REG_EPOCH) + 1);
    io.sleepUs(1000);
    io.setReg(PZ_MIB_REG_PREVIEW_HOLD, 0);
    io.setReg(PZ_MIB_REG_PREVIEW_RING_BASE_LO, static_cast<uint32_t>(c.previewBase));
    io.setReg(PZ_MIB_REG_PREVIEW_RING_BASE_HI, static_cast<uint32_t>(c.previewBase >> 32));
    io.setReg(PZ_MIB_REG_PREVIEW_RING_ENTRIES, c.slots);
    io.setReg(PZ_MIB_REG_PREVIEW_SLOT_BYTES, static_cast<uint32_t>(slot));
    io.setReg(PZ_MIB_REG_RESULT_RING_BASE_LO, static_cast<uint32_t>(c.ringBase));
    io.setReg(PZ_MIB_REG_RESULT_RING_BASE_HI, static_cast<uint32_t>(c.ringBase >> 32));
    io.setReg(PZ_MIB_REG_RESULT_RING_TAIL, io.reg(PZ_MIB_REG_RESULT_RING_HEAD));
    io.setReg(PZ_MIB_REG_RUN_ID_LO, 1);
    io.setReg(PZ_MIB_REG_RUN_ID_HI, 0);
    io.setReg(PZ_MIB_REG_CONTROL, PZ_MIB_CONTROL_ARM);
    return true;
}

bool pollBridgePreview(IPzBridgeIo& io, const BridgePreviewConfig& c, uint64_t lastFrameId,
                       BridgePreviewImage& out, bool* bridgeLost) {
    if (bridgeLost) *bridgeLost = false;
    // Only FRAME records reach the ring in Align; release them so it never fills.
    io.setReg(PZ_MIB_REG_RESULT_RING_TAIL, io.reg(PZ_MIB_REG_RESULT_RING_HEAD));
    const uint32_t newest = io.reg(PZ_MIB_REG_PREVIEW_RING_HEAD); // newest READY slot + 1, 0 = none
    const uint32_t frameId = newest ? io.reg(PZ_MIB_REG_PREVIEW_SLOT_FRAME + 8 * (newest - 1)) : 0;
    if (!newest || frameId == lastFrameId || newest > c.slots) {
        const uint32_t state = io.reg(PZ_MIB_REG_STATE);
        if (bridgeLost) *bridgeLost = state != PZ_MIB_STATE_RUNNING && state != PZ_MIB_STATE_ARMED;
        return false;
    }
    const uint32_t bit = 1u << (newest - 1);
    io.setReg(PZ_MIB_REG_PREVIEW_HOLD, bit);
    if ((io.reg(PZ_MIB_REG_PREVIEW_HOLD) & bit) == 0) return false; // being overwritten: next poll
    out.frameId = frameId;
    out.width = c.width;   // the FRAME/PREVIEW records name the build's 512x96; the
    out.height = c.height; // configured geometry is the truth
    out.pixels.resize(c.slotBytes());
    io.readPreview(static_cast<size_t>(newest - 1) * c.slotBytes(), out.pixels.data(), out.pixels.size());
    io.setReg(PZ_MIB_REG_PREVIEW_HOLD, 0);
    return true;
}

BridgePreviewCounters readBridgeCounters(IPzBridgeIo& io) {
    BridgePreviewCounters k;
    const uint32_t seq = io.reg(PZ_MIB_REG_SNAPSHOT_SEQ);
    io.setReg(PZ_MIB_REG_SNAPSHOT, 1);
    for (int i = 0; i < 100 && io.reg(PZ_MIB_REG_SNAPSHOT_SEQ) == seq; ++i) io.sleepUs(100);
    k.produced = io.reg(PZ_MIB_REG_COUNTER + 4 * PZ_MIB_COUNTER_PREVIEW_PRODUCED);
    k.dropped = io.reg(PZ_MIB_REG_COUNTER + 4 * PZ_MIB_COUNTER_PREVIEW_DROPPED);
    k.framesLost = io.reg(PZ_MIB_REG_COUNTER + 4 * PZ_MIB_COUNTER_FRAMES_LOST);
    k.state = io.reg(PZ_MIB_REG_STATE);
    k.fault = io.reg(PZ_MIB_REG_FAULT);
    return k;
}

bool waitBridgePreview(IPzBridgeIo& io, const BridgePreviewConfig& c, uint64_t lastFrameId,
                       std::chrono::milliseconds timeout, BridgePreviewImage& out, std::string* error) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        bool lost = false;
        if (pollBridgePreview(io, c, lastFrameId, out, &lost)) return true;
        if (lost || std::chrono::steady_clock::now() >= deadline) {
            const auto k = readBridgeCounters(io);
            if (error) {
                *error = std::string(lost ? "bridge left ARMED/RUNNING" : "no new preview published") +
                         " (state " + std::to_string(k.state) + ", fault 0x" + [&] {
                             char b[16];
                             std::snprintf(b, sizeof(b), "%x", k.fault);
                             return std::string(b);
                         }() + ", preview produced " + std::to_string(k.produced) + " dropped " +
                         std::to_string(k.dropped) + ", frames lost " + std::to_string(k.framesLost) +
                         "; HMAX too fast for the tap, or an image older than results8?)";
            }
            return false;
        }
        io.sleepUs(2000);
    }
}

void stopBridgePreview(IPzBridgeIo& io) {
    io.setReg(PZ_MIB_REG_PREVIEW_HOLD, 0);
    io.setReg(PZ_MIB_REG_CONTROL, PZ_MIB_CONTROL_STOP);
}

} // namespace backend::processing::pz
