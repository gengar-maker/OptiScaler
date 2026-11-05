#pragma once
#include <d3d12.h>
#include <xess_d3d12.h>
#include <string>

namespace XeSSNrNative {
struct Status {
    bool active=false,running=false,pending=false,visible=false,failed=false;
    unsigned width=0,height=0;
    double seconds=0,frameMs=0,modelMs=0;
    std::string message;
};
void Create(xess_context_handle_t context,ID3D12Device* device);
void Destroy(xess_context_handle_t context);
void Reset(xess_context_handle_t context);
// Takes a COPY of the game's parameters; modifies only colour/base and a
// one-shot history reset on SNR edit transitions. All other XeSS inputs stay native.
bool Record(xess_context_handle_t context,ID3D12GraphicsCommandList* list,
            xess_d3d12_execute_params_t& parameters,uint64_t frame,bool capture);
Status GetStatus();
} // namespace XeSSNrNative
