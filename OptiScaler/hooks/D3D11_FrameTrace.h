#pragma once

#include <d3d11.h>
#include <dxgi.h>

// Diagnostic only. A marker beside the DLL enables hooks before device
// creation; a second marker triggers capture of the next complete frame.
namespace D3D11FrameTrace {
void Install(ID3D11Device* device);
void BeforePresent(IDXGISwapChain* swapchain, ID3D11Device* device);
void Uninstall();
}
