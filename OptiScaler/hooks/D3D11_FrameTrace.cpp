#include <pch.h>
#include "D3D11_FrameTrace.h"
#include "Hook_Utils.h"
#include <dlssnr/sycl/TekkenSNRXeSS_Dx11.h>
#include <dlssnr/sycl/ValheimNr_Dx11.h>
#include <Util.h>
#include <detours/detours.h>
#include <d3dcompiler.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <iomanip>
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
using MapFn = rewrite_signature<decltype(&ID3D11DeviceContext::Map)>::type;
using UnmapFn = rewrite_signature<decltype(&ID3D11DeviceContext::Unmap)>::type;
using DrawInstancedFn = rewrite_signature<decltype(&ID3D11DeviceContext::DrawInstanced)>::type;
using DrawIndexedInstancedFn = rewrite_signature<decltype(&ID3D11DeviceContext::DrawIndexedInstanced)>::type;
using DispatchFn = rewrite_signature<decltype(&ID3D11DeviceContext::Dispatch)>::type;
using CreatePixelShaderFn = rewrite_signature<decltype(&ID3D11Device::CreatePixelShader)>::type;
using CreateComputeShaderFn = rewrite_signature<decltype(&ID3D11Device::CreateComputeShader)>::type;
using CreateVertexShaderFn = rewrite_signature<decltype(&ID3D11Device::CreateVertexShader)>::type;
DrawFn original_draw = nullptr;
DrawIndexedFn original_draw_indexed = nullptr;
MapFn original_map = nullptr;
UnmapFn original_unmap = nullptr;
DrawInstancedFn original_draw_instanced = nullptr;
DrawIndexedInstancedFn original_draw_indexed_instanced = nullptr;
DispatchFn original_dispatch = nullptr;
CreatePixelShaderFn original_create_pixel_shader = nullptr;
CreateComputeShaderFn original_create_compute_shader = nullptr;
CreateVertexShaderFn original_create_vertex_shader = nullptr;
bool hdr_inspect = false;
bool hdr_inspected = false;
bool hdr_capture_pending = false;
bool hdr_capture_present = false;
ComPtr<ID3D11Resource> hdr_capture_target;
unsigned hdr_sequence_remaining = 0;
constexpr GUID hdr_vertex_code_guid = {0xb44be81e, 0x9e37, 0x452d, {0xab,0xef,0x19,0xee,0xd8,0x2b,0x61,0x58}};
constexpr GUID hdr_vertex_hash_guid = {0x6de3dd58, 0x7c69, 0x4fa1, {0x8a,0x32,0x68,0x1b,0xb9,0x73,0xd0,0x8a}};
constexpr GUID shader_role_guid = {0x2dbb76d9, 0xabec, 0x41d8, {0xb6,0x91,0x33,0x17,0x8d,0xad,0xa1,0xf4}};
enum class ShaderRole : UINT { Taa = 1, GameplayTaa, Battle, Upscale, ToneMap, ColorGrade };
bool HasRole(ID3D11PixelShader* shader, ShaderRole expected) {
    ShaderRole role {}; UINT size = sizeof(role);
    return shader && SUCCEEDED(shader->GetPrivateData(shader_role_guid, &size, &role)) && role == expected;
}
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
constexpr uint64_t tekken_screen50_upscale_shader_fnv = 0x57b422a03e63abb6ull;
constexpr uint64_t tekken_tonemap_shader_fnv = 0xae69caf5eb29a60bull;
constexpr uint64_t tekken_colorgrade_shader_fnv = 0x260885badbf2631aull;
std::atomic<ID3D11PixelShader*> tekken_tonemap_shader {nullptr};
std::atomic<ID3D11PixelShader*> tekken_colorgrade_shader {nullptr};
bool taa_inputs_dumped = false;
bool tekken_snr_xess_enabled = false;
bool tekken_native_snr_xess_enabled = false;
std::atomic<bool> taa_input_capture_pending {false};
std::atomic<ID3D11PixelShader*> tekken_taa_shader {nullptr};
std::atomic<ID3D11PixelShader*> tekken_taa_gameplay_shaders[2] {};
thread_local bool taa_output_after_draw = false;
bool battle_inputs_dumped = false;
std::atomic<bool> battle_capture_pending {false};
std::atomic<ID3D11PixelShader*> tekken_battle_post_shader {nullptr};
std::atomic<bool> battle_present_pending {false};
std::atomic<bool> final_upscale_capture_pending {false};
std::atomic<ID3D11PixelShader*> tekken_screen50_upscale_shader {nullptr};
bool jitter_capture_pending = false;
unsigned jitter_captured_frames = 0;
uint64_t jitter_last_present = ~uint64_t {0};
std::atomic<uint64_t> present_serial {0};
std::atomic<UINT> display_width {0};
std::atomic<UINT> display_height {0};
struct JitterConstant {
    float x = 0.0f;
    float y = 0.0f;
    uint64_t serial = 0;
};
std::mutex jitter_mutex;
std::unordered_map<ID3D11Resource*, const uint8_t*> mapped_jitter_buffers;
std::unordered_map<ID3D11Resource*, JitterConstant> jitter_values;

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

void DumpJitterConstants(ID3D11DeviceContext* context) {
    if (!jitter_capture_pending || jitter_last_present == present_serial)
        return;
    jitter_last_present = present_serial;
    ComPtr<ID3D11Buffer> constants[2];
    ID3D11Buffer* raw[2] {};
    context->PSGetConstantBuffers(0, 2, raw);
    constants[0].Attach(raw[0]);
    constants[1].Attach(raw[1]);
    if (!constants[0] || !constants[1]) return;
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!device) return;
    std::ostringstream name;
    name << "OptiScaler.d3d11trace.jitter." << std::setfill('0')
         << std::setw(2) << jitter_captured_frames;
    const auto base = Util::DllPath().parent_path() / name.str();
    DumpConstantBuffer(device.Get(), context, constants[0].Get(),
                       base.string() + ".cb0.bin");
    DumpConstantBuffer(device.Get(), context, constants[1].Get(),
                       base.string() + ".cb1.bin");
    if (++jitter_captured_frames >= 16) {
        jitter_capture_pending = false;
        std::error_code error;
        std::filesystem::remove(Util::DllPath().parent_path() /
                                "OptiScaler.d3d11trace.jitter.arm", error);
        LOG_INFO("D3D11 TAA constants captured for 16 consecutive frames");
    }
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

// One-shot diagnostics only. The live path never reads GPU exposure back to the CPU.
void InspectHdrVertex(ID3D11DeviceContext* context) {
    if (!hdr_inspect || hdr_inspected) return;
    ComPtr<ID3D11VertexShader> vertex;
    context->VSGetShader(&vertex, nullptr, nullptr);
    if (!vertex) return;
    UINT size = 0;
    vertex->GetPrivateData(hdr_vertex_code_guid, &size, nullptr);
    if (!size || size > 16384) return;
    std::vector<uint8_t> code(size);
    if (FAILED(vertex->GetPrivateData(hdr_vertex_code_guid, &size, code.data()))) return;
    hdr_inspected = true;
    const auto base = (Util::DllPath().parent_path() / "OptiScaler.hdr-vertex").string();
    std::ofstream binary(base + ".cso", std::ios::binary);
    binary.write(reinterpret_cast<const char*>(code.data()), code.size());
    ComPtr<ID3DBlob> assembly;
    if (SUCCEEDED(D3DDisassemble(code.data(), code.size(), 0, nullptr, &assembly))) {
        std::ofstream text(base + ".asm");
        text.write(static_cast<const char*>(assembly->GetBufferPointer()), assembly->GetBufferSize());
    }
    ComPtr<ID3D11Device> device; context->GetDevice(&device);
    for (UINT i = 0; i < 8; ++i) {
        ComPtr<ID3D11Buffer> buffer;
        context->VSGetConstantBuffers(i, 1, &buffer);
        DumpConstantBuffer(device.Get(), context, buffer.Get(), base + ".cb" + std::to_string(i) + ".bin");
        ComPtr<ID3D11ShaderResourceView> view;
        context->VSGetShaderResources(i, 1, &view);
        DumpTexture(device.Get(), context, view.Get(), base + ".t" + std::to_string(i));
    }
    LOG_INFO("Tekken HDR tonemap vertex shader/exposure captured: {}", base);
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

void DumpFinalUpscaleInput(ID3D11DeviceContext* context) {
    if (!final_upscale_capture_pending.load(std::memory_order_acquire)) return;
    std::error_code error;
    const auto base = Util::DllPath().parent_path() /
        "OptiScaler.d3d11trace.upscale";
    if (!std::filesystem::exists(base.string() + ".arm", error)) return;
    ComPtr<ID3D11ShaderResourceView> input;
    context->PSGetShaderResources(0, 1, input.GetAddressOf());
    if (!input) return;
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!device) return;
    DumpTexture(device.Get(), context, input.Get(), base.string() + ".color");
    final_upscale_capture_pending.store(false, std::memory_order_release);
    std::filesystem::remove(base.string() + ".arm", error);
    LOG_INFO("D3D11 final upscale input dumped to {}", base.string());
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
HRESULT WINAPI HookMap(ID3D11DeviceContext* context, ID3D11Resource* resource,
                       UINT subresource, D3D11_MAP map_type, UINT map_flags,
                       D3D11_MAPPED_SUBRESOURCE* mapped) {
    const HRESULT result = original_map(context, resource, subresource,
                                        map_type, map_flags, mapped);
    if (FAILED(result) || !tekken_native_snr_xess_enabled ||
        !resource || !mapped || !mapped->pData ||
        context != traced_context || subresource != 0) return result;
    D3D11_RESOURCE_DIMENSION dimension {};
    resource->GetType(&dimension);
    if (dimension != D3D11_RESOURCE_DIMENSION_BUFFER) return result;
    ComPtr<ID3D11Buffer> buffer;
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&buffer)))) return result;
    D3D11_BUFFER_DESC desc {};
    buffer->GetDesc(&desc);
    if (desc.ByteWidth != 4096 ||
        (desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) == 0) return result;
    std::lock_guard lock(jitter_mutex);
    mapped_jitter_buffers[resource] = static_cast<const uint8_t*>(mapped->pData);
    return result;
}
void WINAPI HookUnmap(ID3D11DeviceContext* context, ID3D11Resource* resource,
                      UINT subresource) {
    if (tekken_native_snr_xess_enabled && context == traced_context &&
        resource && subresource == 0) {
        std::lock_guard lock(jitter_mutex);
        const auto found = mapped_jitter_buffers.find(resource);
        if (found != mapped_jitter_buffers.end()) {
            constexpr size_t offset = 133 * 4 * sizeof(float);
            float sample[4] {};
            std::memcpy(sample, found->second + offset, sizeof(sample));
            const float x = sample[2], y = sample[3];
            const float count = sample[1];
            const bool supported_count = count == 4.0f || count == 8.0f ||
                count == 16.0f || count == 32.0f || count == 64.0f;
            if (std::isfinite(x) && std::isfinite(y) &&
                supported_count && sample[0] >= 0.0f && sample[0] < count &&
                sample[0] == std::floor(sample[0]) &&
                std::abs(x) < 0.75f && std::abs(y) < 0.75f) {
                if (jitter_values.size() > 128) jitter_values.clear();
                jitter_values[resource] = {x, y, present_serial.load()};
            }
            mapped_jitter_buffers.erase(found);
        }
    }
    original_unmap(context, resource, subresource);
}
bool NativeJitter(ID3D11DeviceContext* context, float& x, float& y) {
    ComPtr<ID3D11Buffer> constants;
    context->PSGetConstantBuffers(1, 1, &constants);
    if (!constants) return false;
    std::lock_guard lock(jitter_mutex);
    const auto found = jitter_values.find(
        static_cast<ID3D11Resource*>(constants.Get()));
    if (found == jitter_values.end() ||
        found->second.serial != present_serial.load()) return false;
    x = found->second.x;
    y = found->second.y;
    return true;
}
void WINAPI HookDrawIndexed(ID3D11DeviceContext* context, UINT index_count, UINT start_index, INT base_vertex) {
    Record(context, "DrawIndexed");
    if (context == traced_context &&
        (final_upscale_capture_pending.load(std::memory_order_acquire) ||
         tekken_native_snr_xess_enabled)) {
        ComPtr<ID3D11PixelShader> shader;
        context->PSGetShader(&shader, nullptr, nullptr);
        if (HasRole(shader.Get(), ShaderRole::Upscale)) {
            if (final_upscale_capture_pending.load(std::memory_order_acquire))
                DumpFinalUpscaleInput(context);
            if (tekken_native_snr_xess_enabled) {
                ComPtr<ID3D11PixelShader> replacement;
                ComPtr<ID3D11ShaderResourceView> output;
                if (TekkenSNRXeSSDx11::TryPrepareNativeUpscale(
                        context, replacement, output)) {
                    ComPtr<ID3D11ShaderResourceView> previous_color;
                    context->PSGetShaderResources(0, 1, &previous_color);
                    const auto capture = hdr_capture_pending ? TekkenSNRXeSSDx11::CaptureInputs() : TekkenSNRXeSSDx11::NativeCapture {};
                    if (hdr_capture_pending && capture.history_frames >= 30) {
                        ComPtr<ID3D11Device> device; context->GetDevice(&device);
                        const auto base = (Util::DllPath().parent_path() / "OptiScaler.hdr-capture").string();
                        const std::pair<const char*, ID3D11Texture2D*> images[] = {
                            {"hdr",capture.hdr.Get()}, {"taa",capture.taa.Get()}, {"proxy",capture.proxy.Get()},
                            {"neural",capture.neural.Get()}, {"resolved",capture.resolved.Get()},
                            {"motion",capture.motion.Get()}, {"neural-motion",capture.neural_motion.Get()}, {"depth",capture.depth.Get()},
                            {"xess",capture.xess.Get()}, {"exposure",capture.exposure.Get()},
                            {"neural-input",capture.neural_input.Get()}};
                        for (const auto& item : images)
                            DumpTextureResource(device.Get(), context, item.second, base + "." + item.first);
                        DumpTexture(device.Get(), context, previous_color.Get(), base + ".original-ldr");
                        DumpTexture(device.Get(), context, output.Get(), base + ".processed-ldr");
                        std::error_code capture_error;
                        std::filesystem::remove(Util::DllPath().parent_path() / "OptiScaler.hdr-capture.arm", capture_error);
                        hdr_capture_pending = false;
                        hdr_capture_present = true;
                        ComPtr<ID3D11RenderTargetView> capture_target;
                        context->OMGetRenderTargets(1, &capture_target, nullptr);
                        if (capture_target) capture_target->GetResource(&hdr_capture_target);
                        LOG_INFO("Tekken same-frame HDR/NR/XeSS/postprocess capture complete: {}", base);
                    }
                    ID3D11ShaderResourceView* output_view = output.Get();
                    context->PSSetShader(replacement.Get(), nullptr, 0);
                    context->PSSetShaderResources(0, 1, &output_view);
                    original_draw_indexed(context, index_count, start_index,
                                          base_vertex);
                    ID3D11ShaderResourceView* previous_view = previous_color.Get();
                    context->PSSetShaderResources(0, 1, &previous_view);
                    context->PSSetShader(shader.Get(), nullptr, 0);
                    ComPtr<ID3D11RenderTargetView> target_view;
                    context->OMGetRenderTargets(1, &target_view, nullptr);
                    if (target_view) {
                        ComPtr<ID3D11Resource> target;
                        target_view->GetResource(&target);
                        ValheimNr_Dx11::MarkPreXeSSOutput(context, target.Get());
                    }
                    return;
                }
            }
        }
    }
    if ((tekken_snr_xess_enabled || tekken_native_snr_xess_enabled ||
         jitter_capture_pending) &&
        context == traced_context) {
        ComPtr<ID3D11PixelShader> shader;
        context->PSGetShader(&shader, nullptr, nullptr);
        const bool gameplay_taa = HasRole(shader.Get(), ShaderRole::GameplayTaa);
        if (gameplay_taa) DumpJitterConstants(context);
        float jitter_x = 0.0f, jitter_y = 0.0f;
        const bool jitter_available = !gameplay_taa ||
            !tekken_native_snr_xess_enabled ||
            NativeJitter(context, jitter_x, jitter_y);
        if (gameplay_taa && tekken_native_snr_xess_enabled &&
            !jitter_available) {
            static unsigned missing_jitter = 0;
            if (++missing_jitter == 1 || missing_jitter % 120 == 0)
                LOG_WARN("Tekken native XeSS: current TAA jitter is unavailable; original draw retained");
        }
        if (gameplay_taa && tekken_snr_xess_enabled && jitter_available &&
            (tekken_native_snr_xess_enabled
                 ? TekkenSNRXeSSDx11::TryProcessNativeTaaDraw(
                       context, jitter_x, jitter_y,
                       display_width.load(std::memory_order_acquire),
                       display_height.load(std::memory_order_acquire))
                 : TekkenSNRXeSSDx11::TryReplaceTaaDraw(context))) {
            DumpTaaOutput(context);
            return;
        }
    }
    if (tekken_native_snr_xess_enabled && context == traced_context &&
        TekkenSNRXeSSDx11::NativeSnrEnabled()) {
        ComPtr<ID3D11PixelShader> shader;
        context->PSGetShader(&shader, nullptr, nullptr);
        if (HasRole(shader.Get(), ShaderRole::ToneMap)) {
            ComPtr<ID3D11VertexShader> vertex;
            context->VSGetShader(&vertex, nullptr, nullptr);
            uint64_t hash = 0; UINT bytes = sizeof(hash);
            ComPtr<ID3D11ShaderResourceView> exposure, previous, replacement;
            if (vertex && SUCCEEDED(vertex->GetPrivateData(hdr_vertex_hash_guid, &bytes, &hash)) &&
                hash == 0xfa1f4bec42d7668aull)
                context->VSGetShaderResources(0, 1, &exposure);
            context->PSGetShaderResources(1, 1, &previous);
            if (TekkenSNRXeSSDx11::TryPrepareNativeTonemap(context, exposure.Get(), replacement)) {
                // Keep the original shader, output target, grading, viewport,
                // constants and UI. Substitute only its scene-linear HDR input.
                ID3D11ShaderResourceView* input = replacement.Get();
                context->PSSetShaderResources(1, 1, &input);
                original_draw_indexed(context, index_count, start_index, base_vertex);
                input = previous.Get(); context->PSSetShaderResources(1, 1, &input);
                TekkenSNRXeSSDx11::CompleteNativeTonemap(context);
                const auto capture = hdr_capture_pending ? TekkenSNRXeSSDx11::CaptureInputs() : TekkenSNRXeSSDx11::NativeCapture {};
                if (hdr_capture_pending && capture.history_frames >= 30) {
                    ComPtr<ID3D11Device> device; context->GetDevice(&device);
                    const auto base = (Util::DllPath().parent_path() / "OptiScaler.hdr-capture").string();
                    const std::pair<const char*, ID3D11Texture2D*> images[] = {
                        {"hdr",capture.hdr.Get()}, {"taa",capture.taa.Get()}, {"proxy",capture.proxy.Get()},
                        {"neural",capture.neural.Get()}, {"resolved",capture.resolved.Get()},
                        {"motion",capture.motion.Get()}, {"neural-motion",capture.neural_motion.Get()},
                        {"depth",capture.depth.Get()}, {"exposure",capture.exposure.Get()},
                        {"neural-input",capture.neural_input.Get()}};
                    for (const auto& item : images) DumpTextureResource(device.Get(), context, item.second, base + "." + item.first);
                    ComPtr<ID3D11RenderTargetView> target;
                    ComPtr<ID3D11Resource> resource;
                    ComPtr<ID3D11Texture2D> texture;
                    context->OMGetRenderTargets(1, &target, nullptr);
                    if (target) target->GetResource(&resource);
                    if (resource && SUCCEEDED(resource.As(&texture)))
                        DumpTextureResource(device.Get(), context, texture.Get(), base + ".processed-ldr");
                    std::error_code error;
                    std::filesystem::remove(Util::DllPath().parent_path() / "OptiScaler.hdr-capture.arm", error);
                    hdr_capture_pending = false; hdr_capture_present = true;
                    hdr_capture_target = resource;
                    LOG_INFO("Tekken native TAA->SNR input/postprocess capture complete (no XeSS): {}", base);
                }
                return;
            }
        }
    }
    original_draw_indexed(context, index_count, start_index, base_vertex);
    if (tekken_native_snr_xess_enabled && context == traced_context &&
        TekkenSNRXeSSDx11::NativeHdrEnabled()) {
        ComPtr<ID3D11PixelShader> shader;
        context->PSGetShader(&shader, nullptr, nullptr);
        if (HasRole(shader.Get(), ShaderRole::ToneMap)) {
            InspectHdrVertex(context);
            // This exact vertex shader passes t0[0,0].r straight to PS exposure.
            ComPtr<ID3D11VertexShader> vertex;
            context->VSGetShader(&vertex, nullptr, nullptr);
            uint64_t hash = 0; UINT bytes = sizeof(hash);
            ComPtr<ID3D11ShaderResourceView> exposure;
            if (vertex && SUCCEEDED(vertex->GetPrivateData(hdr_vertex_hash_guid, &bytes, &hash)) &&
                hash == 0xfa1f4bec42d7668aull)
                context->VSGetShaderResources(0, 1, &exposure);
            if (exposure)
                TekkenSNRXeSSDx11::ReplayNativeHdrPostProcess(context,
                    TekkenHdrPostProcessDx11::Pass::ToneMap, original_draw_indexed,
                    index_count, start_index, base_vertex, exposure.Get());
        }
        else if (HasRole(shader.Get(), ShaderRole::ColorGrade))
            TekkenSNRXeSSDx11::ReplayNativeHdrPostProcess(context,
                TekkenHdrPostProcessDx11::Pass::ColorGrade, original_draw_indexed,
                index_count, start_index, base_vertex);
    }
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
        // The engine can create multiple instances of identical bytecode.
        // Attach identity to the object; a single last-seen pointer misses
        // earlier instances and can misidentify a subsequently reused address.
        if (bytecode_size == 1932 || bytecode_size == 2736 || bytecode_size == 6348 ||
            bytecode_size == 6332 || bytecode_size == 1576 || bytecode_size == 760) {
            const auto* bytes = static_cast<const uint8_t*>(bytecode);
            const auto hash = ShaderHash(std::vector<uint8_t>(bytes, bytes + bytecode_size));
            ShaderRole role {};
            if (hash == tekken_tonemap_shader_fnv) role = ShaderRole::ToneMap;
            else if (hash == tekken_colorgrade_shader_fnv) role = ShaderRole::ColorGrade;
            else if (hash == tekken_screen50_upscale_shader_fnv) role = ShaderRole::Upscale;
            else if (hash == tekken_taa_gameplay_shader_fnv[0] || hash == tekken_taa_gameplay_shader_fnv[1]) role = ShaderRole::GameplayTaa;
            else if (hash == tekken_taa_shader_fnv) role = ShaderRole::Taa;
            else if (hash == tekken_battle_post_shader_fnv) role = ShaderRole::Battle;
            if (UINT(role)) (*shader)->SetPrivateData(shader_role_guid, sizeof(role), &role);
        }
        if (bytecode_size == 1932 || bytecode_size == 2736) {
            std::vector<uint8_t> candidate(static_cast<const uint8_t*>(bytecode),
                static_cast<const uint8_t*>(bytecode) + bytecode_size);
            const auto hash = ShaderHash(candidate);
            if (hash == tekken_tonemap_shader_fnv)
                tekken_tonemap_shader.store(*shader, std::memory_order_release);
            if (hash == tekken_colorgrade_shader_fnv)
                tekken_colorgrade_shader.store(*shader, std::memory_order_release);
        }
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
        if (bytecode_size == 760) {
            std::vector<uint8_t> candidate(
                static_cast<const uint8_t*>(bytecode),
                static_cast<const uint8_t*>(bytecode) + bytecode_size);
            if (ShaderHash(candidate) == tekken_screen50_upscale_shader_fnv)
                tekken_screen50_upscale_shader.store(
                    *shader, std::memory_order_release);
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
HRESULT WINAPI HookCreateVertexShader(ID3D11Device* device, const void* bytecode,
                                      SIZE_T size, ID3D11ClassLinkage* linkage,
                                      ID3D11VertexShader** shader) {
    const HRESULT hr = original_create_vertex_shader(device, bytecode, size, linkage, shader);
    if (hdr_inspect && SUCCEEDED(hr) && shader && *shader && bytecode && size <= 16384)
        (*shader)->SetPrivateData(hdr_vertex_code_guid, static_cast<UINT>(size), bytecode);
    if (tekken_native_snr_xess_enabled && SUCCEEDED(hr) && shader && *shader && bytecode && size == 1388) {
        const auto* data = static_cast<const uint8_t*>(bytecode);
        const auto hash = ShaderHash(std::vector<uint8_t>(data, data + size));
        (*shader)->SetPrivateData(hdr_vertex_hash_guid, sizeof(hash), &hash);
    }
    return hr;
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
    const auto game = Util::ExePath().filename();
    tekken_snr_xess_enabled =
        _wcsicmp(game.c_str(), L"TekkenGame-Win64-Shipping.exe") == 0 &&
        std::filesystem::exists(Util::DllPath().parent_path() /
                                "OptiScaler.snr-xess.enable", error);
    tekken_native_snr_xess_enabled = tekken_snr_xess_enabled &&
        std::filesystem::exists(Util::DllPath().parent_path() /
                                "OptiScaler.snr-xess.native.enable", error);
    if (!tekken_snr_xess_enabled &&
        !std::filesystem::exists(Util::DllPath().parent_path() /
                                 "OptiScaler.d3d11trace", error)) return;
    hdr_inspect = std::filesystem::exists(Util::DllPath().parent_path() /
                                          "OptiScaler.snr-xess.hdr.inspect", error);
    hdr_inspected = false;
    taa_input_capture_pending.store(
        std::filesystem::exists(Util::DllPath().parent_path() /
                                "OptiScaler.d3d11trace.inputs.arm", error),
        std::memory_order_release);
    battle_capture_pending.store(
        std::filesystem::exists(Util::DllPath().parent_path() /
                                "OptiScaler.d3d11trace.battle.arm", error),
        std::memory_order_release);
    final_upscale_capture_pending.store(
        std::filesystem::exists(Util::DllPath().parent_path() /
                                "OptiScaler.d3d11trace.upscale.arm", error),
        std::memory_order_release);
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!context) return;
    void** vtable = *reinterpret_cast<void***>(context.Get());
    original_draw_indexed = reinterpret_cast<DrawIndexedFn>(vtable[12]);
    original_draw = reinterpret_cast<DrawFn>(vtable[13]);
    if (tekken_native_snr_xess_enabled) {
        original_map = reinterpret_cast<MapFn>(vtable[14]);
        original_unmap = reinterpret_cast<UnmapFn>(vtable[15]);
    }
    original_draw_indexed_instanced = reinterpret_cast<DrawIndexedInstancedFn>(vtable[20]);
    original_draw_instanced = reinterpret_cast<DrawInstancedFn>(vtable[21]);
    // ID3D11DeviceContext slot 37 is SOSetTargets; Dispatch is slot 41.
    original_dispatch = reinterpret_cast<DispatchFn>(vtable[41]);
    void** device_vtable = *reinterpret_cast<void***>(device);
    original_create_vertex_shader = reinterpret_cast<CreateVertexShaderFn>(device_vtable[12]);
    original_create_pixel_shader =
        reinterpret_cast<CreatePixelShaderFn>(device_vtable[15]);
    original_create_compute_shader =
        reinterpret_cast<CreateComputeShaderFn>(device_vtable[18]);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(reinterpret_cast<PVOID*>(&original_draw_indexed), HookDrawIndexed);
    DetourAttach(reinterpret_cast<PVOID*>(&original_draw), HookDraw);
    if (original_map && original_unmap) {
        DetourAttach(reinterpret_cast<PVOID*>(&original_map), HookMap);
        DetourAttach(reinterpret_cast<PVOID*>(&original_unmap), HookUnmap);
    }
    DetourAttach(reinterpret_cast<PVOID*>(&original_draw_indexed_instanced), HookDrawIndexedInstanced);
    DetourAttach(reinterpret_cast<PVOID*>(&original_draw_instanced), HookDrawInstanced);
    DetourAttach(reinterpret_cast<PVOID*>(&original_dispatch), HookDispatch);
    DetourAttach(reinterpret_cast<PVOID*>(&original_create_pixel_shader), HookCreatePixelShader);
    DetourAttach(reinterpret_cast<PVOID*>(&original_create_vertex_shader), HookCreateVertexShader);
    DetourAttach(reinterpret_cast<PVOID*>(&original_create_compute_shader), HookCreateComputeShader);
    const LONG result = DetourTransactionCommit();
    if (result != NO_ERROR) {
        LOG_ERROR("D3D11 frame trace hook failed: {}", result);
        original_draw_indexed = nullptr;
        original_draw = nullptr;
        original_map = nullptr;
        original_unmap = nullptr;
        original_draw_indexed_instanced = nullptr;
        original_draw_instanced = nullptr;
        original_dispatch = nullptr;
        original_create_pixel_shader = nullptr;
        original_create_vertex_shader = nullptr;
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
    ++present_serial;
    // Explicit diagnostic only: 16 consecutive displayed frames, never enabled
    // by the normal rendering path. Readback makes this unsuitable for timing.
    if (tekken_native_snr_xess_enabled && swapchain) {
        const auto folder = Util::DllPath().parent_path();
        std::error_code sequence_error;
        if (!hdr_sequence_remaining && std::filesystem::exists(folder / "OptiScaler.hdr-sequence.arm", sequence_error)) {
            std::filesystem::remove(folder / "OptiScaler.hdr-sequence.arm", sequence_error);
            hdr_sequence_remaining = 16;
        }
        if (hdr_sequence_remaining) {
            ComPtr<ID3D11Texture2D> backbuffer;
            if (SUCCEEDED(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) {
                ComPtr<ID3D11DeviceContext> immediate;
                device->GetImmediateContext(&immediate);
                std::ostringstream name;
                name << "OptiScaler.hdr-sequence." << std::setw(2) << std::setfill('0') << (16 - hdr_sequence_remaining);
                DumpTextureResource(device, immediate.Get(), backbuffer.Get(), folder / name.str());
            }
            if (!--hdr_sequence_remaining) LOG_INFO("Tekken displayed HDR quality sequence complete: 16 frames");
        }
    }
    if (hdr_capture_present && swapchain) {
        hdr_capture_present = false;
        ComPtr<ID3D11Texture2D> backbuffer;
        if (SUCCEEDED(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) {
            ComPtr<ID3D11DeviceContext> immediate;
            device->GetImmediateContext(&immediate);
            const auto base = Util::DllPath().parent_path() / "OptiScaler.hdr-capture.present";
            DumpTextureResource(device, immediate.Get(), backbuffer.Get(), base);
            LOG_INFO("Tekken HDR capture target matches presented backbuffer: {}", hdr_capture_target.Get() == backbuffer.Get());
        }
        hdr_capture_target.Reset();
    }
    if (tekken_native_snr_xess_enabled && !hdr_capture_pending) {
        std::error_code capture_error;
        hdr_capture_pending = std::filesystem::exists(Util::DllPath().parent_path() / "OptiScaler.hdr-capture.arm", capture_error);
    }
    if (tekken_native_snr_xess_enabled && (present_serial < 20 || present_serial % 120 == 0))
        LOG_INFO("Tekken native frame heartbeat: {}", present_serial.load());
    if (tekken_native_snr_xess_enabled && swapchain) {
        ComPtr<ID3D11Texture2D> backbuffer;
        if (SUCCEEDED(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) {
            D3D11_TEXTURE2D_DESC desc {};
            backbuffer->GetDesc(&desc);
            if (desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM &&
                desc.Width >= 1280 && desc.Height >= 720) {
                display_width.store(desc.Width, std::memory_order_release);
                display_height.store(desc.Height, std::memory_order_release);
            }
        }
    }
    if (!jitter_capture_pending) {
        std::error_code arm_error;
        if (std::filesystem::exists(Util::DllPath().parent_path() /
                                    "OptiScaler.d3d11trace.jitter.arm", arm_error)) {
            jitter_capture_pending = true;
            jitter_captured_frames = 0;
            jitter_last_present = ~uint64_t {0};
            LOG_INFO("D3D11 TAA jitter constants capture armed");
        }
    }
    if (tekken_native_snr_xess_enabled)
        TekkenSNRXeSSDx11::EndNativeFrame();
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
    if (!final_upscale_capture_pending.load(std::memory_order_acquire)) {
        std::error_code arm_error;
        if (std::filesystem::exists(Util::DllPath().parent_path() /
                                    "OptiScaler.d3d11trace.upscale.arm", arm_error)) {
            final_upscale_capture_pending.store(true, std::memory_order_release);
            LOG_INFO("D3D11 final upscale input capture armed");
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
    tekken_tonemap_shader.store(nullptr, std::memory_order_release);
    tekken_colorgrade_shader.store(nullptr, std::memory_order_release);
    battle_present_pending.store(false, std::memory_order_release);
    if (!original_draw) return;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(reinterpret_cast<PVOID*>(&original_draw_indexed), HookDrawIndexed);
    DetourDetach(reinterpret_cast<PVOID*>(&original_draw), HookDraw);
    if (original_map && original_unmap) {
        DetourDetach(reinterpret_cast<PVOID*>(&original_map), HookMap);
        DetourDetach(reinterpret_cast<PVOID*>(&original_unmap), HookUnmap);
    }
    DetourDetach(reinterpret_cast<PVOID*>(&original_draw_indexed_instanced), HookDrawIndexedInstanced);
    DetourDetach(reinterpret_cast<PVOID*>(&original_draw_instanced), HookDrawInstanced);
    DetourDetach(reinterpret_cast<PVOID*>(&original_dispatch), HookDispatch);
    DetourDetach(reinterpret_cast<PVOID*>(&original_create_pixel_shader), HookCreatePixelShader);
    DetourDetach(reinterpret_cast<PVOID*>(&original_create_vertex_shader), HookCreateVertexShader);
    DetourDetach(reinterpret_cast<PVOID*>(&original_create_compute_shader), HookCreateComputeShader);
    DetourTransactionCommit();
    original_draw = nullptr;
    original_map = nullptr;
    original_unmap = nullptr;
    original_create_pixel_shader = nullptr;
    original_create_vertex_shader = nullptr;
    original_create_compute_shader = nullptr;
    traced_device = nullptr;
    traced_context = nullptr;
    pixel_shader_code.clear();
    compute_shader_code.clear();
    used_pixel_shaders.clear();
    used_compute_shaders.clear();
    stored_shader_bytes = 0;
    {
        std::lock_guard lock(jitter_mutex);
        mapped_jitter_buffers.clear();
        jitter_values.clear();
    }
}
}
