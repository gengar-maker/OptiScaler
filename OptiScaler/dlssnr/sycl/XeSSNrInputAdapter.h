#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include "XeSSNrInputContract.h"
#include "../../shaders/dlssnr/DlssNr_Common.h"

namespace XeSSNr {
// All calls record into an OWNED direct list after the game producer completed.
// Game inputs must be simultaneous-access textures in COMMON. Caller fences
// before reusing descriptors/constants or destroying this object.
class InputAdapter {
public:
    InputAdapter(ID3D12Device* device, unsigned width, unsigned height);
    static Microsoft::WRL::ComPtr<ID3D12Resource> MakeOutput(
        ID3D12Device* device, unsigned width, unsigned height);
    // Independent GPU fallback: byte-exact colour crop, output COMMON -> COMMON.
    static void CopyOriginal(ID3D12GraphicsCommandList* list, ID3D12Resource* colour,
        unsigned x, unsigned y, unsigned width, unsigned height, ID3D12Resource* output);
    void Encode(ID3D12GraphicsCommandList* list, ID3D12Resource* output,
        ID3D12Resource* motion, unsigned motionX, unsigned motionY,
        const MotionTransform& transform, const DlssNrConstants& constants,
        ID3D12Resource* exposure = nullptr);
    void Resolve(ID3D12GraphicsCommandList* list, ID3D12Resource* output,
        bool applyModel, ID3D12Resource* exposure = nullptr);
    ID3D12Resource* Proxy() const { return proxy_.Get(); }
    ID3D12Resource* Neural() const { return neural_.Get(); }
    ID3D12Resource* Motion() const { return motion_.Get(); }
private:
    void HdrPass(ID3D12GraphicsCommandList*, unsigned slot, ID3D12Resource* source,
        ID3D12Resource* model, ID3D12Resource* original, ID3D12Resource* exposure,
        ID3D12Resource* target, ID3D12Resource* keep, const DlssNrConstants& constants);
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12Resource> proxy_, original_, neural_, motion_, constants_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> hdrRoot_, motionRoot_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> hdrPso_, motionPso_;
    std::array<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>,3> heaps_;
    DlssNrConstants settings_ {};
    unsigned width_, height_;
};
} // namespace XeSSNr
