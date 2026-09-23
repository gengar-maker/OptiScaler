#include "ArcNrSyclOwnedListBridge.h"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <vector>

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

bool IsRgba16Texture(ID3D12Resource* resource, UINT width, UINT height) {
    if (resource == nullptr) return false;
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
           desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
           desc.Width == width && desc.Height == height &&
           desc.DepthOrArraySize == 1 && desc.SampleDesc.Count == 1 &&
           desc.MipLevels == 1;
}

std::string HResult(const char* operation, HRESULT result) {
    return std::string(operation) + " failed with HRESULT " +
           std::to_string(static_cast<unsigned long>(result));
}

}  // namespace

ArcNrSyclOwnedListBridge::~ArcNrSyclOwnedListBridge() {
    if (fence_event_ != nullptr)
        CloseHandle(static_cast<HANDLE>(fence_event_));
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
    ArcNrSyclLoader& provider, ArcNrSyclFrame frame,
    bool& list_recording,
    std::string& error) {
    list_recording = true;
    if (!device || !queue || !owned_list || !owned_allocator ||
        !provider.Ready() || frame.struct_size != sizeof(frame)) {
        error = "invalid owned-list SYCL bridge arguments";
        return false;
    }
    const auto color_desc = model_color ? model_color->GetDesc() : D3D12_RESOURCE_DESC{};
    const UINT width = static_cast<UINT>(color_desc.Width);
    const UINT height = color_desc.Height;
    if (width == 0 || height == 0 || width > 8192 || height > 8192 ||
        !IsRgba16Texture(model_color, width, height) ||
        !IsRgba16Texture(model_output, width, height)) {
        error = "SYCL same-frame staging needs matching single-sample RGBA16F textures";
        return false;
    }
    if (queue->GetDesc().Type != owned_list->GetType() ||
        owned_list->GetType() == D3D12_COMMAND_LIST_TYPE_COPY) {
        error = "owned command list and queue types do not match";
        return false;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT rows = 0;
    UINT64 row_bytes = 0, total_bytes = 0;
    device->GetCopyableFootprints(&color_desc, 0, 1, 0, &footprint,
                                  &rows, &row_bytes, &total_bytes);
    if (rows != height || row_bytes != static_cast<UINT64>(width) * 8 ||
        total_bytes == 0) {
        error = "unexpected RGBA16F copy footprint";
        return false;
    }
    const auto readback_heap = Heap(D3D12_HEAP_TYPE_READBACK);
    const auto upload_heap = Heap(D3D12_HEAP_TYPE_UPLOAD);
    const auto buffer_desc = BufferDesc(total_bytes);
    ComPtr<ID3D12Resource> readback, upload;
    HRESULT result = device->CreateCommittedResource(
        &readback_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    if (FAILED(result)) {
        error = HResult("readback allocation", result);
        return false;
    }
    result = device->CreateCommittedResource(
        &upload_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
    if (FAILED(result)) {
        error = HResult("upload allocation", result);
        return false;
    }

    D3D12_TEXTURE_COPY_LOCATION source {};
    source.pResource = model_color;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION destination {};
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    Transition(owned_list, model_color, color_state,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    owned_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    Transition(owned_list, model_color, D3D12_RESOURCE_STATE_COPY_SOURCE,
               color_state);
    result = owned_list->Close();
    if (FAILED(result)) {
        // Close failure leaves the list's state unusable for normal recording.
        list_recording = false;
        error = HResult("producer Close", result);
        return false;
    }
    list_recording = false;
    ID3D12CommandList* producer_lists[] = { owned_list };
    queue->ExecuteCommandLists(1, producer_lists);
    if (!WaitForQueue(device, queue, error)) return false;

    // From here the producer submission is complete and the caller's list is
    // closed. Restore it to recording state even when CPU staging or inference
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

    const std::size_t packed_row = static_cast<std::size_t>(row_bytes);
    std::vector<uint16_t> packed_color(static_cast<std::size_t>(width) *
                                       height * 4);
    std::vector<uint16_t> packed_output(packed_color.size());
    void* mapped = nullptr;
    D3D12_RANGE read_range { 0, static_cast<SIZE_T>(total_bytes) };
    result = readback->Map(0, &read_range, &mapped);
    if (FAILED(result) || mapped == nullptr) {
        return fail_after_producer(mapped == nullptr && SUCCEEDED(result)
            ? "readback Map returned a null pointer"
            : HResult("readback Map", result));
    }
    for (UINT y = 0; y < height; ++y)
        std::memcpy(reinterpret_cast<unsigned char*>(packed_color.data()) +
                        static_cast<std::size_t>(y) * packed_row,
                    static_cast<const unsigned char*>(mapped) +
                        footprint.Offset + static_cast<std::size_t>(y) *
                                               footprint.Footprint.RowPitch,
                    packed_row);
    D3D12_RANGE no_write { 0, 0 };
    readback->Unmap(0, &no_write);

    frame.color_rgba_f16 = packed_color.data();
    frame.output_rgba_f16 = packed_output.data();
    if (provider.Evaluate(frame, error) != ARC_NR_SYCL_OK)
        return fail_after_producer(error);

    D3D12_RANGE no_read { 0, 0 };
    result = upload->Map(0, &no_read, &mapped);
    if (FAILED(result) || mapped == nullptr) {
        return fail_after_producer(mapped == nullptr && SUCCEEDED(result)
            ? "upload Map returned a null pointer"
            : HResult("upload Map", result));
    }
    for (UINT y = 0; y < height; ++y)
        std::memcpy(static_cast<unsigned char*>(mapped) + footprint.Offset +
                        static_cast<std::size_t>(y) *
                            footprint.Footprint.RowPitch,
                    reinterpret_cast<const unsigned char*>(packed_output.data()) +
                        static_cast<std::size_t>(y) * packed_row,
                    packed_row);
    D3D12_RANGE write_range { 0, static_cast<SIZE_T>(total_bytes) };
    upload->Unmap(0, &write_range);

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
    source.pResource = upload.Get();
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

    result = owned_allocator->Reset();
    if (SUCCEEDED(result))
        result = owned_list->Reset(owned_allocator, nullptr);
    if (FAILED(result)) {
        error = HResult("resume owned command list", result);
        return false;
    }
    list_recording = true;
    error.clear();
    return true;
}
