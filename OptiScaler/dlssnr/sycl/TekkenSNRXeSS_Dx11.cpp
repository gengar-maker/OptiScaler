#include <pch.h>
#include "TekkenSNRXeSS_Dx11.h"
#include "TekkenTemporalInputs_Dx11.h"
#include "NrHdrAdapter_Dx11.h"
#include "NrDisplayMotion_Dx11.h"
#include "ValheimNr_Dx11.h"

#include <Config.h>
#include <Logger.h>
#include <State.h>
#include <Util.h>
#include <proxies/XeSS_Proxy.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>

namespace TekkenSNRXeSSDx11 {
namespace {
using Microsoft::WRL::ComPtr;

constexpr const char* kLdrConvertShader = R"(
Texture2D<float4> LowColor : register(t0);
RWTexture2D<float4> XeSSColor : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 pixel : SV_DispatchThreadID) {
    uint width, height;
    LowColor.GetDimensions(width, height);
    if (pixel.x < width && pixel.y < height)
        XeSSColor[pixel.xy] = LowColor.Load(int3(pixel.xy, 0));
}
)";

constexpr const char* kPresentShader = R"(
Texture2D<float4> XeSSColor : register(t0);
float4 main(float4 pixel : SV_POSITION) : SV_Target {
    return XeSSColor.Load(int3(int2(pixel.xy), 0));
}
)";

struct DrawInputs {
    ID3D11ShaderResourceView* srvs[4] {};
    ID3D11Buffer* constants[2] {};
    ID3D11RenderTargetView* target = nullptr;
    ID3D11DepthStencilView* depth = nullptr;
    ~DrawInputs() {
        for (auto* item : srvs) if (item) item->Release();
        for (auto* item : constants) if (item) item->Release();
        if (target) target->Release();
        if (depth) depth->Release();
    }
};

struct ContextScope {
    explicit ContextScope(ID3D11DeviceContext* original,
                          ID3DDeviceContextState* isolated)
        : context(original) {
        if (SUCCEEDED(original->QueryInterface(IID_PPV_ARGS(&context1))))
            context1->SwapDeviceContextState(isolated, &previous);
    }
    ~ContextScope() {
        if (context1 && previous)
            context1->SwapDeviceContextState(previous.Get(), nullptr);
    }
    explicit operator bool() const { return context1 && previous; }
    ID3D11DeviceContext* context;
    ComPtr<ID3D11DeviceContext1> context1;
    ComPtr<ID3DDeviceContextState> previous;
};

bool Check(xess_result_t result, const char* step, std::string& error) {
    if (result == XESS_RESULT_SUCCESS || result > 0) return true;
    error = std::string(step) + " returned XeSS result " +
            std::to_string(static_cast<int>(result));
    return false;
}

class Chain {
public:
    ~Chain() { Reset(); }

    bool Execute(ID3D11DeviceContext* context, std::string& error) {
        DrawInputs input;
        context->PSGetShaderResources(0, 4, input.srvs);
        context->PSGetConstantBuffers(0, 2, input.constants);
        context->OMGetRenderTargets(1, &input.target, &input.depth);
        if (!input.target) return false;
        ComPtr<ID3D11Resource> target_resource;
        input.target->GetResource(&target_resource);
        ComPtr<ID3D11Texture2D> target;
        if (FAILED(target_resource.As(&target))) return false;
        D3D11_TEXTURE2D_DESC desc {};
        target->GetDesc(&desc);
        D3D11_RENDER_TARGET_VIEW_DESC view_desc {};
        input.target->GetDesc(&view_desc);
        if (desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
            desc.MipLevels != 1 || desc.ArraySize != 1 ||
            desc.SampleDesc.Count != 1 ||
            view_desc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D ||
            view_desc.Texture2D.MipSlice != 0) return false;

        ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        if (!device || !Initialize(device.Get(), desc.Width, desc.Height, error))
            return false;
        ContextScope scope(context, isolated_state_.Get());
        if (!scope) {
            error = "D3D11.1 context state isolation is unavailable";
            return false;
        }
        TekkenTemporalInputsDx11::Sources sources {
            input.srvs[1], input.srvs[0], input.srvs[3],
            input.constants[0], input.constants[1]};
        TekkenTemporalInputsDx11::Outputs low;
        if (!preparation_.Prepare(context, sources, low, error)) return false;
        if (low.width != input_width_ || low.height != input_height_) {
            error = "XeSS Balanced input dimensions differ from TAA inputs";
            return false;
        }

        const auto* cfg = Config::Instance();
        const bool want_neural = cfg->DlssNrEnabled.value_or_default() &&
            cfg->DlssNrBackend.value_or_default() == 1 &&
            cfg->DlssNrApplyModel.value_or_default();
        if (want_neural != last_applied_neural_)
            reset_history_ = true;
        ID3D11Texture2D* color = low.hdr.Get();
        ComPtr<ID3D11Texture2D> neural;
        if (want_neural && !EncodeHdr(context, low.hdr.Get(), nullptr, error)) return false;
        std::string nr_error;
        const bool applied_neural = want_neural &&
            ValheimNr_Dx11::ProcessBeforeXeSS(
                context, hdr_adapter_.Proxy(), low.motion.Get(),
                low.width, low.height, 0, 0, 0, 0,
                low.width, low.height, 1.0f, 1.0f,
                reset_history_, neural, nr_error, true);
        if (!nr_error.empty()) {
            error = "SYCL NR: " + nr_error;
            return false;
        }
        if (want_neural && !applied_neural) {
            error = "SYCL NR was enabled but did not return a frame";
            return false;
        }
        if (applied_neural) {
            if (!neural || !hdr_adapter_.Resolve(context, neural.Get(), true, error))
                return false;
            color = hdr_adapter_.Output();
        }
        xess_d3d11_execute_params_t execute {};
        execute.pColorTexture = color;
        execute.pVelocityTexture = low.motion.Get();
        execute.pDepthTexture = low.depth.Get();
        execute.pOutputTexture = output_.Get();
        execute.inputWidth = low.width;
        execute.inputHeight = low.height;
        execute.exposureScale = 1.0f;
        execute.resetHistory = reset_history_;
        execute.jitterOffsetX = 0.0f;
        execute.jitterOffsetY = 0.0f;
        // This capture variant has no verified TAA jitter offset yet.
        if (!Check(XeSSProxy::D3D11Execute()(xess_, &execute),
                   "xessD3D11Execute", error)) return false;
        context->CopyResource(target.Get(), output_.Get());
        if (neural)
            ValheimNr_Dx11::MarkPreXeSSOutput(context, target.Get());
        reset_history_ = false;
        last_applied_neural_ = applied_neural;
        ++frame_;
        return true;
    }

    bool ProcessNativeTaa(ID3D11DeviceContext* context,
                          float jitter_x, float jitter_y,
                          UINT display_width, UINT display_height,
                          std::string& error) {
        DrawInputs input;
        context->PSGetShaderResources(0, 4, input.srvs);
        context->PSGetConstantBuffers(0, 2, input.constants);
        context->OMGetRenderTargets(1, &input.target, &input.depth);
        if (!input.target) return false;
        ComPtr<ID3D11Resource> resource;
        input.target->GetResource(&resource);
        ComPtr<ID3D11Texture2D> target;
        if (FAILED(resource.As(&target))) return false;
        D3D11_TEXTURE2D_DESC desc {};
        target->GetDesc(&desc);
        if (desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
            desc.Width < 640 || desc.Height < 360 ||
            desc.Width > 2048 || desc.Height > 1152 ||
            desc.Width > display_width || desc.Height > display_height ||
            desc.Width * 3 < display_width || desc.Height * 3 < display_height ||
            std::abs(double(desc.Width) * display_height - double(desc.Height) * display_width) >
                std::max(display_width, display_height) ||
            desc.MipLevels != 1 || desc.ArraySize != 1 ||
            desc.SampleDesc.Count != 1) return false;
        const auto* cfg = Config::Instance();
        const bool want_upscale = cfg->DlssNrEnabled.value_or_default() &&
            cfg->DlssNrBackend.value_or_default() == 1;
        // Opt-in live A/B: changing the bypass marker switches only the SNR
        // model while keeping the same game scene, XeSS, and camera inputs.
        static const bool live_ab_enabled = [] {
            std::error_code fs_error;
            return std::filesystem::exists(
                Util::DllPath().parent_path() /
                    "OptiScaler.snr-xess.live-ab.enable", fs_error);
        }();
        std::error_code fs_error;
        const bool bypass_model = live_ab_enabled && std::filesystem::exists(
            Util::DllPath().parent_path() /
                "OptiScaler.snr-xess.live-ab.bypass-model", fs_error);
        const bool want_neural = want_upscale &&
            cfg->DlssNrApplyModel.value_or_default() && !bypass_model;
        if (!want_upscale) {
            reset_history_ = true;
            last_applied_neural_ = false;
            return false;
        }
        // While a new profile is exported/compiled, keep original TAA and
        // tone mapping. No resource preparation, fence wait, or permanent
        // failure latch is allowed on this path.
        const bool display_snr = NativeSnrEnabled() || NeuralAfterXeSSEnabled();
        if (want_neural && !ValheimNr_Dx11::PrepareWithMotion(
                display_snr ? display_width : desc.Width,
                display_snr ? display_height : desc.Height)) {
            native_frame_ready_ = false;
            reset_history_ = true;
            last_applied_neural_ = false;
            return false;
        }
        // The HDR experiment must use the actual camera sample, never a
        // clamped substitute. Retain game rendering when the sample is invalid.
        if (NativeHdrEnabled() && (!std::isfinite(jitter_x) ||
            !std::isfinite(jitter_y) || std::abs(jitter_x) > 0.5f ||
            std::abs(jitter_y) > 0.5f)) {
            reset_history_ = true;
            return false;
        }
        if (native_taa_target_.Get() == target.Get())
            return native_frame_ready_;
        ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        if (NativeSnrEnabled() && (desc.Width != display_width || desc.Height != display_height)) {
            reset_history_ = true;
            if (++native_size_mismatch_count_ == 1 || native_size_mismatch_count_ % 240 == 0)
                LOG_WARN("Tekken native SNR requires actual display-resolution rendering: TAA={}x{} display={}x{}; set r.ScreenPercentage=100. Game output retained",
                    desc.Width, desc.Height, display_width, display_height);
            return false;
        }
        if (!device || !Initialize(device.Get(), display_width,
                                    display_height, error, true, desc.Width, desc.Height)) return false;
        // Preparation only touches private CS resources and saves/restores its
        // CS bindings. Keep the game's graphics context active for its TAA draw.
        if (want_neural != last_applied_neural_) reset_history_ = true;
        TekkenTemporalInputsDx11::Sources sources {
            input.srvs[1], input.srvs[0], input.srvs[3],
            input.constants[0], input.constants[1]};
        if (!reset_history_) {
            sources.history_jitter_x = previous_jitter_x_ - jitter_x;
            sources.history_jitter_y = previous_jitter_y_ - jitter_y;
        }
        TekkenTemporalInputsDx11::Outputs low;
        if (!preparation_.Prepare(context, sources, low, error,
                TekkenTemporalInputsDx11::InputScale::Native)) return false;
        context->Flush();
        if (low.width != input_width_ || low.height != input_height_) {
            error = "Native TAA input resolution differs from XeSS input";
            return false;
        }
        if (want_neural != last_applied_neural_) reset_history_ = true;
        if (NativeHdrEnabled()) {
            native_low_ = low;
            native_motion_ = low.motion;
            native_depth_ = low.depth;
            native_taa_target_ = target;
            native_frame_ready_ = true;
            native_want_neural_ = want_neural;
            native_jitter_x_ = jitter_x;
            native_jitter_y_ = jitter_y;
            return true;
        }
        ContextScope scope(context, isolated_state_.Get());
        if (!scope) {
            error = "D3D11.1 context state isolation is unavailable";
            return false;
        }
        ComPtr<ID3D11Texture2D> neural;
        if (want_neural && !EncodeHdr(context, low.hdr.Get(), nullptr, error)) return false;
        std::string nr_error;
        const bool applied_neural = want_neural &&
            ValheimNr_Dx11::ProcessBeforeXeSS(
                context, hdr_adapter_.Proxy(), low.neural_motion.Get(),
                low.width, low.height, 0, 0, 0, 0,
                low.width, low.height, 1.0f, 1.0f,
                reset_history_, neural, nr_error, true);
        if (!nr_error.empty()) {
            error = "SYCL NR: " + nr_error;
            return false;
        }
        if (want_neural && !applied_neural) {
            error = "SYCL NR was enabled but did not return a frame";
            return false;
        }
        if (applied_neural) {
            if (!neural || !hdr_adapter_.Resolve(context, neural.Get(), true, error))
                return false;
        }
        context->CopyResource(target.Get(),
            applied_neural ? hdr_adapter_.Output() : low.hdr.Get());
        native_motion_ = low.motion;
        native_depth_ = low.depth;
        native_taa_target_ = target;
        native_frame_ready_ = true;
        native_neural_applied_ = applied_neural;
        native_jitter_x_ = jitter_x;
        native_jitter_y_ = jitter_y;
        last_applied_neural_ = applied_neural;
        if (frame_ == 0 || frame_ % 60 == 0)
            LOG_INFO("Tekken native TAA inputs: HDR/depth/velocity={}x{} jitter=({:.4f},{:.4f}) NR={} resetHistory={}, awaiting same-frame XeSS pass",
                     low.width, low.height, jitter_x, jitter_y,
                     applied_neural, reset_history_);
        return true;
    }

    bool PrepareNativeUpscale(ID3D11DeviceContext* context,
        ComPtr<ID3D11PixelShader>& shader,
        ComPtr<ID3D11ShaderResourceView>& color,
        std::string& error) {
        if (NativeSnrEnabled()) return false;
        // With NR off, keep the game's own TAA and spatial pass. With NR on
        // and ApplyModel off, use raw HDR + XeSS for an independent A/B.
        if (!native_frame_ready_ || !native_taa_target_ ||
            !native_ || !native_motion_ ||
            !native_depth_ || !present_shader_ || !output_srv_) return false;
        const auto* cfg = Config::Instance();
        if (!cfg->DlssNrEnabled.value_or_default() ||
            cfg->DlssNrBackend.value_or_default() != 1) return false;
        ComPtr<ID3D11ShaderResourceView> source;
        context->PSGetShaderResources(0, 1, &source);
        ComPtr<ID3D11RenderTargetView> target_view;
        context->OMGetRenderTargets(1, &target_view, nullptr);
        if (!source || !target_view) return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC source_view_desc {};
        source->GetDesc(&source_view_desc);
        ComPtr<ID3D11Resource> source_resource, target_resource;
        source->GetResource(&source_resource);
        target_view->GetResource(&target_resource);
        ComPtr<ID3D11Texture2D> source_texture, target_texture;
        if (FAILED(source_resource.As(&source_texture)) ||
            FAILED(target_resource.As(&target_texture))) return false;
        D3D11_TEXTURE2D_DESC source_desc {}, target_desc {};
        source_texture->GetDesc(&source_desc);
        target_texture->GetDesc(&target_desc);
        if (source_desc.Width != input_width_ ||
            source_desc.Height != input_height_ ||
            target_desc.Width != width_ || target_desc.Height != height_ ||
            source_view_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
            source_view_desc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
            source_view_desc.Texture2D.MostDetailedMip != 0 ||
            source_desc.MipLevels != 1 || source_desc.ArraySize != 1 ||
            source_desc.SampleDesc.Count != 1 ||
            target_desc.Format != DXGI_FORMAT_R10G10B10A2_UNORM ||
            target_desc.MipLevels != 1 || target_desc.ArraySize != 1 ||
            target_desc.SampleDesc.Count != 1 ||
            !std::isfinite(native_jitter_x_) ||
            !std::isfinite(native_jitter_y_)) return false;
        if (NativeHdrEnabled()) {
            if (!hdr_post_.FinalColor(source.Get(), color)) return false;
            shader = present_shader_;
            native_upscaled_this_frame_ = true;
            if (reset_history_) history_frames_ = 0;
            ++history_frames_;
            reset_history_ = false;
            ++frame_;
            return true;
        }
        if (!ldr_input_ || !ldr_uav_ || !ldr_shader_) return false;
        ContextScope scope(context, isolated_state_.Get());
        if (!scope) {
            error = "D3D11.1 context state isolation is unavailable";
            return false;
        }
        ID3D11ShaderResourceView* input_view = source.Get();
        ID3D11UnorderedAccessView* output_view = ldr_uav_.Get();
        context->CSSetShader(ldr_shader_.Get(), nullptr, 0);
        context->CSSetShaderResources(0, 1, &input_view);
        context->CSSetUnorderedAccessViews(0, 1, &output_view, nullptr);
        context->Dispatch((input_width_ + 7) / 8,
                          (input_height_ + 7) / 8, 1);
        ID3D11ShaderResourceView* null_view = nullptr;
        ID3D11UnorderedAccessView* null_uav = nullptr;
        context->CSSetShaderResources(0, 1, &null_view);
        context->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
        xess_d3d11_execute_params_t execute {};
        execute.pColorTexture = ldr_input_.Get();
        execute.pVelocityTexture = native_motion_.Get();
        execute.pDepthTexture = native_depth_.Get();
        execute.pOutputTexture = output_.Get();
        execute.inputWidth = input_width_;
        execute.inputHeight = input_height_;
        execute.exposureScale = 1.0f;
        execute.resetHistory = reset_history_;
        // Tekken's 8-sample pattern reaches about +/-0.575 px. XeSS rejects
        // values outside +/-0.5, so bound only the two overshooting samples.
        execute.jitterOffsetX = std::clamp(native_jitter_x_, -0.5f, 0.5f);
        // Tekken stores raster-space Y (down); XeSS's D3D11 convention uses
        // the opposite sign, as in Intel's D3D11 sample integration.
        execute.jitterOffsetY = std::clamp(-native_jitter_y_, -0.5f, 0.5f);
        if ((std::abs(native_jitter_x_) > 0.5f ||
             std::abs(native_jitter_y_) > 0.5f) &&
            (++clamped_jitter_count_ == 1 || clamped_jitter_count_ % 120 == 0))
            LOG_WARN("Tekken native XeSS: projection jitter ({:.4f},{:.4f}) exceeds XeSS range; clamped to ({:.4f},{:.4f}). Camera/input samples remain unmodified, so temporal alignment is approximate",
                     native_jitter_x_, native_jitter_y_,
                     execute.jitterOffsetX, execute.jitterOffsetY);
        if (!Check(XeSSProxy::D3D11Execute()(xess_, &execute),
                   "xessD3D11Execute", error)) return false;
        shader = present_shader_;
        color = output_srv_;
        native_upscaled_this_frame_ = true;
        reset_history_ = false;
        ++frame_;
        return true;
    }

    void EndNativeFrame() {
        // A TAA draw without its matching final upscale must not leave XeSS
        // (or the SNR temporal input) reusing an older frame as history.
        if (!native_upscaled_this_frame_) {
            reset_history_ = true;
            if (native_frame_ready_ &&
                (++missed_upscale_count_ == 1 || missed_upscale_count_ % 120 == 0))
                LOG_WARN("Tekken native SNR->XeSS: TAA frame missed its final upscale; temporal history reset");
        } else {
            previous_jitter_x_ = native_jitter_x_;
            previous_jitter_y_ = native_jitter_y_;
        }
        native_upscaled_this_frame_ = false;
        native_frame_ready_ = false;
        native_neural_applied_ = false;
        native_taa_target_.Reset();
        hdr_post_.EndFrame();
        native_low_ = {};
        native_hdr_executed_ = false;
        debug_neural_.Reset();
    }

    bool ReplayHdr(ID3D11DeviceContext* context, TekkenHdrPostProcessDx11::Pass pass,
                   TekkenHdrPostProcessDx11::DrawIndexed draw, UINT count,
                   UINT start, INT base, std::string& error, ID3D11ShaderResourceView* exposure) {
        if (!native_frame_ready_ || !NativeHdrEnabled() || NativeSnrEnabled()) return false;
        if (pass == TekkenHdrPostProcessDx11::Pass::ToneMap && !native_hdr_executed_) {
            const bool after_xess = NeuralAfterXeSSEnabled();
            ComPtr<ID3D11ShaderResourceView> postprocess_hdr = output_srv_;
            ComPtr<ID3D11ShaderResourceView> source;
            context->PSGetShaderResources(1, 1, &source);
            ComPtr<ID3D11Resource> resource;
            if (source) source->GetResource(&resource);
            if (!exposure || resource.Get() != native_taa_target_.Get() || !native_low_.hdr)
                return false;
            {
                ContextScope scope(context, isolated_state_.Get());
                if (!scope) { error = "HDR execution context isolation unavailable"; return false; }
                if (!hdr_exposure_.Copy(context, exposure, error)) return false;
                ComPtr<ID3D11Texture2D> neural;
                if (native_want_neural_ && !after_xess) {
                    if (!EncodeHdr(context, native_low_.hdr.Get(), hdr_exposure_.View(), error)) return false;
                    const bool applied = ValheimNr_Dx11::ProcessBeforeXeSS(
                        context, hdr_adapter_.Proxy(), native_low_.neural_motion.Get(),
                        input_width_, input_height_, 0, 0, 0, 0,
                        input_width_, input_height_, 1.0f, 1.0f,
                        reset_history_, neural, error, true);
                    if (!applied || !neural) {
                        if (error.empty()) error = "SYCL NR did not produce an HDR proxy result";
                        return false;
                    }
                    if (!hdr_adapter_.Resolve(context, neural.Get(), true, error)) return false;
                }
                xess_d3d11_execute_params_t execute {};
                execute.pColorTexture = native_want_neural_ && !after_xess
                    ? hdr_adapter_.Output() : native_low_.hdr.Get();
                execute.pVelocityTexture = native_motion_.Get();
                execute.pDepthTexture = native_depth_.Get();
                execute.pExposureScaleTexture = hdr_exposure_.Texture();
                execute.pOutputTexture = output_.Get();
                execute.inputWidth = input_width_;
                execute.inputHeight = input_height_;
                execute.exposureScale = 1.0f;
                execute.resetHistory = reset_history_;
                execute.jitterOffsetX = native_jitter_x_;
                execute.jitterOffsetY = -native_jitter_y_;
                // Submit producers as in Intel's D3D11 sample. This does not
                // wait on the CPU or transfer texture data through the host.
                context->Flush();
                if (!Check(XeSSProxy::D3D11Execute()(xess_, &execute), "HDR xessD3D11Execute", error)) return false;
                if (native_want_neural_ && after_xess) {
                    // XeSS may leave its output bound for writing. We are in
                    // our isolated state; unbind it before downstream reads.
                    context->ClearState();
                    // XeSS's color is unjittered scene-linear HDR. Its API does
                    // not return motion; reconstruct matching display-grid
                    // motion from the engine fields, without history jitter.
                    if (!display_motion_.Prepare(context, native_motion_.Get(), native_depth_.Get(),
                            width_, height_, native_jitter_x_, native_jitter_y_, true, error) ||
                        !EncodeHdr(context, output_.Get(), hdr_exposure_.View(), error)) return false;
                    const bool applied = ValheimNr_Dx11::ProcessWithMotion(
                        context, hdr_adapter_.Proxy(), display_motion_.Output(),
                        width_, height_, 0, 0, 0, 0, width_, height_, 1.0f, 1.0f,
                        reset_history_, neural, error, true);
                    if (!applied || !neural) {
                        if (error.empty()) error = "SYCL NR after XeSS did not produce an HDR proxy result";
                        return false;
                    }
                    if (!hdr_adapter_.Resolve(context, neural.Get(), true, error)) return false;
                    if (composed_hdr_.Get() != hdr_adapter_.Output()) {
                        composed_hdr_srv_.Reset(); composed_hdr_.Reset();
                        if (FAILED(device_->CreateShaderResourceView(hdr_adapter_.Output(), nullptr, &composed_hdr_srv_))) {
                            error = "Post-XeSS SNR HDR view creation failed"; return false;
                        }
                        composed_hdr_ = hdr_adapter_.Output();
                    }
                    postprocess_hdr = composed_hdr_srv_;
                }
                debug_neural_ = neural;
            }
            hdr_post_.BeginFrame(native_taa_target_.Get(), postprocess_hdr.Get());
            native_hdr_executed_ = true;
            last_applied_neural_ = native_neural_applied_ = native_want_neural_;
            if (++hdr_submissions_ == 1 || hdr_submissions_ % 60 == 0)
                LOG_INFO("Tekken HDR {}: {}x{} -> {}x{} neural={} snr={}x{} gameExposure=true jitter=({:.4f},{:.4f}) reset={}",
                    after_xess ? "XeSS->SNR" : "SNR->XeSS", input_width_, input_height_, width_, height_, native_want_neural_,
                    after_xess ? width_ : input_width_, after_xess ? height_ : input_height_,
                    native_jitter_x_, native_jitter_y_, reset_history_);
        }
        const bool replayed = hdr_post_.Replay(context, isolated_state_.Get(), pass, draw,
                                               count, start, base, error);
        if (!replayed && (++skipped_post_count_ < 4 || skipped_post_count_ % 240 == 0))
            LOG_INFO("Tekken HDR postprocess retained original: stage={} reason={}", int(pass), hdr_post_.SkipReason());
        return replayed;
    }

    bool PrepareNativeTonemap(ID3D11DeviceContext* context,
        ID3D11ShaderResourceView* exposure, ComPtr<ID3D11ShaderResourceView>& color,
        std::string& error) {
        color.Reset();
        if (!NativeSnrEnabled() || !native_frame_ready_ || native_hdr_executed_ ||
            input_width_ != width_ || input_height_ != height_) return false;
        ComPtr<ID3D11ShaderResourceView> source;
        ComPtr<ID3D11Resource> resource;
        context->PSGetShaderResources(1, 1, &source);
        if (source) source->GetResource(&resource);
        if (!source || resource.Get() != native_taa_target_.Get()) return false;
        // ApplyModel off uses the exact original TAA color and original draw.
        color = source;
        if (native_want_neural_) {
            if (!exposure) return false;
            ContextScope scope(context, isolated_state_.Get());
            if (!scope) { error = "Native SNR context isolation unavailable"; return false; }
            if (!hdr_exposure_.Copy(context, exposure, error) ||
                !display_motion_.Prepare(context, native_motion_.Get(), native_depth_.Get(),
                    width_, height_, native_jitter_x_, native_jitter_y_, true, error) ||
                !EncodeHdr(context, native_taa_target_.Get(), hdr_exposure_.View(), error)) return false;
            ComPtr<ID3D11Texture2D> neural;
            const bool applied = ValheimNr_Dx11::ProcessWithMotion(
                context, hdr_adapter_.Proxy(), display_motion_.Output(), width_, height_,
                0, 0, 0, 0, width_, height_, 1.0f, 1.0f, reset_history_, neural, error, true);
            if (!applied || !neural) {
                if (error.empty()) error = "Native SNR did not produce a frame";
                return false;
            }
            if (!hdr_adapter_.Resolve(context, neural.Get(), true, error)) return false;
            if (composed_hdr_.Get() != hdr_adapter_.Output()) {
                composed_hdr_srv_.Reset(); composed_hdr_.Reset();
                if (FAILED(device_->CreateShaderResourceView(hdr_adapter_.Output(), nullptr, &composed_hdr_srv_))) {
                    error = "Native SNR HDR view creation failed"; return false;
                }
                composed_hdr_ = hdr_adapter_.Output();
            }
            color = composed_hdr_srv_;
            debug_neural_ = neural;
        }
        native_hdr_executed_ = true;
        return true;
    }

    void CompleteNativeTonemap(ID3D11DeviceContext* context) {
        if (!NativeSnrEnabled() || !native_hdr_executed_ || native_upscaled_this_frame_) return;
        ComPtr<ID3D11RenderTargetView> target;
        ComPtr<ID3D11Resource> resource;
        context->OMGetRenderTargets(1, &target, nullptr);
        if (target) target->GetResource(&resource);
        if (!resource) return;
        ValheimNr_Dx11::MarkPreXeSSOutput(context, resource.Get());
        native_upscaled_this_frame_ = true;
        if (reset_history_) history_frames_ = 0;
        ++history_frames_;
        last_applied_neural_ = native_neural_applied_ = native_want_neural_;
        if (++frame_ == 1 || frame_ % 60 == 0)
            LOG_INFO("Tekken native TAA->SNR frame={} source={}x{} snr={}x{} neural={} XeSS=false reset={}",
                frame_, input_width_, input_height_, width_, height_, native_want_neural_, reset_history_);
        reset_history_ = false;
    }

    void Reset() {
        if (xess_ && XeSSProxy::D3D11DestroyContext())
            XeSSProxy::D3D11DestroyContext()(xess_);
        xess_ = nullptr;
        preparation_.Reset();
        hdr_post_.Reset();
        hdr_adapter_.Reset();
        hdr_exposure_.Reset();
        display_motion_.Reset();
        composed_hdr_.Reset(); composed_hdr_srv_.Reset();
        native_hdr_executed_ = false;
        native_low_ = {};
        debug_neural_.Reset();
        native_want_neural_ = false;
        hdr_submissions_ = skipped_post_count_ = 0;
        history_frames_ = 0;
        previous_jitter_x_ = previous_jitter_y_ = 0;
        output_.Reset();
        output_srv_.Reset();
        ldr_input_.Reset();
        ldr_uav_.Reset();
        ldr_shader_.Reset();
        present_shader_.Reset();
        native_motion_.Reset();
        native_depth_.Reset();
        native_taa_target_.Reset();
        isolated_state_.Reset();
        device_.Reset();
        width_ = height_ = input_width_ = input_height_ = 0;
        reset_history_ = true;
        last_applied_neural_ = false;
        native_frame_ready_ = false;
        native_upscaled_this_frame_ = false;
        missed_upscale_count_ = 0;
        clamped_jitter_count_ = 0;
        native_neural_applied_ = false;
        native_ = false;
        native_jitter_x_ = native_jitter_y_ = 0.0f;
        frame_ = 0;
    }

    UINT frame() const { return frame_; }
    NativeCapture CaptureInputs() const {
        NativeCapture capture;
        capture.hdr = native_low_.hdr;
        capture.taa = native_taa_target_;
        capture.history_frames = history_frames_;
        if (native_want_neural_) {
            capture.neural_input = NativeSnrEnabled() ? native_taa_target_
                : NeuralAfterXeSSEnabled() ? output_ : native_low_.hdr;
            capture.proxy = hdr_adapter_.Proxy();
            capture.neural = debug_neural_;
            capture.resolved = hdr_adapter_.Output();
        }
        capture.motion = native_motion_; capture.depth = native_depth_;
        capture.neural_motion = (NativeSnrEnabled() || NeuralAfterXeSSEnabled())
            ? display_motion_.Output() : native_low_.neural_motion.Get();
        capture.xess = output_; capture.exposure = hdr_exposure_.Texture();
        return capture;
    }
    bool neural_applied() const { return last_applied_neural_; }
    void InvalidateHistory() { reset_history_ = true; }

private:
    bool Initialize(ID3D11Device* device, UINT width, UINT height,
                    std::string& error, bool native = false,
                    UINT source_width = 0, UINT source_height = 0) {
        if (!source_width) source_width = width / 2;
        if (!source_height) source_height = height / 2;
        if (device_.Get() == device && width_ == width && height_ == height &&
            input_width_ == source_width && input_height_ == source_height &&
            native_ == native && isolated_state_ &&
            (NativeSnrEnabled() || (xess_ && output_))) return true;
        // Initialization is allowed to touch the immediate context as well.
        // In HDR mode the game's original TAA draw still follows this call.
        ComPtr<ID3D11Device1> device1;
        ComPtr<ID3D11DeviceContext> immediate;
        ComPtr<ID3DDeviceContextState> private_state;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1)))) {
            error = "D3D11.1 device is unavailable";
            return false;
        }
        constexpr D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        D3D_FEATURE_LEVEL chosen {};
        if (FAILED(device1->CreateDeviceContextState(0, &level, 1,
                D3D11_SDK_VERSION, __uuidof(ID3D11Device), &chosen,
                &private_state)) || chosen != level) {
            error = "D3D11 context state creation failed";
            return false;
        }
        device->GetImmediateContext(&immediate);
        ContextScope initialization_scope(immediate.Get(), private_state.Get());
        if (!initialization_scope) { error = "XeSS initialization context isolation failed"; return false; }
        Reset();
        if (NativeSnrEnabled()) {
            if (!native || source_width != width || source_height != height) {
                error = "Native SNR requires input resolution equal to display resolution"; return false;
            }
            isolated_state_ = private_state;
            device_ = device;
            width_ = input_width_ = width;
            height_ = input_height_ = height;
            native_ = true;
            LOG_INFO("Tekken native SNR initialized: {}x{}, game TAA, no XeSS context or execution", width, height);
            return true;
        }
        LOG_INFO("Tekken XeSS initialization in isolated context: {}x{} native={} HDR={}", width, height, native, NativeHdrEnabled());
        if (!XeSSProxy::InitXeSSDx11() || !XeSSProxy::D3D11CreateContext() ||
            !XeSSProxy::D3D11Init() || !XeSSProxy::D3D11Execute() ||
            !XeSSProxy::D3D11GetOptimalInputResolution() ||
            !XeSSProxy::D3D11DestroyContext()) {
            error = "XeSS D3D11 runtime is not installed";
            return false;
        }
        ScopedSkipSpoofingGlobal skip_spoofing {};
        const xess_2d_t output_resolution {width, height};
        const xess_2d_t input_resolution {source_width, source_height};
        if (!Check(XeSSProxy::D3D11CreateContext()(device, &xess_),
                   "xessD3D11CreateContext", error)) return false;
        // Query this runtime's presets. Scale factors changed in XeSS 1.3;
        // neither the preset names nor a fixed 2x assumption define the input.
        const xess_quality_settings_t presets[] = {
            XESS_QUALITY_SETTING_AA, XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS,
            XESS_QUALITY_SETTING_ULTRA_QUALITY, XESS_QUALITY_SETTING_QUALITY,
            XESS_QUALITY_SETTING_BALANCED, XESS_QUALITY_SETTING_PERFORMANCE,
            XESS_QUALITY_SETTING_ULTRA_PERFORMANCE};
        xess_quality_settings_t quality = XESS_QUALITY_SETTING_BALANCED;
        double best_distance = std::numeric_limits<double>::max();
        for (const auto candidate : presets) {
            xess_2d_t optimal {}, minimum {}, maximum {};
            const auto status = XeSSProxy::D3D11GetOptimalInputResolution()(
                xess_, &output_resolution, candidate, &optimal, &minimum, &maximum);
            if (status < XESS_RESULT_SUCCESS || source_width < minimum.x ||
                source_height < minimum.y || source_width > maximum.x || source_height > maximum.y)
                continue;
            const double distance = std::abs(double(optimal.x) - source_width) / width +
                                    std::abs(double(optimal.y) - source_height) / height;
            if (distance < best_distance) { best_distance = distance; quality = candidate; }
        }
        if (best_distance == std::numeric_limits<double>::max()) {
            error = "TAA input resolution is outside all XeSS preset ranges";
            return false;
        }
        LOG_INFO("Tekken XeSS preset={} input={}x{} output={}x{} recommended additional mip bias={:.4f}",
                 int(quality), source_width, source_height, width, height,
                 std::log2(float(source_width) / width));
        xess_d3d11_init_params_t init {};
        init.outputResolution = output_resolution;
        init.qualitySetting = quality;
        init.initFlags = XESS_INIT_FLAG_INVERTED_DEPTH;
        if (native && !NativeHdrEnabled()) init.initFlags |= XESS_INIT_FLAG_LDR_INPUT_COLOR;
        if (!native) init.initFlags |= XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE;
        if (!Check(XeSSProxy::D3D11Init()(xess_, &init),
                   "xessD3D11Init", error)) return false;
        isolated_state_ = private_state;
        D3D11_TEXTURE2D_DESC color_desc {};
        color_desc.Width = width;
        color_desc.Height = height;
        color_desc.MipLevels = color_desc.ArraySize = 1;
        color_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        color_desc.SampleDesc.Count = 1;
        color_desc.Usage = D3D11_USAGE_DEFAULT;
        color_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE |
                               D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(device->CreateTexture2D(&color_desc, nullptr, &output_))) {
            error = "XeSS output texture creation failed";
            return false;
        }
        if (native && FAILED(device->CreateShaderResourceView(
                output_.Get(), nullptr, &output_srv_))) {
            error = "XeSS final color SRV creation failed";
            return false;
        }
        color_desc.Width = input_resolution.x;
        color_desc.Height = input_resolution.y;
        ComPtr<ID3DBlob> code, messages;
        if (native && !NativeHdrEnabled()) {
            if (FAILED(device->CreateTexture2D(&color_desc, nullptr, &ldr_input_)) ||
                FAILED(device->CreateUnorderedAccessView(ldr_input_.Get(), nullptr,
                                                        &ldr_uav_))) {
                error = "XeSS LDR input allocation failed";
                return false;
            }
            code.Reset();
            messages.Reset();
            const HRESULT ldr_compile = D3DCompile(kLdrConvertShader,
                strlen(kLdrConvertShader), "TekkenXeSSLdrInput", nullptr,
                nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3,
                0, &code, &messages);
            if (FAILED(ldr_compile) || FAILED(device->CreateComputeShader(
                    code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                    &ldr_shader_))) {
                error = "XeSS LDR conversion shader failed";
                return false;
            }
        }
        if (native) {
            code.Reset();
            messages.Reset();
            const HRESULT present_compile = D3DCompile(kPresentShader,
                strlen(kPresentShader), "TekkenXeSSPresent", nullptr, nullptr,
                "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                &code, &messages);
            if (FAILED(present_compile) || FAILED(device->CreatePixelShader(
                    code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                    &present_shader_))) {
                error = "XeSS final pixel shader failed";
                return false;
            }
        }
        device_ = device;
        width_ = width;
        height_ = height;
        input_width_ = input_resolution.x;
        input_height_ = input_resolution.y;
        native_ = native;
        LOG_INFO("Tekken XeSS isolated initialization completed");
        return true;
    }

    bool EncodeHdr(ID3D11DeviceContext* context, ID3D11Texture2D* hdr,
                   ID3D11ShaderResourceView* exposure, std::string& error) {
        const auto* cfg = Config::Instance();
        DlssNrConstants settings {};
        settings.WhitePoint = std::max(0.01f, cfg->DlssNrWhitePointScale.value_or_default());
        settings.ExposurePreMul = std::max(0.01f, cfg->DlssNrWhitePointTrim.value_or_default());
        settings.TransferStrength = cfg->DlssNrTransferStrength.value_or_default();
        settings.ColourStrength = cfg->DlssNrColourStrength.value_or_default();
        settings.MaxRatio = cfg->DlssNrMaxRatio.value_or_default();
        settings.ReversibleMode = cfg->DlssNrReversibleMode.value_or_default();
        settings.ApplyModel = 1;
        settings.CompareZoom = 1;
        return hdr_adapter_.Encode(context, hdr, settings, exposure, error);
    }

    TekkenTemporalInputsDx11::Processor preparation_;
    TekkenHdrPostProcessDx11::Bridge hdr_post_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3DDeviceContextState> isolated_state_;
    NrHdrAdapterDx11::Adapter hdr_adapter_;
    NrHdrAdapterDx11::Exposure hdr_exposure_;
    NrDisplayMotionDx11::Reconstructor display_motion_;
    ComPtr<ID3D11Texture2D> composed_hdr_;
    ComPtr<ID3D11ShaderResourceView> composed_hdr_srv_;
    bool native_hdr_executed_ = false;
    ComPtr<ID3D11Texture2D> debug_neural_;
    uint64_t hdr_submissions_ = 0;
    TekkenTemporalInputsDx11::Outputs native_low_;
    bool native_want_neural_ = false;
    ComPtr<ID3D11ComputeShader> ldr_shader_;
    ComPtr<ID3D11PixelShader> present_shader_;
    ComPtr<ID3D11Texture2D> output_;
    ComPtr<ID3D11Texture2D> ldr_input_, native_motion_, native_depth_;
    ComPtr<ID3D11Texture2D> native_taa_target_;
    ComPtr<ID3D11UnorderedAccessView> ldr_uav_;
    ComPtr<ID3D11ShaderResourceView> output_srv_;
    xess_context_handle_t xess_ = nullptr;
    UINT width_ = 0, height_ = 0, input_width_ = 0, input_height_ = 0;
    UINT frame_ = 0;
    bool reset_history_ = true;
    bool last_applied_neural_ = false;
    bool native_ = false;
    bool native_frame_ready_ = false;
    bool native_neural_applied_ = false;
    float native_jitter_x_ = 0.0f;
    float native_jitter_y_ = 0.0f;
    float previous_jitter_x_ = 0.0f, previous_jitter_y_ = 0.0f;
    UINT history_frames_ = 0;
    bool native_upscaled_this_frame_ = false;
    uint64_t missed_upscale_count_ = 0;
    uint64_t clamped_jitter_count_ = 0;
    uint64_t skipped_post_count_ = 0;
    uint64_t native_size_mismatch_count_ = 0;
};

Chain chain;
std::mutex chain_mutex;
bool failed = false;
}

bool NativeHdrEnabled() {
    static const bool enabled = [] {
        std::error_code error;
        return std::filesystem::exists(Util::DllPath().parent_path() /
            "OptiScaler.snr-xess.hdr.enable", error);
    }();
    return enabled;
}

bool NeuralAfterXeSSEnabled() {
    static const bool enabled = [] {
        std::error_code error;
        return std::filesystem::exists(Util::DllPath().parent_path() /
            "OptiScaler.xess-snr.enable", error);
    }();
    return NativeHdrEnabled() && enabled && !NativeSnrEnabled();
}

bool NativeSnrEnabled() {
    static const bool enabled = [] {
        std::error_code error;
        return std::filesystem::exists(Util::DllPath().parent_path() /
            "OptiScaler.snr-native.enable", error);
    }();
    return NativeHdrEnabled() && enabled;
}

bool TryPrepareNativeTonemap(ID3D11DeviceContext* context,
    ID3D11ShaderResourceView* exposure, ComPtr<ID3D11ShaderResourceView>& color) {
    color.Reset();
    if (!context || !NativeSnrEnabled()) return false;
    std::lock_guard lock(chain_mutex);
    if (failed) return false;
    std::string error;
    const bool ready = chain.PrepareNativeTonemap(context, exposure, color, error);
    if (!error.empty()) {
        LOG_ERROR("Tekken native SNR disabled; original tone mapping retained: {}", error);
        chain.Reset(); failed = true; color.Reset();
    }
    return ready;
}

void CompleteNativeTonemap(ID3D11DeviceContext* context) {
    std::lock_guard lock(chain_mutex);
    chain.CompleteNativeTonemap(context);
}

NativeCapture CaptureInputs() {
    std::lock_guard lock(chain_mutex);
    return chain.CaptureInputs();
}

void ReplayNativeHdrPostProcess(ID3D11DeviceContext* context,
    TekkenHdrPostProcessDx11::Pass pass, TekkenHdrPostProcessDx11::DrawIndexed draw,
    UINT count, UINT start, INT base, ID3D11ShaderResourceView* exposure) {
    if (!context || !NativeHdrEnabled()) return;
    std::lock_guard lock(chain_mutex);
    if (failed) return;
    std::string error;
    chain.ReplayHdr(context, pass, draw, count, start, base, error, exposure);
    if (!error.empty()) {
        LOG_ERROR("Tekken HDR postprocess disabled; game output retained: {}", error);
        chain.Reset();
        failed = true;
    }
}

bool TryReplaceTaaDraw(ID3D11DeviceContext* context) {
    if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
        return false;
    std::lock_guard lock(chain_mutex);
    if (failed) return false;
    std::string error;
    if (chain.Execute(context, error)) {
        if (chain.frame() == 1 || chain.frame() % 60 == 0)
            LOG_INFO("Tekken temporal XeSS replaced TAA draw, frame={} neural={}",
                     chain.frame(), chain.neural_applied());
        return true;
    }
    if (!error.empty()) {
        if (error == "Tekken TAA inputs or immediate D3D11 context are missing" ||
            error == "Unsupported Tekken TAA input dimensions or formats" ||
            error == "Unexpected Tekken depth or packed motion SRV format" ||
            error == "XeSS Balanced input dimensions differ from TAA inputs") {
            chain.InvalidateHistory();
            return false;
        }
        LOG_ERROR("Tekken SNR->XeSS disabled; original TAA retained: {}", error);
        chain.Reset();
        failed = true;
    }
    return false;
}

bool TryProcessNativeTaaDraw(ID3D11DeviceContext* context,
                             float jitter_x, float jitter_y,
                             UINT display_width, UINT display_height) {
    if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
        return false;
    std::lock_guard lock(chain_mutex);
    if (failed) return false;
    std::string error;
    const bool replace = chain.ProcessNativeTaa(context,
        jitter_x, jitter_y, display_width, display_height, error);
    if (!error.empty()) {
        if (error == "Tekken TAA inputs or immediate D3D11 context are missing" ||
            error == "Unsupported Tekken TAA input dimensions or formats" ||
            error == "Unexpected Tekken depth or packed motion SRV format") {
            chain.InvalidateHistory();
            return false;
        }
        LOG_ERROR("Tekken native SNR->XeSS TAA disabled: {}", error);
        chain.Reset();
        failed = true;
    }
    // HDR mode runs the original low-resolution TAA for bloom/exposure and
    // safe fallback. The high-resolution color travels in a separate branch.
    return replace && !NativeHdrEnabled();
}

bool TryPrepareNativeUpscale(ID3D11DeviceContext* context,
    ComPtr<ID3D11PixelShader>& shader,
    ComPtr<ID3D11ShaderResourceView>& color) {
    shader.Reset();
    color.Reset();
    if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
        return false;
    std::lock_guard lock(chain_mutex);
    if (failed) return false;
    std::string error;
    const bool ready = chain.PrepareNativeUpscale(context, shader, color, error);
    if (!error.empty()) {
        LOG_ERROR("Tekken native SNR->XeSS upscale disabled: {}", error);
        chain.Reset();
        failed = true;
        return false;
    }
    if (ready && (chain.frame() == 1 || chain.frame() % 60 == 0))
        LOG_INFO("Tekken native {} frame={} neural={} preTonemap={}",
                 NeuralAfterXeSSEnabled() ? "XeSS->SNR" : "SNR->XeSS",
                 chain.frame(), chain.neural_applied(), NativeHdrEnabled());
    return ready;
}

void EndNativeFrame() {
    std::lock_guard lock(chain_mutex);
    chain.EndNativeFrame();
}
}
