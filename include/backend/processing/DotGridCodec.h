#pragma once

// Dot-grid codec cores (ADR 0010): the encode/decode seam, modelled on the
// processing cores (IProcessingKernel, ADR 0006/0007).
//
// Two version axes, never mixed:
//  - the codec CONTRACT says what the dots mean (m-sequence, delta windows,
//    seed hashing, bit <-> displacement mapping, lattice semantics). A
//    fabricated mask never changes, so a contract is frozen forever and every
//    registered design records the contract its mask was made with;
//  - the core VERSION is a build of one contract's encoder + decoder (better
//    detection, speed, bug fixes). It may change decode robustness but must
//    reproduce its contract's frozen gold references
//    (scripts/dot_grid/gold/codec-contract<N>.json) bit for bit on encode.
//
// One core implements exactly one contract. Unlike processing profiles, a
// bench can hold wafers made under several contracts, so the app keeps one
// active core per contract (CodecSet) and a design whose contract has no
// active core is reported as unsupported, never decoded by another core.
//
// Phase 1 ships the bundled contract-1 core only; the C ABI plugin, loader,
// catalog and wheel phases are planned in
// docs/exec-plans/active/2026-10-02-dot-grid-codec-cores.md.

#include "backend/processing/DotGridCodebook.h"
#include "backend/processing/DotGridDecoder.h"
#include "backend/processing/DotGridRegistry.h"

#include <opencv2/core.hpp>

#include <memory>
#include <string>
#include <vector>

namespace backend::dotgrid {

// Contract 1 (kCodecContract1, DotGridCodebook.h): period-63 m-sequence
// (x^6 + x^5 + 1) columns/rows, phases from SplitMix64-hashed 2-symbol delta
// windows, 4-direction displacement = 2 bits.
constexpr int kBundledCodecContract = kCodecContract1;

// Fixed human name of a contract (never reused); empty for an unknown one.
std::string codecLineName(int contract);

struct CodecIdentity {
    std::string coreVersion;        // build of this contract's core (MIB_DOTGRID_CORE_VERSION)
    int contract{0};                // codec contract implemented, exactly one
    std::string line;               // codecLineName(contract)
    std::string source;             // "bundled" | "plugin"
    std::string buildId;            // "mib-dotgrid-<line>-<version>"
    std::string runtimeFingerprint; // platform/compiler identity (for plugin matching)
};

class ICodec {
public:
    virtual ~ICodec() = default;
    virtual const CodecIdentity& identity() const = 0;
    bool servesContract(int contract) const { return identity().contract == contract; }

    // Encode: the pattern definition of a design under this contract. The
    // mask generator and the app must get identical codebooks (gold).
    virtual Codebook encode(const CodebookParams& params, std::vector<Chip> chips,
                            std::string designName) const = 0;

    // Decode a frame against `designs`, all of which must be of this codec's
    // contract (a design of another contract fails closed, never decodes).
    virtual DecodeResult decode(const std::shared_ptr<const Registry>& designs,
                                const cv::Mat& gray, const DecoderConfig& config) const = 0;
};

// The contract-1 core compiled into this build.
std::shared_ptr<const ICodec> bundledCodec();

// Active cores, at most one per contract.
class CodecSet {
public:
    CodecSet() = default;
    static CodecSet bundled();

    // Refuses a second core for a contract already present.
    bool add(std::shared_ptr<const ICodec> codec, std::string* errorOut = nullptr);
    const ICodec* find(int contract) const;
    std::vector<CodecIdentity> identities() const;
    bool empty() const { return codecs_.empty(); }

private:
    std::vector<std::shared_ptr<const ICodec>> codecs_;
};

// A registry bound to the active cores: designs grouped per contract, each
// group decoded by its own core. Exactly one design may decode a frame; a hit
// in two designs (even under different contracts) is "ambiguous design".
class DesignDecoder {
public:
    DesignDecoder(CodecSet codecs, std::shared_ptr<const Registry> registry);

    DecodeResult decode(const cv::Mat& gray, const DecoderConfig& config) const;

    const CodecSet& codecs() const { return codecs_; }
    const Registry& registry() const { return *registry_; }

private:
    struct Group {
        const ICodec* codec{nullptr};
        std::shared_ptr<const Registry> designs;
    };
    CodecSet codecs_;
    std::shared_ptr<const Registry> registry_;
    std::vector<Group> groups_;
};

} // namespace backend::dotgrid
