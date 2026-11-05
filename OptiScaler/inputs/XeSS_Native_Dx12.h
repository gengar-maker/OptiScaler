#pragma once
#include <d3d12.h>
#include <xess_d3d12.h>

// Opt-in native context path. No NGX translation, replacement presets or
// fabricated SDK queries. Diagnostics never alter pixels or resource states.
namespace XeSSNativeDx12 {
bool Enabled();
bool Owns(xess_context_handle_t context);
xess_result_t Create(ID3D12Device* device, xess_context_handle_t* context);
xess_result_t Destroy(xess_context_handle_t context);
xess_result_t Init(xess_context_handle_t context, const xess_d3d12_init_params_t* params);
xess_result_t Execute(xess_context_handle_t context, ID3D12GraphicsCommandList* list,
                      const xess_d3d12_execute_params_t* params);
}
