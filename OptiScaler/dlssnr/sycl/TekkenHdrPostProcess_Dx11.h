#pragma once

#include <d3d11_1.h>
#include <wrl/client.h>
#include <array>
#include <string>

namespace TekkenHdrPostProcessDx11 {
enum class Pass { ToneMap, ColorGrade };
using DrawIndexed = void (WINAPI*)(ID3D11DeviceContext*, UINT, UINT, INT);

// Replays tone mapping and optional color grading at display resolution. The
// original draws must still run, so unknown resource chains retain game output.
// Shader identity is checked by the caller; resource identity is checked here.
class Bridge {
public:
    void BeginFrame(ID3D11Texture2D* low_hdr,
                    ID3D11ShaderResourceView* high_hdr);
    bool Replay(ID3D11DeviceContext* context, ID3DDeviceContextState* isolated,
                Pass pass, DrawIndexed draw, UINT count, UINT start, INT base,
                std::string& error);
    bool FinalColor(ID3D11ShaderResourceView* low_color,
                    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& high_color) const;
    void EndFrame();
    void Reset();
    const std::string& SkipReason() const { return skip_reason_; }

private:
    struct Stage {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> output;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        Microsoft::WRL::ComPtr<ID3D11Buffer> raw_input, raw_output, constants;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> raw_srv;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> raw_uav;
    };
    bool Allocate(ID3D11Device* device, Stage& stage, UINT bytes, UINT width,
                  UINT height, std::string& error);
    std::array<Stage, 2> stages_;
    std::array<Microsoft::WRL::ComPtr<ID3D11Resource>, 3> low_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> high_hdr_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> patch_shader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> patch_params_;
    unsigned completed_ = 0;
    std::string skip_reason_;
};
}
