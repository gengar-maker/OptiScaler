#pragma once

// Versioned C boundary between OptiScaler (MSVC/D3D12) and a separately
// compiled Intel oneAPI SYCL/oneDNN neural-rendering module. This interface
// deliberately uses linear host buffers. A D3D12 readback/upload bridge may
// implement it first; a shared-buffer fast path can be added in a new ABI
// version without changing the neural model's tensor contract.
//
// IMPORTANT: calling Evaluate from an open D3D12 command list is invalid.
// The colour data must be read back only after the list that produced it has
// completed. Likewise, D3D12 may consume the output only after Evaluate has
// returned. The host is responsible for those fences and resource states.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ARC_NR_SYCL_API_VERSION 1u
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
