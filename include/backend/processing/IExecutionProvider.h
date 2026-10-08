#pragma once

// Execution-provider seam (YOFO impl spec S1, #447 E3): where per-frame
// results come from when the science runs in the PZ7035 PL instead of the
// host pipeline (ADR 0008). A provider delivers one ProviderFrame per sensor
// frame on its own thread; ProcessingService ingests them. Qt-free.
//
// Covers profile commit (S2: page + E-modulus table), start/stop, the frame
// sink and status.

#include "backend/processing/ProcessingTypes.h"
#include "backend/processing/pz/PzBridgePreview.h"
#include "backend/processing/pz/PzProfileCompiler.h"
#include "backend/pz/PzRecords.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace backend::processing {

struct ProviderFrame {
    uint64_t runId{0};
    uint64_t frameId{0};         // sensor frame counter (monotonic within a generation)
    uint64_t timestampTicks{0};  // SOF, device ticks
    uint64_t timestampNs{0};     // SOF, ticks converted with the provider's timestamp_hz
    uint32_t epoch{0};
    uint32_t flags{0};           // backend::pz::kFrame* bits
    uint16_t width{0};
    uint16_t height{0};
    uint16_t scienceProfile{0};
    uint16_t profileVersion{0};
    uint16_t resultCount{0};     // RESULTs the device announced for this frame
    bool incomplete{false};      // fewer RESULTs arrived than announced
    // One entry per RESULT, in result order. `cells` holds the full profile
    // values (unet_cells_v2); `objects` the same cells as FilterResults for the
    // shared result paths. Results of an unknown profile appear in neither.
    std::vector<backend::pz::UnetCell> cells;
    std::vector<services::FilterResult> objects;
    bool empty() const { return (flags & backend::pz::kFrameEmpty) != 0; }
    bool invalid() const {
        return (flags & (backend::pz::kFrameInvalid | backend::pz::kFramePartial)) != 0;
    }
};

struct ProviderStatus {
    bool running{false};
    uint64_t frames{0};
    uint64_t results{0};
    // FRAME.flags outcomes (#501 live statistics): no cell passed the size gate, a stage fault
    // (no RESULT follows), and a cut result list (more than 16 cells, or the stage deadline).
    uint64_t emptyFrames{0};
    uint64_t invalidFrames{0};
    uint64_t truncatedFrames{0};
    uint64_t incompleteFrames{0};
    uint64_t decodeErrors{0};   // records rejected (CRC, length, magic, ...)
    uint64_t sequenceGaps{0};
    uint64_t overruns{0};       // the consumer fell behind the device ring
    uint64_t orphanResults{0};  // RESULTs without their FRAME
    uint64_t unknownProfileResults{0};
    std::string lastError;
    // Profile page and table the device last accepted (configure()).
    bool profileCommitted{false};
    uint32_t epoch{0};
};

// The core the device runs (ADR 0011: a core = PL build + model weights),
// read from the bridge's identity registers. Ids are the 32-hex prefixes the
// device reports: ID3 ID2 ID1 ID0 as %08x each, as `pzres id` prints them.
struct ProviderIdentity {
    bool valid{false};
    uint32_t abiVersion{0};
    uint16_t scienceProfile{0};
    uint16_t profileVersion{0};
    std::string buildId;   // first 128 bits of the PL build's git commit
    std::string profileId; // first 128 bits of the model weights' sha256; empty when none
};

class IExecutionProvider {
public:
    using Sink = std::function<void(ProviderFrame&&)>;
    virtual ~IExecutionProvider() = default;
    virtual std::string name() const = 0;
    // Set before start(); called on the provider thread.
    virtual void setSink(Sink sink) = 0;
    // Commit a compiled profile (page + E-modulus table) while stopped (S2).
    virtual bool configure(const pz::CompiledProfile& profile, std::string* error) = 0;
    virtual bool start(uint64_t runId, std::string* error) = 0;
    virtual void stop() = 0;
    virtual ProviderStatus status() const = 0;
    // Side-effect free; a provider without a device reports !valid.
    virtual ProviderIdentity identity() { return {}; }

    // Align whole-frame previews through the bridge's preview writer (#501 P1). Mutually
    // exclusive with a run (start/stop). Providers without a device do not support them.
    virtual bool startPreview(const pz::BridgePreviewConfig& config, std::string* error) {
        (void)config;
        if (error) *error = "the " + name() + " provider has no bridge preview";
        return false;
    }
    // The newest whole frame newer than `lastFrameId`, waiting up to `timeout`.
    virtual bool fetchPreview(uint64_t lastFrameId, std::chrono::milliseconds timeout, pz::BridgePreviewImage& out,
                              std::string* error) {
        (void)lastFrameId, (void)timeout, (void)out;
        if (error) *error = "no bridge preview";
        return false;
    }
    virtual void stopPreview() {}
};

// FilterResult view of one unet_cells_v2 cell, including the Laplacian and
// the cell fields (brightness mean/variance, contour area, pixel and blemish
// counts). Profile values without a FilterResult field (hull perimeter and
// count, area µm²) stay in UnetCell.
services::FilterResult filterResultFromUnetCell(const backend::pz::UnetCell& cell);

} // namespace backend::processing
