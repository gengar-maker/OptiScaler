#pragma once
#define NOMINMAX
#include "XeSSNrInputAdapter.h"
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <DirectXPackedVector.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <vector>
#include <string>

using Microsoft::WRL::ComPtr;
using namespace DirectX::PackedVector;
namespace {
void Check(HRESULT r) { if(FAILED(r)) throw std::runtime_error("HRESULT "+std::to_string(r)); }
void Require(bool v,const char* m) { if(!v) throw std::runtime_error(m); }
void Barrier(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
    if(a==b)return; D3D12_RESOURCE_BARRIER t{};t.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    t.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};l->ResourceBarrier(1,&t);
}
struct Gpu {
    ComPtr<ID3D12Device> d; ComPtr<ID3D12CommandQueue> q; ComPtr<ID3D12CommandAllocator> a;
    ComPtr<ID3D12GraphicsCommandList> l; ComPtr<ID3D12Fence> f; UINT64 value=0;
    Gpu() {
        ComPtr<ID3D12Debug> debug;Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));if(!GetEnvironmentVariableW(L"SNR_TEST_NO_DEBUG",nullptr,0))debug->EnableDebugLayer();
        ComPtr<IDXGIFactory6> factory;Check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)));
        for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> adapter;if(factory->EnumAdapters1(i,&adapter)==DXGI_ERROR_NOT_FOUND)break;
            DXGI_ADAPTER_DESC1 desc{};adapter->GetDesc1(&desc);if(desc.VendorId==0x8086 && !(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) &&
                SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&d))))break;}
        Require(bool(d),"Intel D3D12 GPU unavailable");D3D12_COMMAND_QUEUE_DESC desc{};
        Check(d->CreateCommandQueue(&desc,IID_PPV_ARGS(&q)));
        Check(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&a)));
        Check(d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,a.Get(),nullptr,IID_PPV_ARGS(&l)));
        Check(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f)));
    }
    void Submit(){Check(l->Close());ID3D12CommandList* lists[]={l.Get()};q->ExecuteCommandLists(1,lists);
        Check(q->Signal(f.Get(),++value));HANDLE e=CreateEventW(nullptr,FALSE,FALSE,nullptr);Require(e,"event");
        Check(f->SetEventOnCompletion(value,e));Require(WaitForSingleObject(e,30000)==WAIT_OBJECT_0,"fence timeout");CloseHandle(e);
        Check(a->Reset());Check(l->Reset(a.Get(),nullptr));}
    ComPtr<ID3D12Resource> Buffer(UINT64 n,D3D12_HEAP_TYPE type){D3D12_HEAP_PROPERTIES h{};h.Type=type;
        D3D12_RESOURCE_DESC t{};t.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;t.Width=n;t.Height=t.DepthOrArraySize=t.MipLevels=t.SampleDesc.Count=1;
        t.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;ComPtr<ID3D12Resource> r;
        Check(d->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&t,type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r)));return r;}
    void Upload(ID3D12Resource* r,const void* bytes,size_t row,D3D12_RESOURCE_STATES state){auto desc=r->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 total=0;d->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&total);
        auto b=Buffer(total,D3D12_HEAP_TYPE_UPLOAD);void* p=nullptr;Check(b->Map(0,nullptr,&p));
        for(UINT y=0;y<desc.Height;++y)memcpy(static_cast<char*>(p)+y*fp.Footprint.RowPitch,static_cast<const char*>(bytes)+y*row,row);b->Unmap(0,nullptr);
        Barrier(l.Get(),r,state,D3D12_RESOURCE_STATE_COPY_DEST);D3D12_TEXTURE_COPY_LOCATION src{},dst{};
        src.pResource=b.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;
        dst.pResource=r;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;l->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        Barrier(l.Get(),r,D3D12_RESOURCE_STATE_COPY_DEST,state);Submit();}
    std::vector<char> Read(ID3D12Resource* r,D3D12_RESOURCE_STATES state){auto desc=r->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 total=0,row=0;d->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,&row,&total);
        auto b=Buffer(total,D3D12_HEAP_TYPE_READBACK);Barrier(l.Get(),r,state,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=r;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource=b.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;
        l->CopyTextureRegion(&dst,0,0,0,&src,nullptr);Barrier(l.Get(),r,D3D12_RESOURCE_STATE_COPY_SOURCE,state);Submit();
        void* p=nullptr;Check(b->Map(0,nullptr,&p));std::vector<char> result(row*desc.Height);
        for(UINT y=0;y<desc.Height;++y)memcpy(result.data()+y*row,static_cast<char*>(p)+y*fp.Footprint.RowPitch,row);b->Unmap(0,nullptr);return result;}
};
}
