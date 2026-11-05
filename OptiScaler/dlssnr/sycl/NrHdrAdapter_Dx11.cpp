#include "NrHdrAdapter_Dx11.h"
#include "../../shaders/dlssnr/precompile/DlssNr_Shader_Dx11.h"
#include <cmath>
#include <d3dcompiler.h>

namespace NrHdrAdapterDx11 {
using Microsoft::WRL::ComPtr;

bool Exposure::Copy(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source,
                    std::string& error) {
    if (!context || !source || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
    ComPtr<ID3D11Resource> resource;
    source->GetResource(&resource);
    ComPtr<ID3D11Texture2D> input;
    if (FAILED(resource.As(&input))) return false;
    D3D11_TEXTURE2D_DESC desc {}; input->GetDesc(&desc);
    D3D11_SHADER_RESOURCE_VIEW_DESC view {}; source->GetDesc(&view);
    if (desc.Width != 1 || desc.Height != 1 || desc.SampleDesc.Count != 1 ||
        desc.ArraySize != 1 || desc.MipLevels != 1 || view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
        (view.Format != DXGI_FORMAT_R32_FLOAT && view.Format != DXGI_FORMAT_R32G32_FLOAT &&
         view.Format != DXGI_FORMAT_R16G16B16A16_FLOAT)) return false;
    ComPtr<ID3D11Device> device; context->GetDevice(&device);
    if (device_ != device || !shader_) {
        Reset();
        desc = {}; desc.Width = desc.Height = desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        constexpr char code[] = R"(
            Texture2D<float4> Source : register(t0);
            RWTexture2D<float> Target : register(u0);
            [numthreads(1,1,1)] void main() { Target[uint2(0,0)] = Source.Load(int3(0,0,0)).r; }
        )";
        ComPtr<ID3DBlob> blob;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture_)) ||
            FAILED(device->CreateShaderResourceView(texture_.Get(), nullptr, &srv_)) ||
            FAILED(device->CreateUnorderedAccessView(texture_.Get(), nullptr, &uav_)) ||
            FAILED(D3DCompile(code, sizeof(code)-1, "NrHdrExposure", nullptr, nullptr, "main", "cs_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, nullptr)) ||
            FAILED(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &shader_))) {
            error = "HDR exposure conversion initialization failed";
            Reset(); return false;
        }
        device_ = device;
    }
    ID3D11UnorderedAccessView* output = uav_.Get();
    context->CSSetShader(shader_.Get(), nullptr, 0);
    context->CSSetShaderResources(0, 1, &source);
    context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    context->Dispatch(1, 1, 1);
    source = nullptr; output = nullptr;
    context->CSSetShaderResources(0, 1, &source);
    context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    return true;
}

void Adapter::Reset() {
    *this = Adapter {};
}

bool Adapter::Initialize(ID3D11Device* device, UINT width, UINT height,
                         std::string& error) {
    if (device_.Get() == device && width_ == width && height_ == height && shader_)
        return true;
    Reset();
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = width; desc.Height = height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &original_)) ||
        FAILED(device->CreateTexture2D(&desc, nullptr, &proxy_)) ||
        FAILED(device->CreateTexture2D(&desc, nullptr, &output_)) ||
        FAILED(device->CreateShaderResourceView(original_.Get(), nullptr, &original_srv_)) ||
        FAILED(device->CreateShaderResourceView(proxy_.Get(), nullptr, &proxy_srv_)) ||
        FAILED(device->CreateUnorderedAccessView(original_.Get(), nullptr, &original_uav_)) ||
        FAILED(device->CreateUnorderedAccessView(proxy_.Get(), nullptr, &proxy_uav_)) ||
        FAILED(device->CreateUnorderedAccessView(output_.Get(), nullptr, &output_uav_))) {
        error = "NR HDR adapter texture allocation failed";
        Reset(); return false;
    }
    D3D11_BUFFER_DESC buffer {};
    buffer.ByteWidth = sizeof(DlssNrConstants);
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SAMPLER_DESC sampler {};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
    if (FAILED(device->CreateBuffer(&buffer, nullptr, &constants_)) ||
        FAILED(device->CreateSamplerState(&sampler, &sampler_)) ||
        FAILED(device->CreateComputeShader(DlssNr_Dx11_cso, sizeof(DlssNr_Dx11_cso), nullptr, &shader_))) {
        error = "NR HDR adapter shader initialization failed";
        Reset(); return false;
    }
    device_ = device;
    width_ = width; height_ = height;
    return true;
}

void Adapter::Dispatch(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source,
                       ID3D11ShaderResourceView* model) {
    context->UpdateSubresource(constants_.Get(), 0, nullptr, &settings_, 0, 0);
    ID3D11Buffer* buffer = constants_.Get();
    ID3D11SamplerState* sampler = sampler_.Get();
    ID3D11ShaderResourceView* inputs[] = {
        source, model, settings_.Mode == DlssNrMode_Resolve ? original_srv_.Get() : nullptr,
        nullptr, exposure_.Get()};
    ID3D11UnorderedAccessView* outputs[] = {
        settings_.Mode == DlssNrMode_Encode ? proxy_uav_.Get() : output_uav_.Get(),
        settings_.Mode == DlssNrMode_Encode ? original_uav_.Get() : nullptr};
    context->CSSetShader(shader_.Get(), nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &buffer);
    context->CSSetSamplers(0, 1, &sampler);
    context->CSSetShaderResources(0, 5, inputs);
    context->CSSetUnorderedAccessViews(0, 2, outputs, nullptr);
    context->Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);
    ID3D11ShaderResourceView* null_srvs[5] {};
    ID3D11UnorderedAccessView* null_uavs[2] {};
    context->CSSetShaderResources(0, 5, null_srvs);
    context->CSSetUnorderedAccessViews(0, 2, null_uavs, nullptr);
}

bool Adapter::Encode(ID3D11DeviceContext* context, ID3D11Texture2D* hdr,
                      const DlssNrConstants& settings, ID3D11ShaderResourceView* exposure,
                      std::string& error) {
    encoded_ = false;
    if (!context || !hdr || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
        return false;
    D3D11_TEXTURE2D_DESC desc {};
    hdr->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || desc.SampleDesc.Count != 1 ||
        desc.MipLevels != 1 || desc.ArraySize != 1 || !std::isfinite(settings.WhitePoint) ||
        settings.WhitePoint <= 0 || !std::isfinite(settings.TransferStrength) ||
        settings.TransferStrength < 0 || !std::isfinite(settings.ColourStrength) ||
        settings.ColourStrength < 0 || !std::isfinite(settings.MaxRatio) || settings.MaxRatio < 1 ||
        !std::isfinite(settings.ExposurePreMul) || settings.ExposurePreMul <= 0) {
        error = "NR HDR adapter input or color settings invalid";
        return false;
    }
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!Initialize(device.Get(), desc.Width, desc.Height, error)) return false;
    ComPtr<ID3D11ShaderResourceView> input;
    if (FAILED(device->CreateShaderResourceView(hdr, nullptr, &input))) {
        error = "NR HDR adapter input SRV failed"; return false;
    }
    settings_ = settings;
    settings_.Mode = DlssNrMode_Encode;
    settings_.Width = width_; settings_.Height = height_;
    settings_.UseGameExposure = exposure ? 1 : 0;
    exposure_ = exposure;
    Dispatch(context, input.Get(), nullptr);
    encoded_ = true;
    return true;
}

bool Adapter::Resolve(ID3D11DeviceContext* context, ID3D11Texture2D* neural,
                       bool apply_model, std::string& error) {
    if (!encoded_ || !context || (apply_model && !neural)) return false;
    ComPtr<ID3D11ShaderResourceView> model;
    if (apply_model) {
        D3D11_TEXTURE2D_DESC desc {};
        neural->GetDesc(&desc);
        if (desc.Width != width_ || desc.Height != height_ || desc.SampleDesc.Count != 1 ||
            desc.MipLevels != 1 || desc.ArraySize != 1 || desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
            error = "NR HDR adapter neural result has unexpected dimensions/format";
            return false;
        }
        if (FAILED(device_->CreateShaderResourceView(neural, nullptr, &model))) {
            error = "NR HDR adapter neural SRV failed"; return false;
        }
    }
    settings_.Mode = DlssNrMode_Resolve;
    settings_.ApplyModel = apply_model ? 1 : 0;
    Dispatch(context, proxy_srv_.Get(), apply_model ? model.Get() : proxy_srv_.Get());
    return true;
}
}
