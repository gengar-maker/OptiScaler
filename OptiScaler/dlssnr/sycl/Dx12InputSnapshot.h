#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <filesystem>
#include <string>
#include <vector>

// Diagnostic readback only. Production inference never goes through this API.
// Run after completion of the producer submission, before its consumer.
// Inputs must be simultaneous-access textures, whose state decays to COMMON.
namespace Dx12InputSnapshot {
struct Texture {
    std::string name;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
};
bool Capture(ID3D12Device* device, ID3D12CommandQueue* queue,
             const std::vector<Texture>& textures,
             const std::filesystem::path& newDirectory, std::string& error);
}
