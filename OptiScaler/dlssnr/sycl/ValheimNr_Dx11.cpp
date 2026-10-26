#include <pch.h>
#include "ValheimNr_Dx11.h"

#include "ArcNrSyclLoader.h"
#include "ArcNrSyclOwnedListBridge.h"
#include <Config.h>
#include <Util.h>
#include <with_dx12/dx11_with_dx12.h>
#include <with_dx12/with_dx12.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <wrl/client.h>

namespace ValheimNr_Dx11 {
namespace {
using Microsoft::WRL::ComPtr;

bool IsSupported(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
           format == DXGI_FORMAT_R10G10B10A2_UNORM ||
           format == DXGI_FORMAT_R8G8B8A8_UNORM ||
           format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
           format == DXGI_FORMAT_B8G8R8A8_UNORM ||
           format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

struct Session {
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11DeviceContext> context11;
    ComPtr<ID3D11Texture2D> output11;
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue12;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    Dx11WithDx12::D3D11_TEXTURE2D_RESOURCE_C color {};
    Dx11WithDx12::D3D11_TEXTURE2D_RESOURCE_C output {};
    ArcNrSyclLoader provider;
    ArcNrSyclOwnedListBridge bridge;
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t frame_index = 0;
    bool history_invalid = true;
    bool previous_estimate_motion = false;
    bool failed = false;

    ~Session() { Release(); }

    void Release() {
        // Keep the DLL and its selected Level Zero device alive across
        // swapchain resize; only the resolution-dependent session is rebuilt.
        provider.ResetSession();
        Dx11WithDx12::ReleaseSharedResource(&color);
        Dx11WithDx12::ReleaseSharedResource(&output);
        output11.Reset();
        list.Reset();
        allocator.Reset();
        queue12.Reset();
        device12.Reset();
        context11.Reset();
        device11.Reset();
        width = height = 0;
        format = DXGI_FORMAT_UNKNOWN;
        frame_index = 0;
        history_invalid = true;
        previous_estimate_motion = false;
    }

    bool Initialize(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& desc,
                    std::string& error) {
        Release();
        if (!WithDx12::PrepareD3D12ForD3D11(device, D3D_FEATURE_LEVEL_11_0)) {
            error = "no D3D12 queue on the D3D11 adapter";
            return false;
        }
        device11 = device;
        device11->GetImmediateContext(&context11);
        device12 = WithDx12::GetD3D12Device();
        queue12 = WithDx12::GetD3D12CommandQueue();
        if (!context11 || !device12 || !queue12) {
            error = "D3D11/D3D12 interop initialization failed";
            return false;
        }
        Dx11WithDx12::Init(device11.Get(), context11.Get(), device12.Get(),
                           queue12.Get());

        D3D11_TEXTURE2D_DESC output_desc = desc;
        output_desc.Usage = D3D11_USAGE_DEFAULT;
        output_desc.CPUAccessFlags = 0;
        output_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED |
                                D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        HRESULT hr = device11->CreateTexture2D(&output_desc, nullptr, &output11);
        if (FAILED(hr)) {
            error = "D3D11 NR shared output texture creation failed: " +
                    std::to_string(static_cast<unsigned>(hr));
            return false;
        }
        hr = device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&allocator));
        if (SUCCEEDED(hr))
            hr = device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&list));
        if (FAILED(hr)) {
            error = "D3D11 NR D3D12 command list creation failed: " +
                    std::to_string(static_cast<unsigned>(hr));
            return false;
        }
        hr = list->Close();
        if (FAILED(hr)) {
            error = "D3D11 NR D3D12 command list close failed";
            return false;
        }

        const auto dir = Util::DllPath().parent_path();
        const auto weights = dir / "arc_nr_weights";
        const auto logical = weights / "logical";
        const auto derived = weights / "derived";
        if (!std::filesystem::is_directory(logical) ||
            !std::filesystem::is_directory(derived)) {
            error = "arc_nr_weights/logical and derived are missing";
            return false;
        }
        if (!provider.Loaded() &&
            !provider.Load(dir / "arc_nr_sycl.dll", error)) return false;
        const auto logical_bytes = logical.u8string();
        const auto derived_bytes = derived.u8string();
        const std::string logical_utf8(
            reinterpret_cast<const char*>(logical_bytes.data()),
            logical_bytes.size());
        const std::string derived_utf8(
            reinterpret_cast<const char*>(derived_bytes.data()),
            derived_bytes.size());
        ArcNrSyclCreate create {};
        create.struct_size = sizeof(create);
        create.width = desc.Width;
        create.height = desc.Height;
        create.logical_weights_directory = logical_utf8.c_str();
        create.derived_weights_directory = derived_utf8.c_str();
        if (!provider.Create(create, error)) return false;
        width = desc.Width;
        height = desc.Height;
        format = desc.Format;
        LOG_INFO("D3D11 Present SYCL NR ready: {}x{}, DXGI format {}",
                 width, height, static_cast<int>(format));
        return true;
    }

    bool Process(IDXGISwapChain* swapchain, ID3D11Device* device,
                 std::string& error) {
        ComPtr<ID3D11Texture2D> backbuffer;
        HRESULT hr = swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
        if (FAILED(hr) || !backbuffer) {
            error = "D3D11 NR backbuffer is unavailable";
            return false;
        }
        D3D11_TEXTURE2D_DESC desc {};
        backbuffer->GetDesc(&desc);
        if (!IsSupported(desc.Format) || desc.SampleDesc.Count != 1 ||
            desc.Width == 0 || desc.Height == 0) {
            error = "D3D11 NR backbuffer unsupported: format=" +
                    std::to_string(static_cast<int>(desc.Format)) +
                    " samples=" + std::to_string(desc.SampleDesc.Count) +
                    " size=" + std::to_string(desc.Width) + "x" +
                    std::to_string(desc.Height);
            return false;
        }
        if (device11.Get() != device || width != desc.Width ||
            height != desc.Height || format != desc.Format) {
            LOG_INFO("D3D11 Present SYCL NR reinitialize: device {} -> {}, size {}x{} -> {}x{}, format {} -> {}",
                     static_cast<const void*>(device11.Get()),
                     static_cast<const void*>(device), width, height,
                     desc.Width, desc.Height, static_cast<int>(format),
                     static_cast<int>(desc.Format));
            if (!Initialize(device, desc, error)) return false;
        }
        if (!Dx11WithDx12::PrepareTextureFrom11To12(
                "D3D11 NR color", device12.Get(), backbuffer.Get(), &color,
                true, false, false, 0) ||
            !Dx11WithDx12::PrepareTextureFrom11To12(
                "D3D11 NR output", device12.Get(), output11.Get(), &output,
                false, false, false, 0)) {
            error = "D3D11 NR shared texture preparation failed";
            return false;
        }
        if (!Dx11WithDx12::SyncDx11ToDx12()) {
            error = "D3D11 NR D3D11 to D3D12 fence failed";
            return false;
        }
        hr = allocator->Reset();
        if (SUCCEEDED(hr)) hr = list->Reset(allocator.Get(), nullptr);
        if (FAILED(hr)) {
            error = "D3D11 NR D3D12 command list reset failed";
            return false;
        }
        auto* cfg = Config::Instance();
        const bool estimate_motion = cfg->DlssNrEstimateMotion.value_or_default();
        if (estimate_motion != previous_estimate_motion)
            history_invalid = true;
        previous_estimate_motion = estimate_motion;
        ArcNrSyclFrame frame {};
        frame.struct_size = sizeof(frame);
        frame.frame_index = frame_index++;
        frame.reset_history = !estimate_motion || history_invalid;
        frame.reserved = estimate_motion ? 1u : 0u;
        frame.normalized_style =
            std::min(1.0f, cfg->DlssNrStyle.value_or_default() / 2.0f);
        frame.local_tone = cfg->DlssNrLocalTone.value_or_default();
        frame.local_structure = cfg->DlssNrLocalStructure.value_or_default();
        frame.skin_structure = cfg->DlssNrSkinStructure.value_or_default();
        frame.automatic_structure =
            cfg->DlssNrAutoMask.value_or_default() ? 1.0f : 0.0f;
        frame.intensity = cfg->DlssNrIntensity.value_or_default();
        bool recording = true;
        const bool ok = bridge.Run(device12.Get(), queue12.Get(), list.Get(),
            allocator.Get(), color.Dx12Resource, D3D12_RESOURCE_STATE_COMMON,
            output.Dx12Resource, D3D12_RESOURCE_STATE_COMMON, nullptr,
            0, 0, 0, 0, 0.0f, 0.0f, provider, frame, recording, error,
            estimate_motion);
        if (recording) list->Close();
        if (!ok) return false;
        history_invalid = false;
        if (!Dx11WithDx12::SyncDx12ToDx11()) {
            error = "D3D11 NR D3D12 to D3D11 fence failed";
            return false;
        }
        if (cfg->DlssNrApplyModel.value_or_default())
            context11->CopyResource(backbuffer.Get(), output11.Get());
        return true;
    }
};

Session g_session;
std::mutex g_session_mutex;
std::atomic<bool> g_running {false};
std::atomic<double> g_last_ms {0.0};
std::string g_failure_reason;
}

bool IsSupportedGame() {
    static const bool supported_game = [] {
        const auto executable = Util::ExePath().filename();
        return _wcsicmp(executable.c_str(), L"valheim.exe") == 0 ||
               _wcsicmp(executable.c_str(),
                        L"TekkenGame-Win64-Shipping.exe") == 0;
    }();
    return supported_game;
}

void BeforePresent(IDXGISwapChain* swapchain, ID3D11Device* device) {
    if (!IsSupportedGame() || !swapchain || !device) return;
    // DXGI may present another swapchain while the primary one is in its
    // overlay callback. Never recursively reset the shared list/session.
    static thread_local bool inside = false;
    if (inside) return;
    inside = true;
    struct RecursionGuard { bool& flag; ~RecursionGuard() { flag = false; } } guard {inside};
    std::lock_guard<std::mutex> lock(g_session_mutex);
    if (!Config::Instance()->DlssNrEnabled.value_or_default() ||
        Config::Instance()->DlssNrBackend.value_or_default() != 1) {
        g_session.history_invalid = true;
        g_session.failed = false;
        g_failure_reason.clear();
        g_running = false;
        return;
    }
    if (g_session.failed) return;
    const auto started = std::chrono::steady_clock::now();
    std::string error;
    if (!g_session.Process(swapchain, device, error)) {
        LOG_ERROR("D3D11 Present SYCL NR disabled after failure: {}", error);
        g_session.failed = true;
        g_failure_reason = error;
        g_running = false;
        return;
    }
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    g_last_ms = ms;
    g_failure_reason.clear();
    g_running = true;
    if (g_session.frame_index % 30 == 0)
        LOG_INFO("D3D11 Present NR-PROFILE frame={} {}x{} total={:.2f}ms estimated_motion={}",
                 g_session.frame_index, g_session.width, g_session.height, ms,
                 g_session.previous_estimate_motion);
}
bool IsRunning() { return g_running.load(); }
std::optional<double> LastFrameMs() {
    return g_running.load() ? std::optional<double>(g_last_ms.load())
                            : std::nullopt;
}
std::string FailureReason() {
    std::lock_guard<std::mutex> lock(g_session_mutex);
    return g_failure_reason;
}
void RetryAfterFailure() {
    std::lock_guard<std::mutex> lock(g_session_mutex);
    g_session.Release();
    g_session.failed = false;
    g_failure_reason.clear();
    g_running = false;
}
}
