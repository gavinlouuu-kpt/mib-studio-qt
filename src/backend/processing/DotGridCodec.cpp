#include "backend/processing/DotGridCodec.h"

#include "backend/processing/IProcessingKernel.h"

#include <chrono>

#ifndef MIB_DOTGRID_CORE_VERSION
#error "MIB_DOTGRID_CORE_VERSION must be defined (scripts/dot_grid/dotgrid/VERSION via CMake)"
#endif

namespace backend::dotgrid {

std::string codecLineName(int contract) {
    // Registered once, never reused (ADR 0010); a later contract is named by
    // what it changes.
    switch (contract) {
    case kCodecContract1:
        return "mseq63-delta2";
    default:
        return {};
    }
}

namespace {

class BundledCodec final : public ICodec {
public:
    BundledCodec() {
        id_.coreVersion = MIB_DOTGRID_CORE_VERSION;
        id_.contract = kBundledCodecContract;
        id_.line = codecLineName(kBundledCodecContract);
        id_.source = "bundled";
        id_.buildId = "mib-dotgrid-" + id_.line + "-" + id_.coreVersion;
        // Same toolchain as the bundled processing kernel in this binary.
        id_.runtimeFingerprint = processing::bundledProcessingCoreIdentity().runtimeFingerprint;
    }

    const CodecIdentity& identity() const override { return id_; }

    Codebook encode(const CodebookParams& params, std::vector<Chip> chips,
                    std::string designName) const override {
        return Codebook::generate(params, std::move(chips), std::move(designName));
    }

    DecodeResult decode(const std::shared_ptr<const Registry>& designs, const cv::Mat& gray,
                        const DecoderConfig& config) const override {
        // Decoder is the contract-1 algorithm and skips any other contract.
        return Decoder(designs).decode(gray, config);
    }

private:
    CodecIdentity id_;
};

} // namespace

std::shared_ptr<const ICodec> bundledCodec() {
    static const std::shared_ptr<const ICodec> codec = std::make_shared<const BundledCodec>();
    return codec;
}

CodecSet CodecSet::bundled() {
    CodecSet set;
    set.add(bundledCodec());
    return set;
}

bool CodecSet::add(std::shared_ptr<const ICodec> codec, std::string* errorOut) {
    if (!codec) {
        if (errorOut) *errorOut = "null codec";
        return false;
    }
    const int contract = codec->identity().contract;
    if (find(contract)) {
        if (errorOut)
            *errorOut = "a core for codec contract " + std::to_string(contract) + " is already active";
        return false;
    }
    codecs_.push_back(std::move(codec));
    return true;
}

const ICodec* CodecSet::find(int contract) const {
    for (const auto& c : codecs_)
        if (c->servesContract(contract)) return c.get();
    return nullptr;
}

std::vector<CodecIdentity> CodecSet::identities() const {
    std::vector<CodecIdentity> out;
    for (const auto& c : codecs_)
        out.push_back(c->identity());
    return out;
}

DesignDecoder::DesignDecoder(CodecSet codecs, std::shared_ptr<const Registry> registry)
    : codecs_(std::move(codecs)),
      registry_(registry ? std::move(registry) : std::make_shared<const Registry>()) {
    std::vector<int> contracts;
    for (const auto& d : registry_->designs()) {
        bool seen = false;
        for (int c : contracts)
            seen = seen || c == d.codecContract;
        if (!seen) contracts.push_back(d.codecContract);
    }
    for (int contract : contracts) {
        // Designs without a serving core were never given a codebook by the
        // registry; this only guards hand-built registries.
        if (const ICodec* codec = codecs_.find(contract))
            groups_.push_back({codec, std::make_shared<const Registry>(registry_->withContract(contract))});
    }
}

DecodeResult DesignDecoder::decode(const cv::Mat& gray, const DecoderConfig& config) const {
    const auto t0 = std::chrono::steady_clock::now();
    auto finish = [&](DecodeResult r) {
        r.decodeMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return r;
    };
    if (groups_.empty()) {
        DecodeResult r;
        r.reason = registry_->unsupported().empty()
                       ? "no codebook"
                       : "no active core for the registered designs' codec contract";
        return finish(r);
    }
    // Every design that matched, across all cores. A core that already found
    // two of its own designs reports "ambiguous design (a, b)"; those names
    // count too, so a single hit elsewhere can never mask that ambiguity.
    std::vector<std::string> matched;
    DecodeResult firstHit;
    bool haveHit = false;
    DecodeResult failure;
    bool haveFailure = false;
    int tried = 0;
    for (const auto& g : groups_) {
        DecodeResult r = g.codec->decode(g.designs, gray, config);
        tried += r.designsTried;
        const auto& id = g.codec->identity();
        r.codecContract = id.contract;
        r.coreVersion = id.coreVersion;
        r.coreSource = id.source;
        const std::string prefix = "ambiguous design (";
        if (r.ok) {
            matched.push_back(r.designId);
            if (!haveHit) {
                firstHit = r;
                haveHit = true;
            }
        } else if (r.stage == kDecodeStageAmbiguousDesign || r.reason.rfind(prefix, 0) == 0) {
            const size_t close = r.reason.rfind(')');
            const std::string list = r.reason.substr(prefix.size(), close == std::string::npos
                                                                       ? std::string::npos
                                                                       : close - prefix.size());
            size_t pos = 0;
            while (pos <= list.size()) {
                const size_t comma = list.find(", ", pos);
                matched.push_back(list.substr(pos, comma == std::string::npos ? std::string::npos
                                                                               : comma - pos));
                if (comma == std::string::npos) break;
                pos = comma + 2;
            }
            if (!haveHit) {
                firstHit = r;
                haveHit = true;
            }
        } else if (!haveFailure || r.stage > failure.stage ||
                   (r.stage == failure.stage && r.votes > failure.votes)) {
            // Same ranking as Decoder: the attempt that got furthest, then votes.
            failure = std::move(r);
            haveFailure = true;
        }
    }
    if (matched.size() == 1) {
        firstHit.designsTried = tried;
        return finish(std::move(firstHit));
    }
    if (matched.size() > 1) {
        DecodeResult r = firstHit;
        std::string names;
        for (const auto& m : matched)
            names += (names.empty() ? "" : ", ") + m;
        r.ok = false;
        r.stage = kDecodeStageAmbiguousDesign;
        r.reason = "ambiguous design (" + names + ")";
        r.designId.clear();
        r.designName.clear();
        r.chip.clear();
        r.designsTried = tried;
        return finish(std::move(r));
    }
    failure.designsTried = tried;
    return finish(std::move(failure));
}

} // namespace backend::dotgrid
