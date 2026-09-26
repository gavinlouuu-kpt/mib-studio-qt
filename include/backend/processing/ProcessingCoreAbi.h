#ifndef MIB_PROCESSING_CORE_ABI_H
#define MIB_PROCESSING_CORE_ABI_H

/*
 * Stable, C-compatible boundary for hot-swappable processing kernels.
 *
 * Nothing in this header exposes C++, Qt, OpenCV, exceptions, RTTI, or an
 * allocator.  The host owns every image/error buffer.  A plugin owns its
 * context and must not retain borrowed image pointers after a call returns.
 */

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  define MIB_PROCESSING_CALL __cdecl
#  if defined(MIB_PROCESSING_CORE_PLUGIN_EXPORTS)
#    define MIB_PROCESSING_EXPORT __declspec(dllexport)
#  else
#    define MIB_PROCESSING_EXPORT
#  endif
#else
#  define MIB_PROCESSING_CALL
#  if defined(MIB_PROCESSING_CORE_PLUGIN_EXPORTS)
#    define MIB_PROCESSING_EXPORT __attribute__((visibility("default")))
#  else
#    define MIB_PROCESSING_EXPORT
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define MIB_PROCESSING_ENGINE_ABI_VERSION 1u
#define MIB_PROCESSING_CONTRACT_VERSION 1u
#define MIB_PROCESSING_GET_API_SYMBOL "mib_processing_get_api"
#define MIB_PROCESSING_KERNEL_FLAG_ABSOLUTE_BACKGROUND_DIFFERENCE 0x1u

typedef enum mib_processing_status {
    MIB_PROCESSING_STATUS_OK = 0,
    MIB_PROCESSING_STATUS_INVALID_ARGUMENT = 1,
    MIB_PROCESSING_STATUS_ABI_MISMATCH = 2,
    MIB_PROCESSING_STATUS_BUFFER_TOO_SMALL = 3,
    MIB_PROCESSING_STATUS_PROCESSING_FAILED = 4,
    MIB_PROCESSING_STATUS_INTERNAL_ERROR = 5
} mib_processing_status;

typedef struct mib_processing_image_view {
    uint32_t struct_size;
    uint32_t width;
    uint32_t height;
    uint64_t stride_bytes;
    const uint8_t* data;
    uint64_t data_size_bytes;
} mib_processing_image_view;

typedef struct mib_processing_mutable_image_view {
    uint32_t struct_size;
    uint32_t width;
    uint32_t height;
    uint64_t stride_bytes;
    uint8_t* data;
    uint64_t data_size_bytes;
} mib_processing_mutable_image_view;

typedef struct mib_processing_roi {
    uint32_t struct_size;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} mib_processing_roi;

typedef struct mib_processing_kernel_config {
    uint32_t struct_size;
    int32_t gaussian_blur_size;
    int32_t background_subtract_threshold;
    int32_t morphology_kernel_size;
    int32_t morphology_iterations;
    int32_t empty_frame_pixel_threshold;
    uint32_t flags;
    uint32_t reserved_u32[10];
} mib_processing_kernel_config;

typedef struct mib_processing_core_descriptor {
    uint32_t struct_size;
    uint32_t engine_abi_version;
    uint32_t contract_version;
    uint32_t capabilities;
    const char* core_version;
    const char* build_id;
    const char* runtime_fingerprint;
    uint64_t reserved_u64[8];
} mib_processing_core_descriptor;

typedef void* mib_processing_context;

typedef const mib_processing_core_descriptor* (MIB_PROCESSING_CALL
    *mib_processing_descriptor_fn)(void);
typedef mib_processing_status (MIB_PROCESSING_CALL *mib_processing_create_context_fn)(
    mib_processing_context* out_context, char* error, size_t error_capacity);
typedef void (MIB_PROCESSING_CALL *mib_processing_destroy_context_fn)(
    mib_processing_context context);
typedef mib_processing_status (MIB_PROCESSING_CALL *mib_processing_reset_context_fn)(
    mib_processing_context context, char* error, size_t error_capacity);
typedef mib_processing_status (MIB_PROCESSING_CALL *mib_processing_process_mask_fn)(
    mib_processing_context context,
    const mib_processing_image_view* input,
    const mib_processing_image_view* background,
    const mib_processing_kernel_config* config,
    const mib_processing_roi* roi,
    mib_processing_mutable_image_view* output_mask,
    char* error,
    size_t error_capacity);
typedef mib_processing_status (MIB_PROCESSING_CALL *mib_processing_is_empty_fn)(
    mib_processing_context context,
    const mib_processing_image_view* input,
    const mib_processing_image_view* background,
    const mib_processing_kernel_config* config,
    const mib_processing_roi* roi,
    uint8_t* out_is_empty,
    char* error,
    size_t error_capacity);
typedef mib_processing_status (MIB_PROCESSING_CALL *mib_processing_self_test_fn)(
    char* error, size_t error_capacity);

typedef struct mib_processing_api {
    uint32_t struct_size;
    uint32_t engine_abi_version;
    mib_processing_descriptor_fn descriptor;
    mib_processing_create_context_fn create_context;
    mib_processing_destroy_context_fn destroy_context;
    mib_processing_reset_context_fn reset_context;
    mib_processing_process_mask_fn process_mask;
    mib_processing_is_empty_fn is_empty;
    mib_processing_self_test_fn self_test;
    void* reserved[8];
} mib_processing_api;

typedef mib_processing_status (MIB_PROCESSING_CALL *mib_processing_get_api_fn)(
    uint32_t requested_engine_abi,
    uint32_t host_api_struct_size,
    mib_processing_api* out_api,
    char* error,
    size_t error_capacity);

MIB_PROCESSING_EXPORT mib_processing_status MIB_PROCESSING_CALL mib_processing_get_api(
    uint32_t requested_engine_abi,
    uint32_t host_api_struct_size,
    mib_processing_api* out_api,
    char* error,
    size_t error_capacity);

/* ======================================================================== *
 *  Engine ABI v2 (Processing Contract 2)                                    *
 *                                                                           *
 *  Purely additive: every v1 type above keeps its exact layout so ABI-v1    *
 *  modules load unchanged. A Contract-2 core owns the full version-sensitive *
 *  pipeline (preprocessing filters, absolute difference, per-object         *
 *  Laplacian variance) and advertises it through capability flags.          *
 * ======================================================================== */

#define MIB_PROCESSING_ENGINE_ABI_VERSION_2 2u
#define MIB_PROCESSING_CONTRACT_VERSION_2 2u
#define MIB_PROCESSING_GET_API_V2_SYMBOL "mib_processing_get_api_v2"

/* Capability flags reported in mib_processing_core_descriptor.capabilities. */
#define MIB_PROCESSING_CAP_FULL_PIPELINE 0x1u
#define MIB_PROCESSING_CAP_ABSOLUTE_DIFFERENCE 0x2u
#define MIB_PROCESSING_CAP_FILTER_CHAIN 0x4u
#define MIB_PROCESSING_CAP_OBJECT_LAPLACIAN 0x8u

/* The capability set a Contract-2 profile requires of a core. */
#define MIB_PROCESSING_CAP_CONTRACT2_REQUIRED                                   \
    (MIB_PROCESSING_CAP_FULL_PIPELINE | MIB_PROCESSING_CAP_ABSOLUTE_DIFFERENCE | \
     MIB_PROCESSING_CAP_FILTER_CHAIN | MIB_PROCESSING_CAP_OBJECT_LAPLACIAN)

typedef enum mib_processing_filter_kind {
    MIB_PROCESSING_FILTER_IDENTITY = 0,
    MIB_PROCESSING_FILTER_INVERT = 1,
    MIB_PROCESSING_FILTER_LINEAR_CONTRAST = 2,
    MIB_PROCESSING_FILTER_GAMMA = 3,
    MIB_PROCESSING_FILTER_CLAHE = 4
} mib_processing_filter_kind;

typedef struct mib_processing_filter_stage {
    uint32_t struct_size;
    uint32_t kind; /* mib_processing_filter_kind */
    double alpha;
    double beta;
    double gamma;
    double clip_limit;
    int32_t tile_grid_size;
    uint32_t reserved_u32[5];
} mib_processing_filter_stage;

/* Ordered filter phases. The stage arrays are host-owned for the duration of
 * the call; the plugin must not retain the pointers after it returns. */
typedef struct mib_processing_filter_chain {
    uint32_t struct_size;
    uint32_t input_stage_count;
    uint32_t difference_stage_count;
    uint32_t reserved_u32;
    const mib_processing_filter_stage* input_stages;
    const mib_processing_filter_stage* difference_stages;
} mib_processing_filter_chain;

typedef struct mib_processing_kernel_config_v2 {
    uint32_t struct_size;
    int32_t gaussian_blur_size;
    int32_t difference_threshold; /* canonical; replaces bg_subtract_threshold */
    int32_t morphology_kernel_size;
    int32_t morphology_iterations;
    int32_t empty_frame_pixel_threshold;
    int32_t laplacian_kernel_size;
    uint32_t flags; /* MIB_PROCESSING_KERNEL_FLAG_* */
    const mib_processing_filter_chain* filters; /* NULL == identity */
    uint32_t reserved_u32[16];
} mib_processing_kernel_config_v2;

/* One detected object's metrics (POD). Ring width is intentionally absent. */
typedef struct mib_processing_object_metrics {
    uint32_t struct_size;
    int32_t object_id;
    int32_t object_count;
    int32_t is_valid;
    int32_t touches_border;
    int32_t is_target_group;
    int32_t track_id;
    double area;
    double deformability;
    double area_ratio;
    double laplacian_variance;
    double youngs_modulus;
    double centroid_x;
    double centroid_y;
    double bbox_x;
    double bbox_y;
    double bbox_width;
    double bbox_height;
    double brightness_q1;
    double brightness_q2;
    double brightness_q3;
    double brightness_q4;
    uint32_t reserved_u32[8];
} mib_processing_object_metrics;

/* Host-owned per-object output buffer. The plugin writes at most `capacity`
 * objects and always reports `required`, so a too-small buffer is a
 * deterministic BUFFER_TOO_SMALL with no cross-boundary allocation. */
typedef struct mib_processing_object_buffer {
    uint32_t struct_size;
    uint32_t capacity;
    uint32_t count;
    uint32_t required;
    mib_processing_object_metrics* objects;
} mib_processing_object_buffer;

/* Full Contract-2 pipeline: preprocessing + absolute difference + mask +
 * per-object metrics in one call. `output_mask` may be NULL when the host only
 * wants object metrics. */
typedef mib_processing_status (MIB_PROCESSING_CALL *mib_processing_process_objects_fn)(
    mib_processing_context context,
    const mib_processing_image_view* input,
    const mib_processing_image_view* background,
    const mib_processing_kernel_config_v2* config,
    const mib_processing_roi* roi,
    double pixel_to_micron_factor,
    mib_processing_mutable_image_view* output_mask,
    mib_processing_object_buffer* out_objects,
    char* error,
    size_t error_capacity);

typedef struct mib_processing_api_v2 {
    uint32_t struct_size;
    uint32_t engine_abi_version; /* == MIB_PROCESSING_ENGINE_ABI_VERSION_2 */
    mib_processing_descriptor_fn descriptor;
    mib_processing_create_context_fn create_context;
    mib_processing_destroy_context_fn destroy_context;
    mib_processing_reset_context_fn reset_context;
    mib_processing_process_mask_fn process_mask;         /* v1-compatible mask */
    mib_processing_is_empty_fn is_empty;                 /* v1-compatible empty */
    mib_processing_process_objects_fn process_objects;   /* full v2 pipeline */
    mib_processing_self_test_fn self_test;
    void* reserved[8];
} mib_processing_api_v2;

typedef mib_processing_status (MIB_PROCESSING_CALL *mib_processing_get_api_v2_fn)(
    uint32_t requested_engine_abi,
    uint32_t host_api_struct_size,
    mib_processing_api_v2* out_api,
    char* error,
    size_t error_capacity);

#ifdef __cplusplus
}
#endif

#endif /* MIB_PROCESSING_CORE_ABI_H */
