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
constexpr uint64_t tekken_taa_shader_fnv = 0xfb8a6b0e8667f10dull;
constexpr uint64_t tekken_taa_gameplay_shader_fnv[] = {
    0x0d3ff3622d0f27f1ull, 0x0bbef44d7962be96ull};
constexpr uint64_t tekken_battle_post_shader_fnv = 0xfaed6ba8cf89dc98ull;
bool taa_inputs_dumped = false;
std::atomic<bool> taa_input_capture_pending {false};
std::atomic<ID3D11PixelShader*> tekken_taa_shader {nullptr};
std::atomic<ID3D11PixelShader*> tekken_taa_gameplay_shaders[2] {};
thread_local bool taa_output_after_draw = false;
bool battle_inputs_dumped = false;
std::atomic<bool> battle_capture_pending {false};
std::atomic<ID3D11PixelShader*> tekken_battle_post_shader {nullptr};
std::atomic<bool> battle_present_pending {false};

uint64_t ShaderHash(const std::vector<uint8_t>& bytes) {
    uint64_t hash = 14695981039346656037ull;
    for (uint8_t byte : bytes)
        hash = (hash ^ byte) * 1099511628211ull;
    return hash;
}

void DumpConstantBuffer(ID3D11Device* device, ID3D11DeviceContext* context,
                        ID3D11Buffer* source, const std::filesystem::path& path) {
    if (!source) return;
    D3D11_BUFFER_DESC desc {};
    source->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    desc.StructureByteStride = 0;
    ComPtr<ID3D11Buffer> staging;
    if (FAILED(device->CreateBuffer(&desc, nullptr, &staging))) return;
    context->CopyResource(staging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (file) file.write(static_cast<const char*>(mapped.pData), desc.ByteWidth);
    context->Unmap(staging.Get(), 0);
}

void DumpTextureResource(ID3D11Device* device, ID3D11DeviceContext* context,
                         ID3D11Texture2D* source,
                         const std::filesystem::path& base) {
    if (!source) return;
    D3D11_TEXTURE2D_DESC desc {};
    source->GetDesc(&desc);
    if (desc.ArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1)
        return;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return;
    context->CopyResource(staging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return;
    std::ofstream file(base.string() + ".bin", std::ios::binary | std::ios::trunc);
    if (file) {
        for (UINT row = 0; row < desc.Height; ++row)
            file.write(static_cast<const char*>(mapped.pData) +
                           static_cast<size_t>(row) * mapped.RowPitch,
                       mapped.RowPitch);
    }
    context->Unmap(staging.Get(), 0);
    std::ofstream metadata(base.string() + ".txt", std::ios::trunc);
    if (metadata)
        metadata << "width=" << desc.Width << "\nheight=" << desc.Height
                 << "\nformat=" << static_cast<unsigned>(desc.Format)
                 << "\nrow_pitch=" << mapped.RowPitch << "\n";
}

void DumpTexture(ID3D11Device* device, ID3D11DeviceContext* context,
                 ID3D11ShaderResourceView* view, const std::filesystem::path& base) {
    if (!view) return;
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> source;
    if (SUCCEEDED(resource.As(&source)))
        DumpTextureResource(device, context, source.Get(), base);
}

void DumpTaaInputs(ID3D11DeviceContext* context,
                   const std::array<ID3D11ShaderResourceView*, 16>& srvs) {
    if (taa_inputs_dumped || !srvs[0] || !srvs[1] || !srvs[2] || !srvs[3])
        return;
    auto texture_desc = [](ID3D11ShaderResourceView* view,
                           D3D11_TEXTURE2D_DESC& desc) {
        ComPtr<ID3D11Resource> resource;
        view->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(resource.As(&texture))) return false;
        texture->GetDesc(&desc);
        return true;
    };
    D3D11_TEXTURE2D_DESC depth {}, color {}, history {}, motion {};
    if (!texture_desc(srvs[0], depth) || !texture_desc(srvs[1], color) ||
        !texture_desc(srvs[2], history) || !texture_desc(srvs[3], motion) ||
        color.Width < 640 || color.Height < 360 ||
        depth.Width != color.Width || depth.Height != color.Height ||
        history.Width != color.Width || history.Height != color.Height ||
        motion.Width != color.Width || motion.Height != color.Height)
        return;
    std::error_code error;
    const auto base = Util::DllPath().parent_path() / "OptiScaler.d3d11trace.inputs";
    if (!std::filesystem::exists(base.string() + ".arm", error)) return;
    taa_inputs_dumped = true;
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!device) return;
    std::array<ID3D11Buffer*, 2> buffers {};
    context->PSGetConstantBuffers(0, static_cast<UINT>(buffers.size()), buffers.data());
    for (size_t i = 0; i < buffers.size(); ++i) {
        DumpConstantBuffer(device.Get(), context, buffers[i],
                           base.string() + ".cb" + std::to_string(i) + ".bin");
        if (buffers[i]) buffers[i]->Release();
    }
    DumpTexture(device.Get(), context, srvs[0], base.string() + ".depth");
    DumpTexture(device.Get(), context, srvs[1], base.string() + ".color");
    DumpTexture(device.Get(), context, srvs[2], base.string() + ".history");
    DumpTexture(device.Get(), context, srvs[3], base.string() + ".motion");
    taa_input_capture_pending.store(false, std::memory_order_release);
    taa_output_after_draw = true;
    std::filesystem::remove(base.string() + ".arm", error);
    LOG_INFO("D3D11 TAA inputs dumped to {}", base.string());
}

void DumpBattleInputs(ID3D11DeviceContext* context,
                      const std::array<ID3D11ShaderResourceView*, 16>& srvs) {
    if (battle_inputs_dumped || !srvs[0] || !srvs[1]) return;
    ComPtr<ID3D11Resource> depth_resource, color_resource;
    srvs[0]->GetResource(&depth_resource);
    srvs[1]->GetResource(&color_resource);
    ComPtr<ID3D11Texture2D> depth, color;
    if (FAILED(depth_resource.As(&depth)) ||
        FAILED(color_resource.As(&color))) return;
    D3D11_TEXTURE2D_DESC depth_desc {}, color_desc {};
    depth->GetDesc(&depth_desc);
    color->GetDesc(&color_desc);
    if (color_desc.Width < 640 || color_desc.Height < 360 ||
        depth_desc.Width != color_desc.Width ||
        depth_desc.Height != color_desc.Height ||
        color_desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) return;
    const auto base = Util::DllPath().parent_path() /
                      "OptiScaler.d3d11trace.battle";
    std::error_code error;
    if (!std::filesystem::exists(base.string() + ".arm", error)) return;
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!device) return;
    battle_inputs_dumped = true;
    DumpTextureResource(device.Get(), context, color.Get(),
                        base.string() + ".color");
    DumpTextureResource(device.Get(), context, depth.Get(),
                        base.string() + ".depth");
    battle_capture_pending.store(false, std::memory_order_release);
    battle_present_pending.store(true, std::memory_order_release);
    std::filesystem::remove(base.string() + ".arm", error);
    LOG_INFO("D3D11 battle HDR/depth dumped to {}", base.string());
}

void DumpTaaOutput(ID3D11DeviceContext* context) {
    if (!taa_output_after_draw) return;
    taa_output_after_draw = false;
    std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> views {};
    ID3D11DepthStencilView* depth = nullptr;
    context->OMGetRenderTargets(static_cast<UINT>(views.size()),
                                views.data(), &depth);
    if (!views[0]) {
        for (auto* view : views) if (view) view->Release();
        if (depth) depth->Release();
        return;
    }
    ComPtr<ID3D11Resource> resource;
    views[0]->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (SUCCEEDED(resource.As(&texture))) {
        ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        const auto base = Util::DllPath().parent_path() /
                          "OptiScaler.d3d11trace.inputs.taa_output";
        context->OMSetRenderTargets(0, nullptr, nullptr);
        DumpTextureResource(device.Get(), context, texture.Get(), base);
        context->OMSetRenderTargets(static_cast<UINT>(views.size()),
                                    views.data(), depth);
        LOG_INFO("D3D11 TAA output dumped to {}", base.string());
    }
    for (auto* view : views) if (view) view->Release();
    if (depth) depth->Release();
}

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
    if (context != traced_context) return;
    const bool record_frame = capturing.load(std::memory_order_relaxed);
    if (!record_frame) {
        // The requested TAA pass can appear later than the one frame trace.
        // Keep watching only its shader until the input dump succeeds.
        if ((!taa_input_capture_pending.load(std::memory_order_acquire) &&
             !battle_capture_pending.load(std::memory_order_acquire)) ||
            operation[0] != 'D') return;
        ComPtr<ID3D11PixelShader> shader;
        context->PSGetShader(&shader, nullptr, nullptr);
        const bool taa = shader &&
            taa_input_capture_pending.load(std::memory_order_acquire) &&
            (shader.Get() == tekken_taa_shader.load(std::memory_order_acquire) ||
             shader.Get() == tekken_taa_gameplay_shaders[0].load(std::memory_order_acquire) ||
             shader.Get() == tekken_taa_gameplay_shaders[1].load(std::memory_order_acquire));
        const bool battle = shader &&
            battle_capture_pending.load(std::memory_order_acquire) &&
            shader.Get() == tekken_battle_post_shader.load(std::memory_order_acquire);
        if (!taa && !battle)
            return;
        std::array<ID3D11ShaderResourceView*, 16> inputs {};
        context->PSGetShaderResources(0, static_cast<UINT>(inputs.size()),
                                      inputs.data());
        if (taa) DumpTaaInputs(context, inputs);
        if (battle) DumpBattleInputs(context, inputs);
        for (auto* view : inputs)
            if (view) view->Release();
        return;
    }
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
        if (pixel_shader && operation[0] == 'D') {
            std::lock_guard lock(trace_mutex);
            const auto it = pixel_shader_code.find(pixel_shader.Get());
            if (it != pixel_shader_code.end() &&
                ShaderHash(it->second) == tekken_taa_shader_fnv)
                DumpTaaInputs(context, srvs);
        }
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
    DumpTaaOutput(context);
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
        if (bytecode_size == 6332) {
            std::vector<uint8_t> candidate(
                static_cast<const uint8_t*>(bytecode),
                static_cast<const uint8_t*>(bytecode) + bytecode_size);
            if (ShaderHash(candidate) == tekken_taa_shader_fnv)
                tekken_taa_shader.store(*shader, std::memory_order_release);
        }
        if (bytecode_size == 6348) {
            std::vector<uint8_t> candidate(
                static_cast<const uint8_t*>(bytecode),
                static_cast<const uint8_t*>(bytecode) + bytecode_size);
            const auto hash = ShaderHash(candidate);
            for (size_t i = 0; i < 2; ++i)
                if (hash == tekken_taa_gameplay_shader_fnv[i])
                    tekken_taa_gameplay_shaders[i].store(
                        *shader, std::memory_order_release);
        }
        if (bytecode_size == 1576) {
            std::vector<uint8_t> candidate(
                static_cast<const uint8_t*>(bytecode),
                static_cast<const uint8_t*>(bytecode) + bytecode_size);
            if (ShaderHash(candidate) == tekken_battle_post_shader_fnv)
                tekken_battle_post_shader.store(*shader, std::memory_order_release);
        }
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
    taa_input_capture_pending.store(
        std::filesystem::exists(Util::DllPath().parent_path() /
                                "OptiScaler.d3d11trace.inputs.arm", error),
        std::memory_order_release);
    battle_capture_pending.store(
        std::filesystem::exists(Util::DllPath().parent_path() /
                                "OptiScaler.d3d11trace.battle.arm", error),
        std::memory_order_release);
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

void BeforePresent(IDXGISwapChain* swapchain, ID3D11Device* device) {
    if (!original_draw || !device) return;
    if (!traced_device) {
        ComPtr<ID3D11DeviceContext> context;
        device->GetImmediateContext(&context);
        if (!context) return;
        traced_device = device;
        traced_context = context.Get();
    }
    if (device != traced_device) return;
    if (battle_present_pending.exchange(false, std::memory_order_acq_rel) &&
        swapchain) {
        ComPtr<ID3D11Texture2D> backbuffer;
        if (SUCCEEDED(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) {
            ComPtr<ID3D11DeviceContext> context;
            device->GetImmediateContext(&context);
            DumpTextureResource(device, context.Get(), backbuffer.Get(),
                Util::DllPath().parent_path() /
                    "OptiScaler.d3d11trace.battle.present");
        }
    }
    if (!battle_capture_pending.load(std::memory_order_acquire)) {
        std::error_code arm_error;
        if (std::filesystem::exists(Util::DllPath().parent_path() /
                                    "OptiScaler.d3d11trace.battle.arm", arm_error)) {
            battle_inputs_dumped = false;
            battle_capture_pending.store(true, std::memory_order_release);
            LOG_INFO("D3D11 battle HDR/depth capture armed");
        }
    }
    if (!taa_input_capture_pending.load(std::memory_order_acquire)) {
        std::error_code arm_error;
        if (std::filesystem::exists(Util::DllPath().parent_path() /
                                    "OptiScaler.d3d11trace.inputs.arm", arm_error)) {
            taa_inputs_dumped = false;
            taa_input_capture_pending.store(true, std::memory_order_release);
            LOG_INFO("D3D11 TAA input capture armed for the next full-size pass");
        }
    }
    if (present_count >= 2) return;
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
    taa_input_capture_pending.store(false, std::memory_order_release);
    tekken_taa_shader.store(nullptr, std::memory_order_release);
    for (auto& shader : tekken_taa_gameplay_shaders)
        shader.store(nullptr, std::memory_order_release);
    battle_capture_pending.store(false, std::memory_order_release);
    tekken_battle_post_shader.store(nullptr, std::memory_order_release);
    battle_present_pending.store(false, std::memory_order_release);
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
