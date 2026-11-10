#define NOMINMAX
#include "XeSSNrInputAdapter.h"
// D3D12 accepts this SM5 bytecode too. Share the exact validated HDR adapter;
// the legacy D3D12 header is an independently generated, older snapshot.
#include "../../shaders/dlssnr/precompile/DlssNr_Shader_Dx11.h"
#include <d3dcompiler.h>
#include <cstring>
#include <stdexcept>
#include <string>

namespace XeSSNr
{
namespace
{
using Microsoft::WRL::ComPtr;
void Check(HRESULT hr, const char* what)
{
    if (FAILED(hr))
        throw std::runtime_error(std::string(what) + ": " + std::to_string(hr));
}
void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES before,
             D3D12_RESOURCE_STATES after)
{
    if (before == after)
        return;
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
    list->ResourceBarrier(1, &b);
}
ComPtr<ID3D12Resource> Texture(ID3D12Device* device, unsigned width, unsigned height, DXGI_FORMAT format,
                               bool simultaneous = false)
{
    D3D12_RESOURCE_DESC d {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = d.MipLevels = d.SampleDesc.Count = 1;
    d.Format = format;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (simultaneous)
        d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    D3D12_HEAP_PROPERTIES h {};
    h.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> r;
    Check(device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &d,
                                          simultaneous ? D3D12_RESOURCE_STATE_COMMON
                                                       : D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                          nullptr, IID_PPV_ARGS(&r)),
          "native NR texture");
    return r;
}
ComPtr<ID3D12RootSignature> Root(ID3D12Device* device, bool hdr)
{
    D3D12_DESCRIPTOR_RANGE ranges[2] {};
    ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, hdr ? 5u : 1u, 0, 0, 0 };
    ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, hdr ? 2u : 1u, 0, 0, hdr ? 5u : 1u };
    D3D12_ROOT_PARAMETER p[2] {};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[0].DescriptorTable = { 2, ranges };
    p[1].ParameterType = hdr ? D3D12_ROOT_PARAMETER_TYPE_CBV : D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    if (hdr)
        p[1].Descriptor = { 0, 0 };
    else
        p[1].Constants = { 0, 0, 8 };
    D3D12_STATIC_SAMPLER_DESC s {};
    s.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    s.MaxLOD = D3D12_FLOAT32_MAX;
    s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    D3D12_ROOT_SIGNATURE_DESC d {};
    d.NumParameters = 2;
    d.pParameters = p;
    d.NumStaticSamplers = hdr ? 1 : 0;
    d.pStaticSamplers = hdr ? &s : nullptr;
    ComPtr<ID3DBlob> blob, error;
    Check(D3D12SerializeRootSignature(&d, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error), "native NR root serialize");
    ComPtr<ID3D12RootSignature> r;
    Check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&r)),
          "native NR root");
    return r;
}
void Srv(ID3D12Device* d, ID3D12Resource* r, D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC v {};
    v.Format = r ? r->GetDesc().Format : DXGI_FORMAT_R16G16B16A16_FLOAT;
    v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    v.Texture2D.MipLevels = 1;
    v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d->CreateShaderResourceView(r, &v, h);
}
void Uav(ID3D12Device* d, ID3D12Resource* r, D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC v {};
    v.Format = r ? r->GetDesc().Format : DXGI_FORMAT_R16G16B16A16_FLOAT;
    v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    d->CreateUnorderedAccessView(r, nullptr, &v, h);
}
} // namespace

ReadableCopy::ReadableCopy(ID3D12Device* device) : device_(device), root_(Root(device, false))
{
    constexpr char code[] = R"(
        Texture2D<float4> Source : register(t0);
        RWTexture2D<float4> Target : register(u0);
        cbuffer Params : register(b0) { uint2 size; uint2 base; }
        [numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) {
            if (all(id.xy < size)) Target[id.xy] = Source.Load(int3(id.xy + base, 0));
        })";
    ComPtr<ID3DBlob> shader, errors;
    Check(D3DCompile(code, sizeof(code) - 1, "XeSSReadOnlySnapshot", nullptr, nullptr, "main", "cs_5_0",
                     D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors),
          "native NR snapshot shader");
    D3D12_COMPUTE_PIPELINE_STATE_DESC p {};
    p.pRootSignature = root_.Get();
    p.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
    Check(device->CreateComputePipelineState(&p, IID_PPV_ARGS(&pipeline_)), "native NR snapshot pipeline");
    D3D12_DESCRIPTOR_HEAP_DESC h { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
                                   0 };
    Check(device->CreateDescriptorHeap(&h, IID_PPV_ARGS(&heap_)), "native NR snapshot descriptors");
}
void ReadableCopy::Record(ID3D12GraphicsCommandList* list, unsigned slot, ID3D12Resource* source, unsigned x,
                          unsigned y, unsigned width, unsigned height, ID3D12Resource* target)
{
    if (slot >= 2 || !source || !target || !width || !height)
        throw std::invalid_argument("invalid native NR read-only snapshot");
    const auto stride = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = heap_->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += SIZE_T(slot) * 2 * stride;
    Srv(device_.Get(), source, cpu);
    cpu.ptr += stride;
    Uav(device_.Get(), target, cpu);
    auto gpu = heap_->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += UINT64(slot) * 2 * stride;
    // Source is already a legal XeSS SRV, or implicitly promotable COMMON.
    // Changing its state would lose the game's exact combined read-state bits.
    Barrier(list, target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap* heap = heap_.Get();
    list->SetDescriptorHeaps(1, &heap);
    list->SetComputeRootSignature(root_.Get());
    list->SetPipelineState(pipeline_.Get());
    list->SetComputeRootDescriptorTable(0, gpu);
    const unsigned constants[] { width, height, x, y };
    list->SetComputeRoot32BitConstants(1, 4, constants, 0);
    list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    Barrier(list, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
}

InputAdapter::InputAdapter(ID3D12Device* device, unsigned width, unsigned height)
    : device_(device), width_(width), height_(height)
{
    if (!device || !width || !height || width > 8192 || height > 8192)
        throw std::invalid_argument("invalid native NR adapter extent");
    proxy_ = Texture(device, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    original_ = Texture(device, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    neural_ = Texture(device, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    motion_ = Texture(device, width, height, DXGI_FORMAT_R32G32_FLOAT);
    hdrRoot_ = Root(device, true);
    motionRoot_ = Root(device, false);
    D3D12_COMPUTE_PIPELINE_STATE_DESC p {};
    p.pRootSignature = hdrRoot_.Get();
    p.CS = { DlssNr_Dx11_cso, sizeof(DlssNr_Dx11_cso) };
    Check(device->CreateComputePipelineState(&p, IID_PPV_ARGS(&hdrPso_)), "native NR HDR pipeline");
    constexpr char motionShader[] = R"(
        Texture2D<float2> Source : register(t0);
        RWTexture2D<float2> Target : register(u0);
        cbuffer Params : register(b0) { uint2 size; uint2 base; float2 scale; float2 bias; }
        [numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID) {
            if (any(id.xy>=size)) return;
            float2 v=Source.Load(int3(id.xy+base,0))*scale+bias;
            Target[id.xy]=all(isfinite(v))?v:float2(size)*4;
        })";
    ComPtr<ID3DBlob> shader, error;
    Check(D3DCompile(motionShader, sizeof(motionShader) - 1, "XeSSNrMotion", nullptr, nullptr, "main", "cs_5_0",
                     D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &error),
          "native NR motion compile");
    p.pRootSignature = motionRoot_.Get();
    p.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
    Check(device->CreateComputePipelineState(&p, IID_PPV_ARGS(&motionPso_)), "native NR motion pipeline");
    for (auto& h : heaps_)
    {
        D3D12_DESCRIPTOR_HEAP_DESC d { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 7,
                                       D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
        Check(device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&h)), "native NR descriptors");
    }
    D3D12_HEAP_PROPERTIES h {};
    h.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC d {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = 2 * sizeof(DlssNrConstants);
    d.Height = d.DepthOrArraySize = d.MipLevels = d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Check(device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                          IID_PPV_ARGS(&constants_)),
          "native NR constants");
}

ComPtr<ID3D12Resource> InputAdapter::MakeOutput(ID3D12Device* d, unsigned w, unsigned h)
{
    return Texture(d, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, true);
}
ComPtr<ID3D12Resource> InputAdapter::MakeExposure(ID3D12Device* device)
{
    return Texture(device, 1, 1, DXGI_FORMAT_R32_FLOAT, true);
}
void InputAdapter::CopyOriginal(ID3D12GraphicsCommandList* list, ID3D12Resource* colour, unsigned x, unsigned y,
                                unsigned w, unsigned h, ID3D12Resource* output)
{
    Barrier(list, colour, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(list, output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION src {}, dst {};
    src.pResource = colour;
    dst.pResource = output;
    src.Type = dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box { x, y, 0, x + w, y + h, 1 };
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    Barrier(list, colour, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(list, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
}
void InputAdapter::HdrPass(ID3D12GraphicsCommandList* list, unsigned slot, ID3D12Resource* source,
                           ID3D12Resource* model, ID3D12Resource* original, ID3D12Resource* exposure,
                           ID3D12Resource* target, ID3D12Resource* keep, const DlssNrConstants& c)
{
    const auto step = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = heaps_[slot]->GetCPUDescriptorHandleForHeapStart();
    for (auto* r : { source, model, original, static_cast<ID3D12Resource*>(nullptr), exposure })
    {
        Srv(device_.Get(), r, cpu);
        cpu.ptr += step;
    }
    Uav(device_.Get(), target, cpu);
    cpu.ptr += step;
    Uav(device_.Get(), keep, cpu);
    void* data = nullptr;
    const D3D12_RANGE noRead { 0, 0 };
    Check(constants_->Map(0, &noRead, &data), "native NR constants Map");
    memcpy(static_cast<char*>(data) + slot * sizeof(c), &c, sizeof(c));
    constants_->Unmap(0, nullptr);
    ID3D12DescriptorHeap* heap = heaps_[slot].Get();
    list->SetDescriptorHeaps(1, &heap);
    list->SetComputeRootSignature(hdrRoot_.Get());
    list->SetPipelineState(hdrPso_.Get());
    list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRootConstantBufferView(1, constants_->GetGPUVirtualAddress() + slot * sizeof(c));
    list->Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);
}
void InputAdapter::Encode(ID3D12GraphicsCommandList* list, ID3D12Resource* output, ID3D12Resource* motion, unsigned x,
                          unsigned y, const MotionTransform& t, const DlssNrConstants& constants,
                          ID3D12Resource* exposure)
{
    auto c = constants;
    c.Mode = DlssNrMode_Encode;
    c.Width = width_;
    c.Height = height_;
    c.UseGameExposure = exposure ? 1 : 0;
    settings_ = c;
    Barrier(list, output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (exposure)
        Barrier(list, exposure, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    HdrPass(list, 0, output, nullptr, nullptr, exposure, proxy_.Get(), original_.Get(), c);
    if (exposure)
        Barrier(list, exposure, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(list, output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(list, proxy_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(list, original_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const bool motionDecays = (motion->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) != 0;
    if (motionDecays)
        Barrier(list, motion, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto cpu = heaps_[2]->GetCPUDescriptorHandleForHeapStart();
    Srv(device_.Get(), motion, cpu);
    cpu.ptr += device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    Uav(device_.Get(), motion_.Get(), cpu);
    ID3D12DescriptorHeap* heap = heaps_[2].Get();
    list->SetDescriptorHeaps(1, &heap);
    list->SetComputeRootSignature(motionRoot_.Get());
    list->SetPipelineState(motionPso_.Get());
    list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    struct
    {
        unsigned w, h, x, y;
        Vec2 scale, bias;
    } motionConstants { width_, height_, x, y, t.scale, t.bias };
    static_assert(sizeof(motionConstants) == 32);
    list->SetComputeRoot32BitConstants(1, 8, &motionConstants, 0);
    list->Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);
    if (motionDecays)
        Barrier(list, motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(list, motion_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}
void InputAdapter::Resolve(ID3D12GraphicsCommandList* list, ID3D12Resource* output, bool applyModel,
                           ID3D12Resource* exposure)
{
    auto c = settings_;
    c.Mode = DlssNrMode_Resolve;
    c.ApplyModel = applyModel ? 1 : 0;
    Barrier(list, output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(list, neural_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (exposure)
        Barrier(list, exposure, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    HdrPass(list, 1, proxy_.Get(), neural_.Get(), original_.Get(), exposure, output, nullptr, c);
    if (exposure)
        Barrier(list, exposure, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    Barrier(list, neural_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(list, proxy_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(list, original_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(list, motion_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}
} // namespace XeSSNr
