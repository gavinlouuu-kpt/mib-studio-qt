// V2-5 proof: the host ABI/contract/capability negotiation only lets a
// Contract-2 profile activate a core that advertises ABI >= 2, contract == 2,
// and every required capability, and keeps ABI-v1 modules restricted to
// Contract 1. Matching is by equality, not ordering.
#include "backend/processing/ProcessingCoreCapabilities.h"
#include "support/assert.h"

namespace proc = backend::processing;

int main() {
    const std::uint32_t all = MIB_PROCESSING_CAP_CONTRACT2_REQUIRED;

    // Contract-2 acceptance.
    MIB_EXPECT(proc::coreSatisfiesContract2(2, 2, all), "abi2 + contract2 + all caps accepted");
    MIB_EXPECT(proc::coreSatisfiesContract2(3, 2, all), "a newer ABI still satisfies");

    // Rejections.
    MIB_EXPECT(!proc::coreSatisfiesContract2(1, 2, all), "ABI v1 cannot serve Contract 2");
    MIB_EXPECT(!proc::coreSatisfiesContract2(2, 1, all), "Contract-1 core rejected for Contract 2");
    MIB_EXPECT(!proc::coreSatisfiesContract2(2, 2, all & ~MIB_PROCESSING_CAP_OBJECT_LAPLACIAN),
               "missing the Laplacian capability is rejected");
    MIB_EXPECT(!proc::coreSatisfiesContract2(2, 2, all & ~MIB_PROCESSING_CAP_ABSOLUTE_DIFFERENCE),
               "missing absolute difference is rejected");
    MIB_EXPECT(!proc::coreSatisfiesContract2(2, 2, 0u), "no capabilities is rejected");

    // ABI-v1 modules serve only Contract 1.
    MIB_EXPECT(proc::abiV1ServesContract(1, 1), "ABI v1 serves Contract 1");
    MIB_EXPECT(!proc::abiV1ServesContract(1, 2), "ABI v1 never serves Contract 2");
    MIB_EXPECT(!proc::abiV1ServesContract(2, 1), "ABI v2 module is not the v1 legacy path");

    // Host requests the right ABI per profile contract.
    MIB_EXPECT(proc::engineAbiForContract(2) == MIB_PROCESSING_ENGINE_ABI_VERSION_2,
               "Contract-2 profile requests ABI v2");
    MIB_EXPECT(proc::engineAbiForContract(1) == MIB_PROCESSING_ENGINE_ABI_VERSION,
               "Contract-1 profile requests ABI v1");

    return mib::test::exitCode();
}
