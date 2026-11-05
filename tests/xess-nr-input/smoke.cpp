#include "gpu_fixture.h"
int main(){try{
    Gpu gpu;constexpr unsigned w=64,h=32,tw=80,th=48;
    auto colour=XeSSNr::InputAdapter::MakeOutput(gpu.d.Get(),tw,th);
    auto motion=XeSSNr::InputAdapter::MakeOutput(gpu.d.Get(),tw,th); // RGBA input: shader reads RG only.
    std::vector<HALF> pixels(tw*th*4),vectors(tw*th*4);
    for(unsigned y=0;y<th;++y)for(unsigned x=0;x<tw;++x){const auto i=(y*tw+x)*4;
        pixels[i]=XMConvertFloatToHalf(float(x%31)*.13f);pixels[i+1]=XMConvertFloatToHalf(float(y%23)*.37f);
        pixels[i+2]=XMConvertFloatToHalf(float((x+y)%47)*.91f);pixels[i+3]=XMConvertFloatToHalf(.25f+float(x%7)*.125f);
        vectors[i]=XMConvertFloatToHalf(float(int(x)-40)/128.f);vectors[i+1]=XMConvertFloatToHalf(float(int(y)-24)/256.f);}
    gpu.Upload(colour.Get(),pixels.data(),tw*8,D3D12_RESOURCE_STATE_COMMON);
    gpu.Upload(motion.Get(),vectors.data(),tw*8,D3D12_RESOURCE_STATE_COMMON);
    XeSSNr::InputAdapter adapter(gpu.d.Get(),w,h);
    std::vector<char> expected(w*h*8);
    for(unsigned y=0;y<h;++y)memcpy(expected.data()+y*w*8,pixels.data()+((y+5)*tw+3)*4,w*8);
    for(bool ndc:{false,true})for(bool jittered:{false,true})for(bool apply:{false,true}){
        auto out=XeSSNr::InputAdapter::MakeOutput(gpu.d.Get(),w,h);
        XeSSNr::InputAdapter::CopyOriginal(gpu.l.Get(),colour.Get(),3,5,w,h,out.Get());gpu.Submit();
        Require(gpu.Read(out.Get(),D3D12_RESOURCE_STATE_COMMON)==expected,"colour crop bypass changed bytes");
        const auto t=XeSSNr::MotionToRenderPixels(w,h,{2,-2},ndc,jittered,{.25f,-.125f},{-.125f,.25f});
        DlssNrConstants c{};c.WhitePoint=1;c.ExposurePreMul=1;c.TransferStrength=c.ColourStrength=1;c.MaxRatio=2;c.CompareZoom=1;
        adapter.Encode(gpu.l.Get(),out.Get(),motion.Get(),7,2,t,c);gpu.Submit();
        const auto converted=gpu.Read(adapter.Motion(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){const auto i=((y+2)*tw+x+7)*4;
            float actual[2];memcpy(actual,converted.data()+(y*w+x)*8,8);
            const float vx=XMConvertHalfToFloat(vectors[i])*(ndc?float(w):2.f)+(jittered?0.f:-.375f);
            const float vy=XMConvertHalfToFloat(vectors[i+1])*(ndc?float(h):-2.f)+(jittered?0.f:.375f);
            Require(std::abs(actual[0]-vx)<1e-6f && std::abs(actual[1]-vy)<1e-6f,"motion coordinate/sign mismatch");}
        // Identity model output, independent of shader arithmetic.
        Barrier(gpu.l.Get(),adapter.Proxy(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(gpu.l.Get(),adapter.Neural(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);
        gpu.l->CopyResource(adapter.Neural(),adapter.Proxy());
        Barrier(gpu.l.Get(),adapter.Proxy(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(gpu.l.Get(),adapter.Neural(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);gpu.Submit();
        adapter.Resolve(gpu.l.Get(),out.Get(),apply);gpu.Submit();const auto actual=gpu.Read(out.Get(),D3D12_RESOURCE_STATE_COMMON);
        if(!apply)Require(actual==expected,"ApplyModel=false changed HDR/alpha");
        for(size_t i=0;i<actual.size()/2;++i){HALF ah,bh;memcpy(&ah,actual.data()+i*2,2);memcpy(&bh,expected.data()+i*2,2);
            if(i%4==3)Require(ah==bh,"alpha changed");else Require(std::abs(XMConvertHalfToFloat(ah)-XMConvertHalfToFloat(bh))<.003f*std::max(1.f,XMConvertHalfToFloat(bh)),"neutral HDR error");}
    }
    // Projection-based independent sign check: jitter moves geometry by J;
    // sample locations are p-J. The previous pixel is p+Jprevious-Jcurrent.
    const XeSSNr::Vec2 previous{-.2f,.3f},current{.4f,-.1f};
    const auto t=XeSSNr::MotionToRenderPixels(w,h,{2,-2},true,false,current,previous);
    Require(std::abs((12.f+t.bias.x-previous.x)-(12.f-current.x))<1e-5f,"static jitter x reprojection failed");
    Require(std::abs((9.f+t.bias.y-previous.y)-(9.f-current.y))<1e-5f,"static jitter y reprojection failed");
    Require(XeSSNr::ScalarWhitePoint(2,4)==.125f,"scalar exposure multiplier contract");
    ComPtr<ID3D12InfoQueue> debug;Check(gpu.d.As(&debug));
    for(UINT64 i=0;i<debug->GetNumStoredMessages();++i){SIZE_T n=0;debug->GetMessage(i,nullptr,&n);std::vector<char> bytes(n);
        auto* m=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());debug->GetMessage(i,m,&n);
        if(m->Severity<=D3D12_MESSAGE_SEVERITY_WARNING){std::cerr<<m->pDescription<<'\n';throw std::runtime_error("D3D12 validation failed");}}
    std::cout<<"PASS: D3D12 HDR adapter, nonzero bases, exact off/copy, neutral HDR, alpha, pixel/NDC motion, jitter signs, repeated frames; debug clean\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
