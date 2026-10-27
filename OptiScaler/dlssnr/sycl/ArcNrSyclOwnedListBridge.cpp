#include <pch.h>
#include "ArcNrSyclOwnedListBridge.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>

namespace {

using Microsoft::WRL::ComPtr;

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (before == after) return;
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
}

D3D12_RESOURCE_DESC BufferDesc(UINT64 bytes) {
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
    return desc;
}

D3D12_HEAP_PROPERTIES Heap(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES properties {};
    properties.Type = type;
    properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

UINT ColorFormat(ID3D12Resource* resource, UINT width, UINT height) {
    if (resource == nullptr) return UINT_MAX;
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.Width != width || desc.Height != height ||
        desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1 ||
        desc.MipLevels != 1) return UINT_MAX;
    switch (desc.Format) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return 0;
    case DXGI_FORMAT_R10G10B10A2_UNORM: return 3;
    case DXGI_FORMAT_R8G8B8A8_UNORM: return 1;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return 1;
    case DXGI_FORMAT_B8G8R8A8_UNORM: return 2;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return 2;
    default: return UINT_MAX;
    }
}

UINT MotionFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_TYPELESS:
        return 1;
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_TYPELESS:
        return 2;
    default:
        return 0;
    }
}

std::string HResult(const char* operation, HRESULT result) {
    return std::string(operation) + " failed with HRESULT " +
           std::to_string(static_cast<unsigned long>(result));
}

}  // namespace

ArcNrSyclOwnedListBridge::~ArcNrSyclOwnedListBridge() {
    ClearSharedCache();
    if (fence_event_ != nullptr)
        CloseHandle(static_cast<HANDLE>(fence_event_));
}

void ArcNrSyclOwnedListBridge::ClearSharedCache() {
    if (cache_color_handle_) CloseHandle(static_cast<HANDLE>(cache_color_handle_));
    if (cache_output_handle_) CloseHandle(static_cast<HANDLE>(cache_output_handle_));
    if (cache_motion_handle_) CloseHandle(static_cast<HANDLE>(cache_motion_handle_));
    cache_color_handle_ = nullptr;
    cache_output_handle_ = nullptr;
    cache_motion_handle_ = nullptr;
    cache_color_buffer_.Reset();
    cache_output_buffer_.Reset();
    cache_motion_buffer_.Reset();
    cache_color_heap_.Reset();
    cache_output_heap_.Reset();
    cache_motion_heap_.Reset();
    cache_device_.Reset();
    cache_color_bytes_ = 0;
    cache_motion_bytes_ = 0;
    cache_ready_ = false;
}

bool ArcNrSyclOwnedListBridge::WaitForQueue(ID3D12Device* device,
                                             ID3D12CommandQueue* queue,
                                             std::string& error) {
    // A bridge may be reused after the game recreates its D3D12 device. A
    // fence from the old device is not valid on the new queue.
    if (fence_device_.Get() != device) {
        fence_.Reset();
        fence_device_ = device;
        fence_value_ = 0;
    }
    if (fence_ == nullptr) {
        const HRESULT result = device->CreateFence(
            0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
        if (FAILED(result)) {
            error = HResult("CreateFence", result);
            return false;
        }
    }
    if (fence_event_ == nullptr) {
        fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (fence_event_ == nullptr) {
            error = "CreateEvent failed";
            return false;
        }
    }
    const UINT64 value = ++fence_value_;
    HRESULT result = queue->Signal(fence_.Get(), value);
    if (FAILED(result)) {
        error = HResult("queue Signal", result);
        return false;
    }
    result = fence_->SetEventOnCompletion(value, static_cast<HANDLE>(fence_event_));
    if (FAILED(result)) {
        error = HResult("SetEventOnCompletion", result);
        return false;
    }
    const DWORD wait = WaitForSingleObject(static_cast<HANDLE>(fence_event_), 30000);
    if (wait != WAIT_OBJECT_0) {
        error = "D3D12 producer/upload fence timed out or failed";
        return false;
    }
    return true;
}

bool ArcNrSyclOwnedListBridge::Run(
    ID3D12Device* device, ID3D12CommandQueue* queue,
    ID3D12GraphicsCommandList* owned_list,
    ID3D12CommandAllocator* owned_allocator,
    ID3D12Resource* model_color, D3D12_RESOURCE_STATES color_state,
    ID3D12Resource* model_output, D3D12_RESOURCE_STATES output_state,
    ID3D12Resource* motion,
    UINT motion_subrect_width, UINT motion_subrect_height,
    UINT motion_base_x, UINT motion_base_y,
    float motion_scale_x, float motion_scale_y,
    ArcNrSyclLoader& provider, ArcNrSyclFrame frame,
    bool& list_recording,
    std::string& error,
    bool provider_estimated_motion) {
    list_recording = true;
    if (!device || !queue || !owned_list || !owned_allocator ||
        !provider.Ready() || frame.struct_size != sizeof(frame)) {
        error = "invalid owned-list SYCL bridge arguments";
        return false;
    }
    const auto color_desc = model_color ? model_color->GetDesc() : D3D12_RESOURCE_DESC{};
    const UINT width = static_cast<UINT>(color_desc.Width);
    const UINT height = color_desc.Height;
    const UINT color_format = ColorFormat(model_color, width, height);
    if (width == 0 || height == 0 || width > 8192 || height > 8192 ||
        color_format == UINT_MAX ||
        ColorFormat(model_output, width, height) != color_format) {
        error = "SYCL shared GPU path needs matching single-sample RGBA16F/RGBA8/BGRA8/RGB10A2 textures";
        return false;
    }
    if (queue->GetDesc().Type != owned_list->GetType() ||
        owned_list->GetType() == D3D12_COMMAND_LIST_TYPE_COPY) {
        error = "owned command list and queue types do not match";
        return false;
    }

    const D3D12_RESOURCE_DESC motion_desc =
        motion ? motion->GetDesc() : D3D12_RESOURCE_DESC{};
    const UINT motion_format = MotionFormat(motion_desc.Format);
    const bool motion_usable = motion != nullptr && motion_format != 0 &&
        motion_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        motion_desc.DepthOrArraySize == 1 && motion_desc.MipLevels == 1 &&
        motion_desc.SampleDesc.Count == 1 &&
        motion_desc.Width <= 8192 && motion_desc.Height != 0 &&
        motion_desc.Height <= 8192 &&
        motion_base_x < motion_desc.Width && motion_base_y < motion_desc.Height &&
        motion_subrect_width != 0 && motion_subrect_height != 0 &&
        std::isfinite(motion_scale_x) && std::isfinite(motion_scale_y);
    if (!motion_usable) {
        if (!provider_estimated_motion)
            frame.reset_history = 1;
        if (motion != nullptr) {
            static std::atomic<bool> warned {false};
            if (!warned.exchange(true))
                LOG_WARN("SYCL temporal history disabled: unsupported motion texture format {} or dimensions {}x{}",
                         static_cast<int>(motion_desc.Format), motion_desc.Width, motion_desc.Height);
        }
    }
    motion_subrect_width = motion_usable ? std::min<UINT>(
        motion_subrect_width, static_cast<UINT>(motion_desc.Width) - motion_base_x) : 0;
    motion_subrect_height = motion_usable ? std::min<UINT>(
        motion_subrect_height, motion_desc.Height - motion_base_y) : 0;
    if (motion_usable) {
        static std::atomic<bool> reported {false};
        if (!reported.exchange(true))
            LOG_INFO("SYCL motion input: DXGI format {}, texture {}x{}, active {}x{} at {},{}, scales {}x{}",
                     static_cast<int>(motion_desc.Format), motion_desc.Width,
                     motion_desc.Height, motion_subrect_width,
                     motion_subrect_height, motion_base_x, motion_base_y,
                     motion_scale_x, motion_scale_y);
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT rows = 0;
    UINT64 row_bytes = 0, total_bytes = 0;
    device->GetCopyableFootprints(&color_desc, 0, 1, 0, &footprint,
                                  &rows, &row_bytes, &total_bytes);
    const UINT bytes_per_pixel = color_format == 0 ? 8 : 4;
    if (rows != height || row_bytes != static_cast<UINT64>(width) * bytes_per_pixel ||
        total_bytes == 0) {
        error = "unexpected color copy footprint";
        return false;
    }
    const auto buffer_desc = BufferDesc(total_bytes);
    const auto allocation = device->GetResourceAllocationInfo(0, 1, &buffer_desc);
    if (allocation.SizeInBytes == UINT64_MAX) {
        error = "invalid shared GPU buffer allocation";
        return false;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT motion_footprint {};
    UINT motion_rows = 0;
    UINT64 motion_row_bytes = 0, motion_total_bytes = 0;
    D3D12_RESOURCE_DESC motion_buffer_desc {};
    D3D12_RESOURCE_ALLOCATION_INFO motion_allocation {};
    if (motion_usable) {
        device->GetCopyableFootprints(&motion_desc, 0, 1, 0, &motion_footprint,
                                      &motion_rows, &motion_row_bytes,
                                      &motion_total_bytes);
        const UINT bytes_per_pixel = motion_format == 1 ? 4 : 8;
        if (motion_rows != motion_desc.Height ||
            motion_row_bytes != motion_desc.Width *
                static_cast<UINT64>(bytes_per_pixel) ||
            motion_total_bytes == 0) {
            error = "unexpected motion copy footprint";
            return false;
        }
        motion_buffer_desc = BufferDesc(motion_total_bytes);
        motion_allocation = device->GetResourceAllocationInfo(
            0, 1, &motion_buffer_desc);
        if (motion_allocation.SizeInBytes == UINT64_MAX) {
            error = "invalid shared motion buffer allocation";
            return false;
        }
    }
    using Clock = std::chrono::steady_clock;
    const auto profile_start = Clock::now();
    const auto milliseconds = [](Clock::time_point begin,
                                 Clock::time_point end) {
        return std::chrono::duration<double, std::milli>(end - begin).count();
    };
    D3D12_HEAP_DESC heap_desc {};
    heap_desc.SizeInBytes = allocation.SizeInBytes;
    heap_desc.Alignment = allocation.Alignment;
    heap_desc.Properties = Heap(D3D12_HEAP_TYPE_DEFAULT);
    heap_desc.Flags = D3D12_HEAP_FLAG_SHARED |
                      D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
    const unsigned long long desired_motion_bytes =
        motion_usable ? motion_total_bytes : 0;
    if (!cache_ready_ || cache_device_.Get() != device ||
        cache_color_bytes_ != total_bytes ||
        cache_motion_bytes_ != desired_motion_bytes) {
        ClearSharedCache();
        cache_device_ = device;
        HRESULT create_result = device->CreateHeap(
            &heap_desc, IID_PPV_ARGS(&cache_color_heap_));
        if (SUCCEEDED(create_result))
            create_result = device->CreateHeap(
                &heap_desc, IID_PPV_ARGS(&cache_output_heap_));
        if (SUCCEEDED(create_result))
            create_result = device->CreatePlacedResource(
                cache_color_heap_.Get(), 0, &buffer_desc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&cache_color_buffer_));
        if (SUCCEEDED(create_result))
            create_result = device->CreatePlacedResource(
                cache_output_heap_.Get(), 0, &buffer_desc,
                D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr,
                IID_PPV_ARGS(&cache_output_buffer_));
        if (SUCCEEDED(create_result) && motion_usable) {
            D3D12_HEAP_DESC motion_heap_desc = heap_desc;
            motion_heap_desc.SizeInBytes = motion_allocation.SizeInBytes;
            motion_heap_desc.Alignment = motion_allocation.Alignment;
            create_result = device->CreateHeap(
                &motion_heap_desc, IID_PPV_ARGS(&cache_motion_heap_));
            if (SUCCEEDED(create_result))
                create_result = device->CreatePlacedResource(
                    cache_motion_heap_.Get(), 0, &motion_buffer_desc,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                    IID_PPV_ARGS(&cache_motion_buffer_));
        }
        if (FAILED(create_result)) {
            error = HResult("shared GPU buffer allocation", create_result);
            ClearSharedCache();
            return false;
        }
        create_result = device->CreateSharedHandle(
            cache_color_heap_.Get(), nullptr, GENERIC_ALL, nullptr,
            reinterpret_cast<HANDLE*>(&cache_color_handle_));
        if (SUCCEEDED(create_result))
            create_result = device->CreateSharedHandle(
                cache_output_heap_.Get(), nullptr, GENERIC_ALL, nullptr,
                reinterpret_cast<HANDLE*>(&cache_output_handle_));
        if (SUCCEEDED(create_result) && motion_usable)
            create_result = device->CreateSharedHandle(
                cache_motion_heap_.Get(), nullptr, GENERIC_ALL, nullptr,
                reinterpret_cast<HANDLE*>(&cache_motion_handle_));
        if (FAILED(create_result)) {
            error = HResult("shared GPU handle creation", create_result);
            ClearSharedCache();
            return false;
        }
        cache_color_bytes_ = total_bytes;
        cache_motion_bytes_ = desired_motion_bytes;
        cache_ready_ = true;
    }
    auto& color_buffer = cache_color_buffer_;
    auto& output_buffer = cache_output_buffer_;
    auto& motion_buffer = cache_motion_buffer_;
    HRESULT result;
    const auto allocated = Clock::now();

    D3D12_TEXTURE_COPY_LOCATION source {};
    source.pResource = model_color;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION destination {};
    destination.pResource = color_buffer.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    Transition(owned_list, model_color, color_state,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    owned_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    Transition(owned_list, model_color, D3D12_RESOURCE_STATE_COPY_SOURCE,
               color_state);
    if (motion_usable) {
        D3D12_TEXTURE_COPY_LOCATION motion_source {};
        motion_source.pResource = motion;
        motion_source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        motion_source.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION motion_dest {};
        motion_dest.pResource = motion_buffer.Get();
        motion_dest.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        motion_dest.PlacedFootprint = motion_footprint;
        Transition(owned_list, motion,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        owned_list->CopyTextureRegion(&motion_dest, 0, 0, 0,
                                      &motion_source, nullptr);
        Transition(owned_list, motion, D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    result = owned_list->Close();
    if (FAILED(result)) {
        // Close failure leaves the list's state unusable for normal recording.
        list_recording = false;
        error = HResult("producer Close", result);
        return false;
    }
    list_recording = false;
    ID3D12CommandList* producer_lists[] = { owned_list };
    cache_ready_ = false;
    queue->ExecuteCommandLists(1, producer_lists);
    if (!WaitForQueue(device, queue, error)) return false;
    const auto producer_ready = Clock::now();

    // From here the producer submission is complete and the caller's list is
    // closed. Restore it to recording state even when GPU import or inference
    // fails, so the bridge caller can finish its own list without recording
    // against a closed object. A failed fence wait above is different: the
    // allocator may still be in use and must not be reset.
    const auto fail_after_producer = [&](const std::string& reason) {
        HRESULT resume = owned_allocator->Reset();
        if (SUCCEEDED(resume))
            resume = owned_list->Reset(owned_allocator, nullptr);
        list_recording = SUCCEEDED(resume);
        error = reason;
        if (FAILED(resume))
            error += "; owned list could not be resumed (" +
                     HResult("Reset", resume) + ")";
        return false;
    };

    ArcNrSyclGpuFrame gpu_frame {};
    gpu_frame.struct_size = sizeof(gpu_frame);
    gpu_frame.controls = frame;
    gpu_frame.color_heap_nt_handle = cache_color_handle_;
    gpu_frame.output_heap_nt_handle = cache_output_handle_;
    gpu_frame.heap_bytes = allocation.SizeInBytes;
    gpu_frame.row_pitch_bytes = footprint.Footprint.RowPitch;
    gpu_frame.pixel_format = color_format;
    if (motion_usable) {
        gpu_frame.motion_heap_nt_handle = cache_motion_handle_;
        gpu_frame.motion_heap_bytes = motion_allocation.SizeInBytes;
        gpu_frame.motion_row_pitch_bytes = motion_footprint.Footprint.RowPitch;
        gpu_frame.motion_width = static_cast<UINT>(motion_desc.Width);
        gpu_frame.motion_height = motion_desc.Height;
        gpu_frame.motion_base_x = motion_base_x;
        gpu_frame.motion_base_y = motion_base_y;
        gpu_frame.motion_subrect_width = motion_subrect_width;
        gpu_frame.motion_subrect_height = motion_subrect_height;
        gpu_frame.motion_format = motion_format;
        gpu_frame.motion_scale_x = motion_scale_x;
        gpu_frame.motion_scale_y = motion_scale_y;
    }
    ArcNrSyclGpuTimings sycl_timings {};
    gpu_frame.timings = &sycl_timings;
    if (provider.EvaluateGpu(gpu_frame, error) != ARC_NR_SYCL_OK)
        return fail_after_producer(error);
    const auto provider_ready = Clock::now();

    ComPtr<ID3D12CommandAllocator> upload_allocator;
    ComPtr<ID3D12GraphicsCommandList> upload_list;
    result = device->CreateCommandAllocator(queue->GetDesc().Type,
                                             IID_PPV_ARGS(&upload_allocator));
    if (SUCCEEDED(result))
        result = device->CreateCommandList(0, queue->GetDesc().Type,
                                            upload_allocator.Get(), nullptr,
                                            IID_PPV_ARGS(&upload_list));
    if (FAILED(result)) {
        return fail_after_producer(
            HResult("upload command list creation", result));
    }
    source = {};
    source.pResource = output_buffer.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprint;
    destination = {};
    destination.pResource = model_output;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = 0;
    Transition(upload_list.Get(), model_output, output_state,
               D3D12_RESOURCE_STATE_COPY_DEST);
    upload_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    Transition(upload_list.Get(), model_output, D3D12_RESOURCE_STATE_COPY_DEST,
               output_state);
    result = upload_list->Close();
    if (FAILED(result)) {
        return fail_after_producer(HResult("upload Close", result));
    }
    ID3D12CommandList* upload_lists[] = { upload_list.Get() };
    queue->ExecuteCommandLists(1, upload_lists);
    if (!WaitForQueue(device, queue, error)) return false;
    cache_ready_ = true;
    const auto output_ready = Clock::now();

    result = owned_allocator->Reset();
    if (SUCCEEDED(result))
        result = owned_list->Reset(owned_allocator, nullptr);
    if (FAILED(result)) {
        error = HResult("resume owned command list", result);
        return false;
    }
    list_recording = true;
    static std::atomic<unsigned int> profile_frame {0};
    const unsigned int frame_number = profile_frame.fetch_add(1);
    if (frame_number == 0 || frame_number % 30 == 0) {
        LOG_INFO("NR-PROFILE frame={} {}x{} temporal={} motion={} alloc={:.2f}ms producer={:.2f}ms provider={:.2f}ms output={:.2f}ms; SYCL import={:.2f} input={:.2f} prepare={:.2f} model={:.2f} compose={:.2f} export={:.2f} release={:.2f} total={:.2f}ms",
                 frame_number, width, height, sycl_timings.temporal_active,
                 motion_format,
                 milliseconds(profile_start, allocated),
                 milliseconds(allocated, producer_ready),
                 milliseconds(producer_ready, provider_ready),
                 milliseconds(provider_ready, output_ready),
                 sycl_timings.import_us / 1000.0,
                 sycl_timings.input_us / 1000.0,
                 sycl_timings.prepare_us / 1000.0,
                 sycl_timings.model_us / 1000.0,
                 sycl_timings.compose_us / 1000.0,
                 sycl_timings.output_us / 1000.0,
                 sycl_timings.release_us / 1000.0,
                 sycl_timings.total_us / 1000.0);
    }
    error.clear();
    return true;
}
