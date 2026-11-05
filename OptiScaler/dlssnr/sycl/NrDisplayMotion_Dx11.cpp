#include "NrDisplayMotion_Dx11.h"
#include <d3dcompiler.h>
#include <cmath>

namespace NrDisplayMotionDx11 {
namespace {
using Microsoft::WRL::ComPtr;
constexpr char kShader[] = R"(
Texture2D<float2> Motion : register(t0);
Texture2D<float> Depth : register(t1);
RWTexture2D<float2> Result : register(u0);
cbuffer Parameters : register(b0) {
    uint2 InputSize; uint2 OutputSize;
    float2 RasterJitter; uint ReverseZ; uint Padding;
    float2 InputPerOutput; float2 OutputPerInput;
};
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= OutputSize)) return;
    // A projected point at unjittered p is rasterized at p + RasterJitter.
    // Locate that point in the current render grid; do not add inter-frame
    // jitter to velocity because both SNR color/history grids are unjittered.
    precise float2 samplePosition = (float2(id.xy) + 0.5) * InputPerOutput
                          + RasterJitter - 0.5;
    int2 first = int2(floor(samplePosition));
    float bestDepth = ReverseZ ? -1.0 : 2.0;
    float bestDistance = 1.0e30;
    float2 velocity = 0;
    bool valid = false;
    // Select one surface from the 2x2 footprint. Never interpolate foreground
    // and background velocities. For equal depth use the nearest sample.
    [unroll] for (int y = 0; y < 2; ++y) {
        [unroll] for (int x = 0; x < 2; ++x) {
            int2 pixel = clamp(first + int2(x,y), int2(0,0), int2(InputSize)-1);
            float z = Depth.Load(int3(pixel,0));
            float2 v = Motion.Load(int3(pixel,0));
            float2 delta = float2(pixel) - samplePosition;
            float distance = dot(delta,delta);
            bool nearer = ReverseZ ? z > bestDepth : z < bestDepth;
            if (isfinite(z) && z >= 0 && z <= 1 && all(isfinite(v)) &&
                (!valid || nearer || (z == bestDepth && distance < bestDistance))) {
                bestDepth = z; bestDistance = distance; velocity = v; valid = true;
            }
        }
    }
    precise float2 scaled = velocity * OutputPerInput;
    // An invalid sample must reject history, not silently reuse zero motion.
    Result[id.xy] = valid && all(isfinite(scaled)) && all(abs(scaled) < 65000.0)
        ? scaled : float2(OutputSize) * 2.0;
}
)";

bool TextureValid(ID3D11Texture2D* texture, ID3D11Device* device,
                  DXGI_FORMAT format, D3D11_TEXTURE2D_DESC& desc) {
    if (!texture) return false;
    ComPtr<ID3D11Device> owner;
    texture->GetDevice(&owner);
    texture->GetDesc(&desc);
    return owner.Get() == device && desc.Format == format &&
        desc.MipLevels == 1 && desc.ArraySize == 1 && desc.SampleDesc.Count == 1 &&
        (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0;
}
}

void Reconstructor::Reset() { *this = Reconstructor {}; }

bool Reconstructor::Prepare(ID3D11DeviceContext* context, ID3D11Texture2D* motion,
    ID3D11Texture2D* depth, UINT display_width, UINT display_height,
    float raster_jitter_x, float raster_jitter_y, bool reverse_z, std::string& error) {
    if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
        !std::isfinite(raster_jitter_x) || !std::isfinite(raster_jitter_y) ||
        std::abs(raster_jitter_x) > 0.5f || std::abs(raster_jitter_y) > 0.5f ||
        !display_width || !display_height || display_width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        display_height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
        error = "Invalid display motion context, dimensions or raster jitter"; return false;
    }
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    D3D11_TEXTURE2D_DESC mv {}, z {}, out {};
    if (!TextureValid(motion, device.Get(), DXGI_FORMAT_R16G16_FLOAT, mv) ||
        !TextureValid(depth, device.Get(), DXGI_FORMAT_R32_FLOAT, z) ||
        mv.Width != z.Width || mv.Height != z.Height ||
        mv.Width > display_width || mv.Height > display_height) {
        error = "Display motion requires matching RG16F motion and R32F depth render grids"; return false;
    }
    if (device_ != device) Reset();
    if (!shader_) {
        ComPtr<ID3DBlob> code, messages;
        if (FAILED(D3DCompile(kShader, sizeof(kShader)-1, "NrDisplayMotion", nullptr, nullptr,
                "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS, 0, &code, &messages)) ||
            FAILED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader_))) {
            error = messages ? static_cast<const char*>(messages->GetBufferPointer()) : "Display motion shader failed";
            Reset(); return false;
        }
        D3D11_BUFFER_DESC buffer {};
        buffer.ByteWidth = 48; buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(device->CreateBuffer(&buffer, nullptr, &constants_))) {
            error = "Display motion constants allocation failed"; Reset(); return false;
        }
        device_ = device;
    }
    if (motion_.Get() != motion || depth_.Get() != depth) {
        motion_srv_.Reset(); depth_srv_.Reset();
        motion_.Reset(); depth_.Reset();
        if (FAILED(device->CreateShaderResourceView(motion, nullptr, &motion_srv_)) ||
            FAILED(device->CreateShaderResourceView(depth, nullptr, &depth_srv_))) {
            error = "Display motion input views failed"; return false;
        }
        motion_ = motion; depth_ = depth;
    }
    if (output_) output_->GetDesc(&out);
    if (out.Width != display_width || out.Height != display_height) {
        output_.Reset(); output_uav_.Reset();
        out = {}; out.Width = display_width; out.Height = display_height;
        out.MipLevels = out.ArraySize = out.SampleDesc.Count = 1;
        out.Format = DXGI_FORMAT_R16G16_FLOAT;
        out.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(device->CreateTexture2D(&out, nullptr, &output_)) ||
            FAILED(device->CreateUnorderedAccessView(output_.Get(), nullptr, &output_uav_))) {
            error = "Display motion output allocation failed"; Reset(); return false;
        }
    }
    const struct { UINT width, height, display_width, display_height;
                   float jitter_x, jitter_y; UINT reverse_z, padding;
                   float input_per_output_x, input_per_output_y, output_per_input_x, output_per_input_y; } parameters {
        mv.Width, mv.Height, display_width, display_height, raster_jitter_x, raster_jitter_y,
        reverse_z ? 1u : 0u, 0u, float(mv.Width) / display_width, float(mv.Height) / display_height,
        float(display_width) / mv.Width, float(display_height) / mv.Height};
    context->UpdateSubresource(constants_.Get(), 0, nullptr, &parameters, 0, 0);
    ID3D11ShaderResourceView* inputs[] = {motion_srv_.Get(), depth_srv_.Get()};
    ID3D11UnorderedAccessView* output = output_uav_.Get();
    ID3D11Buffer* constants = constants_.Get();
    context->CSSetShader(shader_.Get(), nullptr, 0);
    context->CSSetShaderResources(0, 2, inputs);
    context->CSSetConstantBuffers(0, 1, &constants);
    context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    context->Dispatch((display_width+7)/8, (display_height+7)/8, 1);
    inputs[0] = inputs[1] = nullptr; output = nullptr;
    context->CSSetShaderResources(0, 2, inputs);
    context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    return true;
}
}
