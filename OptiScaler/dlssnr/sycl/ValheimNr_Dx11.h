#pragma once

#include <d3d11.h>
#include <dxgi.h>
#include <optional>
#include <string>
#include <wrl/client.h>

// Games without an intercepted NGX/XeSS/FSR call can explicitly opt in with
// OptiScaler.snr-present.enable. This SDR final-frame path runs SNR at display
// resolution, including the game's HUD. Engine vectors are not implied.
// It is called before OptiScaler draws its own overlay.
namespace ValheimNr_Dx11 {
void BeforePresent(IDXGISwapChain* swapchain, ID3D11Device* device);
bool IsSupportedGame();
bool IsRunning();
std::optional<double> LastFrameMs();
std::optional<double> LastModelMs();
struct PreparationStatus {
    bool pending = false;
    unsigned width = 0, height = 0;
    double seconds = 0;
    std::string message;
    bool visible = false;
    bool failed = false;
};
PreparationStatus GetPreparationStatus();
// Called before touching engine resources; false leaves the original draw.
bool PrepareWithMotion(UINT width, UINT height);
std::string FailureReason();
void RetryAfterFailure();

// GPU-only SNR pass at the supplied color resolution. Motion is current-to-
// previous displacement with explicit grid dimensions and pixel scale. Suitable
// both before and after upscaling; caller supplies matching temporal semantics.
// Returns a separate texture and leaves input color untouched.
bool ProcessWithMotion(ID3D11DeviceContext* context,
                       ID3D11Texture2D* color,
                       ID3D11Texture2D* motion,
                       UINT input_width, UINT input_height,
                       UINT color_base_x, UINT color_base_y,
                       UINT motion_base_x, UINT motion_base_y,
                       UINT motion_width, UINT motion_height,
                       float motion_scale_x, float motion_scale_y,
                       bool reset_history,
                       Microsoft::WRL::ComPtr<ID3D11Texture2D>& result,
                       std::string& error,
                       bool external_hdr_composition = false);

// Compatibility entry point for existing pre-upscale integrations.
inline bool ProcessBeforeXeSS(ID3D11DeviceContext* context,
    ID3D11Texture2D* color, ID3D11Texture2D* motion, UINT input_width, UINT input_height,
    UINT color_base_x, UINT color_base_y, UINT motion_base_x, UINT motion_base_y,
    UINT motion_width, UINT motion_height, float motion_scale_x, float motion_scale_y,
    bool reset_history, Microsoft::WRL::ComPtr<ID3D11Texture2D>& result,
    std::string& error, bool external_hdr_composition = false) {
    return ProcessWithMotion(context, color, motion, input_width, input_height,
        color_base_x, color_base_y, motion_base_x, motion_base_y, motion_width,
        motion_height, motion_scale_x, motion_scale_y, reset_history, result,
        error, external_hdr_composition);
}

// Called after the integrated SNR/XeSS result reaches its display target. Prevents the
// final-frame fallback from running the model a second time on that frame.
void MarkPreXeSSOutput(ID3D11DeviceContext* context, ID3D11Resource* output);
}
