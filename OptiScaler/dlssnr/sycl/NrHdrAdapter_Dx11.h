#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <string>
#include "../../shaders/dlssnr/DlssNr_Common.h"

namespace NrHdrAdapterDx11 {
// Uses the same encode/resolve HLSL as the D3D12 NR path, compiled for SM5.0.
// Caller owns context-state isolation. Resources never leave the GPU.
class Adapter {
public:
    bool Encode(ID3D11DeviceContext* context, ID3D11Texture2D* hdr,
                const DlssNrConstants& settings, ID3D11ShaderResourceView* exposure,
                std::string& error);
    bool Resolve(ID3D11DeviceContext* context, ID3D11Texture2D* neural,
                 bool apply_model, std::string& error);
    ID3D11Texture2D* Proxy() const { return proxy_.Get(); }
    ID3D11Texture2D* Output() const { return output_.Get(); }
    void Reset();
private:
    bool Initialize(ID3D11Device* device, UINT width, UINT height, std::string& error);
    void Dispatch(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source,
                  ID3D11ShaderResourceView* model);
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> original_, proxy_, output_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> original_srv_, proxy_srv_, exposure_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> original_uav_, proxy_uav_, output_uav_;
    DlssNrConstants settings_ {};
    UINT width_ = 0, height_ = 0;
    bool encoded_ = false;
};
// Converts the game's final scalar exposure to XeSS's required R32_FLOAT
// texture without CPU readback. Both consumers then use the identical scale.
class Exposure {
public:
    bool Copy(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source,
              std::string& error);
    ID3D11Texture2D* Texture() const { return texture_.Get(); }
    ID3D11ShaderResourceView* View() const { return srv_.Get(); }
    void Reset() { *this = Exposure {}; }
private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader_;
};
}
