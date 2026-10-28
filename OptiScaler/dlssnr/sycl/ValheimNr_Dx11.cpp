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
#include <cmath>
#include <filesystem>
#include <mutex>
#include <string>
#include <wrl/client.h>

namespace ValheimNr_Dx11 {
namespace {
using Microsoft::WRL::ComPtr;
std::mutex g_interop_mutex;

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
    Dx11WithDx12::D3D11_TEXTURE2D_RESOURCE_C motion {};
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
        bridge.Reset();
        Dx11WithDx12::ReleaseSharedResource(&color);
        Dx11WithDx12::ReleaseSharedResource(&output);
        Dx11WithDx12::ReleaseSharedResource(&motion);
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

    bool ProcessTexture(ID3D11Texture2D* input, ID3D11Device* device,
                        ID3D11Texture2D* motion_input,
                        UINT input_width, UINT input_height,
                        UINT motion_base_x, UINT motion_base_y,
                        UINT motion_width, UINT motion_height,
                        float motion_scale_x, float motion_scale_y,
                        bool external_reset, bool copy_back,
                        ComPtr<ID3D11Texture2D>* result,
                        std::string& error) {
        // Dx11WithDx12 maintains process-wide D3D11/D3D12 synchronization
        // objects. Present and pre-XeSS sessions must not rebind them at once.
        std::lock_guard<std::mutex> interop_lock(g_interop_mutex);
        if (!input) {
            error = "D3D11 NR input is unavailable";
            return false;
        }
        D3D11_TEXTURE2D_DESC desc {};
        input->GetDesc(&desc);
        if (!IsSupported(desc.Format) || desc.SampleDesc.Count != 1 ||
            desc.Width == 0 || desc.Height == 0 ||
            desc.Width != input_width || desc.Height != input_height) {
            error = "D3D11 NR input unsupported: format=" +
                    std::to_string(static_cast<int>(desc.Format)) +
                    " samples=" + std::to_string(desc.SampleDesc.Count) +
                    " size=" + std::to_string(desc.Width) + "x" +
                    std::to_string(desc.Height);
            return false;
        }
        if (motion_input &&
            (motion_width == 0 || motion_height == 0 ||
             !std::isfinite(motion_scale_x) ||
             !std::isfinite(motion_scale_y))) {
            error = "D3D11 NR motion metadata is invalid";
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
        Dx11WithDx12::Init(device11.Get(), context11.Get(),
                           device12.Get(), queue12.Get());
        if (!Dx11WithDx12::PrepareTextureFrom11To12(
                "D3D11 NR color", device12.Get(), input, &color,
                true, false, false, 0) ||
            !Dx11WithDx12::PrepareTextureFrom11To12(
                "D3D11 NR output", device12.Get(), output11.Get(), &output,
                false, false, false, 0) ||
            (motion_input && !Dx11WithDx12::PrepareTextureFrom11To12(
                "D3D11 NR motion", device12.Get(), motion_input, &motion,
                true, false, false, 0))) {
            error = "D3D11 NR shared texture preparation failed";
            return false;
        }
        if (!Dx11WithDx12::SyncDx11ToDx12()) {
            error = "D3D11 NR D3D11 to D3D12 fence failed";
            return false;
        }
        HRESULT hr = allocator->Reset();
        if (SUCCEEDED(hr)) hr = list->Reset(allocator.Get(), nullptr);
        if (FAILED(hr)) {
            error = "D3D11 NR D3D12 command list reset failed";
            return false;
        }
        auto* cfg = Config::Instance();
        const bool estimate_motion = !motion_input &&
            cfg->DlssNrEstimateMotion.value_or_default();
        if (estimate_motion != previous_estimate_motion)
            history_invalid = true;
        previous_estimate_motion = estimate_motion;
        ArcNrSyclFrame frame {};
        frame.struct_size = sizeof(frame);
        frame.frame_index = frame_index++;
        frame.reset_history = external_reset || history_invalid ||
            (!estimate_motion && !motion_input);
        const auto strength_q10 = [](float value) -> uint32_t {
            return static_cast<uint32_t>(std::lround(
                std::clamp(value, 0.0f, 4095.0f / 1024.0f) * 1024.0f));
        };
        const uint32_t colour_strength = strength_q10(
            cfg->DlssNrColourStrength.value_or_default());
        const uint32_t transfer_strength = strength_q10(
            cfg->DlssNrTransferStrength.value_or_default());
        frame.reserved = (estimate_motion ? 1u : 0u) | 2u |
                         (colour_strength << 8) | (transfer_strength << 20);
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
            output.Dx12Resource, D3D12_RESOURCE_STATE_COMMON,
            motion_input ? motion.Dx12Resource : nullptr,
            motion_width, motion_height, motion_base_x, motion_base_y,
            motion_scale_x, motion_scale_y, provider, frame, recording, error,
            estimate_motion);
        if (recording) list->Close();
        if (!ok) return false;
        history_invalid = false;
        if (!Dx11WithDx12::SyncDx12ToDx11()) {
            error = "D3D11 NR D3D12 to D3D11 fence failed";
            return false;
        }
        if (copy_back && cfg->DlssNrApplyModel.value_or_default())
            context11->CopyResource(input, output11.Get());
        if (result) *result = output11;
        return true;
    }

    bool Process(IDXGISwapChain* swapchain, ID3D11Device* device,
                 std::string& error) {
        ComPtr<ID3D11Texture2D> backbuffer;
        const HRESULT hr = swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
        if (FAILED(hr) || !backbuffer) {
            error = "D3D11 NR backbuffer is unavailable";
            return false;
        }
        D3D11_TEXTURE2D_DESC desc {};
        backbuffer->GetDesc(&desc);
        return ProcessTexture(backbuffer.Get(), device, nullptr,
                              desc.Width, desc.Height, 0, 0, 0, 0,
                              0.0f, 0.0f, false, true, nullptr, error);
    }
};

Session g_session;
std::mutex g_session_mutex;
Session g_pre_xess_session;
std::mutex g_pre_xess_mutex;
ComPtr<ID3D11Texture2D> g_pre_xess_subrect_input11;
ComPtr<ID3D11Device> g_pre_xess_subrect_device;
std::atomic<bool> g_running {false};
std::atomic<double> g_last_ms {0.0};
std::string g_failure_reason;
struct PreXeSSPresent {
    ComPtr<ID3D11Device> device;
    UINT width = 0;
    UINT height = 0;
    std::chrono::steady_clock::time_point submitted {};
    bool pending = false;
};
// Protected by g_session_mutex, the same lock used by BeforePresent.
PreXeSSPresent g_pre_xess_present;
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
        // A disabled pass must give its GPU allocations back to the game.
        // Otherwise a stage load can still compete with the resident model,
        // despite no neural frame being submitted.
        if (g_session.device11) {
            std::lock_guard<std::mutex> interop_lock(g_interop_mutex);
            g_session.Release();
            LOG_INFO("D3D11 Present SYCL NR disabled: released model session and shared textures");
        }
        g_session.failed = false;
        g_pre_xess_present = {};
        g_failure_reason.clear();
        g_running = false;
        return;
    }
    if (g_pre_xess_present.pending) {
        // Another swapchain can present first. Match the XeSS output size and
        // device before consuming this frame's pre-upscale SNR marker.
        ComPtr<ID3D11Texture2D> backbuffer;
        D3D11_TEXTURE2D_DESC desc {};
        if (SUCCEEDED(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer))))
            backbuffer->GetDesc(&desc);
        const bool matches = device == g_pre_xess_present.device.Get() &&
            desc.Width == g_pre_xess_present.width &&
            desc.Height == g_pre_xess_present.height;
        if (matches) {
            if (g_session.device11) {
                std::lock_guard<std::mutex> interop_lock(g_interop_mutex);
                g_session.Release();
            }
            g_pre_xess_present.pending = false;
            g_running = true;
            return;
        }
        if (std::chrono::steady_clock::now() - g_pre_xess_present.submitted >
            std::chrono::milliseconds(250))
            g_pre_xess_present = {};
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
    std::lock_guard<std::mutex> interop_lock(g_interop_mutex);
    g_session.Release();
    g_session.failed = false;
    g_pre_xess_present = {};
    g_failure_reason.clear();
    g_running = false;
}
void MarkPreXeSSOutput(ID3D11DeviceContext* context, ID3D11Resource* output) {
    if (!IsSupportedGame() || !context || !output) return;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(output->QueryInterface(IID_PPV_ARGS(&texture)))) return;
    D3D11_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    std::lock_guard<std::mutex> lock(g_session_mutex);
    g_pre_xess_present.device = std::move(device);
    g_pre_xess_present.width = desc.Width;
    g_pre_xess_present.height = desc.Height;
    g_pre_xess_present.submitted = std::chrono::steady_clock::now();
    g_pre_xess_present.pending = true;
}

bool ProcessBeforeXeSS(ID3D11DeviceContext* context,
                       ID3D11Texture2D* color,
                       ID3D11Texture2D* motion_input,
                       UINT input_width, UINT input_height,
                       UINT color_base_x, UINT color_base_y,
                       UINT motion_base_x, UINT motion_base_y,
                       UINT motion_width, UINT motion_height,
                       float motion_scale_x, float motion_scale_y,
                       bool reset_history,
                       ComPtr<ID3D11Texture2D>& result,
                       std::string& error) {
    std::lock_guard<std::mutex> lock(g_pre_xess_mutex);
    result.Reset();
    const auto* cfg = Config::Instance();
    if (!cfg->DlssNrEnabled.value_or_default() ||
        cfg->DlssNrBackend.value_or_default() != 1) {
        if (g_pre_xess_session.device11) {
            std::lock_guard<std::mutex> interop_lock(g_interop_mutex);
            g_pre_xess_session.Release();
        }
        g_pre_xess_subrect_input11.Reset();
        g_pre_xess_subrect_device.Reset();
        g_pre_xess_session.failed = false;
        return false;
    }
    if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
        !color || !motion_input || input_width == 0 || input_height == 0) {
        error = "XeSS pre-upscale SNR needs immediate context, color and motion";
        return false;
    }
    D3D11_TEXTURE2D_DESC color_desc {}, motion_desc {};
    color->GetDesc(&color_desc);
    motion_input->GetDesc(&motion_desc);
    const bool motion_format =
        motion_desc.Format == DXGI_FORMAT_R16G16_FLOAT ||
        motion_desc.Format == DXGI_FORMAT_R16G16_TYPELESS ||
        motion_desc.Format == DXGI_FORMAT_R32G32_FLOAT ||
        motion_desc.Format == DXGI_FORMAT_R32G32_TYPELESS;
    if (!motion_format || motion_desc.SampleDesc.Count != 1 ||
        !IsSupported(color_desc.Format) || color_desc.SampleDesc.Count != 1 ||
        color_desc.ArraySize != 1 || color_desc.MipLevels != 1 ||
        color_base_x >= color_desc.Width ||
        color_base_y >= color_desc.Height ||
        input_width > color_desc.Width - color_base_x ||
        input_height > color_desc.Height - color_base_y ||
        motion_base_x >= motion_desc.Width ||
        motion_base_y >= motion_desc.Height ||
        motion_width == 0 || motion_height == 0 ||
        motion_width > motion_desc.Width - motion_base_x ||
        motion_height > motion_desc.Height - motion_base_y) {
        error = "XeSS pre-upscale SNR requires a supported color subrect and valid R16G16/R32G32 motion";
        return false;
    }
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!device) {
        error = "XeSS pre-upscale D3D11 device is unavailable";
        return false;
    }
    if (g_pre_xess_session.failed) return false;
    ID3D11Texture2D* nr_input = color;
    if (color_base_x || color_base_y || color_desc.Width != input_width ||
        color_desc.Height != input_height) {
        D3D11_TEXTURE2D_DESC extracted_desc = color_desc;
        extracted_desc.Width = input_width;
        extracted_desc.Height = input_height;
        extracted_desc.Usage = D3D11_USAGE_DEFAULT;
        extracted_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        extracted_desc.CPUAccessFlags = 0;
        extracted_desc.MiscFlags = 0;
        D3D11_TEXTURE2D_DESC cached_desc {};
        if (g_pre_xess_subrect_input11)
            g_pre_xess_subrect_input11->GetDesc(&cached_desc);
        if (!g_pre_xess_subrect_input11 ||
            g_pre_xess_subrect_device.Get() != device.Get() ||
            cached_desc.Width != extracted_desc.Width ||
            cached_desc.Height != extracted_desc.Height ||
            cached_desc.Format != extracted_desc.Format) {
            g_pre_xess_subrect_input11.Reset();
            g_pre_xess_subrect_device.Reset();
            const HRESULT hr = device->CreateTexture2D(
                &extracted_desc, nullptr, &g_pre_xess_subrect_input11);
            if (FAILED(hr)) {
                error = "XeSS pre-upscale SNR color subrect texture creation failed";
                return false;
            }
            g_pre_xess_subrect_device = device;
        }
        const D3D11_BOX source_box {color_base_x, color_base_y, 0,
            color_base_x + input_width, color_base_y + input_height, 1};
        context->CopySubresourceRegion(g_pre_xess_subrect_input11.Get(),
                                      0, 0, 0, 0, color, 0, &source_box);
        nr_input = g_pre_xess_subrect_input11.Get();
    }
    if (!g_pre_xess_session.ProcessTexture(nr_input, device.Get(), motion_input,
            input_width, input_height, motion_base_x, motion_base_y,
            motion_width, motion_height, motion_scale_x, motion_scale_y,
            reset_history, false, &result, error)) {
        g_pre_xess_session.failed = true;
        std::lock_guard<std::mutex> interop_lock(g_interop_mutex);
        g_pre_xess_session.Release();
        LOG_ERROR("XeSS pre-upscale SYCL NR skipped: {}", error);
        return false;
    }
    return cfg->DlssNrApplyModel.value_or_default();
}
}
