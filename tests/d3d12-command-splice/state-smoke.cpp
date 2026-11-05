#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <regex>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <DirectXPackedVector.h>
#include <xess_d3d12.h>
#ifdef SPLICE_TEST
#include "D3D12CommandSplice.h"
#include "Dx12InputSnapshot.h"
#endif
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
using DirectX::PackedVector::XMConvertFloatToHalf;
static void Check(HRESULT r) { if (FAILED(r)) throw std::runtime_error("D3D failure " + std::to_string(r)); }
static void XCheck(xess_result_t r) { if (r != XESS_RESULT_SUCCESS) throw std::runtime_error("XeSS failure " + std::to_string(r)); }
template<class T> T Proc(HMODULE m, const char* name) {
    auto p = reinterpret_cast<T>(GetProcAddress(m, name));
    if (!p) throw std::runtime_error(name);
    return p;
}
static D3D12_HEAP_PROPERTIES Heap(D3D12_HEAP_TYPE type) { D3D12_HEAP_PROPERTIES p{}; p.Type=type; return p; }
static D3D12_RESOURCE_DESC Buffer(UINT64 bytes) {
    D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; d.Width=bytes;
    d.Height=d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1; d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR; return d;
}
static void Barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER t{}; t.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    t.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b}; l->ResourceBarrier(1,&t);
}

#include <d3dcompiler.h>
int main() {
try {
    ComPtr<ID3D12Debug> validation; Check(D3D12GetDebugInterface(IID_PPV_ARGS(&validation)));
    validation->EnableDebugLayer();
    ComPtr<IDXGIFactory6> factory; Check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    for(UINT i=0;SUCCEEDED(factory->EnumAdapters1(i,&adapter));++i) {
        DXGI_ADAPTER_DESC1 desc{}; adapter->GetDesc1(&desc);
        if(desc.VendorId==0x8086 && wcsstr(desc.Description,L"Arc")) break;
        adapter.Reset();
    }
    if(!adapter) throw std::runtime_error("No Arc");
    ComPtr<ID3D12Device> device; Check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)));
    std::string error;
    if(!D3D12CommandSplice::Install(device.Get(),[](const auto& text){std::cerr<<text<<'\n';},error)) throw std::runtime_error(error);
    D3D12_COMMAND_QUEUE_DESC q{}; ComPtr<ID3D12CommandQueue> queue; Check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator; Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list; Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));
    Check(list->Close()); Check(list->Reset(allocator.Get(),nullptr));
    auto heap=Heap(D3D12_HEAP_TYPE_DEFAULT); auto desc=Buffer(16); desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> output; Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&output)));
    output->SetName(L"SpliceStateOutput");
    heap=Heap(D3D12_HEAP_TYPE_READBACK); desc.Flags=D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> readback; Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)));
    D3D12_ROOT_PARAMETER params[2]{}; params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[0].Descriptor.ShaderRegister=0; params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants={0,0,4};
    D3D12_ROOT_SIGNATURE_DESC signature{}; signature.NumParameters=2; signature.pParameters=params;
    ComPtr<ID3DBlob> serialized,errors;
    Check(D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors));
    ComPtr<ID3D12RootSignature> root; Check(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root)));
    const char* shader="RWStructuredBuffer<uint> data : register(u0); cbuffer C : register(b0) { uint4 a; }; [numthreads(1,1,1)] void main() { data[0]=a.x+a.y+a.z+a.w; }";
    ComPtr<ID3DBlob> code; Check(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"main","cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors));
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{}; psoDesc.pRootSignature=root.Get(); psoDesc.CS={code->GetBufferPointer(),code->GetBufferSize()};
    ComPtr<ID3D12PipelineState> pso; Check(device->CreateComputePipelineState(&psoDesc,IID_PPV_ARGS(&pso)));
    list->SetComputeRootSignature(root.Get()); list->SetPipelineState(pso.Get());
    list->SetComputeRootUnorderedAccessView(0,output->GetGPUVirtualAddress());
    UINT constants[]={1,2,3,4}; list->SetComputeRoot32BitConstants(1,4,constants,0);
    list->Dispatch(1,1,1);
    D3D12_RESOURCE_BARRIER uav{}; uav.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV; uav.UAV.pResource=output.Get(); list->ResourceBarrier(1,&uav);
    constants[1]=7; constants[2]=11;
    list->SetComputeRoot32BitConstants(1,2,constants+1,1);
    list->SetComputeRoot32BitConstant(1,13,3);
    for(auto& c:constants) c=999; // cached pointer bugs must change the result
    unsigned callbacks=0;
    if(!D3D12CommandSplice::Begin(list.Get(),[&](auto*,auto*){++callbacks;},error)) throw std::runtime_error(error);
    // No pipeline/root/constant rebinding after the split.
    list->Dispatch(1,1,1);
    list->ResourceBarrier(1,&uav);
    Barrier(list.Get(),output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(),0,output.Get(),0,16);
    Check(list->Close()); ID3D12CommandList* lists[]={list.Get()}; queue->ExecuteCommandLists(1,lists);
    if(callbacks!=1) throw std::runtime_error("boundary callback missing");
    UINT* data=nullptr; Check(readback->Map(0,nullptr,reinterpret_cast<void**>(&data)));
    const UINT actual=data[0]; readback->Unmap(0,nullptr);
    if(actual!=32) throw std::runtime_error("inherited root constants/PSO/UAV lost: "+std::to_string(actual));
    ComPtr<ID3D12InfoQueue> messages; Check(device.As(&messages));
    for(UINT64 i=0;i<messages->GetNumStoredMessages();++i) {
        SIZE_T size=0; messages->GetMessage(i,nullptr,&size); std::vector<char> storage(size);
        auto* msg=reinterpret_cast<D3D12_MESSAGE*>(storage.data()); Check(messages->GetMessage(i,msg,&size));
        if(msg->Severity<=D3D12_MESSAGE_SEVERITY_WARNING) { std::cerr<<msg->pDescription<<'\n'; throw std::runtime_error("D3D12 validation error/warning"); }
    }
    std::cout<<"PASS: inherited root signature, PSO, UAV, deep-copied partial constants; buffer decay; producer/consumer ordering; D3D12 debug clean\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
