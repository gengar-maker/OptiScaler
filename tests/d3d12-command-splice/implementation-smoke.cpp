#define NOMINMAX
#include "../../OptiScaler/dlssnr/sycl/D3D12CommandSplice.cpp"
#include "../xess-nr-input/gpu_fixture.h"
#include <d3dcompiler.h>

// Test-only alternate implementation on our own idle COM object. It forwards
// to the saved native implementation without passing through the entry point
// discovered from OptiScaler's probe. This models a different game CL class.
using DrawSignature=decltype(&ID3D12GraphicsCommandList::DrawInstanced);
using DrawHook=D3D12CommandSplice::Hook<12,DrawSignature>;
DrawHook::Fn alternateOriginal=nullptr;
unsigned alternateCalls=0;
__declspec(noinline) void STDMETHODCALLTYPE AlternateDraw(ID3D12GraphicsCommandList* self,UINT a,UINT b,UINT c,UINT d) {
    ++alternateCalls;alternateOriginal(self,a,b,c,d);
}
using ConstantsHook=D3D12CommandSplice::Hook<36,decltype(&ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstants)>;
ConstantsHook::Fn alternateConstantsOriginal=nullptr;
ID3D12GraphicsCommandList* expectedAlternateObject=nullptr;
__declspec(noinline) void STDMETHODCALLTYPE AlternateConstants(ID3D12GraphicsCommandList* self,UINT index,UINT count,const void* values,UINT offset) {
    Require(self==expectedAlternateObject,"source implementation called on incompatible tail object");
    alternateConstantsOriginal(self,index,count,values,offset);
}
struct AlternateImplementation {
    ID3D12GraphicsCommandList* object;
    void** saved;
    std::array<void*,86> table{};
    explicit AlternateImplementation(ID3D12GraphicsCommandList* list):object(list),saved(*reinterpret_cast<void***>(list)) {
        size_t count=60;
#define VERSION(N,COUNT) {ComPtr<ID3D12GraphicsCommandList##N> version;if(SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&version))))count=COUNT;}
        VERSION(1,66);VERSION(2,67);VERSION(3,68);VERSION(4,77);VERSION(5,79);
        VERSION(6,80);VERSION(7,81);VERSION(8,82);VERSION(9,84);VERSION(10,86);
#undef VERSION
        std::copy(saved,saved+count,table.begin());
        alternateOriginal=DrawHook::original;
        table[12]=reinterpret_cast<void*>(AlternateDraw);
        alternateConstantsOriginal=ConstantsHook::original;
        expectedAlternateObject=list;
        table[36]=reinterpret_cast<void*>(AlternateConstants);
        Apply();
    }
    void Apply(){*reinterpret_cast<void***>(object)=table.data();}
    ~AlternateImplementation(){*reinterpret_cast<void***>(object)=saved;}
};
int main(int argc, char** argv)
{
    try
    {
        const bool baseline = argc > 1 && std::string(argv[1]) == "--baseline";
        Gpu gpu;
        std::string error;
        Require(D3D12CommandSplice::Install(
                    gpu.d.Get(), [](const auto& m) { std::cerr << m << '\n'; }, error),
                error.c_str());
        // Establish a recorded Reset boundary after installing the hooks.
        gpu.Submit();
        AlternateImplementation implementation(gpu.l.Get());
        // A newly observed implementation must get a fresh recording boundary.
        (void)D3D12CommandSplice::Begin(gpu.l.Get(),[](auto*,auto*){},error);
        gpu.Submit();

        D3D12_DESCRIPTOR_HEAP_DESC hd { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
        ComPtr<ID3D12DescriptorHeap> rtv;
        Check(gpu.d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv)));
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = td.Height = 16;
        td.DepthOrArraySize = td.MipLevels = td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear {};
        clear.Format = td.Format;
        ComPtr<ID3D12Resource> first, second, spare;
        Check(gpu.d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                             IID_PPV_ARGS(&spare)));
        Check(gpu.d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                             IID_PPV_ARGS(&first)));
        Check(gpu.d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                             IID_PPV_ARGS(&second)));
        std::array<ComPtr<ID3D12Resource>, 3> targets = { first, second, spare };
        std::array<ComPtr<ID3D12Resource>, 2> depths;
        auto depthDesc = td;
        depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
        depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE depthClear {};
        depthClear.Format = depthDesc.Format;
        depthClear.DepthStencil = { 1, 0 };
        for (auto& depth : depths)
            Check(gpu.d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &depthDesc,
                                                 D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear, IID_PPV_ARGS(&depth)));
        ComPtr<ID3D12DescriptorHeap> dsv;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hd.NumDescriptors = 1;
        Check(gpu.d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv)));
        const auto depthHandle = dsv->GetCPUDescriptorHandleForHeapStart();
        D3D12_ROOT_PARAMETER rp {};
        rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp.Constants = { 0, 0, 4 };
        D3D12_ROOT_SIGNATURE_DESC rd {};
        rd.NumParameters = 1;
        rd.pParameters = &rp;
        rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> sig, errors;
        Check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &errors));
        ComPtr<ID3D12RootSignature> root;
        Check(gpu.d->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root)));
        const char* shader = R"(
        cbuffer C:register(b0){float4 colour;}
        float4 vs(uint id:SV_VertexID):SV_Position {float2 xy=float2((id<<1)&2,id&2);return float4(xy*float2(2,-2)+float2(-1,1),.25,1);}
        struct Output {float4 a:SV_Target0;float4 b:SV_Target1;}; Output ps(){Output o;o.a=colour;o.b=colour*.5;return o;})";
        ComPtr<ID3DBlob> vs, ps;
        Check(D3DCompile(shader, strlen(shader), nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &errors));
        Check(D3DCompile(shader, strlen(shader), nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &errors));
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd {};
        pd.pRootSignature = root.Get();
        pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.DepthStencilState.DepthEnable = TRUE;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.StencilEnable = FALSE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 2;
        pd.RTVFormats[0] = pd.RTVFormats[1] = td.Format;
        pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pd.SampleDesc.Count = 1;
        ComPtr<ID3D12PipelineState> pso;
        Check(gpu.d->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)));
        const UINT stride = gpu.d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        const auto slot = [&](UINT i)
        { return D3D12_CPU_DESCRIPTOR_HANDLE { rtv->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(i) * stride }; };
        unsigned callbacks = 0, expectedCallbacks = 0;
        for (UINT frame = 0; frame < 18; ++frame)
        {
            implementation.Apply();
            const UINT a = frame % 3, b = (frame + 1) % 3, unused = (frame + 2) % 3, z = frame % 2;
            const BOOL contiguous = (frame / 3) % 2;
            const bool split = !baseline && frame % 3 != 2;
            const float zero[4] {};
            for (auto& target : targets)
            {
                gpu.d->CreateRenderTargetView(target.Get(), nullptr, slot(3));
                gpu.l->ClearRenderTargetView(slot(3), zero, 0, nullptr);
            }
            for (auto& depth : depths)
            {
                gpu.d->CreateDepthStencilView(depth.Get(), nullptr, depthHandle);
                gpu.l->ClearDepthStencilView(depthHandle, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
            }
            D3D12_CPU_DESCRIPTOR_HANDLE handles[] = { slot(0), slot(contiguous ? 1 : 2) };
            gpu.d->CreateRenderTargetView(targets[a].Get(), nullptr, handles[0]);
            gpu.d->CreateRenderTargetView(targets[b].Get(), nullptr, handles[1]);
            gpu.d->CreateDepthStencilView(depths[z].Get(), nullptr, depthHandle);
            gpu.l->OMSetRenderTargets(2, handles, contiguous, &depthHandle);
            if (frame % 4 == 0)
            {
                gpu.l->ClearState(nullptr);
                gpu.l->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
                gpu.l->OMSetRenderTargets(2, handles, contiguous, &depthHandle);
            }
            D3D12_VIEWPORT viewport { 0, 0, 16, 16, 0, 1 };
            D3D12_RECT rect { 0, 0, 16, 16 };
            gpu.l->RSSetViewports(1, &viewport);
            gpu.l->RSSetScissorRects(1, &rect);
            gpu.l->SetPipelineState(pso.Get());
            gpu.l->SetGraphicsRootSignature(root.Get());
            gpu.l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            const float colour[4] = { (frame + 1) / 32.f, .5f, .75f, 1 };
            gpu.l->SetGraphicsRoot32BitConstants(0, 2, colour, 0);
            gpu.l->SetGraphicsRoot32BitConstants(0, 2, colour + 2, 2);
            // CPU RTV/DSV descriptors are consumed at bind time. Reusing both heaps
            // before Begin must not redirect subsequent drawing to the spare targets.
            for (auto h : handles)
                gpu.d->CreateRenderTargetView(targets[unused].Get(), nullptr, h);
            gpu.d->CreateDepthStencilView(depths[1 - z].Get(), nullptr, depthHandle);
            if (split)
            {
                ++expectedCallbacks;
                Require(
                    D3D12CommandSplice::Begin(gpu.l.Get(), [&](auto*, auto* queue) {
                        ++callbacks;
                        // Work between prefix and tail erases all render targets.
                        // A draw that bypasses redirection runs BEFORE this work
                        // and is lost, even though both command lists complete.
                        ComPtr<ID3D12CommandAllocator> allocator;
                        ComPtr<ID3D12GraphicsCommandList> clearList;
                        Check(gpu.d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
                        Check(gpu.d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&clearList)));
                        for(auto& target:targets){
                            gpu.d->CreateRenderTargetView(target.Get(),nullptr,slot(3));
                            clearList->ClearRenderTargetView(slot(3),zero,0,nullptr);
                        }
                        for(auto& depth:depths){
                            gpu.d->CreateDepthStencilView(depth.Get(),nullptr,depthHandle);
                            clearList->ClearDepthStencilView(depthHandle,D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);
                        }
                        Check(clearList->Close());ID3D12CommandList* clearLists[]={clearList.Get()};
                        queue->ExecuteCommandLists(1,clearLists);
                        ComPtr<ID3D12Fence> done;Check(gpu.d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&done)));
                        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);Require(event,"test event");
                        Check(queue->Signal(done.Get(),1));Check(done->SetEventOnCompletion(1,event));
                        Require(WaitForSingleObject(event,30000)==WAIT_OBJECT_0,"test callback fence");CloseHandle(event);
                    }, error), error.c_str());
            }
            gpu.l->DrawInstanced(3, 1, 0, 0);
            gpu.Submit();
            for (UINT i = 0; i < targets.size(); ++i)
            {
                const auto bytes = gpu.Read(targets[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
                std::array<float, 4> expected {};
                if (i != unused)
                    for (UINT c = 0; c < 4; ++c)
                        expected[c] = colour[c] * (i == a ? 1.f : .5f);
                for (size_t pixel = 0; pixel < bytes.size(); pixel += sizeof(expected))
                    if (memcmp(bytes.data() + pixel, expected.data(), sizeof(expected)))
                        throw std::runtime_error("wrong/stale graphics pixel: frame=" + std::to_string(frame) +
                                                 " target=" + std::to_string(i));
            }
            for (UINT i = 0; i < depths.size(); ++i)
            {
                const auto bytes = gpu.Read(depths[i].Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
                const float expected = i == z ? .25f : 1.f;
                for (size_t pixel = 0; pixel < bytes.size(); pixel += sizeof(float))
                    Require(memcmp(bytes.data() + pixel, &expected, sizeof(float)) == 0, "wrong depth target");
            }
        }
        Require(callbacks == expectedCallbacks, "missing/repeated callback");
        ComPtr<ID3D12InfoQueue> debug;
        if(!GetEnvironmentVariableW(L"SNR_TEST_NO_DEBUG",nullptr,0))Check(gpu.d.As(&debug));
        for (UINT64 i = 0; debug && i < debug->GetNumStoredMessages(); ++i)
        {
            SIZE_T n = 0;
            debug->GetMessage(i, nullptr, &n);
            std::vector<char> bytes(n);
            auto* m = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
            debug->GetMessage(i, m, &n);
            if (m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
            {
                std::cerr << m->pDescription << '\n';
                throw std::runtime_error("D3D12 validation failed");
            }
        }
        std::cout
            << "PASS alternate implementation: " << (baseline ? "baseline" : "splice")
            << "; 18 changing frames, rotating MRT/DSV, descriptor recycling, ClearState, split off/on; " << (debug ? "debug clean" : "debug disabled") << "\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
