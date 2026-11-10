#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <xell.h>
#include <xell_d3d12.h>
#include <xefg_swapchain.h>
#include <xefg_swapchain_d3d12.h>
#include <nvapi.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <array>
#include <dbghelp.h>
#include <cstdio>
#pragma comment(lib, "dbghelp.lib")
using Microsoft::WRL::ComPtr;
LONG WINAPI Exception(EXCEPTION_POINTERS* e)
{
    auto process = GetCurrentProcess();
    SymInitialize(process, nullptr, TRUE);
    auto ctx = *e->ContextRecord;
    STACKFRAME64 frame {};
    frame.AddrPC = { ctx.Rip, AddrModeFlat };
    frame.AddrStack = { ctx.Rsp, AddrModeFlat };
    frame.AddrFrame = { ctx.Rbp, AddrModeFlat };
    fprintf(stderr, "EXCEPTION %08lx at %p\n", e->ExceptionRecord->ExceptionCode, e->ExceptionRecord->ExceptionAddress);
    for (int i = 0; i < 18; ++i)
    {
        HMODULE m = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(frame.AddrPC.Offset), &m);
        wchar_t path[MAX_PATH] {};
        GetModuleFileNameW(m, path, MAX_PATH);
        char buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] {};
        auto symbol = reinterpret_cast<SYMBOL_INFO*>(buf);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;
        DWORD64 offset = 0;
        SymFromAddr(process, frame.AddrPC.Offset, &offset, symbol);
        fprintf(stderr, "%ls + %llx %s + %llx\n", path, frame.AddrPC.Offset - (UINT64) m, symbol->Name, offset);
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &ctx, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
            break;
    }
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
template <class T> T Proc(HMODULE module, const char* name)
{
    auto fn = reinterpret_cast<T>(GetProcAddress(module, name));
    if (!fn)
        throw std::runtime_error(name);
    return fn;
}
void Check(HRESULT result)
{
    if (FAILED(result))
        throw std::runtime_error("D3D " + std::to_string(result));
}
template <class T> void Sdk(T result, const char* name)
{
    if (int(result) < 0)
        throw std::runtime_error(std::string(name) + " " + std::to_string(int(result)));
}
void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
             D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
    list->ResourceBarrier(1, &b);
}
int wmain(int argc, wchar_t** argv)
{
    try
    {
        SetUnhandledExceptionFilter(Exception);
        std::cout.setf(std::ios::unitbuf);
        if (argc != 3)
            throw std::runtime_error("usage: smoke GAME_DIRECTORY [pure|reflex]");
        auto game = std::filesystem::path(argv[1]);
        SetDllDirectoryW(game.c_str());
        auto ll = LoadLibraryW((game / L"libxell.dll").c_str());
        auto fg = LoadLibraryW((game / L"libxess_fg.dll").c_str());
        if (!ll || !fg)
            throw std::runtime_error("LoadLibrary SDK");
#define LL(name) auto name##Fn = Proc<decltype(&name)>(ll, #name)
#define FG(name) auto name##Fn = Proc<decltype(&name)>(fg, #name)
        LL(xellD3D12CreateContext);
        LL(xellDestroyContext);
        LL(xellSetSleepMode);
        LL(xellGetSleepMode);
        LL(xellSleep);
        LL(xellAddMarkerData);
        FG(xefgSwapChainD3D12CreateContext);
        FG(xefgSwapChainSetLatencyReduction);
        FG(xefgSwapChainD3D12InitFromSwapChainDesc);
        FG(xefgSwapChainD3D12GetSwapChainPtr);
        FG(xefgSwapChainSetEnabled);
        FG(xefgSwapChainSetPresentId);
        FG(xefgSwapChainGetLastPresentStatus);
        FG(xefgSwapChainTagFrameConstants);
        FG(xefgSwapChainD3D12TagFrameResource);
        FG(xefgSwapChainDestroy);
        ComPtr<IDXGIFactory6> factory;
        Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; SUCCEEDED(factory->EnumAdapters1(i, &adapter)); ++i)
        {
            DXGI_ADAPTER_DESC1 d {};
            adapter->GetDesc1(&d);
            if (d.VendorId == 0x8086 && wcsstr(d.Description, L"Arc"))
            {
                std::wcout << d.Description << L'\n';
                break;
            }
            adapter.Reset();
        }
        if (!adapter)
            throw std::runtime_error("No Intel Arc");
        ComPtr<ID3D12Device> device;
        Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)));
        ComPtr<ID3D12CommandQueue> queue;
        D3D12_COMMAND_QUEUE_DESC q {};
        Check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
        xell_context_handle_t latency = nullptr;
        Sdk(xellD3D12CreateContextFn(device.Get(), &latency), "XeLL create");
        if (!latency)
            throw std::runtime_error("XeLL returned success with NULL context");
        auto proxy = GetModuleHandleW(L"dxgi.dll");
        if (std::wstring(argv[2]) == L"reflex")
        {
            proxy = LoadLibraryW(L"nvapi64.dll");
            if (!proxy)
                throw std::runtime_error("NVAPI replacement not loaded");
        }
        using Query = void*(__cdecl*) (unsigned);
        auto query = reinterpret_cast<Query>(GetProcAddress(proxy, "nvapi_QueryInterface"));
        decltype(&NvAPI_D3D_SetSleepMode) reflex = nullptr;
        if (std::wstring(argv[2]) == L"reflex")
        {
            if (!query)
                throw std::runtime_error("OptiScaler nvapi export absent");
            Sdk(reinterpret_cast<decltype(&NvAPI_Initialize)>(query(0x0150e828))(), "NvAPI initialize");
            reflex = reinterpret_cast<decltype(reflex)>(query(0xac1ca9e0));
            NV_SET_SLEEP_MODE_PARAMS params {};
            params.version = NV_SET_SLEEP_MODE_PARAMS_VER;
            auto r = reflex(device.Get(), &params);
            std::cout << "Reflex setSleep off result=" << r << '\n';
        }
        xell_sleep_params_t sleep {};
        sleep.bLowLatencyMode = 1;
        Sdk(xellSetSleepModeFn(latency, &sleep), "XeLL setSleep");
        xell_sleep_params_t actual {};
        Sdk(xellGetSleepModeFn(latency, &actual), "XeLL getSleep");
        std::cout << "XeLL requested=1 actual=" << actual.bLowLatencyMode << '\n';
        if (!actual.bLowLatencyMode)
            throw std::runtime_error("REGRESSION: native XeLL enable was silently ignored");
        xefg_swapchain_handle_t context = nullptr;
        Sdk(xefgSwapChainD3D12CreateContextFn(device.Get(), &context), "FG create");
        Sdk(xefgSwapChainSetLatencyReductionFn(context, latency), "FG latency");
        auto instance = GetModuleHandleW(nullptr);
        WNDCLASSW wc {};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = instance;
        wc.lpszClassName = L"NativeFgSmoke";
        RegisterClassW(&wc);
        HWND window = CreateWindowW(wc.lpszClassName, L"OptiScaler native XeSS FG validation", WS_OVERLAPPEDWINDOW, 40,
                                    40, 640, 400, nullptr, nullptr, instance, nullptr);
        if (!window)
            throw std::runtime_error("CreateWindow");
        ShowWindow(window, SW_SHOW);
        constexpr UINT width = 640, height = 360;
        DXGI_SWAP_CHAIN_DESC1 desc {};
        desc.Width = width;
        desc.Height = height;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 3;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs {};
        fs.Windowed = TRUE;
        xefg_swapchain_d3d12_init_params_t init {};
        init.maxInterpolatedFrames = 1;
        Sdk(xefgSwapChainD3D12InitFromSwapChainDescFn(context, window, &desc, &fs, queue.Get(), factory.Get(), &init),
            "FG init");
        ComPtr<IDXGISwapChain3> swapchain;
        Sdk(xefgSwapChainD3D12GetSwapChainPtrFn(context, IID_PPV_ARGS(&swapchain)), "FG swapchain");
        ComPtr<ID3D12CommandAllocator> allocator;
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList> list;
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                        IID_PPV_ARGS(&list)));
        Check(list->Close());
        ComPtr<ID3D12Fence> fence;
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        UINT64 fenceId = 0;
        auto wait = [&]
        {
            Check(queue->Signal(fence.Get(), ++fenceId));
            Check(fence->SetEventOnCompletion(fenceId, event));
            if (WaitForSingleObject(event, 30000) != WAIT_OBJECT_0)
                throw std::runtime_error("GPU timeout");
        };
        ComPtr<ID3D12DescriptorHeap> rtvs;
        D3D12_DESCRIPTOR_HEAP_DESC hd {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 5;
        Check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvs)));
        const UINT stride = device->GetDescriptorHandleIncrementSize(hd.Type);
        auto rtv = [&](UINT i)
        {
            auto h = rtvs->GetCPUDescriptorHandleForHeapStart();
            h.ptr += i * stride;
            return h;
        };
        std::array<ComPtr<ID3D12Resource>, 3> buffers;
        for (UINT i = 0; i < 3; ++i)
        {
            Check(swapchain->GetBuffer(i, IID_PPV_ARGS(&buffers[i])));
            device->CreateRenderTargetView(buffers[i].Get(), nullptr, rtv(i));
        }
        auto texture = [&](DXGI_FORMAT format, UINT index)
        {
            D3D12_HEAP_PROPERTIES heap {};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC d {};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = width;
            d.Height = height;
            d.DepthOrArraySize = d.MipLevels = d.SampleDesc.Count = 1;
            d.Format = format;
            d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            ComPtr<ID3D12Resource> resource;
            Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
                                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                  IID_PPV_ARGS(&resource)));
            device->CreateRenderTargetView(resource.Get(), nullptr, rtv(index));
            return resource;
        };
        auto motion = texture(DXGI_FORMAT_R16G16_FLOAT, 3);
        auto depth = texture(DXGI_FORMAT_R32_FLOAT, 4);
        UINT generated = 0, disabled = 0;
        for (UINT frame = 1; frame <= 72; ++frame)
        {
            MSG msg {};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            const bool enabled = frame <= 24 || frame > 48;
            if (frame == 1 || frame == 25 || frame == 49)
                Sdk(xefgSwapChainSetEnabledFn(context, enabled), "FG enable");
            // Reproduce the game's unrelated Reflex calls while native XeLL/FG is active.
            if (reflex)
            {
                NV_SET_SLEEP_MODE_PARAMS p {};
                p.version = NV_SET_SLEEP_MODE_PARAMS_VER;
                Sdk(reflex(device.Get(), &p), "Reflex");
            }
            Sdk(xellSleepFn(latency, frame), "sleep");
            for (auto marker :
                 { XELL_SIMULATION_START, XELL_INPUT_SAMPLE, XELL_SIMULATION_END, XELL_RENDERSUBMIT_START })
                Sdk(xellAddMarkerDataFn(latency, frame, marker), "marker");
            wait();
            Check(allocator->Reset());
            Check(list->Reset(allocator.Get(), nullptr));
            UINT index = swapchain->GetCurrentBackBufferIndex();
            Barrier(list.Get(), buffers[index].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
            float color[] = { .1f + frame * .002f, .25f, .45f, 1 };
            list->ClearRenderTargetView(rtv(index), color, 0, nullptr);
            Barrier(list.Get(), buffers[index].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
            for (auto pair : { std::pair { motion.Get(), 3u }, std::pair { depth.Get(), 4u } })
            {
                Barrier(list.Get(), pair.first, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_RENDER_TARGET);
                float value[4] = {};
                if (pair.second == 4)
                    value[0] = .5f;
                list->ClearRenderTargetView(rtv(pair.second), value, 0, nullptr);
                Barrier(list.Get(), pair.first, D3D12_RESOURCE_STATE_RENDER_TARGET,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            if (enabled)
            {
                for (auto pair : { std::pair { motion.Get(), XEFG_SWAPCHAIN_RES_MOTION_VECTOR },
                                   std::pair { depth.Get(), XEFG_SWAPCHAIN_RES_DEPTH } })
                {
                    xefg_swapchain_d3d12_resource_data_t resource {};
                    resource.type = pair.second;
                    resource.validity = XEFG_SWAPCHAIN_RV_ONLY_NOW;
                    resource.pResource = pair.first;
                    resource.resourceSize = { width, height };
                    resource.incomingState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                    Sdk(xefgSwapChainD3D12TagFrameResourceFn(context, list.Get(), frame, &resource), "FG resource");
                }
                xefg_swapchain_frame_constant_data_t data {};
                for (UINT i = 0; i < 4; ++i)
                    data.viewMatrix[i * 5] = data.projectionMatrix[i * 5] = 1;
                data.motionVectorScaleX = data.motionVectorScaleY = 1;
                data.frameRenderTime = 20;
                data.resetHistory = frame == 1 || frame == 49;
                Sdk(xefgSwapChainTagFrameConstantsFn(context, frame, &data), "FG constants");
                Sdk(xefgSwapChainSetPresentIdFn(context, frame), "FG present ID");
            }
            Check(list->Close());
            ID3D12CommandList* lists[] = { list.Get() };
            queue->ExecuteCommandLists(1, lists);
            Sdk(xellAddMarkerDataFn(latency, frame, XELL_RENDERSUBMIT_END), "marker");
            Sdk(xellAddMarkerDataFn(latency, frame, XELL_PRESENT_START), "marker");
            Check(swapchain->Present(0, DXGI_PRESENT_ALLOW_TEARING));
            Sdk(xellAddMarkerDataFn(latency, frame, XELL_PRESENT_END), "marker");
            xefg_swapchain_present_status_t status {};
            Sdk(xefgSwapChainGetLastPresentStatusFn(context, &status), "FG status");
            std::cout << "frame=" << frame << " enabled=" << status.isFrameGenEnabled
                      << " presented=" << status.framesPresented << " result=" << int(status.frameGenResult) << '\n';
            if (enabled && status.isFrameGenEnabled && status.frameGenResult == 0 && status.framesPresented > 1)
                ++generated;
            if (!enabled && !status.isFrameGenEnabled && status.framesPresented == 1)
                ++disabled;
            Sleep(20);
        }
        wait();
        buffers = {};
        swapchain.Reset();
        Sdk(xefgSwapChainDestroyFn(context), "FG destroy");
        Sdk(xellDestroyContextFn(latency), "XeLL destroy");
        CloseHandle(event);
        DestroyWindow(window);
        if (generated < 35 || disabled != 24)
            throw std::runtime_error("FG on/off/on validation failed: generated=" + std::to_string(generated) +
                                     " disabled=" + std::to_string(disabled));
        std::cout << "PASS: XeLL enable preserved; FG on/off/on generated=" << generated << " disabled=" << disabled
                  << '\n';
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
