#define NOMINMAX
#include "../../OptiScaler/dlssnr/sycl/NrDisplayMotion_Dx11.h"
#include <DirectXPackedVector.h>
#include <d3d11sdklayers.h>
#include <dxgi1_6.h>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace DirectX::PackedVector;
using Motion = std::array<float, 2>;
static void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
static void Check(HRESULT hr) { Require(SUCCEEDED(hr), "D3D call failed"); }

static ComPtr<ID3D11Texture2D> Texture(ID3D11Device* device, UINT width, UINT height,
    DXGI_FORMAT format, const void* pixels) {
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = width; desc.Height = height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = format; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data {pixels, width * 4, 0};
    ComPtr<ID3D11Texture2D> texture;
    Check(device->CreateTexture2D(&desc, &data, &texture));
    return texture;
}
static ComPtr<ID3D11Texture2D> MotionTexture(ID3D11Device* device, UINT width, UINT height,
    const std::vector<Motion>& pixels) {
    std::vector<HALF> halves;
    for (const auto& pixel : pixels) for (float value : pixel) halves.push_back(XMConvertFloatToHalf(value));
    return Texture(device, width, height, DXGI_FORMAT_R16G16_FLOAT, halves.data());
}
static std::vector<Motion> Read(ID3D11DeviceContext* context, ID3D11Texture2D* texture) {
    ComPtr<ID3D11Device> device; context->GetDevice(&device);
    D3D11_TEXTURE2D_DESC desc {}; texture->GetDesc(&desc);
    desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> copy; Check(device->CreateTexture2D(&desc, nullptr, &copy));
    context->CopyResource(copy.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE map {}; Check(context->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &map));
    std::vector<Motion> result(desc.Width * desc.Height);
    for (UINT y = 0; y < desc.Height; ++y) for (UINT x = 0; x < desc.Width; ++x) {
        const auto* row = reinterpret_cast<const HALF*>(static_cast<const char*>(map.pData) + y * map.RowPitch);
        result[y * desc.Width + x] = {XMConvertHalfToFloat(row[x*2]), XMConvertHalfToFloat(row[x*2+1])};
    }
    context->Unmap(copy.Get(), 0); return result;
}

int main(int argc, char** argv) {
    try {
        ComPtr<IDXGIFactory1> factory; Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> arc, candidate;
        for (UINT i = 0; SUCCEEDED(factory->EnumAdapters1(i, &candidate)); ++i) {
            DXGI_ADAPTER_DESC1 desc {}; candidate->GetDesc1(&desc);
            if (desc.VendorId == 0x8086 && wcsstr(desc.Description, L"Arc")) { arc = candidate; break; }
            candidate.Reset();
        }
        Require(bool(arc), "Intel Arc unavailable");
        ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        Check(D3D11CreateDevice(arc.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_DEBUG,
            &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context));
        ComPtr<ID3D11InfoQueue> debug; Check(device.As(&debug));
        NrDisplayMotionDx11::Reconstructor adapter;
        std::string error;
        const std::vector<Motion> constant(6, {1.5f,-2.25f});
        auto motion = MotionTexture(device.Get(), 3, 2, constant);
        const std::vector<float> depths(6, .5f);
        auto depth = Texture(device.Get(), 3, 2, DXGI_FORMAT_R32_FLOAT, depths.data());
        for (const auto& size : std::vector<std::array<UINT,2>>{{6,4},{9,4},{3,2}}) {
            ID3D11Texture2D* cached = nullptr;
            for (const Motion jitter : std::vector<Motion>{{0,0},{.375f,-.125f},{-.125f,-.375f}}) {
                Require(adapter.Prepare(context.Get(), motion.Get(), depth.Get(), size[0], size[1],
                    jitter[0], jitter[1], true, error), error);
                if (cached) Require(cached == adapter.Output(), "output allocated every frame");
                cached = adapter.Output();
                const Motion expected {1.5f*size[0]/3, -2.25f*size[1]/2};
                for (const auto& pixel : Read(context.Get(), adapter.Output()))
                    Require(pixel == expected, "constant translation has wrong scale/sign or jitter was added");
            }
        }
        Require(Read(context.Get(), motion.Get()) == constant, "source motion modified");

        // One foreground and one background texel. Analytic expectations for
        // the four display columns ensure that motion is selected, not blended.
        motion = MotionTexture(device.Get(), 2, 1, {{1,0},{-3,0}});
        for (bool reverse_z : {false, true}) {
            const float step[] = {reverse_z ? .8f : .2f, reverse_z ? .2f : .8f};
            depth = Texture(device.Get(), 2, 1, DXGI_FORMAT_R32_FLOAT, step);
            for (float jitter : {0.0f, .5f, -.5f}) {
                Require(adapter.Prepare(context.Get(), motion.Get(), depth.Get(), 4, 2,
                    jitter, 0, reverse_z, error), error);
                const std::array<float,4> expected = jitter > 0 ? std::array<float,4>{2,2,-6,-6}
                    : jitter < 0 ? std::array<float,4>{2,2,2,2} : std::array<float,4>{2,2,2,-6};
                const auto result = Read(context.Get(), adapter.Output());
                for (UINT i = 0; i < result.size(); ++i)
                    Require(result[i] == Motion{expected[i%4],0}, "depth edge or current-jitter sampling mismatch");
            }
        }
        // Static scene with a changing raster sample must remain stationary.
        motion = MotionTexture(device.Get(), 2, 1, {{0,0},{0,0}});
        Require(adapter.Prepare(context.Get(), motion.Get(), depth.Get(), 4, 2, .5f, -.5f, true, error), error);
        for (const auto& pixel : Read(context.Get(), adapter.Output()))
            Require(pixel == Motion{0,0}, "static scene acquired jitter velocity");

        // Invalid surface data must yield out-of-image history coordinates.
        const float nan = std::numeric_limits<float>::quiet_NaN();
        motion = MotionTexture(device.Get(), 2, 1, {{nan,0},{nan,0}});
        Require(adapter.Prepare(context.Get(), motion.Get(), depth.Get(), 4, 2, 0, 0, true, error), error);
        for (const auto& pixel : Read(context.Get(), adapter.Output()))
            Require(pixel == Motion{8,4}, "invalid motion did not reject history");
        Require(!adapter.Prepare(context.Get(), motion.Get(), depth.Get(), 4, 2, nan, 0, true, error), "NaN jitter accepted");
        Require(!adapter.Prepare(context.Get(), motion.Get(), depth.Get(), 1, 1, 0, 0, true, error), "undersized display accepted");
        Require(!adapter.Prepare(context.Get(), depth.Get(), depth.Get(), 4, 2, 0, 0, true, error), "wrong motion format accepted");
        adapter.Reset(); Require(adapter.Output() == nullptr, "reset retained output");
        if (argc == 2) {
            // Optional external 960x540 Tekken fixture; captures stay outside Git.
            const std::filesystem::path folder(argv[1]);
            const auto load = [&](const char* name, DXGI_FORMAT format) {
                std::ifstream file(folder / name, std::ios::binary);
                std::vector<char> bytes((std::istreambuf_iterator<char>(file)), {});
                Require(bytes.size() == 960*540*4, "unexpected capture size");
                return Texture(device.Get(), 960, 540, format, bytes.data());
            };
            auto captured_motion = load("OptiScaler.hdr-capture.motion.bin", DXGI_FORMAT_R16G16_FLOAT);
            auto captured_depth = load("OptiScaler.hdr-capture.depth.bin", DXGI_FORMAT_R32_FLOAT);
            const Motion jitters[] = {{.375f,-.125f},{.125f,.375f},{-.375f,.125f},{-.125f,-.375f}};
            for (UINT i = 0; i < 4; ++i) {
                Require(adapter.Prepare(context.Get(), captured_motion.Get(), captured_depth.Get(),
                    1920,1080,jitters[i][0],jitters[i][1],true,error), error);
                std::vector<HALF> halves;
                for (const auto& pixel : Read(context.Get(),adapter.Output()))
                    for (float v : pixel) halves.push_back(XMConvertFloatToHalf(v));
                std::ofstream file(folder / ("motion-recomputed-" + std::to_string(i) + ".bin"), std::ios::binary);
                file.write(reinterpret_cast<const char*>(halves.data()), halves.size()*sizeof(HALF));
                Require(bool(file), "capture replay output failed");
            }
            std::cout << "Captured render-grid motion/depth replayed for all four jitter phases\n";
        }
        context->ClearState();
        for (UINT64 i = 0; i < debug->GetNumStoredMessages(); ++i) {
            SIZE_T size = 0; debug->GetMessage(i, nullptr, &size); std::vector<char> bytes(size);
            auto* message = reinterpret_cast<D3D11_MESSAGE*>(bytes.data()); debug->GetMessage(i, message, &size);
            if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING)
                throw std::runtime_error(message->pDescription);
        }
        std::cout << "PASS: display motion scale/sign, raster sampling, depth edges, static jitter, invalid history, cache/resize/reset; D3D11 debug clean\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
