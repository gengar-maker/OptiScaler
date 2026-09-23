#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <string>

#include "ArcNrSyclApi.h"
#include "ArcNrSyclLoader.h"

// Correctness-first, same-frame D3D12 -> SYCL -> D3D12 boundary.
//
// Only call this for a command list and allocator owned by OptiScaler. On
// success the producer list has been submitted, the SYCL model has finished,
// its answer has been uploaded, and the SAME list/allocator have been reset
// into recording state for the caller's subsequent resolve and copy-back.
//
// This is deliberately not used for a game's open D3D12 command list. It
// stalls twice and copies via host memory; a shared-buffer implementation can
// optimize the same contract after Windows correctness tests pass.
// If Run fails after submitting the producer and its fence does not complete,
// the owned list remains closed and its allocator must not be reset. Callers
// must treat that as a frame-fatal error, not attempt normal close/submit.
class ArcNrSyclOwnedListBridge {
public:
    ArcNrSyclOwnedListBridge() = default;
    ~ArcNrSyclOwnedListBridge();
    ArcNrSyclOwnedListBridge(const ArcNrSyclOwnedListBridge&) = delete;
    ArcNrSyclOwnedListBridge& operator=(const ArcNrSyclOwnedListBridge&) = delete;

    bool Run(ID3D12Device* device, ID3D12CommandQueue* queue,
             ID3D12GraphicsCommandList* owned_list,
             ID3D12CommandAllocator* owned_allocator,
             ID3D12Resource* model_color,
             D3D12_RESOURCE_STATES color_state,
             ID3D12Resource* model_output,
             D3D12_RESOURCE_STATES output_state,
             ArcNrSyclLoader& provider,
             ArcNrSyclFrame frame,
             bool& list_recording,
             std::string& error);

private:
    bool WaitForQueue(ID3D12Device* device, ID3D12CommandQueue* queue,
                      std::string& error);
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    Microsoft::WRL::ComPtr<ID3D12Device> fence_device_;
    void* fence_event_ = nullptr;
    unsigned long long fence_value_ = 0;
};
