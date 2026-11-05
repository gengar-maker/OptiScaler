#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include "TekkenHdrPostProcess_Dx11.h"

// Opt-in replacement for Tekken's identified full-resolution TAA draw.
// The caller has already checked the exact pixel-shader bytecode hash.
// Returns false whenever the original draw should run unchanged.
namespace TekkenSNRXeSSDx11 {
struct NativeCapture {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> hdr, taa, proxy, neural, resolved, motion, neural_motion, depth, xess, exposure, neural_input;
    UINT history_frames = 0;
};
NativeCapture CaptureInputs();
bool TryReplaceTaaDraw(ID3D11DeviceContext* context);
// Native 2x path: TAA inputs are already at XeSS input resolution. The TAA
// hook prepares temporal inputs. With the HDR opt-in, SNR and XeSS execute
// when the game's tonemap exposure is available, and postprocess draws run at display
// resolution; otherwise the older final-pass LDR experiment is used.
bool TryProcessNativeTaaDraw(ID3D11DeviceContext* context,
                             float jitter_x, float jitter_y,
                             UINT display_width, UINT display_height);
bool TryPrepareNativeUpscale(ID3D11DeviceContext* context,
    Microsoft::WRL::ComPtr<ID3D11PixelShader>& shader,
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& color);
bool NativeHdrEnabled();
bool NeuralAfterXeSSEnabled();
bool NativeSnrEnabled();
// Native-resolution comparison: consume the game's completed TAA output and
// replace only the HDR color input of the original tone-map draw. No XeSS.
bool TryPrepareNativeTonemap(ID3D11DeviceContext* context,
    ID3D11ShaderResourceView* exposure,
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& color);
void CompleteNativeTonemap(ID3D11DeviceContext* context);
void ReplayNativeHdrPostProcess(ID3D11DeviceContext* context,
    TekkenHdrPostProcessDx11::Pass pass, TekkenHdrPostProcessDx11::DrawIndexed draw,
    UINT count, UINT start, INT base, ID3D11ShaderResourceView* exposure = nullptr);
void EndNativeFrame();
}
