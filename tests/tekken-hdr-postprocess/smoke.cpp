#define NOMINMAX
#include "../../OptiScaler/dlssnr/sycl/TekkenHdrPostProcess_Dx11.h"
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <d3d11sdklayers.h>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace TekkenHdrPostProcessDx11;
static void Require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
static void Check(HRESULT hr) { Require(SUCCEEDED(hr), "D3D call failed"); }
static ComPtr<ID3DBlob> Compile(const char* text, const char* profile) {
    ComPtr<ID3DBlob> code, errors;
    auto hr = D3DCompile(text, strlen(text), "test", nullptr, nullptr, "main", profile,
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr) && errors) std::cerr << static_cast<const char*>(errors->GetBufferPointer());
    Check(hr); return code;
}
struct Image {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;
};
static Image MakeImage(ID3D11Device* device, UINT w, UINT h, DXGI_FORMAT format) {
    D3D11_TEXTURE2D_DESC d {};
    d.Width=w; d.Height=h; d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
    d.Format=format; d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
    Image image;
    Check(device->CreateTexture2D(&d,nullptr,&image.texture));
    Check(device->CreateShaderResourceView(image.texture.Get(),nullptr,&image.srv));
    Check(device->CreateRenderTargetView(image.texture.Get(),nullptr,&image.rtv));
    return image;
}
static std::vector<float> ReadBuffer(ID3D11DeviceContext* context, ID3D11Buffer* source) {
    ComPtr<ID3D11Device> device; context->GetDevice(&device);
    D3D11_BUFFER_DESC d {}; source->GetDesc(&d);
    d.Usage=D3D11_USAGE_STAGING; d.BindFlags=d.MiscFlags=0; d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> copy; Check(device->CreateBuffer(&d,nullptr,&copy));
    context->CopyResource(copy.Get(),source);
    D3D11_MAPPED_SUBRESOURCE map {}; Check(context->Map(copy.Get(),0,D3D11_MAP_READ,0,&map));
    std::vector<float> result(d.ByteWidth/4);
    memcpy(result.data(),map.pData,d.ByteWidth); context->Unmap(copy.Get(),0);
    return result;
}
static UINT callback_count=0, current_stage=0;
static float current_scale=2.0f;
static ComPtr<ID3D11InfoQueue> debug_info;
static std::vector<float> original_constants;
static void WINAPI Draw(ID3D11DeviceContext* context, UINT count, UINT start, INT base) {
    Require(count==3 && start==0 && base==0,"draw arguments changed");
    ComPtr<ID3D11Buffer> patched;
    context->PSGetConstantBuffers(current_stage ? 0 : 1,1,&patched);
    auto expected=original_constants;
    for (int i=0;i<2;++i) expected[114*4+i]*=current_scale;
    for (int slot=115;slot<=(current_stage?116:115);++slot)
        for (int i=0;i<4;++i) expected[slot*4+i]*=i<2?current_scale:1.0f/current_scale;
    Require(ReadBuffer(context,patched.Get())==expected,"GPU patch changed wrong constants");
    ++callback_count;
    context->DrawIndexed(count,start,base);
}
static void Test(ID3D11Device* device, ID3D11DeviceContext* context,
                 ID3DDeviceContextState* isolated, Bridge& bridge, UINT w, UINT h, bool large_scissor = false,
                 float scale = 2.0f, bool with_grade = true) {
    current_scale=scale;
    const UINT display_w=UINT(w*scale), display_h=UINT(h*scale);
    auto low=MakeImage(device,w,h,DXGI_FORMAT_R16G16B16A16_FLOAT);
    auto high=MakeImage(device,display_w,display_h,DXGI_FORMAT_R16G16B16A16_FLOAT);
    auto tone=MakeImage(device,w,h,DXGI_FORMAT_B8G8R8A8_UNORM);
    auto grade=MakeImage(device,w,h,DXGI_FORMAT_B8G8R8A8_UNORM);
    const float hdr[]={0.25f,0.5f,2.0f,1.0f};
    context->ClearRenderTargetView(high.rtv.Get(),hdr);
    original_constants.assign(1024,0.125f);
    original_constants[456]=original_constants[457]=0;
    for (int slot=115;slot<=116;++slot) {
        original_constants[slot*4]=float(w); original_constants[slot*4+1]=float(h);
        original_constants[slot*4+2]=1.0f/w; original_constants[slot*4+3]=1.0f/h;
    }
    D3D11_BUFFER_DESC bd {}; bd.ByteWidth=4096; bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA data {original_constants.data(),0,0};
    ComPtr<ID3D11Buffer> cb; Check(device->CreateBuffer(&bd,&data,&cb));
    auto* cb_raw=cb.Get(); context->PSSetConstantBuffers(0,1,&cb_raw);
    context->PSSetConstantBuffers(1,1,&cb_raw);
    const UINT indices[]={0,1,2}; bd.ByteWidth=sizeof(indices); bd.BindFlags=D3D11_BIND_INDEX_BUFFER;
    data.pSysMem=indices; ComPtr<ID3D11Buffer> ib; Check(device->CreateBuffer(&bd,&data,&ib));
    context->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R32_UINT,0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    auto vs=Compile("float4 main(uint id:SV_VertexID):SV_POSITION { return float4(id==2?3:-1,id==1?3:-1,0,1); }","vs_5_0");
    ComPtr<ID3D11VertexShader> vertex; Check(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex));
    context->VSSetShader(vertex.Get(),nullptr,0);
    auto ps=Compile(R"(
      Texture2D<float4> Hdr:register(t1);
      cbuffer View:register(b1) { float4 Values[256]; };
      float4 main(float4 p:SV_POSITION):SV_Target {
        float2 uv=(p.xy-Values[114].xy)*Values[115].zw;
        return float4(uv,Hdr.Load(int3(p.xy,0)).r,1);
      })","ps_5_0");
    ComPtr<ID3D11PixelShader> tonemap; Check(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&tonemap));
    ps=Compile(R"(
      Texture2D<float4> Tone:register(t3);
      cbuffer View:register(b0) { float4 Values[256]; };
      float4 main(float4 p:SV_POSITION):SV_Target {
        float4 c=Tone.Load(int3(p.xy,0));
        return float4(c.rg, p.x*Values[116].z,1);
      })","ps_5_0");
    ComPtr<ID3D11PixelShader> colorgrade; Check(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&colorgrade));
    D3D11_RASTERIZER_DESC rd {}; rd.FillMode=D3D11_FILL_SOLID; rd.CullMode=D3D11_CULL_NONE;
    rd.ScissorEnable=TRUE; rd.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster; Check(device->CreateRasterizerState(&rd,&raster));
    context->RSSetState(raster.Get());
    D3D11_VIEWPORT vp {0,0,float(w),float(h),0,1}; context->RSSetViewports(1,&vp);
    D3D11_RECT scissor {0,0,LONG(w * (large_scissor ? 2 : 1)),LONG(h * (large_scissor ? 2 : 1))}; context->RSSetScissorRects(1,&scissor);
    bridge.BeginFrame(low.texture.Get(),high.srv.Get());
    std::string error;
    ComPtr<ID3D11ShaderResourceView> final;
    Require(!bridge.FinalColor(grade.srv.Get(),final),"incomplete frame accepted");
    auto* rtv=tone.rtv.Get(); auto* srv=low.srv.Get();
    context->OMSetRenderTargets(1,&rtv,nullptr);
    context->PSSetShaderResources(1,1,&srv); context->PSSetShader(tonemap.Get(),nullptr,0);
    Require(!bridge.Replay(context,isolated,Pass::ColorGrade,Draw,3,0,0,error),"out-of-order grade accepted");
    current_stage=0;
    const bool tone_ok=bridge.Replay(context,isolated,Pass::ToneMap,Draw,3,0,0,error);
    if (!tone_ok) std::cerr<<"tone replay rejected: "<<error<<'\n';
    Require(tone_ok,"tone replay failed");
    ComPtr<ID3D11RenderTargetView> restored; context->OMGetRenderTargets(1,&restored,nullptr);
    Require(restored==tone.rtv,"render target not restored");
    ComPtr<ID3D11Buffer> restored_cb; context->PSGetConstantBuffers(1,1,&restored_cb);
    Require(restored_cb==cb,"constants binding not restored");
    ComPtr<ID3D11ShaderResourceView> restored_srv; context->PSGetShaderResources(1,1,&restored_srv);
    Require(restored_srv==low.srv,"shader resource not restored");
    UINT n=1; D3D11_VIEWPORT restored_vp {}; context->RSGetViewports(&n,&restored_vp);
    Require(n==1 && restored_vp.Width==w && restored_vp.Height==h,"viewport not restored");
    n=1; D3D11_RECT restored_scissor {}; context->RSGetScissorRects(&n,&restored_scissor);
    Require(n==1 && restored_scissor.right==scissor.right && restored_scissor.bottom==scissor.bottom,"scissor not restored");
    Require(ReadBuffer(context,cb.Get())==original_constants,"original constants modified");
    Require(bridge.FinalColor(tone.srv.Get(),final),"tone-only frame missing");
    Require(!bridge.FinalColor(grade.srv.Get(),final) && !final,"unrendered grade accepted or output not cleared");
    Require(!bridge.FinalColor(low.srv.Get(),final) && !final,"HDR source accepted as final color");
    if (with_grade) {
        rtv=grade.rtv.Get(); context->OMSetRenderTargets(1,&rtv,nullptr);
        // Reject a lookalike texture that is not the actual previous pass output.
        srv=grade.srv.Get(); context->OMSetRenderTargets(0,nullptr,nullptr);
        context->PSSetShaderResources(3,1,&srv);
        rtv=tone.rtv.Get(); context->OMSetRenderTargets(1,&rtv,nullptr);
        Require(!bridge.Replay(context,isolated,Pass::ColorGrade,Draw,3,0,0,error),"unrelated resource accepted");
        srv=nullptr; context->PSSetShaderResources(3,1,&srv);
        rtv=grade.rtv.Get(); context->OMSetRenderTargets(1,&rtv,nullptr);
        srv=tone.srv.Get(); context->PSSetShaderResources(3,1,&srv);
        context->PSSetShader(colorgrade.Get(),nullptr,0); current_stage=1;
        const bool grade_ok=bridge.Replay(context,isolated,Pass::ColorGrade,Draw,3,0,0,error);
        if (!grade_ok) std::cerr<<"grade replay rejected: "<<error<<'\n';
        Require(grade_ok,"grade replay failed");
        Require(!bridge.FinalColor(tone.srv.Get(),final),"wrong final input accepted");
        Require(bridge.FinalColor(grade.srv.Get(),final),"completed frame missing");
    } else {
        Require(bridge.FinalColor(tone.srv.Get(),final),"tone-only final color missing");
    }
    ComPtr<ID3D11Resource> resource; final->GetResource(&resource);
    ComPtr<ID3D11Texture2D> output; Check(resource.As(&output));
    D3D11_TEXTURE2D_DESC td {}; output->GetDesc(&td);
    Require(td.Width==display_w && td.Height==display_h,"wrong output dimensions");
    td.Usage=D3D11_USAGE_STAGING; td.BindFlags=0; td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging; Check(device->CreateTexture2D(&td,nullptr,&staging));
    context->CopyResource(staging.Get(),output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped {}; Check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
    for (UINT y=0;y<display_h;++y) for (UINT x=0;x<display_w;++x) {
        const auto* pixel=static_cast<const unsigned char*>(mapped.pData)+y*mapped.RowPitch+x*4;
        float u=(x+0.5f)/display_w, v=(y+0.5f)/display_h;
        Require(std::abs(pixel[2]/255.0f-u)<0.005f && std::abs(pixel[1]/255.0f-v)<0.005f &&
                std::abs(pixel[0]/255.0f-(with_grade?u:hdr[0]))<0.005f && pixel[3]==255,"promoted pixel/coordinate mismatch");
    }
    context->Unmap(staging.Get(),0);
    bridge.EndFrame(); Require(!bridge.FinalColor(grade.srv.Get(),final),"stale frame accepted");
    Require(!bridge.FinalColor(tone.srv.Get(),final) && !final,"stale tone-only frame accepted");
    context->ClearState();
}
int main() {
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* exception) -> LONG {
        std::cerr<<"SEH "<<std::hex<<exception->ExceptionRecord->ExceptionCode<<std::dec<<'\n';
        if (debug_info) for (UINT64 i=0;i<debug_info->GetNumStoredMessages();++i) {
            SIZE_T size=0; debug_info->GetMessage(i,nullptr,&size); std::vector<char> bytes(size);
            auto* m=reinterpret_cast<D3D11_MESSAGE*>(bytes.data()); debug_info->GetMessage(i,m,&size);
            if (m->Severity<=D3D11_MESSAGE_SEVERITY_WARNING) std::cerr<<m->pDescription<<'\n';
        }
        return EXCEPTION_EXECUTE_HANDLER;
    });
    try {
        std::cerr<<"Creating D3D11 debug device\n";
        ComPtr<IDXGIFactory1> factory; Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> arc,candidate;
        for (UINT i=0;SUCCEEDED(factory->EnumAdapters1(i,&candidate));++i) {
            DXGI_ADAPTER_DESC1 d {}; candidate->GetDesc1(&d);
            if (d.VendorId==0x8086 && wcsstr(d.Description,L"Arc")) { arc=candidate; break; }
            candidate.Reset();
        }
        Require(bool(arc),"Intel Arc unavailable");
        ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
        const D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_11_0; D3D_FEATURE_LEVEL selected {};
        Check(D3D11CreateDevice(arc.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_DEBUG,
            &level,1,D3D11_SDK_VERSION,&device,&selected,&context));
        Check(device.As(&debug_info));
        debug_info->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION,FALSE);
        debug_info->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR,FALSE);
        debug_info->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_WARNING,FALSE);
        std::cerr<<"Creating context state\n";
        ComPtr<ID3D11Device1> device1; Check(device.As(&device1));
        ComPtr<ID3DDeviceContextState> isolated;
        Check(device1->CreateDeviceContextState(0,&level,1,D3D11_SDK_VERSION,
            __uuidof(ID3D11Device),&selected,&isolated));
        Bridge bridge;
        std::cerr<<"Testing replay\n";
        Test(device.Get(),context.Get(),isolated.Get(),bridge,16,12);
        Test(device.Get(),context.Get(),isolated.Get(),bridge,16,12,false,2.0f,false);
        Test(device.Get(),context.Get(),isolated.Get(),bridge,24,16);
        Test(device.Get(),context.Get(),isolated.Get(),bridge,24,16,true);
        Test(device.Get(),context.Get(),isolated.Get(),bridge,24,16,true,1.5f);
        Test(device.Get(),context.Get(),isolated.Get(),bridge,24,16,true,1.5f,false);
        Test(device.Get(),context.Get(),isolated.Get(),bridge,24,16,false,1.0f);
        Require(callback_count==12,"unexpected replay count");
        ComPtr<ID3D11InfoQueue> info; Check(device.As(&info));
        for (UINT64 i=0;i<info->GetNumStoredMessages();++i) {
            SIZE_T size=0; info->GetMessage(i,nullptr,&size); std::vector<char> bytes(size);
            auto* m=reinterpret_cast<D3D11_MESSAGE*>(bytes.data()); info->GetMessage(i,m,&size);
            if (m->Severity<=D3D11_MESSAGE_SEVERITY_WARNING) {
                std::cerr<<m->pDescription<<'\n'; throw std::runtime_error("D3D debug validation failed");
            }
        }
        std::cout<<"PASS: HDR postprocess pixels, GPU constants, state restoration, resource pairing, frame reset and resize; D3D debug clean\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr<<e.what()<<'\n';
        if (debug_info) for (UINT64 i=0;i<debug_info->GetNumStoredMessages();++i) {
            SIZE_T size=0; debug_info->GetMessage(i,nullptr,&size); std::vector<char> bytes(size);
            auto* m=reinterpret_cast<D3D11_MESSAGE*>(bytes.data()); debug_info->GetMessage(i,m,&size);
            if (m->Severity<=D3D11_MESSAGE_SEVERITY_WARNING) std::cerr<<m->pDescription<<'\n';
        }
        return 1;
    }
}
