#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <chrono>
#include <memory>
#include <vector>
#include <stdexcept>
#include <string>
#include "ArcNrSyclApi.h"

using Microsoft::WRL::ComPtr;
static void Check(HRESULT hr, const char* what) {
    if (FAILED(hr)) throw std::runtime_error(std::string(what) + " HRESULT " + std::to_string(hr));
}
static D3D12_RESOURCE_DESC Buffer(UINT64 size, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    return desc;
}
struct Shared {
    ComPtr<ID3D12Heap> heap;
    ComPtr<ID3D12Resource> buffer;
    HANDLE handle = nullptr;
    ~Shared() { if (handle) CloseHandle(handle); }
};
static void Wait(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) {
    Check(queue->Signal(fence, value), "Signal");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Check(fence->SetEventOnCompletion(value, event), "SetEventOnCompletion");
    if (WaitForSingleObject(event, 30000) != WAIT_OBJECT_0) throw std::runtime_error("fence timeout");
    CloseHandle(event);
}
int main(int argc, char** argv) {
    try {
        if (argc != 5 && argc != 7 && argc != 8) throw std::runtime_error("usage: arc_nr_sycl_provider_gpu_smoke LOGICAL DERIVED INPUT OUTPUT [WIDTH HEIGHT [--temporal|--temporal-shift|--temporal-shift4|--estimate|--estimate-shift4|--rgba8|--rgb10a2]]");
        const bool temporal = argc == 8 &&
            (std::string(argv[7]) == "--temporal" ||
             std::string(argv[7]) == "--temporal-shift" ||
             std::string(argv[7]) == "--temporal-shift4");
        const bool shift = argc == 8 && std::string(argv[7]) == "--temporal-shift";
        const bool shift4 = argc == 8 && (std::string(argv[7]) == "--temporal-shift4" ||
                                          std::string(argv[7]) == "--estimate-shift4");
        const bool estimate = argc == 8 && (std::string(argv[7]) == "--estimate" ||
                                             std::string(argv[7]) == "--estimate-shift4");
        const bool rgba8 = argc == 8 && std::string(argv[7]) == "--rgba8";
        const bool rgb10a2 = argc == 8 && std::string(argv[7]) == "--rgb10a2";
        if (argc == 8 && !temporal && !estimate && !rgba8 && !rgb10a2) throw std::runtime_error("expected temporal, estimate, RGBA8 or RGB10A2 mode");
        const UINT width = argc >= 7 ? static_cast<UINT>(std::stoul(argv[5])) : 320;
        const UINT height = argc >= 7 ? static_cast<UINT>(std::stoul(argv[6])) : 320;
        if (width == 0 || height == 0 || width % 32 != 0) throw std::runtime_error("test width must be divisible by 32");
        const UINT64 bytes = UINT64(width) * height * ((rgba8 || rgb10a2) ? 4 : 8);
        std::ifstream file(argv[3], std::ios::binary);
        std::vector<char> color(bytes);
        if (!file.read(color.data(), bytes)) throw std::runtime_error("input read failed");
        ComPtr<IDXGIFactory6> factory;
        Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "factory");
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; SUCCEEDED(factory->EnumAdapters1(i, &adapter)); ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            if (desc.VendorId == 0x8086 && wcsstr(desc.Description, L"Arc")) break;
            adapter.Reset();
        }
        if (!adapter) throw std::runtime_error("Intel Arc D3D12 adapter absent");
        ComPtr<IDXGIAdapter3> memory_adapter;
        adapter.As(&memory_adapter);
        const auto memory = [&](const char* label) {
            if (!memory_adapter) return;
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            if (SUCCEEDED(memory_adapter->QueryVideoMemoryInfo(
                    0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
                constexpr double mib = 1024.0 * 1024.0;
                std::cout << "memory=" << label
                          << " usage_mib=" << info.CurrentUsage / mib
                          << " budget_mib=" << info.Budget / mib << '\n';
            }
        };
        memory("before_device");
        ComPtr<ID3D12Device> device;
        Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
        auto desc = Buffer(bytes, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
        auto allocation = device->GetResourceAllocationInfo(0, 1, &desc);
        auto make_shared = [&](D3D12_RESOURCE_STATES state) {
            auto shared = std::make_unique<Shared>();
            D3D12_HEAP_DESC heap_desc{};
            heap_desc.SizeInBytes = allocation.SizeInBytes;
            heap_desc.Alignment = allocation.Alignment;
            heap_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
            heap_desc.Properties.CreationNodeMask = 1;
            heap_desc.Properties.VisibleNodeMask = 1;
            heap_desc.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
            Check(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&shared->heap)), "shared heap");
            Check(device->CreatePlacedResource(shared->heap.Get(), 0, &desc, state, nullptr,
                                               IID_PPV_ARGS(&shared->buffer)), "placed buffer");
            Check(device->CreateSharedHandle(shared->heap.Get(), nullptr, GENERIC_ALL, nullptr,
                                             &shared->handle), "shared handle");
            return shared;
        };
        auto input = make_shared(D3D12_RESOURCE_STATE_COPY_DEST);
        auto output = make_shared(D3D12_RESOURCE_STATE_COPY_SOURCE);
        auto motion = temporal ? make_shared(D3D12_RESOURCE_STATE_COPY_DEST) : nullptr;
        D3D12_HEAP_PROPERTIES upload_heap{};
        upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        upload_heap.CreationNodeMask = upload_heap.VisibleNodeMask = 1;
        D3D12_HEAP_PROPERTIES readback_heap = upload_heap;
        readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
        auto plain_desc = Buffer(bytes, D3D12_RESOURCE_FLAG_NONE);
        ComPtr<ID3D12Resource> upload, shifted_upload, readback, motion_upload;
        Check(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &plain_desc,
              D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)), "upload");
        Check(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &plain_desc,
              D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "readback");
        if (temporal) {
            Check(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE,
                  &plain_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                  IID_PPV_ARGS(&motion_upload)), "motion upload");
            void* motion_data = nullptr;
            D3D12_RANGE empty{0,0};
            Check(motion_upload->Map(0, &empty, &motion_data), "motion Map");
            memset(motion_data, 0, bytes);
            if (shift || (shift4 && temporal)) {
                auto* values = static_cast<uint16_t*>(motion_data);
                for (UINT64 i = 0; i < UINT64(width) * height; ++i)
                    values[2 * i] = shift4 ? 0x4400u : 0x3c00u;
            }
            motion_upload->Unmap(0, nullptr);
        }
        void* mapped = nullptr;
        D3D12_RANGE no_read{0,0};
        Check(upload->Map(0, &no_read, &mapped), "upload Map");
        memcpy(mapped, color.data(), bytes);
        upload->Unmap(0, nullptr);
        if (shift4) {
            if (rgba8 || rgb10a2) throw std::runtime_error("shift4 requires RGBA16F");
            Check(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE,
                  &plain_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                  IID_PPV_ARGS(&shifted_upload)), "shifted upload");
            Check(shifted_upload->Map(0, &no_read, &mapped), "shifted Map");
            auto* shifted = static_cast<char*>(mapped);
            for (UINT y = 0; y < height; ++y)
                for (UINT x = 0; x < width; ++x)
                    memcpy(shifted + (UINT64(y) * width + x) * 8,
                           color.data() + (UINT64(y) * width +
                               (x + 4 < width ? x + 4 : width - 1)) * 8, 8);
            shifted_upload->Unmap(0, nullptr);
        }
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12Fence> fence;
        Check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "queue");
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&allocator)), "allocator");
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, allocator.Get(), nullptr,
                                        IID_PPV_ARGS(&list)), "list");
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
        list->CopyBufferRegion(input->buffer.Get(), 0, upload.Get(), 0, bytes);
        if (temporal)
            list->CopyBufferRegion(motion->buffer.Get(), 0,
                                   motion_upload.Get(), 0, bytes);
        Check(list->Close(), "input close");
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        Wait(queue.Get(), fence.Get(), 1);
        HMODULE module = LoadLibraryW(L"arc_nr_sycl.dll");
        if (!module) throw std::runtime_error("provider LoadLibrary failed");
        auto get_api = reinterpret_cast<PFN_ArcNrSyclGetApi>(GetProcAddress(module, ARC_NR_SYCL_GET_API_NAME));
        ArcNrSyclApi api{};
        api.struct_size = sizeof(api);
        if (!get_api || !get_api(ARC_NR_SYCL_API_VERSION, &api)) throw std::runtime_error("provider ABI mismatch");
        ArcNrSyclCreate create{sizeof(create), width, height, argv[1], argv[2]};
        char error[512]{};
        const auto create_start = std::chrono::steady_clock::now();
        void* session = api.create(&create, error, sizeof(error));
        if (!session) throw std::runtime_error(error);
        std::cout << "create_ms=" << std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - create_start).count() << '\n';
        memory("after_create");
        ArcNrSyclGpuFrame frame{};
        frame.struct_size = sizeof(frame);
        frame.controls.struct_size = sizeof(frame.controls);
        frame.controls.reset_history = 1;
        frame.controls.reserved = estimate ? 1u : 0u;
        frame.controls.local_tone = 1;
        frame.controls.local_structure = 1;
        frame.controls.skin_structure = -1;
        frame.controls.automatic_structure = 1;
        frame.controls.intensity = 1;
        frame.color_heap_nt_handle = input->handle;
        frame.output_heap_nt_handle = output->handle;
        frame.heap_bytes = allocation.SizeInBytes;
        frame.row_pitch_bytes = width * ((rgba8 || rgb10a2) ? 4 : 8);
        frame.pixel_format = rgb10a2 ? 3 : (rgba8 ? 1 : 0);
        if (temporal) {
            frame.motion_heap_nt_handle = motion->handle;
            frame.motion_heap_bytes = allocation.SizeInBytes;
            frame.motion_row_pitch_bytes = width * 4;
            frame.motion_width = width;
            frame.motion_height = height;
            frame.motion_subrect_width = width;
            frame.motion_subrect_height = height;
            frame.motion_format = 1;
            frame.motion_scale_x = 1;
            frame.motion_scale_y = 1;
        }
        ArcNrSyclGpuTimings timings{};
        frame.timings = &timings;
        ArcNrSyclStatus status = ARC_NR_SYCL_OK;
        for (int i = 0; i < (shift4 ? 2 : 4) && status == ARC_NR_SYCL_OK; ++i) {
            if (shift4 && i == 1) {
                Check(allocator->Reset(), "shift allocator reset");
                Check(list->Reset(allocator.Get(), nullptr), "shift list reset");
                list->CopyBufferRegion(input->buffer.Get(), 0,
                                       shifted_upload.Get(), 0, bytes);
                Check(list->Close(), "shift close");
                queue->ExecuteCommandLists(1, lists);
                Wait(queue.Get(), fence.Get(), 3);
            }
            frame.controls.frame_index = i;
            frame.controls.reset_history = !(temporal || estimate) || i == 0;
            const auto start = std::chrono::steady_clock::now();
            status = api.evaluate_gpu(session, &frame, error, sizeof(error));
            const double wall_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
            std::cout << "eval=" << i << " wall_ms=" << wall_ms
                      << " import_ms=" << timings.import_us / 1000.0
                      << " input_ms=" << timings.input_us / 1000.0
                      << " prepare_ms=" << timings.prepare_us / 1000.0
                      << " model_ms=" << timings.model_us / 1000.0
                      << " compose_ms=" << timings.compose_us / 1000.0
                      << " output_ms=" << timings.output_us / 1000.0
                      << " release_ms=" << timings.release_us / 1000.0
                      << " total_ms=" << timings.total_us / 1000.0 << '\n';
            std::cout << "temporal=" << timings.temporal_active << '\n';
            memory("after_eval");
        }
        api.destroy(session);
        memory("after_destroy");
        if (status != ARC_NR_SYCL_OK) throw std::runtime_error(error);
        Check(allocator->Reset(), "allocator reset");
        Check(list->Reset(allocator.Get(), nullptr), "list reset");
        list->CopyBufferRegion(readback.Get(), 0, output->buffer.Get(), 0, bytes);
        Check(list->Close(), "output close");
        queue->ExecuteCommandLists(1, lists);
        Wait(queue.Get(), fence.Get(), shift4 ? 4 : 2);
        D3D12_RANGE range{0,bytes};
        Check(readback->Map(0, &range, &mapped), "readback Map");
        std::ofstream out(argv[4], std::ios::binary);
        out.write(static_cast<const char*>(mapped), bytes);
        readback->Unmap(0, nullptr);
        std::cout << "shared GPU provider evaluated " << width << 'x' << height
                  << (rgb10a2 ? " RGB10A2" : (rgba8 ? " RGBA8" : " RGBA16F")) << " frame\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
