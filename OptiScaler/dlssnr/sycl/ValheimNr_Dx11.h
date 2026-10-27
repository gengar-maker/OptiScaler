#pragma once

#include <d3d11.h>
#include <dxgi.h>
#include <optional>
#include <string>
#include <wrl/client.h>

// Valheim and Tekken 7 have no NGX/XeSS/FSR call for OptiScaler to intercept.
// This opt-in final-frame path runs SYCL SNR at the display resolution.
// It is called before OptiScaler draws its own overlay.
namespace ValheimNr_Dx11 {
void BeforePresent(IDXGISwapChain* swapchain, ID3D11Device* device);
bool IsSupportedGame();
bool IsRunning();
std::optional<double> LastFrameMs();
std::string FailureReason();
void RetryAfterFailure();

// GPU-only SNR pass for a renderer that supplies XeSS inputs before temporal
// upscaling. Returns a separate low-resolution texture; the game's color
// texture remains untouched. False means XeSS should use its original input.
bool ProcessBeforeXeSS(ID3D11DeviceContext* context,
                       ID3D11Texture2D* color,
                       ID3D11Texture2D* motion,
                       UINT input_width, UINT input_height,
                       UINT color_base_x, UINT color_base_y,
                       UINT motion_base_x, UINT motion_base_y,
                       UINT motion_width, UINT motion_height,
                       float motion_scale_x, float motion_scale_y,
                       bool reset_history,
                       Microsoft::WRL::ComPtr<ID3D11Texture2D>& result,
                       std::string& error);
}
