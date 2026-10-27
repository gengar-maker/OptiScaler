#include <pch.h>
#include "D3D11_FrameTrace.h"
#include "Hook_Utils.h"
#include <Util.h>
#include <detours/detours.h>
#include <array>
#include <atomic>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <wrl/client.h>

namespace D3D11FrameTrace {
namespace {
using Microsoft::WRL::ComPtr;
using DrawFn = rewrite_signature<decltype(&ID3D11DeviceContext::Draw)>::type;
using DrawIndexedFn = rewrite_signature<decltype(&ID3D11DeviceContext::DrawIndexed)>::type;
using DrawInstancedFn = rewrite_signature<decltype(&ID3D11DeviceContext::DrawInstanced)>::type;
using DrawIndexedInstancedFn = rewrite_signature<decltype(&ID3D11DeviceContext::DrawIndexedInstanced)>::type;
using DispatchFn = rewrite_signature<decltype(&ID3D11DeviceContext::Dispatch)>::type;
DrawFn original_draw = nullptr;
DrawIndexedFn original_draw_indexed = nullptr;
DrawInstancedFn original_draw_instanced = nullptr;
DrawIndexedInstancedFn original_draw_indexed_instanced = nullptr;
DispatchFn original_dispatch = nullptr;
std::mutex trace_mutex;
std::deque<std::string> events;
std::atomic<bool> capturing {false};
ID3D11Device* traced_device = nullptr;
ID3D11DeviceContext* traced_context = nullptr;
unsigned present_count = 0;
uint64_t event_count = 0;
constexpr size_t max_events = 4096;

std::string DescribeResource(ID3D11Resource* resource) {
    if (!resource) return "-";
    std::ostringstream out;
    out << resource;
    ComPtr<ID3D11Texture2D> texture;
    if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) {
        D3D11_TEXTURE2D_DESC desc {};
        texture->GetDesc(&desc);
        out << '@' << desc.Width << 'x' << desc.Height << ':'
            << static_cast<unsigned>(desc.Format);
    }
    return out.str();
}

template <typename View> std::string DescribeView(View* view) {
    if (!view) return "-";
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    return DescribeResource(resource.Get());
}

template <typename View, size_t N>
void AppendViews(std::ostringstream& out, const std::array<View*, N>& views) {
    bool first = true;
    for (size_t i = 0; i < N; ++i) {
        if (!views[i]) continue;
        if (!first) out << '|';
        out << i << '=' << DescribeView(views[i]);
        views[i]->Release();
        first = false;
    }
}

void Record(ID3D11DeviceContext* context, const char* operation) {
    if (!capturing.load(std::memory_order_relaxed) || context != traced_context)
        return;
    // Get* methods add references; release every returned object below.
    std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> rtvs {};
    ComPtr<ID3D11DepthStencilView> depth;
    context->OMGetRenderTargets(static_cast<UINT>(rtvs.size()), rtvs.data(), &depth);
    std::array<ID3D11ShaderResourceView*, 16> srvs {};
    std::array<ID3D11UnorderedAccessView*, 8> uavs {};
    ComPtr<ID3D11PixelShader> pixel_shader;
    ComPtr<ID3D11ComputeShader> compute_shader;
    const bool compute = operation[0] == 'C';
    if (compute) {
        context->CSGetShaderResources(0, static_cast<UINT>(srvs.size()), srvs.data());
        context->CSGetUnorderedAccessViews(0, static_cast<UINT>(uavs.size()), uavs.data());
        context->CSGetShader(&compute_shader, nullptr, nullptr);
    } else {
        context->PSGetShaderResources(0, static_cast<UINT>(srvs.size()), srvs.data());
        context->PSGetShader(&pixel_shader, nullptr, nullptr);
    }
    std::ostringstream out;
    out << operation << ';' << (compute ? static_cast<void*>(compute_shader.Get())
                                        : static_cast<void*>(pixel_shader.Get())) << ';';
    AppendViews(out, rtvs);
    out << ';' << DescribeView(depth.Get()) << ';';
    AppendViews(out, srvs);
    out << ';';
    AppendViews(out, uavs);
    std::lock_guard lock(trace_mutex);
    ++event_count;
    if (events.size() == max_events) events.pop_front();
    events.emplace_back(out.str());
}

void WINAPI HookDraw(ID3D11DeviceContext* context, UINT vertex_count, UINT start_vertex) {
    Record(context, "Draw");
    original_draw(context, vertex_count, start_vertex);
}
void WINAPI HookDrawIndexed(ID3D11DeviceContext* context, UINT index_count, UINT start_index, INT base_vertex) {
    Record(context, "DrawIndexed");
    original_draw_indexed(context, index_count, start_index, base_vertex);
}
void WINAPI HookDrawInstanced(ID3D11DeviceContext* context, UINT vertices, UINT instances, UINT start_vertex, UINT start_instance) {
    Record(context, "DrawInstanced");
    original_draw_instanced(context, vertices, instances, start_vertex, start_instance);
}
void WINAPI HookDrawIndexedInstanced(ID3D11DeviceContext* context, UINT indices, UINT instances, UINT start_index, INT base_vertex, UINT start_instance) {
    Record(context, "DrawIndexedInstanced");
    original_draw_indexed_instanced(context, indices, instances, start_index, base_vertex, start_instance);
}
void WINAPI HookDispatch(ID3D11DeviceContext* context, UINT x, UINT y, UINT z) {
    Record(context, "CSDispatch");
    original_dispatch(context, x, y, z);
}
}

void Install(ID3D11Device* device) {
    if (!device || original_draw) return;
    std::error_code error;
    if (!std::filesystem::exists(Util::DllPath().parent_path() /
                                 "OptiScaler.d3d11trace", error)) return;
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!context) return;
    void** vtable = *reinterpret_cast<void***>(context.Get());
    original_draw_indexed = reinterpret_cast<DrawIndexedFn>(vtable[12]);
    original_draw = reinterpret_cast<DrawFn>(vtable[13]);
    original_draw_indexed_instanced = reinterpret_cast<DrawIndexedInstancedFn>(vtable[20]);
    original_draw_instanced = reinterpret_cast<DrawInstancedFn>(vtable[21]);
    // ID3D11DeviceContext slot 37 is SOSetTargets; Dispatch is slot 41.
    original_dispatch = reinterpret_cast<DispatchFn>(vtable[41]);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(reinterpret_cast<PVOID*>(&original_draw_indexed), HookDrawIndexed);
    DetourAttach(reinterpret_cast<PVOID*>(&original_draw), HookDraw);
    DetourAttach(reinterpret_cast<PVOID*>(&original_draw_indexed_instanced), HookDrawIndexedInstanced);
    DetourAttach(reinterpret_cast<PVOID*>(&original_draw_instanced), HookDrawInstanced);
    DetourAttach(reinterpret_cast<PVOID*>(&original_dispatch), HookDispatch);
    const LONG result = DetourTransactionCommit();
    if (result != NO_ERROR) {
        LOG_ERROR("D3D11 frame trace hook failed: {}", result);
        original_draw_indexed = nullptr;
        original_draw = nullptr;
        original_draw_indexed_instanced = nullptr;
        original_draw_instanced = nullptr;
        original_dispatch = nullptr;
        return;
    }
    LOG_INFO("D3D11 frame trace hooks installed; waiting for capture marker");
}

void BeforePresent(IDXGISwapChain*, ID3D11Device* device) {
    if (!original_draw || !device || present_count >= 2) return;
    if (!traced_device) {
        ComPtr<ID3D11DeviceContext> context;
        device->GetImmediateContext(&context);
        if (!context) return;
        traced_device = device;
        traced_context = context.Get();
    }
    if (device != traced_device) return;
    if (present_count == 0) {
        std::error_code error;
        if (!std::filesystem::exists(Util::DllPath().parent_path() /
                                     "OptiScaler.d3d11trace.capture", error)) return;
        present_count = 1;
        capturing.store(true, std::memory_order_release);
        return;
    }
    present_count = 2;
    capturing.store(false, std::memory_order_release);
    const auto path = Util::DllPath().parent_path() / "OptiScaler.d3d11trace.csv";
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        LOG_ERROR("Cannot write D3D11 frame trace to {}", path.string());
        return;
    }
    file << "sequence;operation;shader;rtv;depth;srv;uav\n";
    std::lock_guard lock(trace_mutex);
    uint64_t sequence = event_count - events.size();
    for (const auto& event : events) file << ++sequence << ';' << event << '\n';
    LOG_INFO("D3D11 frame trace: {} events, last {} saved to {}",
             event_count, events.size(), path.string());
    std::error_code error;
    std::filesystem::remove(Util::DllPath().parent_path() /
                            "OptiScaler.d3d11trace.capture", error);
    std::filesystem::remove(Util::DllPath().parent_path() /
                            "OptiScaler.d3d11trace", error);
}

void Uninstall() {
    capturing.store(false, std::memory_order_release);
    if (!original_draw) return;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(reinterpret_cast<PVOID*>(&original_draw_indexed), HookDrawIndexed);
    DetourDetach(reinterpret_cast<PVOID*>(&original_draw), HookDraw);
    DetourDetach(reinterpret_cast<PVOID*>(&original_draw_indexed_instanced), HookDrawIndexedInstanced);
    DetourDetach(reinterpret_cast<PVOID*>(&original_draw_instanced), HookDrawInstanced);
    DetourDetach(reinterpret_cast<PVOID*>(&original_dispatch), HookDispatch);
    DetourTransactionCommit();
    original_draw = nullptr;
    traced_device = nullptr;
    traced_context = nullptr;
}
}
