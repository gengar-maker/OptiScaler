#include "pch.h"
#include "NativeXeFG.h"
#include <proxies/KernelBase_Proxy.h>
#include <xefg_swapchain.h>
#include <xefg_swapchain_d3d12.h>
#include <detours/detours.h>
#include <mutex>
#include <unordered_map>

namespace NativeXeFG
{
namespace
{
struct Context
{
    void* latency = nullptr;
    uint64_t presents = 0;
    bool enabled = false;
};
std::mutex mutex;
std::unordered_map<xefg_swapchain_handle_t, Context> contexts;
HMODULE hooked = nullptr;
#define FUNCTION(name) decltype(&name) original_##name = nullptr
FUNCTION(xefgSwapChainD3D12CreateContext);
FUNCTION(xefgSwapChainD3D12InitFromSwapChain);
FUNCTION(xefgSwapChainD3D12InitFromSwapChainDesc);
FUNCTION(xefgSwapChainD3D12GetSwapChainPtr);
FUNCTION(xefgSwapChainSetLatencyReduction);
FUNCTION(xefgSwapChainSetEnabled);
FUNCTION(xefgSwapChainSetPresentId);
FUNCTION(xefgSwapChainTagFrameConstants);
FUNCTION(xefgSwapChainD3D12TagFrameResource);
FUNCTION(xefgSwapChainSetNumInterpolatedFrames);
FUNCTION(xefgSwapChainDestroy);
FUNCTION(xefgSwapChainGetLastPresentStatus);
#undef FUNCTION

xefg_swapchain_result_t Create(ID3D12Device* device, xefg_swapchain_handle_t* context)
{
    const auto result = original_xefgSwapChainD3D12CreateContext(device, context);
    if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS && context && *context)
    {
        std::lock_guard lock(mutex);
        contexts.emplace(*context, Context {});
    }
    LOG_INFO("XEFG-NATIVE create context={:X} result={}",
             result == XEFG_SWAPCHAIN_RESULT_SUCCESS && context ? (size_t) *context : 0, (int) result);
    return result;
}
xefg_swapchain_result_t Latency(xefg_swapchain_handle_t context, void* latency)
{
    const auto result = original_xefgSwapChainSetLatencyReduction(context, latency);
    if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        std::lock_guard lock(mutex);
        contexts[context].latency = latency;
    }
    LOG_INFO("XEFG-NATIVE latency context={:X} xell={:X} result={}", (size_t) context, (size_t) latency, (int) result);
    return result;
}
xefg_swapchain_result_t Init(xefg_swapchain_handle_t context, ID3D12CommandQueue* queue,
                             const xefg_swapchain_d3d12_init_params_t* params)
{
    const auto result = original_xefgSwapChainD3D12InitFromSwapChain(context, queue, params);
    LOG_INFO("XEFG-NATIVE init context={:X} queue={:X} flags={} maxFrames={} result={}", (size_t) context,
             (size_t) queue, params ? params->initFlags : 0, params ? params->maxInterpolatedFrames : 0, (int) result);
    return result;
}
xefg_swapchain_result_t InitDesc(xefg_swapchain_handle_t context, HWND window, const DXGI_SWAP_CHAIN_DESC1* desc,
                                 const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, ID3D12CommandQueue* queue,
                                 IDXGIFactory2* factory, const xefg_swapchain_d3d12_init_params_t* params)
{
    const auto result =
        original_xefgSwapChainD3D12InitFromSwapChainDesc(context, window, desc, fullscreen, queue, factory, params);
    LOG_INFO("XEFG-NATIVE initDesc context={:X} size={}x{} windowed={} flags={} maxFrames={} result={}",
             (size_t) context, desc ? desc->Width : 0, desc ? desc->Height : 0, !fullscreen || fullscreen->Windowed,
             params ? params->initFlags : 0, params ? params->maxInterpolatedFrames : 0, (int) result);
    return result;
}
xefg_swapchain_result_t Swapchain(xefg_swapchain_handle_t context, REFIID iid, void** swapchain)
{
    const auto result = original_xefgSwapChainD3D12GetSwapChainPtr(context, iid, swapchain);
    LOG_INFO("XEFG-NATIVE swapchain context={:X} ptr={:X} result={}", (size_t) context,
             result == XEFG_SWAPCHAIN_RESULT_SUCCESS && swapchain ? (size_t) *swapchain : 0, (int) result);
    return result;
}
xefg_swapchain_result_t Enable(xefg_swapchain_handle_t context, uint32_t enabled)
{
    const auto result = original_xefgSwapChainSetEnabled(context, enabled);
    if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        std::lock_guard lock(mutex);
        contexts[context].enabled = enabled != 0;
    }
    LOG_INFO("XEFG-NATIVE enable context={:X} enabled={} result={}", (size_t) context, enabled, (int) result);
    return result;
}
xefg_swapchain_result_t PresentId(xefg_swapchain_handle_t context, uint32_t id)
{
    bool report = false;
    {
        std::lock_guard lock(mutex);
        auto& state = contexts[context];
        ++state.presents;
        report = state.presents <= 5 || state.presents % 120 == 0;
    }
    // Query the previous presentation; do not alter the game's frame IDs or inputs.
    if (report && original_xefgSwapChainGetLastPresentStatus)
    {
        xefg_swapchain_present_status_t status {};
        const auto result = original_xefgSwapChainGetLastPresentStatus(context, &status);
        LOG_INFO("XEFG-NATIVE previous-present context={:X} nextId={} query={} enabled={} frames={} interpolation={}",
                 (size_t) context, id, (int) result, status.isFrameGenEnabled, status.framesPresented,
                 (int) status.frameGenResult);
    }
    const auto result = original_xefgSwapChainSetPresentId(context, id);
    if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        LOG_WARN("XEFG-NATIVE presentId={} result={}", id, (int) result);
    return result;
}
xefg_swapchain_result_t Constants(xefg_swapchain_handle_t context, uint32_t id,
                                  const xefg_swapchain_frame_constant_data_t* data)
{
    const auto result = original_xefgSwapChainTagFrameConstants(context, id, data);
    if (id % 120 == 0 || result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        LOG_INFO("XEFG-NATIVE constants id={} reset={} renderMs={} mvScale={},{} jitter={},{} result={}", id,
                 data ? data->resetHistory : 0, data ? data->frameRenderTime : 0, data ? data->motionVectorScaleX : 0,
                 data ? data->motionVectorScaleY : 0, data ? data->jitterOffsetX : 0, data ? data->jitterOffsetY : 0,
                 (int) result);
    return result;
}
xefg_swapchain_result_t Resource(xefg_swapchain_handle_t context, ID3D12CommandList* list, uint32_t id,
                                 const xefg_swapchain_d3d12_resource_data_t* data)
{
    const auto result = original_xefgSwapChainD3D12TagFrameResource(context, list, id, data);
    if (data && (id % 120 == 0 || result != XEFG_SWAPCHAIN_RESULT_SUCCESS))
        LOG_INFO("XEFG-NATIVE resource id={} type={} size={}x{} validity={} state={} result={}", id, (int) data->type,
                 data->resourceSize.x, data->resourceSize.y, (int) data->validity, (unsigned) data->incomingState,
                 (int) result);
    return result;
}
xefg_swapchain_result_t Count(xefg_swapchain_handle_t context, uint32_t count)
{
    const auto result = original_xefgSwapChainSetNumInterpolatedFrames(context, count);
    LOG_INFO("XEFG-NATIVE generated-count context={:X} count={} result={}", (size_t) context, count, (int) result);
    return result;
}
xefg_swapchain_result_t Destroy(xefg_swapchain_handle_t context)
{
    const auto result = original_xefgSwapChainDestroy(context);
    if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        std::lock_guard lock(mutex);
        contexts.erase(context);
    }
    LOG_INFO("XEFG-NATIVE destroy context={:X} result={}", (size_t) context, (int) result);
    return result;
}
} // namespace
bool HasLatencyContext()
{
    std::lock_guard lock(mutex);
    for (const auto& [handle, context] : contexts)
        if (context.latency)
            return true;
    return false;
}
bool UsesLatencyContext(void* latency)
{
    if (!latency)
        return false;
    std::lock_guard lock(mutex);
    for (const auto& [handle, context] : contexts)
        if (context.latency == latency)
            return true;
    return false;
}
bool Hook(HMODULE module)
{
    if (!module || !Config::Instance()->DlssNrNativeXeSS.value_or_default())
        return false;
    std::lock_guard lock(mutex);
    if (hooked)
        return hooked == module;
#define RESOLVE(name)                                                                                                  \
    original_##name = reinterpret_cast<decltype(original_##name)>(KernelBaseProxy::GetProcAddress_()(module, #name))
    RESOLVE(xefgSwapChainD3D12CreateContext);
    RESOLVE(xefgSwapChainSetLatencyReduction);
    RESOLVE(xefgSwapChainD3D12InitFromSwapChain);
    RESOLVE(xefgSwapChainD3D12InitFromSwapChainDesc);
    RESOLVE(xefgSwapChainD3D12GetSwapChainPtr);
    RESOLVE(xefgSwapChainSetEnabled);
    RESOLVE(xefgSwapChainSetPresentId);
    RESOLVE(xefgSwapChainTagFrameConstants);
    RESOLVE(xefgSwapChainD3D12TagFrameResource);
    RESOLVE(xefgSwapChainSetNumInterpolatedFrames);
    RESOLVE(xefgSwapChainDestroy);
    RESOLVE(xefgSwapChainGetLastPresentStatus);
#undef RESOLVE
    LONG error = DetourTransactionBegin();
    if (error != NO_ERROR)
        return false;
    error = DetourUpdateThread(GetCurrentThread());
#define ATTACH(name, hook)                                                                                             \
    if (error == NO_ERROR && original_##name)                                                                          \
    error = DetourAttach(reinterpret_cast<PVOID*>(&original_##name), hook)
    ATTACH(xefgSwapChainD3D12CreateContext, Create);
    ATTACH(xefgSwapChainSetLatencyReduction, Latency);
    ATTACH(xefgSwapChainD3D12InitFromSwapChain, Init);
    ATTACH(xefgSwapChainD3D12InitFromSwapChainDesc, InitDesc);
    ATTACH(xefgSwapChainD3D12GetSwapChainPtr, Swapchain);
    ATTACH(xefgSwapChainSetEnabled, Enable);
    ATTACH(xefgSwapChainSetPresentId, PresentId);
    ATTACH(xefgSwapChainTagFrameConstants, Constants);
    ATTACH(xefgSwapChainD3D12TagFrameResource, Resource);
    ATTACH(xefgSwapChainSetNumInterpolatedFrames, Count);
    ATTACH(xefgSwapChainDestroy, Destroy);
#undef ATTACH
    if (error == NO_ERROR)
        error = DetourTransactionCommit();
    else
        DetourTransactionAbort();
    if (error == NO_ERROR)
        hooked = module;
    LOG_INFO("XEFG-NATIVE hooks result={}", error);
    return error == NO_ERROR;
}
} // namespace NativeXeFG
