#pragma once

// Versioned C boundary between OptiScaler (MSVC/D3D12) and a separately
// compiled Intel oneAPI SYCL/oneDNN neural-rendering module. This interface
// supports both host test buffers and Windows NT shared GPU heap handles.
//
// IMPORTANT: calling EvaluateGpu from an open D3D12 command list is invalid.
// The producer list must complete before SYCL accesses the shared heap.
// D3D12 may consume output only after EvaluateGpu has returned. The host
// owns both fences and all resource-state transitions.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ARC_NR_SYCL_API_VERSION 5u
#define ARC_NR_SYCL_GET_API_NAME "arc_nr_sycl_get_api"

typedef enum ArcNrSyclStatus {
    ARC_NR_SYCL_OK = 0,
    ARC_NR_SYCL_INVALID_ARGUMENT = 1,
    ARC_NR_SYCL_UNSUPPORTED_DEVICE = 2,
    ARC_NR_SYCL_MODEL_UNAVAILABLE = 3,
    ARC_NR_SYCL_INFERENCE_FAILED = 4,
} ArcNrSyclStatus;

typedef struct ArcNrSyclCreate {
    uint32_t struct_size;
    uint32_t width;
    uint32_t height;
    // UTF-8 directories containing the exported MLX logical and derived
    // weights. The callee copies these paths during Create.
    const char* logical_weights_directory;
    const char* derived_weights_directory;
} ArcNrSyclCreate;

typedef struct ArcNrSyclFrame {
    uint32_t struct_size;
    uint32_t frame_index;
    uint32_t reset_history;
    // Bit 0: GPU-estimate motion from consecutive color frames when no
    // caller motion texture is available. Other bits are reserved.
    uint32_t reserved;
    // Exactly width*height*4 FP16 values, tightly packed RGBA. Colour is
    // normalized before model feature construction. Output is normalized
    // RGB with alpha 1. The caller owns both buffers until Evaluate returns.
    const uint16_t* color_rgba_f16;
    uint16_t* output_rgba_f16;
    // Optional temporal inputs. Either both are null, or both are present.
    // History is the preceding output, RGBA FP16. Motion has exactly
    // width*height*2 FP16 values, in normalized history-UV units.
    const uint16_t* previous_output_rgba_f16;
    const uint16_t* motion_rg_f16;
    float normalized_style;
    float local_tone;
    float local_structure;
    float skin_structure;
    float automatic_structure;
    float intensity;
} ArcNrSyclFrame;

// Both handles refer to D3D12 cross-adapter shared heaps. Each contains a
// placed buffer at offset zero with D3D12's RGBA16F copyable footprint.
// The caller waits for the input D3D12 copy before EvaluateGpu, then submits
// the output D3D12 copy only after EvaluateGpu returns. No host pixel transfer.
typedef struct ArcNrSyclGpuTimings {
    uint64_t import_us;
    uint64_t input_us;
    uint64_t prepare_us;
    uint64_t model_us;
    uint64_t compose_us;
    uint64_t output_us;
    uint64_t release_us;
    uint64_t total_us;
    uint32_t temporal_active;
} ArcNrSyclGpuTimings;

typedef struct ArcNrSyclGpuFrame {
    uint32_t struct_size;
    ArcNrSyclFrame controls;
    void* color_heap_nt_handle;
    void* output_heap_nt_handle;
    uint64_t heap_bytes;
    uint32_t row_pitch_bytes;
    // 0: RGBA16F, 1: RGBA8 UNORM, 2: BGRA8 UNORM,
    // 3: RGB10A2 UNORM. Packed formats allow native-resolution D3D11
    // swapchain processing without CPU staging.
    uint32_t pixel_format;
    // Optional caller-owned timing output. All values include GPU waits.
    ArcNrSyclGpuTimings* timings;
    // Optional motion-vector texture copied into a second shared GPU heap.
    // Format 1 is RG16F, format 2 is RG32F. The usable render subrect starts
    // at motion_base_x/y and is sampled at the SNR output resolution. Scales
    // convert stored vector components to motion-subrect pixel displacement.
    void* motion_heap_nt_handle;
    uint64_t motion_heap_bytes;
    uint32_t motion_row_pitch_bytes;
    uint32_t motion_width;
    uint32_t motion_height;
    uint32_t motion_base_x;
    uint32_t motion_base_y;
    uint32_t motion_subrect_width;
    uint32_t motion_subrect_height;
    uint32_t motion_format;
    float motion_scale_x;
    float motion_scale_y;
} ArcNrSyclGpuFrame;

typedef struct ArcNrSyclApi {
    uint32_t struct_size;
    uint32_t version;
    // On failure, Create returns null and writes a NUL-terminated diagnostic
    // if error_bytes > 0. Destroy accepts null. Evaluate is synchronous.
    void* (*create)(const ArcNrSyclCreate* create, char* error,
                    uint32_t error_bytes);
    ArcNrSyclStatus (*evaluate)(void* session, const ArcNrSyclFrame* frame,
                                 char* error, uint32_t error_bytes);
    void (*destroy)(void* session);
    ArcNrSyclStatus (*evaluate_gpu)(void* session,
                                    const ArcNrSyclGpuFrame* frame,
                                    char* error, uint32_t error_bytes);
} ArcNrSyclApi;

// Exported by arc_nr_sycl.dll. Returns 1 only when the version, size, and
// required function pointers are supported. OptiScaler leaves its NVIDIA
// backend untouched if this module is absent.
typedef int (*PFN_ArcNrSyclGetApi)(uint32_t requested_version,
                                    ArcNrSyclApi* out_api);
int arc_nr_sycl_get_api(uint32_t requested_version, ArcNrSyclApi* out_api);

#ifdef __cplusplus
}
#endif
