#include "TekkenTemporalInputs_Dx11.h"

#include <d3dcompiler.h>
#include <array>
#include <cstring>
#include <cmath>

namespace TekkenTemporalInputsDx11 {
namespace {
using Microsoft::WRL::ComPtr;

constexpr const char* kPrepareShader = R"(
Texture2D<float4> Color : register(t0);
Texture2D<float> Depth : register(t1);
Texture2D<float2> PackedMotion : register(t2);
cbuffer SceneConstants : register(b0) { float4 Scene[64]; };
cbuffer ViewConstants : register(b1) { float4 View[256]; };
cbuffer HistoryJitter : register(b2) { float2 JitterDelta; float2 Padding; };
RWTexture2D<float4> OutHdr : register(u0);
RWTexture2D<float2> OutNeuralMotion : register(u1);
RWTexture2D<float2> OutMotion : register(u2);
RWTexture2D<float> OutDepth : register(u3);

[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID) {
    uint fullWidth, fullHeight;
    Color.GetDimensions(fullWidth, fullHeight);
    uint2 low = dispatchId.xy;
#if NATIVE_INPUT
    if (low.x >= fullWidth || low.y >= fullHeight) return;
    uint2 source = low;
#else
    if (low.x >= fullWidth / 2 || low.y >= fullHeight / 2) return;
    uint2 source = low * 2;
#endif
    float4 hdr = 0;
    float nearestDepth = -1;
    uint2 nearest = source;
#if NATIVE_INPUT
    hdr = Color.Load(int3(source, 0));
    nearestDepth = Depth.Load(int3(source, 0));
#else
    [unroll] for (uint y = 0; y < 2; ++y) {
        [unroll] for (uint x = 0; x < 2; ++x) {
            uint2 pixel = source + uint2(x, y);
            hdr += Color.Load(int3(pixel, 0));
            float depth = Depth.Load(int3(pixel, 0));
            // Tekken uses reverse Z. The nearest surface has the largest depth.
            if (depth > nearestDepth) {
                nearestDepth = depth;
                nearest = pixel;
            }
        }
    }
    hdr *= 0.25;
#endif
    float2 uv = (float2(nearest) + 0.5) / float2(fullWidth, fullHeight);
    float2 ndc = (uv * Scene[1].xy - 0.5 - Scene[17].zw) / Scene[17].xy;
    float4 position = float4(ndc, nearestDepth, 1);
    float4 reprojection = position.x * View[108] + position.y * View[109]
                        + position.z * View[110] + View[111];
    float2 cameraMotion = abs(reprojection.w) > 1.0e-8
        ? ndc - reprojection.xy / reprojection.w : 0;
    float2 packed = PackedMotion.Load(int3(nearest, 0));
    float2 objectMotion = packed * 4.008016 - 2.003978;
    float2 ndcMotion = packed.x * Scene[41].w > 0 ? objectMotion : cameraMotion;
    float2 previousUv = ((ndc - ndcMotion) * Scene[17].xy
                       + Scene[17].zw + 0.5) * Scene[1].zw;
    OutHdr[low] = hdr;
    float2 motionPixels = (previousUv - uv) * float2(
#if NATIVE_INPUT
        fullWidth, fullHeight
#else
        fullWidth / 2, fullHeight / 2
#endif
    );
    OutMotion[low] = motionPixels;
    OutNeuralMotion[low] = motionPixels + JitterDelta;
    OutDepth[low] = nearestDepth;
}
)";

bool TextureDesc(ID3D11ShaderResourceView* view, D3D11_TEXTURE2D_DESC& desc) {
    if (!view) return false;
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource.As(&texture))) return false;
    texture->GetDesc(&desc);
    return true;
}

bool ConstantsDesc(ID3D11Buffer* buffer, UINT expected_bytes) {
    if (!buffer) return false;
    D3D11_BUFFER_DESC desc {};
    buffer->GetDesc(&desc);
    return desc.ByteWidth == expected_bytes &&
           (desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0;
}

struct ComputeState {
    explicit ComputeState(ID3D11DeviceContext* value) : context(value) {
        context->CSGetShader(&shader, nullptr, nullptr);
        context->CSGetShaderResources(0, static_cast<UINT>(srvs.size()), srvs.data());
        context->CSGetConstantBuffers(0, static_cast<UINT>(buffers.size()), buffers.data());
        context->CSGetUnorderedAccessViews(0, static_cast<UINT>(uavs.size()), uavs.data());
    }
    ~ComputeState() {
        context->CSSetShader(shader, nullptr, 0);
        context->CSSetShaderResources(0, static_cast<UINT>(srvs.size()), srvs.data());
        context->CSSetConstantBuffers(0, static_cast<UINT>(buffers.size()), buffers.data());
        context->CSSetUnorderedAccessViews(0, static_cast<UINT>(uavs.size()), uavs.data(), nullptr);
        if (shader) shader->Release();
        for (auto* item : srvs) if (item) item->Release();
        for (auto* item : buffers) if (item) item->Release();
        for (auto* item : uavs) if (item) item->Release();
    }
    ID3D11DeviceContext* context;
    ID3D11ComputeShader* shader = nullptr;
    std::array<ID3D11ShaderResourceView*, 3> srvs {};
    std::array<ID3D11Buffer*, 3> buffers {};
    std::array<ID3D11UnorderedAccessView*, 4> uavs {};
};
}  // namespace

void Processor::Reset() {
    for (auto& texture : textures_) texture.Reset();
    for (auto& uav : uavs_) uav.Reset();
    shader_.Reset();
    jitter_constants_.Reset();
    device_.Reset();
    width_ = height_ = 0;
    scale_ = InputScale::Downsample2x;
}

bool Processor::Initialize(ID3D11Device* device, UINT width, UINT height,
                           InputScale scale, std::string& error) {
    if (device_.Get() == device && width_ == width && height_ == height &&
        scale_ == scale && shader_)
        return true;
    Reset();
    ComPtr<ID3DBlob> bytecode, messages;
    const D3D_SHADER_MACRO macros[] = {
        {"NATIVE_INPUT", scale == InputScale::Native ? "1" : "0"},
        {nullptr, nullptr}};
    const HRESULT compile = D3DCompile(kPrepareShader, strlen(kPrepareShader),
        "TekkenTemporalInputs", macros, nullptr, "main", "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bytecode, &messages);
    if (FAILED(compile)) {
        error = messages ? std::string(static_cast<const char*>(messages->GetBufferPointer()),
                                         messages->GetBufferSize()) : "D3D11 input shader compile failed";
        return false;
    }
    if (FAILED(device->CreateComputeShader(bytecode->GetBufferPointer(),
                                           bytecode->GetBufferSize(), nullptr, &shader_))) {
        error = "D3D11 input shader creation failed";
        return false;
    }
    constexpr DXGI_FORMAT formats[] = {
        DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16_FLOAT,
        DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32_FLOAT};
    for (size_t i = 0; i < 4; ++i) {
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = formats[i];
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &textures_[i])) ||
            FAILED(device->CreateUnorderedAccessView(textures_[i].Get(), nullptr, &uavs_[i]))) {
            error = "D3D11 input texture or UAV creation failed";
            Reset();
            return false;
        }
    }
    D3D11_BUFFER_DESC jitter_desc {};
    jitter_desc.ByteWidth = 16;
    jitter_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&jitter_desc, nullptr, &jitter_constants_))) {
        error = "SNR history jitter constants allocation failed";
        Reset(); return false;
    }
    device_ = device;
    width_ = width;
    height_ = height;
    scale_ = scale;
    return true;
}

bool Processor::Prepare(ID3D11DeviceContext* context, const Sources& sources,
                        Outputs& outputs, std::string& error, InputScale scale) {
    outputs = {};
    if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
        !sources.color || !sources.depth || !sources.packed_motion ||
        !sources.scene_constants || !sources.view_constants ||
        !std::isfinite(sources.history_jitter_x) || !std::isfinite(sources.history_jitter_y)) {
        error = "Tekken TAA inputs or immediate D3D11 context are missing";
        return false;
    }
    D3D11_TEXTURE2D_DESC color {}, depth {}, motion {};
    if (!TextureDesc(sources.color, color) || !TextureDesc(sources.depth, depth) ||
        !TextureDesc(sources.packed_motion, motion) ||
        color.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
        motion.Format != DXGI_FORMAT_R16G16_UNORM ||
        depth.Width != color.Width || depth.Height != color.Height ||
        motion.Width != color.Width || motion.Height != color.Height ||
        color.Width < 640 || color.Height < 360 ||
        (scale == InputScale::Downsample2x &&
         ((color.Width & 1) || (color.Height & 1))) ||
        color.MipLevels != 1 || color.ArraySize != 1 ||
        color.SampleDesc.Count != 1 || depth.SampleDesc.Count != 1 ||
        motion.SampleDesc.Count != 1) {
        error = "Unsupported Tekken TAA input dimensions or formats";
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC color_view {}, depth_view {}, motion_view {};
    sources.color->GetDesc(&color_view);
    sources.depth->GetDesc(&depth_view);
    sources.packed_motion->GetDesc(&motion_view);
    if (color_view.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
        depth_view.Format != DXGI_FORMAT_R24_UNORM_X8_TYPELESS ||
        motion_view.Format != DXGI_FORMAT_R16G16_UNORM ||
        color_view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
        depth_view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
        motion_view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
        color_view.Texture2D.MostDetailedMip != 0 ||
        depth_view.Texture2D.MostDetailedMip != 0 ||
        motion_view.Texture2D.MostDetailedMip != 0 ||
        !ConstantsDesc(sources.scene_constants, 64 * 4 * sizeof(float)) ||
        !ConstantsDesc(sources.view_constants, 256 * 4 * sizeof(float))) {
        error = "Unexpected Tekken TAA input view or constant buffer layout";
        return false;
    }
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    const UINT output_width = color.Width /
        (scale == InputScale::Native ? 1u : 2u);
    const UINT output_height = color.Height /
        (scale == InputScale::Native ? 1u : 2u);
    if (!device || !Initialize(device.Get(), output_width, output_height,
                               scale, error))
        return false;
    ComputeState old(context);
    ID3D11ShaderResourceView* inputs[] = {
        sources.color, sources.depth, sources.packed_motion};
    const float jitter[] = {sources.history_jitter_x, sources.history_jitter_y, 0, 0};
    context->UpdateSubresource(jitter_constants_.Get(), 0, nullptr, jitter, 0, 0);
    ID3D11Buffer* constants[] = {sources.scene_constants, sources.view_constants, jitter_constants_.Get()};
    ID3D11UnorderedAccessView* output_views[] = {
        uavs_[0].Get(), uavs_[1].Get(), uavs_[2].Get(), uavs_[3].Get()};
    context->CSSetShader(shader_.Get(), nullptr, 0);
    context->CSSetShaderResources(0, 3, inputs);
    context->CSSetConstantBuffers(0, 3, constants);
    context->CSSetUnorderedAccessViews(0, 4, output_views, nullptr);
    context->Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);
    ID3D11ShaderResourceView* null_inputs[3] {};
    ID3D11UnorderedAccessView* null_outputs[4] {};
    context->CSSetShaderResources(0, 3, null_inputs);
    context->CSSetUnorderedAccessViews(0, 4, null_outputs, nullptr);
    outputs.hdr = textures_[0];
    outputs.motion = textures_[2];
    outputs.neural_motion = textures_[1];
    outputs.depth = textures_[3];
    outputs.width = width_;
    outputs.height = height_;
    return true;
}
}  // namespace TekkenTemporalInputsDx11
