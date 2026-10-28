#include <pch.h>
#include "D3D11_FrameTrace.h"
#include "Hook_Utils.h"
#include <Util.h>
#include <detours/detours.h>
#include <d3dcompiler.h>
#include <array>
#include <atomic>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <wrl/client.h>

namespace D3D11FrameTrace {
namespace {
using Microsoft::WRL::ComPtr;
using DrawFn = rewrite_signature<decltype(&ID3D11DeviceContext::Draw)>::type;
using DrawIndexedFn = rewrite_signature<decltype(&ID3D11DeviceContext::DrawIndexed)>::type;
using DrawInstancedFn = rewrite_signature<decltype(&ID3D11DeviceContext::DrawInstanced)>::type;
using DrawIndexedInstancedFn = rewrite_signature<decltype(&ID3D11DeviceContext::DrawIndexedInstanced)>::type;
using DispatchFn = rewrite_signature<decltype(&ID3D11DeviceContext::Dispatch)>::type;
using CreatePixelShaderFn = rewrite_signature<decltype(&ID3D11Device::CreatePixelShader)>::type;
using CreateComputeShaderFn = rewrite_signature<decltype(&ID3D11Device::CreateComputeShader)>::type;
DrawFn original_draw = nullptr;
DrawIndexedFn original_draw_indexed = nullptr;
DrawInstancedFn original_draw_instanced = nullptr;
DrawIndexedInstancedFn original_draw_indexed_instanced = nullptr;
DispatchFn original_dispatch = nullptr;
CreatePixelShaderFn original_create_pixel_shader = nullptr;
CreateComputeShaderFn original_create_compute_shader = nullptr;
std::mutex trace_mutex;
std::deque<std::string> events;
std::unordered_map<ID3D11PixelShader*, std::vector<uint8_t>> pixel_shader_code;
std::unordered_map<ID3D11ComputeShader*, std::vector<uint8_t>> compute_shader_code;
std::unordered_set<ID3D11PixelShader*> used_pixel_shaders;
std::unordered_set<ID3D11ComputeShader*> used_compute_shaders;
size_t stored_shader_bytes = 0;
std::atomic<bool> capturing {false};
ID3D11Device* traced_device = nullptr;
ID3D11DeviceContext* traced_context = nullptr;
unsigned present_count = 0;
uint64_t event_count = 0;
constexpr size_t max_events = 4096;
constexpr size_t max_shader_bytes = 64 * 1024 * 1024;

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
    std::array<char, 256> name {};
    UINT name_size = static_cast<UINT>(name.size());
    if (SUCCEEDED(resource->GetPrivateData(WKPDID_D3DDebugObjectName,
                                           &name_size, name.data()))) {
        out << '#';
        for (UINT i = 0; i < name_size; ++i) {
            const char c = name[i];
            out << ((c == ';' || c == '\n' || c == '\r') ? '_' : c);
        }
    }
    return out.str();
}

template <typename View> std::string DescribeView(View* view) {
    if (!view) return "-";
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    std::ostringstream out;
    out << DescribeResource(resource.Get());
    if constexpr (std::is_same_v<View, ID3D11ShaderResourceView>) {
        D3D11_SHADER_RESOURCE_VIEW_DESC desc {};
        view->GetDesc(&desc);
        out << '~' << static_cast<unsigned>(desc.Format);
    } else if constexpr (std::is_same_v<View, ID3D11RenderTargetView>) {
        D3D11_RENDER_TARGET_VIEW_DESC desc {};
        view->GetDesc(&desc);
        out << '~' << static_cast<unsigned>(desc.Format);
    } else if constexpr (std::is_same_v<View, ID3D11UnorderedAccessView>) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC desc {};
        view->GetDesc(&desc);
        out << '~' << static_cast<unsigned>(desc.Format);
    } else if constexpr (std::is_same_v<View, ID3D11DepthStencilView>) {
        D3D11_DEPTH_STENCIL_VIEW_DESC desc {};
        view->GetDesc(&desc);
        out << '~' << static_cast<unsigned>(desc.Format);
    }
    return out.str();
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
    if (pixel_shader) used_pixel_shaders.insert(pixel_shader.Get());
    if (compute_shader) used_compute_shaders.insert(compute_shader.Get());
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
HRESULT WINAPI HookCreatePixelShader(ID3D11Device* device, const void* bytecode,
                                     SIZE_T bytecode_size, ID3D11ClassLinkage* linkage,
                                     ID3D11PixelShader** shader) {
    const HRESULT result = original_create_pixel_shader(device, bytecode,
                                                         bytecode_size, linkage, shader);
    if (SUCCEEDED(result) && shader && *shader && bytecode && bytecode_size) {
        std::lock_guard lock(trace_mutex);
        const auto existing = pixel_shader_code.find(*shader);
        const size_t old_size = existing == pixel_shader_code.end() ? 0 :
                                existing->second.size();
        if (bytecode_size <= max_shader_bytes - (stored_shader_bytes - old_size)) {
            auto& code = pixel_shader_code[*shader];
            stored_shader_bytes -= code.size();
            code.assign(static_cast<const uint8_t*>(bytecode),
                        static_cast<const uint8_t*>(bytecode) + bytecode_size);
            stored_shader_bytes += code.size();
        }
    }
    return result;
}
HRESULT WINAPI HookCreateComputeShader(ID3D11Device* device, const void* bytecode,
                                       SIZE_T bytecode_size, ID3D11ClassLinkage* linkage,
                                       ID3D11ComputeShader** shader) {
    const HRESULT result = original_create_compute_shader(device, bytecode,
                                                           bytecode_size, linkage, shader);
    if (SUCCEEDED(result) && shader && *shader && bytecode && bytecode_size) {
        std::lock_guard lock(trace_mutex);
        const auto existing = compute_shader_code.find(*shader);
        const size_t old_size = existing == compute_shader_code.end() ? 0 :
                                existing->second.size();
        if (bytecode_size <= max_shader_bytes - (stored_shader_bytes - old_size)) {
            auto& code = compute_shader_code[*shader];
            stored_shader_bytes -= code.size();
            code.assign(static_cast<const uint8_t*>(bytecode),
                        static_cast<const uint8_t*>(bytecode) + bytecode_size);
            stored_shader_bytes += code.size();
        }
    }
    return result;
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
    void** device_vtable = *reinterpret_cast<void***>(device);
    original_create_pixel_shader =
        reinterpret_cast<CreatePixelShaderFn>(device_vtable[15]);
    original_create_compute_shader =
        reinterpret_cast<CreateComputeShaderFn>(device_vtable[18]);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(reinterpret_cast<PVOID*>(&original_draw_indexed), HookDrawIndexed);
    DetourAttach(reinterpret_cast<PVOID*>(&original_draw), HookDraw);
    DetourAttach(reinterpret_cast<PVOID*>(&original_draw_indexed_instanced), HookDrawIndexedInstanced);
    DetourAttach(reinterpret_cast<PVOID*>(&original_draw_instanced), HookDrawInstanced);
    DetourAttach(reinterpret_cast<PVOID*>(&original_dispatch), HookDispatch);
    DetourAttach(reinterpret_cast<PVOID*>(&original_create_pixel_shader), HookCreatePixelShader);
    DetourAttach(reinterpret_cast<PVOID*>(&original_create_compute_shader), HookCreateComputeShader);
    const LONG result = DetourTransactionCommit();
    if (result != NO_ERROR) {
        LOG_ERROR("D3D11 frame trace hook failed: {}", result);
        original_draw_indexed = nullptr;
        original_draw = nullptr;
        original_draw_indexed_instanced = nullptr;
        original_draw_instanced = nullptr;
        original_dispatch = nullptr;
        original_create_pixel_shader = nullptr;
        original_create_compute_shader = nullptr;
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
    const auto shader_dir = Util::DllPath().parent_path() / "OptiScaler.d3d11trace.shaders";
    std::error_code error;
    std::filesystem::create_directories(shader_dir, error);
    size_t dumped_shaders = 0;
    if (!error) {
        for (auto* shader : used_pixel_shaders) {
            const auto found = pixel_shader_code.find(shader);
            if (found == pixel_shader_code.end()) continue;
            std::ostringstream name;
            name << "ps_" << shader;
            const auto base = shader_dir / name.str();
            std::ofstream bytecode(base.string() + ".cso", std::ios::binary | std::ios::trunc);
            bytecode.write(reinterpret_cast<const char*>(found->second.data()),
                           found->second.size());
            Microsoft::WRL::ComPtr<ID3DBlob> disassembly;
            if (SUCCEEDED(D3DDisassemble(found->second.data(), found->second.size(),
                                         0, nullptr, &disassembly))) {
                std::ofstream assembly(base.string() + ".asm", std::ios::binary | std::ios::trunc);
                assembly.write(static_cast<const char*>(disassembly->GetBufferPointer()),
                               disassembly->GetBufferSize());
            }
            ++dumped_shaders;
        }
        for (auto* shader : used_compute_shaders) {
            const auto found = compute_shader_code.find(shader);
            if (found == compute_shader_code.end()) continue;
            std::ostringstream name;
            name << "cs_" << shader;
            const auto base = shader_dir / name.str();
            std::ofstream bytecode(base.string() + ".cso", std::ios::binary | std::ios::trunc);
            bytecode.write(reinterpret_cast<const char*>(found->second.data()),
                           found->second.size());
            ComPtr<ID3DBlob> disassembly;
            if (SUCCEEDED(D3DDisassemble(found->second.data(), found->second.size(),
                                         0, nullptr, &disassembly))) {
                std::ofstream assembly(base.string() + ".asm", std::ios::binary | std::ios::trunc);
                assembly.write(static_cast<const char*>(disassembly->GetBufferPointer()),
                               disassembly->GetBufferSize());
            }
            ++dumped_shaders;
        }
    }
    LOG_INFO("D3D11 frame trace: {} events, last {} saved to {}",
             event_count, events.size(), path.string());
    LOG_INFO("D3D11 frame trace: {} pixel/compute shaders dumped to {}",
             dumped_shaders, shader_dir.string());
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
    DetourDetach(reinterpret_cast<PVOID*>(&original_create_pixel_shader), HookCreatePixelShader);
    DetourDetach(reinterpret_cast<PVOID*>(&original_create_compute_shader), HookCreateComputeShader);
    DetourTransactionCommit();
    original_draw = nullptr;
    original_create_pixel_shader = nullptr;
    original_create_compute_shader = nullptr;
    traced_device = nullptr;
    traced_context = nullptr;
    pixel_shader_code.clear();
    compute_shader_code.clear();
    used_pixel_shaders.clear();
    used_compute_shaders.clear();
    stored_shader_bytes = 0;
}
}
