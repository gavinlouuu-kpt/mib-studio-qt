/* V2-5 proof: engine ABI v2 is a valid POD C surface that coexists with ABI v1
 * (v1 layout unchanged) and advertises the Contract-2 capabilities. */
#include "backend/processing/ProcessingCoreAbi.h"

#include <stddef.h>

/* ABI v1 is untouched. */
_Static_assert(MIB_PROCESSING_ENGINE_ABI_VERSION == 1u, "ABI v1 changed");
_Static_assert(sizeof(mib_processing_kernel_config) == 68u, "v1 config size changed");
_Static_assert(offsetof(mib_processing_kernel_config, flags) == 24u, "v1 flag offset changed");

/* ABI v2 constants. */
_Static_assert(MIB_PROCESSING_ENGINE_ABI_VERSION_2 == 2u, "ABI v2 version");
_Static_assert(MIB_PROCESSING_CONTRACT_VERSION_2 == 2u, "contract v2 version");

/* Capability flags are distinct single bits and combine to the required set. */
_Static_assert(MIB_PROCESSING_CAP_FULL_PIPELINE == 0x1u, "cap bit");
_Static_assert(MIB_PROCESSING_CAP_ABSOLUTE_DIFFERENCE == 0x2u, "cap bit");
_Static_assert(MIB_PROCESSING_CAP_FILTER_CHAIN == 0x4u, "cap bit");
_Static_assert(MIB_PROCESSING_CAP_OBJECT_LAPLACIAN == 0x8u, "cap bit");
_Static_assert(MIB_PROCESSING_CAP_CONTRACT2_REQUIRED == 0xFu, "required cap set");

/* Every v2 struct is size-versioned (starts with struct_size). */
_Static_assert(offsetof(mib_processing_filter_stage, struct_size) == 0u, "filter stage");
_Static_assert(offsetof(mib_processing_filter_chain, struct_size) == 0u, "filter chain");
_Static_assert(offsetof(mib_processing_kernel_config_v2, struct_size) == 0u, "config v2");
_Static_assert(offsetof(mib_processing_object_metrics, struct_size) == 0u, "object metrics");
_Static_assert(offsetof(mib_processing_object_buffer, struct_size) == 0u, "object buffer");
_Static_assert(offsetof(mib_processing_api_v2, struct_size) == 0u, "api v2");
_Static_assert(offsetof(mib_processing_api_v2, engine_abi_version) == 4u, "api v2 version slot");

/* The per-object result advertises the v2 focus metric and no ring field. */
_Static_assert(sizeof(((mib_processing_object_metrics*)0)->laplacian_variance) == sizeof(double),
               "laplacian variance is a double");

int main(void) {
    mib_processing_api_v2 api = {0};
    mib_processing_kernel_config_v2 config = {0};
    mib_processing_object_buffer buffer = {0};
    api.struct_size = (uint32_t)sizeof(api);
    api.engine_abi_version = MIB_PROCESSING_ENGINE_ABI_VERSION_2;
    config.struct_size = (uint32_t)sizeof(config);
    buffer.struct_size = (uint32_t)sizeof(buffer);

    /* A too-small buffer is expressible without allocation: capacity < required. */
    buffer.capacity = 0u;
    buffer.required = 3u;

    return (api.engine_abi_version == 2u && config.struct_size > 0u &&
            buffer.required > buffer.capacity && MIB_PROCESSING_GET_API_V2_SYMBOL[0] == 'm')
               ? 0
               : 1;
}
