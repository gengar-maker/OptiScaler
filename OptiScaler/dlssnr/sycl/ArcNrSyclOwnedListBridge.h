#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <string>

#include "NrGpuTimer.h"
#include "ArcNrSyclApi.h"
#include "ArcNrSyclLoader.h"

// Correctness-first, same-frame D3D12 -> SYCL -> D3D12 boundary.
//
// Only call this for a command list and allocator owned by OptiScaler. On
// success the producer list has been submitted, the SYCL model has finished,
// its answer has been copied from a shared GPU buffer, and the SAME list/allocator have been reset
// into recording state for the caller's subsequent resolve and copy-back.
//
// This is deliberately not used for a game's open D3D12 command list. It
// stalls twice for explicit queue fences and uses D3D12/SYCL shared GPU heaps.
// If Run fails after submitting the producer and its fence does not complete,
// the owned list remains closed and its allocator must not be reset. Callers
// must treat that as a frame-fatal error, not attempt normal close/submit.
class ArcNrSyclOwnedListBridge
{
  public:
    ArcNrSyclOwnedListBridge() = default;
    ~ArcNrSyclOwnedListBridge();
    ArcNrSyclOwnedListBridge(const ArcNrSyclOwnedListBridge&) = delete;
    ArcNrSyclOwnedListBridge& operator=(const ArcNrSyclOwnedListBridge&) = delete;

    // defer_output_copy records the copy on the returned owned list. The
    // caller must submit and complete that list before reusing the bridge.
    // This batches copy-back and HDR resolve without an extra CPU fence.
    bool Run(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* owned_list,
             ID3D12CommandAllocator* owned_allocator, ID3D12Resource* model_color, D3D12_RESOURCE_STATES color_state,
             ID3D12Resource* model_output, D3D12_RESOURCE_STATES output_state, ID3D12Resource* motion,
             UINT motion_subrect_width, UINT motion_subrect_height, UINT motion_base_x, UINT motion_base_y,
             float motion_scale_x, float motion_scale_y, ArcNrSyclLoader& provider, ArcNrSyclFrame frame,
             bool& list_recording, std::string& error, bool provider_estimated_motion = false,
             NrGpuTimer* gpu_timer = nullptr, bool defer_output_copy = false, bool local_shared_heap = false);
    bool Flush(ID3D12Device* device, ID3D12CommandQueue* queue, std::string& error)
    {
        return WaitForQueue(device, queue, error);
    }
    // Release shared heaps and synchronization objects when a D3D11 Present
    // session is switched off or rebuilt. The bridge object itself is reused.
    void Reset();
    const ArcNrSyclGpuTimings& LastProviderTimings() const { return last_timings_; }

  private:
    ArcNrSyclGpuTimings last_timings_ {};
    bool WaitForQueue(ID3D12Device* device, ID3D12CommandQueue* queue, std::string& error);
    void ClearSharedCache();
    Microsoft::WRL::ComPtr<ID3D12Device> cache_device_;
    unsigned long long cache_color_bytes_ = 0;
    unsigned long long cache_motion_bytes_ = 0;
    bool cache_ready_ = false;
    bool cache_local_shared_ = false;
    Microsoft::WRL::ComPtr<ID3D12Heap> cache_color_heap_;
    Microsoft::WRL::ComPtr<ID3D12Heap> cache_output_heap_;
    Microsoft::WRL::ComPtr<ID3D12Heap> cache_motion_heap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> cache_color_buffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> cache_output_buffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> cache_motion_buffer_;
    void* cache_color_handle_ = nullptr;
    void* cache_output_handle_ = nullptr;
    void* cache_motion_handle_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    Microsoft::WRL::ComPtr<ID3D12Device> fence_device_;
    void* fence_event_ = nullptr;
    unsigned long long fence_value_ = 0;
};
