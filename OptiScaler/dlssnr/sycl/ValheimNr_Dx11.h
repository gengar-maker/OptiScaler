#pragma once

#include <d3d11.h>
#include <dxgi.h>
#include <optional>
#include <string>

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
}
