#define NOMINMAX
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#include "../../OptiScaler/dlssnr/sycl/TekkenTemporalInputs_Dx11.h"

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

static void Require(bool okay, const char* message) {
    if (!okay) throw std::runtime_error(message);
}

static std::vector<uint8_t> Read(const fs::path& path, size_t expected) {
    std::ifstream file(path, std::ios::binary);
    Require(static_cast<bool>(file), "input file is missing");
    std::vector<uint8_t> bytes(std::istreambuf_iterator<char>(file), {});
    Require(bytes.size() == expected, "unexpected file size");
    return bytes;
}

static ComPtr<ID3D11Texture2D> Texture(
    ID3D11Device* device, const fs::path& file, DXGI_FORMAT format,
    UINT width, UINT height, UINT bytes_per_pixel) {
    const auto bytes = Read(file, static_cast<size_t>(width) * height * bytes_per_pixel);
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data {bytes.data(), width * bytes_per_pixel, 0};
    ComPtr<ID3D11Texture2D> result;
    Require(SUCCEEDED(device->CreateTexture2D(&desc, &data, &result)),
            "CreateTexture2D failed");
    return result;
}

static ComPtr<ID3D11ShaderResourceView> View(
    ID3D11Device* device, ID3D11Texture2D* texture, DXGI_FORMAT format) {
    D3D11_SHADER_RESOURCE_VIEW_DESC desc {};
    desc.Format = format;
    desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> result;
    Require(SUCCEEDED(device->CreateShaderResourceView(texture, &desc, &result)),
            "CreateShaderResourceView failed");
    return result;
}

static ComPtr<ID3D11Buffer> Buffer(
    ID3D11Device* device, const fs::path& file, UINT size) {
    const auto bytes = Read(file, size);
    D3D11_BUFFER_DESC desc {};
    desc.ByteWidth = size;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA data {bytes.data(), 0, 0};
    ComPtr<ID3D11Buffer> result;
    Require(SUCCEEDED(device->CreateBuffer(&desc, &data, &result)),
            "CreateBuffer failed");
    return result;
}

static std::vector<uint8_t> Download(ID3D11Device* device,
                                      ID3D11DeviceContext* context,
                                      ID3D11Texture2D* source,
                                      UINT bytes_per_pixel) {
    D3D11_TEXTURE2D_DESC desc {};
    source->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    Require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)),
            "staging texture failed");
    context->CopyResource(staging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped {};
    Require(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)),
            "staging map failed");
    const size_t row_size = static_cast<size_t>(desc.Width) * bytes_per_pixel;
    std::vector<uint8_t> result(row_size * desc.Height);
    for (UINT y = 0; y < desc.Height; ++y)
        memcpy(result.data() + y * row_size,
               static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch,
               row_size);
    context->Unmap(staging.Get(), 0);
    return result;
}

static float Half(uint16_t bits) {
    const int sign = (bits & 0x8000) ? -1 : 1;
    const int exponent = (bits >> 10) & 31;
    const int fraction = bits & 1023;
    if (exponent == 31) return fraction ? NAN : sign * INFINITY;
    if (exponent == 0) return sign * std::ldexp(static_cast<float>(fraction), -24);
    return sign * std::ldexp(1.0f + fraction / 1024.0f, exponent - 15);
}

static void Compare(const std::vector<uint8_t>& actual,
                    const std::vector<uint8_t>& expected,
                    bool fp16, double max_mean, double max_peak,
                    const char* name) {
    Require(actual.size() == expected.size(), "output size mismatch");
    const size_t item_size = fp16 ? 2 : 4;
    double sum = 0, peak = 0;
    for (size_t offset = 0; offset < actual.size(); offset += item_size) {
        double value = 0, reference = 0;
        if (fp16) {
            uint16_t a, b;
            memcpy(&a, actual.data() + offset, 2);
            memcpy(&b, expected.data() + offset, 2);
            value = Half(a);
            reference = Half(b);
        } else {
            float a, b;
            memcpy(&a, actual.data() + offset, 4);
            memcpy(&b, expected.data() + offset, 4);
            value = a;
            reference = b;
        }
        Require(std::isfinite(value) && std::isfinite(reference),
                "non-finite output value");
        const double difference = std::abs(value - reference);
        sum += difference;
        peak = std::max(peak, difference);
    }
    const double mean = sum / (actual.size() / item_size);
    std::cout << name << " mean_abs=" << mean << " peak_abs=" << peak << '\n';
    Require(mean <= max_mean && peak <= max_peak,
            "GPU output differs from captured-frame reference");
}

int main(int argc, char** argv) {
    try {
        Require(argc == 2 || argc == 3,
                "usage: smoke.exe <captured-frame-folder> [native]");
        const fs::path folder = argv[1];
        const bool native = argc == 3 && std::string(argv[2]) == "native";
        const UINT width = native ? 960 : 1920;
        const UINT height = native ? 540 : 1080;
        ComPtr<IDXGIFactory6> factory;
        Require(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))),
                "DXGI factory failed");
        ComPtr<IDXGIAdapter1> arc, candidate;
        for (UINT i = 0; SUCCEEDED(factory->EnumAdapters1(i, &candidate)); ++i) {
            DXGI_ADAPTER_DESC1 desc {};
            candidate->GetDesc1(&desc);
            if (desc.VendorId == 0x8086 && wcsstr(desc.Description, L"Arc")) {
                arc = candidate;
                break;
            }
            candidate.Reset();
        }
        Require(static_cast<bool>(arc), "Intel Arc adapter unavailable");
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        D3D_FEATURE_LEVEL chosen {};
        Require(SUCCEEDED(D3D11CreateDevice(arc.Get(), D3D_DRIVER_TYPE_UNKNOWN,
            nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, &chosen, &context)),
            "D3D11CreateDevice failed");
        const auto prefix = folder / "OptiScaler.d3d11trace.inputs";
        auto color = Texture(device.Get(), prefix.string() + ".color.bin",
                             DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, 8);
        auto depth = Texture(device.Get(), prefix.string() + ".depth.bin",
                             DXGI_FORMAT_R24G8_TYPELESS, width, height, 4);
        auto motion = Texture(device.Get(), prefix.string() + ".motion.bin",
                              DXGI_FORMAT_R16G16_UNORM, width, height, 4);
        auto color_view = View(device.Get(), color.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
        auto depth_view = View(device.Get(), depth.Get(), DXGI_FORMAT_R24_UNORM_X8_TYPELESS);
        auto motion_view = View(device.Get(), motion.Get(), DXGI_FORMAT_R16G16_UNORM);
        auto scene = Buffer(device.Get(), prefix.string() + ".cb0.bin", 1024);
        auto view = Buffer(device.Get(), prefix.string() + ".cb1.bin", 4096);
        TekkenTemporalInputsDx11::Sources sources {
            color_view.Get(), depth_view.Get(), motion_view.Get(),
            scene.Get(), view.Get()};
        TekkenTemporalInputsDx11::Processor processor;
        TekkenTemporalInputsDx11::Outputs result;
        std::string error;
        Require(processor.Prepare(context.Get(), sources, result, error,
            native ? TekkenTemporalInputsDx11::InputScale::Native
                   : TekkenTemporalInputsDx11::InputScale::Downsample2x),
            error.c_str());
        Require(result.width == 960 && result.height == 540,
                "unexpected Balanced dimensions");
        D3D11_BUFFER_DESC bad_constants {};
        bad_constants.ByteWidth = 512;
        bad_constants.Usage = D3D11_USAGE_DEFAULT;
        bad_constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ComPtr<ID3D11Buffer> wrong_scene;
        Require(SUCCEEDED(device->CreateBuffer(&bad_constants, nullptr, &wrong_scene)),
                "invalid-layout test buffer creation failed");
        sources.scene_constants = wrong_scene.Get();
        TekkenTemporalInputsDx11::Outputs rejected;
        error.clear();
        Require(!processor.Prepare(context.Get(), sources, rejected, error,
            native ? TekkenTemporalInputsDx11::InputScale::Native
                   : TekkenTemporalInputsDx11::InputScale::Downsample2x) &&
                error.find("constant buffer layout") != std::string::npos,
                "mismatched game constant buffer was accepted");
        sources.scene_constants = scene.Get();
        constexpr size_t pixel_count = 960ull * 540ull;
        Compare(Download(device.Get(), context.Get(), result.hdr.Get(), 8),
                Read(folder / "balanced-hdr.rgba16f", pixel_count * 8),
                true, 0.0002, 0.1, "HDR");
        Compare(Download(device.Get(), context.Get(), result.motion.Get(), 4),
                Read(folder / "balanced-motion.rg16f", pixel_count * 4),
                true, 0.002, 0.15, "motion");
        Compare(Download(device.Get(), context.Get(), result.depth.Get(), 4),
                Read(folder / "balanced-depth.r32f", pixel_count * 4),
                false, 0.00001, 0.0001, "depth");
        const auto unjittered = Download(device.Get(), context.Get(), result.motion.Get(), 4);
        sources.history_jitter_x = .25f;
        sources.history_jitter_y = -.5f;
        Require(processor.Prepare(context.Get(), sources, result, error,
            native ? TekkenTemporalInputsDx11::InputScale::Native
                   : TekkenTemporalInputsDx11::InputScale::Downsample2x),
            "jittered history preparation failed");
        Require(Download(device.Get(), context.Get(), result.motion.Get(), 4) == unjittered,
                "SNR jitter contaminated XeSS motion");
        const auto corrected = Download(device.Get(), context.Get(), result.neural_motion.Get(), 4);
        for (size_t i = 0; i < pixel_count * 2; ++i) {
            uint16_t a, b;
            memcpy(&a, unjittered.data() + i * 2, 2);
            memcpy(&b, corrected.data() + i * 2, 2);
            const float expected = Half(a) + (i % 2 ? -.5f : .25f);
            Require(std::abs(Half(b) - expected) <= .001f * (1 + std::abs(expected)),
                    "SNR history jitter has wrong sign or units");
        }
        std::cout << "Independent XeSS velocity and SNR previous-minus-current jitter PASS\n";
        std::cout << "GPU TAA input preparation PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
