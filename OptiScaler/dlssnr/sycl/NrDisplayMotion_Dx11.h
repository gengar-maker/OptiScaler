#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <string>

namespace NrDisplayMotionDx11 {
// Reconstructs an unjittered display-grid motion field from render-grid depth
// and unjittered current-to-previous motion (in render pixels). XeSS does not
// expose its internal reconstructed motion. This is our conservative adapter,
// not new motion information produced by the upscaler.
// Caller owns immediate-context state isolation. All work stays on the GPU.
class Reconstructor {
public:
    bool Prepare(ID3D11DeviceContext* context, ID3D11Texture2D* motion,
                 ID3D11Texture2D* depth, UINT display_width, UINT display_height,
                 float raster_jitter_x, float raster_jitter_y, bool reverse_z,
                 std::string& error);
    ID3D11Texture2D* Output() const { return output_.Get(); }
    void Reset();

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> motion_, depth_, output_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> motion_srv_, depth_srv_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> output_uav_;
};
}
