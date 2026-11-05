#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <DirectXPackedVector.h>
#include <xess_d3d12.h>
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
int wmain(int argc, wchar_t** argv) {
try {
    if (argc != 4) throw std::runtime_error("usage: smoke SDK_DLL OUTPUT_FILE MODE[scalar|texture|auto|ldr]");
    const std::wstring mode=argv[3];
    auto sdk=LoadLibraryW(argv[1]); if (!sdk) throw std::runtime_error("LoadLibrary XeSS");
#define API(n) auto n##Fn=Proc<decltype(&n)>(sdk,#n)
    API(xessD3D12CreateContext); API(xessD3D12Init); API(xessD3D12Execute); API(xessDestroyContext);
    API(xessGetOptimalInputResolution); API(xessGetInputResolution); API(xessGetProperties);
    API(xessSetExposureMultiplier); API(xessGetExposureMultiplier);
    API(xessSetVelocityScale); API(xessGetVelocityScale); API(xessSetJitterScale); API(xessGetJitterScale);
    API(xessD3D12GetInitParams); API(xessForceLegacyScaleFactors);
    ComPtr<IDXGIFactory6> factory; Check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i=0;SUCCEEDED(factory->EnumAdapters1(i,&adapter));++i) {
        DXGI_ADAPTER_DESC1 desc{}; adapter->GetDesc1(&desc);
        if (desc.VendorId==0x8086 && wcsstr(desc.Description,L"Arc")) break;
        adapter.Reset();
    }
    if (!adapter) throw std::runtime_error("No Arc adapter");
    ComPtr<ID3D12Device> device; Check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)));
    ComPtr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC q{}; Check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator; Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list; Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));
    ComPtr<ID3D12Fence> fence; Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr); if (!event) throw std::runtime_error("CreateEvent");
    UINT64 value=0;
    auto submit=[&] {
        Check(list->Close()); ID3D12CommandList* lists[]={list.Get()}; queue->ExecuteCommandLists(1,lists);
        Check(queue->Signal(fence.Get(),++value)); Check(fence->SetEventOnCompletion(value,event));
        if (WaitForSingleObject(event,30000)!=WAIT_OBJECT_0) throw std::runtime_error("GPU fence timed out");
        Check(allocator->Reset()); Check(list->Reset(allocator.Get(),nullptr));
    };
    xess_context_handle_t context=nullptr; XCheck(xessD3D12CreateContextFn(device.Get(),&context));
    const xess_2d_t output{640,360};
    for (bool legacy : {false,true,false}) {
        XCheck(xessForceLegacyScaleFactorsFn(context,legacy));
        for (int quality=100;quality<=106;++quality) {
            xess_2d_t optimal{},minimum{},maximum{};
            const auto result=xessGetOptimalInputResolutionFn(context,&output,(xess_quality_settings_t)quality,&optimal,&minimum,&maximum);
            std::cout << "query " << legacy << ' ' << quality << ' ' << int(result) << ' ' << optimal.x << ' ' << optimal.y << ' ' << minimum.x << ' ' << minimum.y << ' ' << maximum.x << ' ' << maximum.y << '\n';
        }
    }
    xess_properties_t properties{}; XCheck(xessGetPropertiesFn(context,&output,&properties));
    std::cout << "properties " << properties.requiredDescriptorCount << ' ' << properties.tempBufferHeapSize << ' ' << properties.tempTextureHeapSize << '\n';
    XCheck(xessSetExposureMultiplierFn(context,1.25f)); float multiplier=0; XCheck(xessGetExposureMultiplierFn(context,&multiplier));
    XCheck(xessSetVelocityScaleFn(context,2.0f,-2.0f)); float vx=0,vy=0; XCheck(xessGetVelocityScaleFn(context,&vx,&vy));
    XCheck(xessSetJitterScaleFn(context,1.0f,-1.0f)); float jx=0,jy=0; XCheck(xessGetJitterScaleFn(context,&jx,&jy));
    if(multiplier!=1.25f||vx!=2||vy!=-2||jx!=1||jy!=-1) throw std::runtime_error("SDK context properties lost");
    xess_d3d12_init_params_t init{}; init.outputResolution=output; init.qualitySetting=XESS_QUALITY_SETTING_QUALITY;
    init.initFlags=XESS_INIT_FLAG_INVERTED_DEPTH;
    if(mode==L"texture") init.initFlags|=XESS_INIT_FLAG_EXPOSURE_SCALE_TEXTURE;
    if(mode==L"auto") init.initFlags|=XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE;
    if(mode==L"ldr") init.initFlags|=XESS_INIT_FLAG_LDR_INPUT_COLOR;
    XCheck(xessD3D12InitFn(context,&init));
    xess_d3d12_init_params_t returned{}; XCheck(xessD3D12GetInitParamsFn(context,&returned));
    // The SDK may normalize flags (including LDR). Compare its returned
    // contract with the intercepted run instead of assuming an exact echo.
    std::cout << "returnedInit " << returned.initFlags << ' ' << int(returned.qualitySetting) << ' ' << returned.outputResolution.x << ' ' << returned.outputResolution.y << '\n';
    xess_2d_t input{},minimum{},maximum{}; XCheck(xessGetOptimalInputResolutionFn(context,&output,init.qualitySetting,&input,&minimum,&maximum));
    std::vector<ComPtr<ID3D12Resource>> uploads;
    auto texture=[&](UINT w,UINT h,DXGI_FORMAT format,bool out,UINT kind) {
        D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width=w; d.Height=h;
        d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1; d.Format=format;
        d.Flags=out?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE;
        auto heap=Heap(D3D12_HEAP_TYPE_DEFAULT); ComPtr<ID3D12Resource> r;
        Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,out?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r)));
        if(out) return r;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 total=0; device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&total);
        heap=Heap(D3D12_HEAP_TYPE_UPLOAD); auto bd=Buffer(total); ComPtr<ID3D12Resource> upload;
        Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)));
        char* mapped=nullptr; Check(upload->Map(0,nullptr,reinterpret_cast<void**>(&mapped))); std::memset(mapped,0,total);
        for(UINT y=0;y<h;++y) for(UINT x=0;x<w;++x) {
            if(kind==0) {
                auto* p=reinterpret_cast<uint16_t*>(mapped+y*fp.Footprint.RowPitch)+x*4;
                p[0]=XMConvertFloatToHalf(.05f+float(x%31)/32); p[1]=XMConvertFloatToHalf(.1f+float(y%47)/32);
                p[2]=XMConvertFloatToHalf(.2f+float((x+y)%53)/32); p[3]=XMConvertFloatToHalf(1);
            } else if(kind==2||kind==3) {
                reinterpret_cast<float*>(mapped+y*fp.Footprint.RowPitch)[x]=kind==2?.5f:.75f;
            }
        }
        upload->Unmap(0,nullptr);
        D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource=upload.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint=fp;
        D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource=r.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr); Barrier(list.Get(),r.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        uploads.push_back(upload); return r;
    };
    // Nonzero colour/MV/depth bases exercise active subrects.
    auto color=texture(input.x+8,input.y+8,DXGI_FORMAT_R16G16B16A16_FLOAT,false,0);
    auto motion=texture(input.x+8,input.y+8,DXGI_FORMAT_R16G16_FLOAT,false,1);
    auto depth=texture(input.x+8,input.y+8,DXGI_FORMAT_R32_FLOAT,false,2);
    auto exposure=texture(1,1,DXGI_FORMAT_R32_FLOAT,false,3);
    auto result=texture(output.x+8,output.y+8,DXGI_FORMAT_R16G16B16A16_FLOAT,true,0);
    submit(); uploads.clear();
    for(UINT frame=0;frame<12;++frame) {
        xess_d3d12_execute_params_t p{}; p.pColorTexture=color.Get(); p.pVelocityTexture=motion.Get(); p.pDepthTexture=depth.Get(); p.pOutputTexture=result.Get();
        if(mode==L"texture") p.pExposureScaleTexture=exposure.Get();
        p.inputWidth=input.x; p.inputHeight=input.y; p.exposureScale=.75f; p.resetHistory=frame==0||frame==6;
        p.inputColorBase=p.inputMotionVectorBase=p.inputDepthBase={3,4}; p.outputColorBase={2,3};
        p.jitterOffsetX=(float(frame%3)-1)/3; p.jitterOffsetY=(float(frame%4)-1.5f)/4;
        XCheck(xessD3D12ExecuteFn(context,list.Get(),&p)); submit();
    }
    auto desc=result->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 total=0;
    device->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&total);
    auto heap=Heap(D3D12_HEAP_TYPE_READBACK); auto bd=Buffer(total); ComPtr<ID3D12Resource> readback;
    Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)));
    Barrier(list.Get(),result.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{},dst{}; src.pResource=result.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource=readback.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint=fp;
    list->CopyTextureRegion(&dst,0,0,0,&src,nullptr); submit();
    char* data=nullptr; Check(readback->Map(0,nullptr,reinterpret_cast<void**>(&data)));
    std::ofstream file(std::filesystem::path(argv[2]),std::ios::binary);
    for(UINT y=0;y<output.y;++y) file.write(data+(y+3)*fp.Footprint.RowPitch+2*8,output.x*8);
    if(!file) throw std::runtime_error("Output write failed"); readback->Unmap(0,nullptr);
    XCheck(xessDestroyContextFn(context)); CloseHandle(event);
    std::cout << "PASS native XeSS contract + 12 GPU frames " << input.x << 'x' << input.y << " -> " << output.x << 'x' << output.y << '\n';
    return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
