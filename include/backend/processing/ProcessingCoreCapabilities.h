#pragma once

// Host-side ABI/contract/capability negotiation for the processing-core loader.
// Qt-free. Keeps the equality-not-ordering contract rule and the ABI-v1 ->
// Contract-1 restriction in one place. See
// docs/architecture/processing-contract-compatibility.md.

#include "backend/processing/ProcessingCoreAbi.h"

#include <cstdint>

namespace backend::processing {

// A Contract-2 profile may activate a core only if it advertises engine ABI
// >= 2, contract == 2, and every required capability bit (full pipeline,
// absolute difference, filter chain, per-object Laplacian variance).
inline bool coreSatisfiesContract2(std::uint32_t engineAbiVersion,
                                   std::uint32_t contractVersion,
                                   std::uint32_t capabilities) {
    if (engineAbiVersion < MIB_PROCESSING_ENGINE_ABI_VERSION_2) {
        return false;
    }
    if (contractVersion != MIB_PROCESSING_CONTRACT_VERSION_2) {
        return false;
    }
    return (capabilities & MIB_PROCESSING_CAP_CONTRACT2_REQUIRED) ==
           MIB_PROCESSING_CAP_CONTRACT2_REQUIRED;
}

// An ABI-v1 module serves only Contract 1; it must never back a Contract-2
// profile (the science differs, so the match is by equality, not ordering).
inline bool abiV1ServesContract(std::uint32_t engineAbiVersion, std::uint32_t contractVersion) {
    if (engineAbiVersion != MIB_PROCESSING_ENGINE_ABI_VERSION) {
        return false;
    }
    return contractVersion == MIB_PROCESSING_CONTRACT_VERSION;
}

// The engine ABI the host requests when negotiating a core for a given profile
// contract version.
inline std::uint32_t engineAbiForContract(int profileContractVersion) {
    return profileContractVersion >= static_cast<int>(MIB_PROCESSING_CONTRACT_VERSION_2)
               ? MIB_PROCESSING_ENGINE_ABI_VERSION_2
               : MIB_PROCESSING_ENGINE_ABI_VERSION;
}

} // namespace backend::processing
