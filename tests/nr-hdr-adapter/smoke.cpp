#define NOMINMAX
#include "../../OptiScaler/dlssnr/sycl/NrHdrAdapter_Dx11.h"
#include <DirectXPackedVector.h>
#include <d3d11sdklayers.h>
#include <dxgi1_6.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
using namespace DirectX::PackedVector;
using Pixel = std::array<float, 4>;
static void Require(bool v, const std::string& s) { if (!v) throw std::runtime_error(s); }
static void Check(HRESULT h) { Require(SUCCEEDED(h), "D3D call failed"); }
static ComPtr<ID3D11Texture2D> Image(ID3D11Device* d, const std::vector<Pixel>& p,
                                   UINT width = 0, UINT height = 1) {
    if (!width) width = UINT(p.size());
    Require(size_t(width)*height == p.size(), "invalid image dimensions");
    std::vector<HALF> halves;
    for (const auto& x : p) for (float f : x) halves.push_back(XMConvertFloatToHalf(f));
    D3D11_TEXTURE2D_DESC desc {}; desc.Width=width; desc.Height=height;
    desc.ArraySize=desc.MipLevels=desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data {halves.data(),width*8,0};
    ComPtr<ID3D11Texture2D> t; Check(d->CreateTexture2D(&desc,&data,&t)); return t;
}
static std::vector<Pixel> Read(ID3D11DeviceContext* c, ID3D11Texture2D* t) {
    ComPtr<ID3D11Device> d; c->GetDevice(&d);
    D3D11_TEXTURE2D_DESC desc {}; t->GetDesc(&desc);
    desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stage; Check(d->CreateTexture2D(&desc,nullptr,&stage));
    c->CopyResource(stage.Get(),t);
    D3D11_MAPPED_SUBRESOURCE m {}; Check(c->Map(stage.Get(),0,D3D11_MAP_READ,0,&m));
    std::vector<Pixel> p(size_t(desc.Width)*desc.Height);
    for (UINT y=0;y<desc.Height;++y) for (UINT x=0;x<desc.Width;++x) for (UINT i=0;i<4;++i)
        p[size_t(y)*desc.Width+x][i]=XMConvertHalfToFloat(
            reinterpret_cast<const HALF*>(static_cast<const char*>(m.pData)+size_t(y)*m.RowPitch)[x*4+i]);
    c->Unmap(stage.Get(),0); return p;
}
static float Srgb(float v) { return v<=0.0031308f ? 12.92f*v : 1.055f*std::pow(v,1/2.4f)-0.055f; }
static Pixel Expected(Pixel p,float white) {
    for (int i=0;i<3;++i) p[i]=std::max(0.0f,p[i])/white;
    const float l=p[0]*.2126f+p[1]*.7152f+p[2]*.0722f;
    if (l>.75f) for (int i=0;i<3;++i) p[i]*=(.75f+.25f*(1-std::exp(-(l-.75f)/.25f)))/l;
    const float peak=std::max({1.0f,p[0],p[1],p[2]});
    for (int i=0;i<3;++i) p[i]=Srgb(p[i]/peak);
    return p;
}
static std::vector<Pixel> Load(const char* path, UINT w, UINT h) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const size_t count=size_t(w)*h*4;
    Require(file && file.tellg()==std::streamoff(count*2), "invalid capture size");
    file.seekg(0);
    std::vector<HALF> raw(count);
    Require(bool(file.read(reinterpret_cast<char*>(raw.data()), count*2)), "capture read failed");
    std::vector<Pixel> pixels(size_t(w)*h);
    for (size_t i=0;i<count;++i) {
        pixels[i/4][i%4]=XMConvertHalfToFloat(raw[i]);
        Require(std::isfinite(pixels[i/4][i%4]), "nonfinite capture");
    }
    return pixels;
}
static void Save(const char* path, const std::vector<Pixel>& pixels) {
    std::vector<HALF> raw(pixels.size()*4);
    for (size_t i=0;i<raw.size();++i) {
        Require(std::isfinite(pixels[i/4][i%4]), "nonfinite adapter output");
        raw[i]=XMConvertFloatToHalf(pixels[i/4][i%4]);
    }
    std::ofstream file(path,std::ios::binary);
    Require(bool(file.write(reinterpret_cast<const char*>(raw.data()),raw.size()*2)), "output write failed");
}
int main(int argc, char** argv) {
    try {
        ComPtr<IDXGIFactory1> factory; Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> arc, candidate;
        for (UINT i=0;SUCCEEDED(factory->EnumAdapters1(i,&candidate));++i) {
            DXGI_ADAPTER_DESC1 a {}; candidate->GetDesc1(&a);
            if (a.VendorId==0x8086 && wcsstr(a.Description,L"Arc")) { arc=candidate; break; }
            candidate.Reset();
        }
        Require(bool(arc),"Intel Arc unavailable");
        ComPtr<ID3D11Device> d; ComPtr<ID3D11DeviceContext> c;
        D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_11_0;
        Check(D3D11CreateDevice(arc.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_DEBUG,
            &level,1,D3D11_SDK_VERSION,&d,nullptr,&c));
        ComPtr<ID3D11InfoQueue> debug; Check(d.As(&debug));
        debug->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION,FALSE);
        debug->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR,FALSE);
        debug->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_WARNING,FALSE);
        if (argc != 1) {
            const bool encode=std::string(argv[1])=="--encode";
            Require((encode && argc==7) || (std::string(argv[1])=="--resolve" && argc==8),
                    "Usage: smoke --encode HDR WIDTH HEIGHT EXPOSURE PROXY | --resolve HDR WIDTH HEIGHT EXPOSURE NEURAL OUTPUT");
            const UINT w=UINT(std::stoul(argv[3])), h=UINT(std::stoul(argv[4]));
            Require(w>0 && h>0 && w<=8192 && h<=8192,"invalid capture extent");
            float exposure=0;
            std::ifstream efile(argv[5],std::ios::binary);
            Require(bool(efile.read(reinterpret_cast<char*>(&exposure),sizeof(exposure))) &&
                    std::isfinite(exposure) && exposure>0,"invalid captured exposure");
            auto hdr=Image(d.Get(),Load(argv[2],w,h),w,h);
            auto e=Image(d.Get(),{{exposure,0,0,0}});
            ComPtr<ID3D11ShaderResourceView> esrv; Check(d->CreateShaderResourceView(e.Get(),nullptr,&esrv));
            DlssNrConstants settings {}; settings.WhitePoint=1; settings.ExposurePreMul=1;
            settings.MaxRatio=2; settings.TransferStrength=1; settings.ColourStrength=1;
            settings.CompareZoom=1;
            NrHdrAdapterDx11::Adapter adapter; std::string error;
            Require(adapter.Encode(c.Get(),hdr.Get(),settings,esrv.Get(),error),error);
            if (encode) Save(argv[6],Read(c.Get(),adapter.Proxy()));
            else {
                auto neural=Image(d.Get(),Load(argv[6],w,h),w,h);
                Require(adapter.Resolve(c.Get(),neural.Get(),true,error),error);
                Save(argv[7],Read(c.Get(),adapter.Output()));
            }
        }
        std::vector<Pixel> pixels {{0,0,0,.5f},{.001f,.002f,.003f,1},{.18f,.18f,.18f,1},
            {.7f,.2f,.1f,1},{1,1,1,1},{4,.1f,.2f,1},{.01f,.1f,32,1},{32,16,8,1}};
        auto input=Image(d.Get(),pixels); pixels=Read(c.Get(),input.Get());
        NrHdrAdapterDx11::Adapter a; std::string error;
        for (float white : {.125f,1.0f,8.0f}) for (bool use_exposure : {false,true})
            for (float colour_strength : {0.0f, 1.0f}) {
            DlssNrConstants settings {}; settings.WhitePoint=white; settings.ExposurePreMul=1;
            settings.MaxRatio=2; settings.TransferStrength=1; settings.CompareZoom=1;
            settings.ColourStrength=colour_strength;
            auto exposure=Image(d.Get(),{{1/white,0,0,0}});
            ComPtr<ID3D11ShaderResourceView> e; Check(d->CreateShaderResourceView(exposure.Get(),nullptr,&e));
            NrHdrAdapterDx11::Exposure converted;
            Require(converted.Copy(c.Get(),e.Get(),error),error);
            D3D11_TEXTURE2D_DESC ed {}; converted.Texture()->GetDesc(&ed);
            Require(ed.Format==DXGI_FORMAT_R32_FLOAT && ed.Width==1 && ed.Height==1,"XeSS exposure contract violated");
            ed.Usage=D3D11_USAGE_STAGING; ed.BindFlags=0; ed.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> read_exposure; Check(d->CreateTexture2D(&ed,nullptr,&read_exposure));
            c->CopyResource(read_exposure.Get(),converted.Texture());
            D3D11_MAPPED_SUBRESOURCE em {}; Check(c->Map(read_exposure.Get(),0,D3D11_MAP_READ,0,&em));
            const float scale=*static_cast<const float*>(em.pData); c->Unmap(read_exposure.Get(),0);
            Require(scale==1/white,"GPU exposure conversion changed scale");
            e=converted.View();
            Require(a.Encode(c.Get(),input.Get(),settings,use_exposure?e.Get():nullptr,error),error);
            const auto proxy=Read(c.Get(),a.Proxy());
            for (size_t p=0;p<pixels.size();++p) for (int i=0;i<4;++i)
                Require(std::abs(proxy[p][i]-Expected(pixels[p],white)[i])<.0012f,"proxy disagrees with independent sRGB/soft-knee reference");
            Require(a.Resolve(c.Get(),nullptr,false,error),error);
            Require(Read(c.Get(),a.Output())==pixels,"ApplyModel=false changed HDR or alpha");
            Require(a.Resolve(c.Get(),a.Proxy(),true,error),error);
            const auto neutral=Read(c.Get(),a.Output());
            for (size_t p=0;p<pixels.size();++p) for (int i=0;i<4;++i)
                Require(std::abs(neutral[p][i]-pixels[p][i])<=.002f*std::max(1.0f,pixels[p][i]),"neutral model changed HDR color");
            auto changed=proxy;
            for (auto& p : changed) { p[0]=std::min(1.0f,p[0]+.12f); p[1]*=.8f; }
            auto model=Image(d.Get(),changed);
            Require(a.Resolve(c.Get(),model.Get(),true,error),error);
            const auto edited=Read(c.Get(),a.Output());
            bool colour_edit_visible=false;
            for (size_t p=0;p<pixels.size();++p) {
                for (int i=0;i<3;++i) Require(std::isfinite(edited[p][i]) && edited[p][i]>=0,"nonfinite/negative HDR result");
                Require(edited[p][3]==pixels[p][3],"alpha changed");
                const float cross=edited[p][0]*pixels[p][2]-edited[p][2]*pixels[p][0];
                if (colour_strength==0)
                    Require(std::abs(cross)<=.002f*std::max(1.0f,pixels[p][0]*pixels[p][2]),"ColourStrength=0 changed red/blue chromaticity");
                else if (std::abs(cross)>.01f*std::max(1.0f,pixels[p][0]*pixels[p][2]))
                    colour_edit_visible=true;
            }
            if (colour_strength==1)
                Require(colour_edit_visible,"Full-colour mode suppressed the model's chromatic edit");
            settings.TransferStrength=0;
            Require(a.Encode(c.Get(),input.Get(),settings,use_exposure?e.Get():nullptr,error),error);
            Require(a.Resolve(c.Get(),model.Get(),true,error),error);
            Require(Read(c.Get(),a.Output())==pixels,"TransferStrength=0 changed HDR");
        }
        for (UINT64 i=0;i<debug->GetNumStoredMessages();++i) {
            SIZE_T n=0; debug->GetMessage(i,nullptr,&n); std::vector<char> b(n);
            auto* m=reinterpret_cast<D3D11_MESSAGE*>(b.data()); debug->GetMessage(i,m,&n);
            if (m->Severity<=D3D11_MESSAGE_SEVERITY_WARNING) {
                std::cerr<<m->pDescription<<'\n'; throw std::runtime_error("D3D debug validation failed");
            }
        }
        std::cout<<"PASS: existing HDR shader; CPU sRGB/knee reference, live exposure, exact bypass, neutral HDR and alpha at colour strengths 0/1; full-colour edits retained; D3D11 debug clean\n";
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
