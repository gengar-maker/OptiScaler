#pragma once

#include <d3d11.h>
#include <string>
#include <wrl/client.h>

// Tekken's TAA shader receives full-resolution HDR, reverse-Z depth and
// packed R16G16_UNORM velocity. This adapter prepares XeSS Balanced inputs on
// the GPU. The SNR/XeSS chain consuming them is independent of the game.
namespace TekkenTemporalInputsDx11 {

struct Sources {
    ID3D11ShaderResourceView* color = nullptr;
    ID3D11ShaderResourceView* depth = nullptr;
    ID3D11ShaderResourceView* packed_motion = nullptr;
    ID3D11Buffer* scene_constants = nullptr;
    ID3D11Buffer* view_constants = nullptr;
    // Previous minus current raster jitter, in output pixels. XeSS consumes
    // unjittered velocity; SNR warps a history in the jittered color grid.
    float history_jitter_x = 0;
    float history_jitter_y = 0;
};

struct Outputs {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> hdr;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> motion;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> neural_motion;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> depth;
    UINT width = 0;
    UINT height = 0;
};

enum class InputScale { Native, Downsample2x };

class Processor {
public:
    bool Prepare(ID3D11DeviceContext* context, const Sources& sources,
                 Outputs& outputs, std::string& error,
                 InputScale scale = InputScale::Downsample2x);
    void Reset();

private:
    bool Initialize(ID3D11Device* device, UINT width, UINT height,
                    InputScale scale, std::string& error);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> jitter_constants_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> textures_[4];
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uavs_[4];
    UINT width_ = 0;
    UINT height_ = 0;
    InputScale scale_ = InputScale::Downsample2x;
};

}  // namespace TekkenTemporalInputsDx11
