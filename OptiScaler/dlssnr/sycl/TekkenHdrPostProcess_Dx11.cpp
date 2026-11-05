#include "TekkenHdrPostProcess_Dx11.h"
#include <d3dcompiler.h>
#include <cstring>
#include <algorithm>
#include <cmath>

namespace TekkenHdrPostProcessDx11 {
namespace {
using Microsoft::WRL::ComPtr;

constexpr char kPatch[] = R"(
ByteAddressBuffer Source : register(t0);
RWByteAddressBuffer Target : register(u0);
cbuffer Parameters : register(b0) { uint Bytes; uint Grade; float2 Scale; };
[numthreads(64,1,1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint address = id.x * 16;
    if (address >= Bytes) return;
    uint4 bits = Source.Load4(address);
    // Verified UE4 View layout: rect origin, size/inverse-size, buffer size.
    // All other constants (including exposure and grading) stay bit-identical.
    if (id.x == 114) bits.xy = asuint(asfloat(bits.xy) * Scale);
    if (id.x == 115 || (Grade && id.x == 116))
        bits = asuint(asfloat(bits) * float4(Scale, 1.0 / Scale));
    Target.Store4(address, bits);
}
)";

struct ComputeScope {
    ComPtr<ID3D11DeviceContext1> context;
    ComPtr<ID3DDeviceContextState> previous;
    ComputeScope(ID3D11DeviceContext* source, ID3DDeviceContextState* state) {
        if (state && SUCCEEDED(source->QueryInterface(IID_PPV_ARGS(&context))))
            context->SwapDeviceContextState(state, &previous);
    }
    ~ComputeScope() {
        if (previous) context->SwapDeviceContextState(previous.Get(), nullptr);
    }
};

// Only bindings changed by the replay. Compute work uses its own context state.
struct DrawState {
    ID3D11DeviceContext* context;
    UINT srv_slot, cb_slot;
    ComPtr<ID3D11ShaderResourceView> source;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11DeviceContext1> context1;
    UINT first_constant = 0, constant_count = 0;
    UINT viewport_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    UINT scissor_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] {};
    D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] {};
    bool scissor_enabled = false;
    bool modified = false;
    DrawState(ID3D11DeviceContext* c, UINT s, UINT b) : context(c), srv_slot(s), cb_slot(b) {
        c->PSGetShaderResources(s, 1, &source);
        if (SUCCEEDED(c->QueryInterface(IID_PPV_ARGS(&context1))))
            context1->PSGetConstantBuffers1(b, 1, &constants, &first_constant, &constant_count);
        c->OMGetRenderTargets(1, &target, nullptr);
        c->RSGetViewports(&viewport_count, viewports);
        c->RSGetScissorRects(&scissor_count, scissors);
        ComPtr<ID3D11RasterizerState> raster;
        c->RSGetState(&raster);
        if (raster) { D3D11_RASTERIZER_DESC desc {}; raster->GetDesc(&desc); scissor_enabled = desc.ScissorEnable != FALSE; }
    }
    ~DrawState() {
        if (!modified) return;
        ID3D11ShaderResourceView* srv = source.Get();
        ID3D11Buffer* cb = constants.Get();
        ID3D11RenderTargetView* rtv = target.Get();
        context->PSSetShaderResources(srv_slot, 1, &srv);
        context1->PSSetConstantBuffers1(cb_slot, 1, &cb, &first_constant, &constant_count);
        context->OMSetRenderTargets(1, &rtv, nullptr);
        context->RSSetViewports(viewport_count, viewports);
        context->RSSetScissorRects(scissor_count, scissors);
    }
};

bool IsSingleColorTarget(ID3D11DeviceContext* context) {
    ID3D11RenderTargetView* targets[8] {};
    ID3D11DepthStencilView* depth = nullptr;
    ID3D11UnorderedAccessView* uavs[8] {};
    context->OMGetRenderTargets(8, targets, &depth);
    context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 8, uavs);
    bool valid = targets[0] && !depth;
    for (unsigned i = 0; i < 8; ++i) {
        if (i && targets[i]) valid = false;
        if (uavs[i]) { valid = false; uavs[i]->Release(); }
        if (targets[i]) targets[i]->Release();
    }
    if (depth) depth->Release();
    return valid;
}
}

void Bridge::BeginFrame(ID3D11Texture2D* low_hdr, ID3D11ShaderResourceView* high_hdr) {
    EndFrame();
    low_[0] = low_hdr;
    high_hdr_ = high_hdr;
}

bool Bridge::Allocate(ID3D11Device* device, Stage& stage, UINT bytes,
                      UINT width, UINT height, std::string& error) {
    if (stage.output && stage.constants && stage.raw_srv && stage.raw_uav &&
        stage.rtv && stage.srv && patch_shader_ && patch_params_) {
        D3D11_TEXTURE2D_DESC desc {};
        D3D11_BUFFER_DESC buffer {};
        stage.output->GetDesc(&desc);
        stage.constants->GetDesc(&buffer);
        if (desc.Width == width && desc.Height == height && buffer.ByteWidth == bytes)
            return true;
    }
    stage = {};
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = width; desc.Height = height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &stage.output)) ||
        FAILED(device->CreateRenderTargetView(stage.output.Get(), nullptr, &stage.rtv)) ||
        FAILED(device->CreateShaderResourceView(stage.output.Get(), nullptr, &stage.srv))) {
        error = "HDR postprocess target allocation failed"; return false;
    }
    D3D11_BUFFER_DESC buffer {};
    buffer.ByteWidth = bytes;
    buffer.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    buffer.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if (FAILED(device->CreateBuffer(&buffer, nullptr, &stage.raw_input))) {
        error = "HDR constant input allocation failed"; return false;
    }
    buffer.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(device->CreateBuffer(&buffer, nullptr, &stage.raw_output))) {
        error = "HDR constant output allocation failed"; return false;
    }
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buffer.MiscFlags = 0;
    if (FAILED(device->CreateBuffer(&buffer, nullptr, &stage.constants))) {
        error = "HDR patched constant allocation failed"; return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC srv {};
    srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.BufferEx.NumElements = bytes / 4;
    srv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav {};
    uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.Buffer.NumElements = bytes / 4;
    uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    if (FAILED(device->CreateShaderResourceView(stage.raw_input.Get(), &srv, &stage.raw_srv)) ||
        FAILED(device->CreateUnorderedAccessView(stage.raw_output.Get(), &uav, &stage.raw_uav))) {
        error = "HDR raw constant views failed"; return false;
    }
    if (!patch_shader_ || !patch_params_) {
        ComPtr<ID3DBlob> code, messages;
        if (FAILED(D3DCompile(kPatch, sizeof(kPatch) - 1, "HdrPostProcessConstants",
                nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3,
                0, &code, &messages)) || FAILED(device->CreateComputeShader(
                    code->GetBufferPointer(), code->GetBufferSize(), nullptr, &patch_shader_))) {
            error = "HDR constant patch shader failed"; return false;
        }
        buffer.ByteWidth = 16;
        if (FAILED(device->CreateBuffer(&buffer, nullptr, &patch_params_))) {
            error = "HDR patch parameters allocation failed"; return false;
        }
    }
    return true;
}

bool Bridge::Replay(ID3D11DeviceContext* context, ID3DDeviceContextState* isolated,
                    Pass pass, DrawIndexed draw, UINT count, UINT start, INT base,
                    std::string& error) {
    skip_reason_.clear();
    const auto skip = [this](std::string reason) { skip_reason_ = std::move(reason); return false; };
    const unsigned index = pass == Pass::ToneMap ? 0 : 1;
    if (!context || !draw || !high_hdr_ || completed_ != index || !low_[index] ||
        context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
        !IsSingleColorTarget(context)) return skip("pass order or target bindings");
    DrawState saved(context, index ? 3 : 1, index ? 0 : 1);
    if (!saved.source || !saved.target || !saved.constants || saved.viewport_count != 1)
        return skip("missing source/target/constants or viewport count=" + std::to_string(saved.viewport_count));
    ComPtr<ID3D11Resource> source, target;
    saved.source->GetResource(&source);
    saved.target->GetResource(&target);
    if (source.Get() != low_[index].Get() || target.Get() == source.Get()) return skip("resource chain mismatch");
    ComPtr<ID3D11Texture2D> texture, high_texture;
    if (FAILED(target.As(&texture))) return skip("target is not Texture2D");
    D3D11_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);
    D3D11_RENDER_TARGET_VIEW_DESC view {};
    saved.target->GetDesc(&view);
    D3D11_SHADER_RESOURCE_VIEW_DESC source_view {};
    saved.source->GetDesc(&source_view);
    high_hdr_->GetResource(&source);
    if (FAILED(source.As(&high_texture))) return skip("HDR is not Texture2D");
    D3D11_TEXTURE2D_DESC high_desc {};
    high_texture->GetDesc(&high_desc);
    D3D11_BUFFER_DESC buffer {};
    saved.constants->GetDesc(&buffer);
    const auto& vp = saved.viewports[0];
    if (view.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
        high_desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
        high_desc.SampleDesc.Count != 1 || high_desc.ArraySize != 1 || high_desc.MipLevels != 1 ||
        view.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || view.Texture2D.MipSlice ||
        source_view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
        source_view.Texture2D.MostDetailedMip ||
        source_view.Format != (index ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT) ||
        desc.SampleDesc.Count != 1 || desc.ArraySize != 1 || desc.MipLevels != 1 ||
        desc.Width > high_desc.Width || desc.Height > high_desc.Height ||
        desc.Width * 3 < high_desc.Width || desc.Height * 3 < high_desc.Height ||
        buffer.ByteWidth < (index ? 148u : 116u) * 16 || buffer.ByteWidth > 65536 ||
        buffer.ByteWidth % 16 || vp.TopLeftX != 0 || vp.TopLeftY != 0 ||
        vp.Width != desc.Width || vp.Height != desc.Height)
        return skip("format/dimensions/viewport: RTV=" + std::to_string(view.Format) + " SRV=" +
            std::to_string(source_view.Format) + " CBbytes=" + std::to_string(buffer.ByteWidth) +
            " vp=" + std::to_string(vp.TopLeftX) + "," + std::to_string(vp.TopLeftY) + "," +
            std::to_string(vp.Width) + "x" + std::to_string(vp.Height) + " target=" +
            std::to_string(desc.Width) + "x" + std::to_string(desc.Height));
    // Guard constant-buffer ranges used through the D3D11.1 API as well.
    if (!saved.context1 || saved.first_constant || saved.constant_count < buffer.ByteWidth / 16)
        return skip("CB range=" + std::to_string(saved.first_constant) + "," + std::to_string(saved.constant_count));
    for (UINT i = 0; saved.scissor_enabled && i < saved.scissor_count; ++i) {
        const auto& r = saved.scissors[i];
        if (r.right < r.left || r.bottom < r.top) return skip("inverted scissor rectangle");
    }
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    Stage& stage = stages_[index];
    if (!Allocate(device.Get(), stage, buffer.ByteWidth, high_desc.Width, high_desc.Height, error))
        return false;
    {
        ComputeScope scope(context, isolated);
        if (!scope.previous) { error = "HDR context isolation unavailable"; return false; }
        context->CopyResource(stage.raw_input.Get(), saved.constants.Get());
        const struct { UINT bytes, grade; float scale_x, scale_y; } parameters {
            buffer.ByteWidth, index, float(high_desc.Width) / desc.Width, float(high_desc.Height) / desc.Height};
        context->UpdateSubresource(patch_params_.Get(), 0, nullptr, &parameters, 0, 0);
        ID3D11Buffer* cb = patch_params_.Get();
        ID3D11ShaderResourceView* srv = stage.raw_srv.Get();
        ID3D11UnorderedAccessView* uav = stage.raw_uav.Get();
        context->CSSetShader(patch_shader_.Get(), nullptr, 0);
        context->CSSetConstantBuffers(0, 1, &cb);
        context->CSSetShaderResources(0, 1, &srv);
        context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        context->Dispatch((buffer.ByteWidth / 16 + 63) / 64, 1, 1);
        srv = nullptr; uav = nullptr;
        context->CSSetShaderResources(0, 1, &srv);
        context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        context->CopyResource(stage.constants.Get(), stage.raw_output.Get());
    }
    D3D11_VIEWPORT viewport = vp;
    viewport.Width = float(high_desc.Width); viewport.Height = float(high_desc.Height);
    const float scale_x = float(high_desc.Width) / desc.Width;
    const float scale_y = float(high_desc.Height) / desc.Height;
    D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] {};
    for (UINT i = 0; i < saved.scissor_count; ++i) {
        const auto& r = saved.scissors[i];
        // UE can keep a display-sized scissor on a render-sized viewport.
        // D3D11 clips to their intersection; scale that intersection, not an
        // assumed exact match, and restore the original rectangle afterwards.
        scissors[i] = saved.scissor_enabled ? D3D11_RECT {
            LONG(std::floor(std::clamp(r.left, 0L, LONG(desc.Width)) * scale_x)),
            LONG(std::floor(std::clamp(r.top, 0L, LONG(desc.Height)) * scale_y)),
            LONG(std::ceil(std::clamp(r.right, 0L, LONG(desc.Width)) * scale_x)),
            LONG(std::ceil(std::clamp(r.bottom, 0L, LONG(desc.Height)) * scale_y))} : r;
    }
    ID3D11ShaderResourceView* srv = index ? stages_[0].srv.Get() : high_hdr_.Get();
    ID3D11Buffer* cb = stage.constants.Get();
    ID3D11RenderTargetView* rtv = stage.rtv.Get();
    saved.modified = true;
    context->OMSetRenderTargets(1, &rtv, nullptr);
    context->PSSetShaderResources(saved.srv_slot, 1, &srv);
    context->PSSetConstantBuffers(saved.cb_slot, 1, &cb);
    context->RSSetViewports(1, &viewport);
    context->RSSetScissorRects(saved.scissor_count, scissors);
    draw(context, count, start, base);
    low_[index + 1] = target;
    ++completed_;
    return true;
}

bool Bridge::FinalColor(ID3D11ShaderResourceView* low_color,
                        ComPtr<ID3D11ShaderResourceView>& high_color) const {
    high_color.Reset();
    if (!low_color || completed_ == 0 || completed_ > stages_.size() || !low_[completed_])
        return false;
    ComPtr<ID3D11Resource> resource;
    low_color->GetResource(&resource);
    // Customization feeds tone mapping directly into the final spatial pass;
    // battle scenes may insert a separate grading pass. Select the last replay
    // only when the final draw consumes that exact same-frame game resource.
    if (resource.Get() != low_[completed_].Get()) return false;
    high_color = stages_[completed_ - 1].srv;
    return high_color != nullptr;
}
void Bridge::EndFrame() {
    for (auto& resource : low_) resource.Reset();
    high_hdr_.Reset();
    completed_ = 0;
}
void Bridge::Reset() {
    EndFrame();
    stages_ = {};
    patch_shader_.Reset();
    patch_params_.Reset();
}
}
