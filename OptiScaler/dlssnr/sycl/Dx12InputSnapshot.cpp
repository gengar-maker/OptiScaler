#define NOMINMAX
#include "Dx12InputSnapshot.h"
#include <windows.h>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>

namespace Dx12InputSnapshot {
namespace {
using Microsoft::WRL::ComPtr;
struct Readback {
    Texture source;
    ComPtr<ID3D12Resource> buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT rows = 0;
    UINT64 rowBytes = 0;
};
struct Submission {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    std::vector<Readback> images;
    HANDLE event = nullptr;
    ~Submission() { if (event) CloseHandle(event); }
};
// On a device timeout the GPU may still reference the readbacks and allocator.
std::mutex failedMutex;
std::vector<std::shared_ptr<Submission>> failedSubmissions;
void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
             D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &b);
}
}

bool Capture(ID3D12Device* device, ID3D12CommandQueue* queue,
             const std::vector<Texture>& textures,
             const std::filesystem::path& directory, std::string& error) {
    if (!device || !queue || textures.empty() || textures.size() > 8 ||
        queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        error = "invalid diagnostic capture arguments"; return false;
    }
    auto submission = std::make_shared<Submission>();
    auto fail = [&](const char* stage, HRESULT hr) {
        std::ostringstream text; text << stage << ": HRESULT 0x" << std::hex << unsigned(hr);
        error = text.str(); return false;
    };
    auto hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&submission->allocator));
    if (FAILED(hr)) return fail("capture allocator", hr);
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, submission->allocator.Get(),
                                   nullptr, IID_PPV_ARGS(&submission->list));
    if (FAILED(hr)) return fail("capture list", hr);
    UINT64 totalBytes = 0;
    for (const auto& texture : textures) {
        if (!texture.resource || texture.name.empty() || texture.name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos) {
            error = "invalid diagnostic texture name/resource"; return false;
        }
        const auto desc = texture.resource->GetDesc();
        D3D12_FEATURE_DATA_FORMAT_INFO format {desc.Format};
        hr = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &format, sizeof(format));
        if (FAILED(hr) || format.PlaneCount != 1 || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            desc.MipLevels != 1 || desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1 ||
            !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)) {
            error = "capture requires a single-plane, single-mip simultaneous-access 2D texture"; return false;
        }
        Readback image; image.source = texture;
        UINT64 bytes = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &image.footprint, &image.rows, &image.rowBytes, &bytes);
        if (!bytes || bytes > 256ull*1024*1024 || totalBytes + bytes > 512ull*1024*1024) {
            error = "diagnostic capture exceeds 512 MiB budget"; return false;
        }
        totalBytes += bytes;
        D3D12_HEAP_PROPERTIES heap {}; heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer {}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes; buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&image.buffer));
        if (FAILED(hr)) return fail("capture readback", hr);
        Barrier(submission->list.Get(), texture.resource.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src {}, dst {};
        src.pResource = texture.resource.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = image.buffer.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = image.footprint;
        submission->list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Barrier(submission->list.Get(), texture.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        submission->images.push_back(std::move(image));
    }
    hr = submission->list->Close(); if (FAILED(hr)) return fail("capture Close", hr);
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&submission->fence));
    if (FAILED(hr)) return fail("capture fence", hr);
    submission->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!submission->event) return fail("capture event", HRESULT_FROM_WIN32(GetLastError()));
    ID3D12CommandList* lists[] = {submission->list.Get()}; queue->ExecuteCommandLists(1, lists);
    hr = queue->Signal(submission->fence.Get(), 1);
    if (SUCCEEDED(hr)) hr = submission->fence->SetEventOnCompletion(1, submission->event);
    const bool complete = SUCCEEDED(hr) && WaitForSingleObject(submission->event, 30000) == WAIT_OBJECT_0 &&
                          submission->fence->GetCompletedValue() != UINT64_MAX;
    if (!complete) {
        std::lock_guard lock(failedMutex); failedSubmissions.push_back(submission);
        return fail("capture GPU fence", FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    }
    std::error_code ec;
    if (!std::filesystem::create_directory(directory, ec)) {
        error = "capture output must be a new directory: " + ec.message(); return false;
    }
    for (const auto& image : submission->images) {
        const auto desc = image.source.resource->GetDesc();
        const SIZE_T readEnd = image.footprint.Offset + SIZE_T(image.rows-1)*image.footprint.Footprint.RowPitch + SIZE_T(image.rowBytes);
        D3D12_RANGE range {0, readEnd}; void* mapped = nullptr;
        hr = image.buffer->Map(0, &range, &mapped);
        if (FAILED(hr)) return fail("capture Map", hr);
        std::ofstream file(directory / (image.source.name + ".bin"), std::ios::binary);
        for (UINT y = 0; y < image.rows; ++y)
            file.write(static_cast<const char*>(mapped) + image.footprint.Offset + SIZE_T(y)*image.footprint.Footprint.RowPitch,
                       static_cast<std::streamsize>(image.rowBytes));
        D3D12_RANGE noWrite {0, 0}; image.buffer->Unmap(0, &noWrite);
        if (!file) { error = "capture file write failed"; return false; }
        std::ofstream meta(directory / (image.source.name + ".json"));
        meta << "{\"width\":" << desc.Width << ",\"height\":" << desc.Height
             << ",\"dxgi_format\":" << unsigned(desc.Format) << ",\"row_bytes\":" << image.rowBytes
             << ",\"rows\":" << image.rows << "}\n";
        if (!meta) { error = "capture metadata write failed"; return false; }
    }
    error.clear(); return true;
}
}
