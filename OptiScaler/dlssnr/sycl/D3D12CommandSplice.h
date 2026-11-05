#pragma once
#include <d3d12.h>
#include <functional>
#include <string>

// Experimental synchronous cross-API boundary. Opt-in; no game integration
// until baseline + debug-layer tests pass. Only DIRECT queues are supported.
namespace D3D12CommandSplice {
using Work = std::function<void(ID3D12Device*, ID3D12CommandQueue*)>;
using Log = std::function<void(const std::string&)>;
bool Install(ID3D12Device* device, Log logger, std::string& error);
// Close must still be called by the game. Work runs after submission and
// completion of the producer prefix, before the continuation is submitted.
// The callback must initialize every replacement input even on model failure.
// This prototype deliberately waits for the continuation too, bounding all
// temporary lifetimes by actual fences instead of frame-count heuristics.
bool Begin(ID3D12GraphicsCommandList* list, Work work, std::string& error);
ID3D12GraphicsCommandList* Continuation(ID3D12GraphicsCommandList* list);
// Suppress routing/tracking for commands recorded directly by SDKs and bridge.
struct InternalScope {
    InternalScope();
    ~InternalScope();
    InternalScope(const InternalScope&) = delete;
    InternalScope& operator=(const InternalScope&) = delete;
};
}
