#pragma once

#include "backend/processing/ProcessingTypes.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace backend::recording {

enum class FcsEventMode { Detection };

struct FcsField {
    std::string key; // persisted HDF compound member
    std::string pnn; // canonical FCS measurement name (PnN)
    std::string pns;
    std::string unit;
    double range{4294967295.0};
};

struct FcsMetadata {
    std::string source;
    int contract{0};
    std::string exportTime;
    std::string mibVersion;
    std::string runId;
    // Deprecated compatibility field; serialization uses FcsWriteOptions::pixelToMicron.
    double pixelToMicron{0.0};
};

struct FcsWriteOptions {
    int contract{1};
    double pixelToMicron{0.4886};
    FcsEventMode eventMode{FcsEventMode::Detection};
    FcsMetadata metadata;
    const std::atomic<bool>* cancel{nullptr};
};

struct FcsWriteResult {
    uint64_t events{0};
    std::vector<FcsField> fields;
    std::string error;
    bool cancelled{false};
};

struct FcsLayout {
    uint64_t beginText{58};
    uint64_t endText{57};
    uint64_t beginData{58};
    uint64_t endData{57};
    bool overflow{false};
};

// Layout/header seams keep the 8-digit legacy header rule testable without
// allocating a multi-gigabyte event buffer or writing a giant fixture.
FcsLayout planFcsLayout(uint64_t textLength, uint64_t dataBytes);
std::string formatFcsHeader(const FcsLayout& layout);

// The registry is intentionally exposed as data so tests and future exporters
// can inspect the contract/stored-field decision without parsing FCS bytes.
std::vector<FcsField> fcsFieldRegistry(int contract, const std::vector<std::string>& storedMembers);

FcsWriteResult writeFcs(const std::string& fcsPath, const std::string& eventMapPath,
                        const std::vector<services::ProcessedFrame>& frames,
                        const std::vector<std::string>& storedMembers,
                        const FcsWriteOptions& options = {});

} // namespace backend::recording
