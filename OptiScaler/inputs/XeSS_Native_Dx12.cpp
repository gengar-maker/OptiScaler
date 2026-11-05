#include "pch.h"
#include "XeSS_Native_Dx12.h"
#include "XeSS_Nr_Dx12.h"
#include <proxies/XeSS_Proxy.h>
#include <detours/detours.h>
#include <wrl/client.h>
#include <array>
#include <dlssnr/sycl/D3D12CommandSplice.h>
#include <dlssnr/sycl/Dx12InputSnapshot.h>
#include <Util.h>
#include <fstream>
#include <sstream>
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace XeSSNativeDx12 {
namespace {
using Microsoft::WRL::ComPtr;
struct Context {
    xess_d3d12_init_params_t init {};
    uint64_t frames = 0;
    UINT width = 0, height = 0;
};
std::mutex mutex;
std::unordered_map<xess_context_handle_t, Context> contexts;
struct Tail {
    uint64_t frame = 0;
    unsigned draws = 0, dispatches = 0, copies = 0, indirect = 0, barriers = 0;
    std::array<ID3D12Resource*, 6> resources {};
    std::array<D3D12_RESOURCE_STATES, 6> finalStates {};
    bool closed = false;
};
std::unordered_map<ID3D12GraphicsCommandList*, Tail> tails;
std::atomic<bool> hooksReady {false};
std::mutex hookMutex;
using CloseFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
using ResetFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
using IndexedFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
using DispatchFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
using BarrierFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
using CopyFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT, const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
using CopyResourceFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*);
using IndirectFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64);
CloseFn closeOriginal = nullptr;
ResetFn resetOriginal = nullptr;
DrawFn drawOriginal = nullptr;
IndexedFn indexedOriginal = nullptr;
DispatchFn dispatchOriginal = nullptr;
BarrierFn barrierOriginal = nullptr;
CopyFn copyOriginal = nullptr;
CopyResourceFn copyResourceOriginal = nullptr;
IndirectFn indirectOriginal = nullptr;

void Count(ID3D12GraphicsCommandList* list, unsigned Tail::* field) {
    std::lock_guard lock(mutex);
    if (auto it = tails.find(list); it != tails.end() && !it->second.closed)
        ++(it->second.*field);
}
HRESULT STDMETHODCALLTYPE CloseHook(ID3D12GraphicsCommandList* list) {
    const HRESULT result = closeOriginal(list);
    std::lock_guard lock(mutex);
    if (auto it = tails.find(list); it != tails.end()) {
        auto& t = it->second;
        LOG_INFO("XESS-NATIVE boundary frame={} list={:X} close={:X} tail draws={} dispatches={} copies={} indirect={} barriers={} final color={} mv={} depth={} exposure={} mask={} output={}",
            t.frame, (size_t)list, (unsigned)result, t.draws, t.dispatches, t.copies, t.indirect, t.barriers,
            (unsigned)t.finalStates[0], (unsigned)t.finalStates[1], (unsigned)t.finalStates[2],
            (unsigned)t.finalStates[3], (unsigned)t.finalStates[4], (unsigned)t.finalStates[5]);
        t.closed = true;
    }
    return result;
}
HRESULT STDMETHODCALLTYPE ResetHook(ID3D12GraphicsCommandList* list, ID3D12CommandAllocator* allocator, ID3D12PipelineState* state) {
    { std::lock_guard lock(mutex); tails.erase(list); }
    return resetOriginal(list, allocator, state);
}
void STDMETHODCALLTYPE DrawHook(ID3D12GraphicsCommandList* list, UINT a, UINT b, UINT c, UINT d) {
    Count(list, &Tail::draws); drawOriginal(list, a, b, c, d);
}
void STDMETHODCALLTYPE IndexedHook(ID3D12GraphicsCommandList* list, UINT a, UINT b, UINT c, INT d, UINT e) {
    Count(list, &Tail::draws); indexedOriginal(list, a, b, c, d, e);
}
void STDMETHODCALLTYPE DispatchHook(ID3D12GraphicsCommandList* list, UINT x, UINT y, UINT z) {
    Count(list, &Tail::dispatches); dispatchOriginal(list, x, y, z);
}
void STDMETHODCALLTYPE CopyHook(ID3D12GraphicsCommandList* list, const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box) {
    Count(list, &Tail::copies); copyOriginal(list, dst, x, y, z, src, box);
}
void STDMETHODCALLTYPE CopyResourceHook(ID3D12GraphicsCommandList* list, ID3D12Resource* dst, ID3D12Resource* src) {
    Count(list, &Tail::copies); copyResourceOriginal(list, dst, src);
}
void STDMETHODCALLTYPE IndirectHook(ID3D12GraphicsCommandList* list, ID3D12CommandSignature* signature, UINT count, ID3D12Resource* args, UINT64 offset, ID3D12Resource* counter, UINT64 counterOffset) {
    Count(list, &Tail::indirect); indirectOriginal(list, signature, count, args, offset, counter, counterOffset);
}
void STDMETHODCALLTYPE BarrierHook(ID3D12GraphicsCommandList* list, UINT count, const D3D12_RESOURCE_BARRIER* barriers) {
    {
        std::lock_guard lock(mutex);
        if (auto it = tails.find(list); it != tails.end() && !it->second.closed) {
            auto& t = it->second;
            t.barriers += count;
            for (UINT b = 0; b < count; ++b) {
                if (barriers[b].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) continue;
                const auto& tr = barriers[b].Transition;
                for (size_t r = 0; r < t.resources.size(); ++r)
                    if (t.resources[r] && t.resources[r] == tr.pResource)
                        t.finalStates[r] = tr.StateAfter;
            }
        }
    }
    barrierOriginal(list, count, barriers);
}
void InstallProbeHooks(ID3D12GraphicsCommandList* list) {
    if (hooksReady.load()) return;
    std::lock_guard lock(hookMutex);
    if (hooksReady.load()) return;
    void** table = *reinterpret_cast<void***>(list);
    closeOriginal = reinterpret_cast<CloseFn>(table[9]);
    resetOriginal = reinterpret_cast<ResetFn>(table[10]);
    drawOriginal = reinterpret_cast<DrawFn>(table[12]);
    indexedOriginal = reinterpret_cast<IndexedFn>(table[13]);
    dispatchOriginal = reinterpret_cast<DispatchFn>(table[14]);
    copyOriginal = reinterpret_cast<CopyFn>(table[16]);
    copyResourceOriginal = reinterpret_cast<CopyResourceFn>(table[17]);
    barrierOriginal = reinterpret_cast<BarrierFn>(table[26]);
    indirectOriginal = reinterpret_cast<IndirectFn>(table[59]);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(reinterpret_cast<PVOID*>(&closeOriginal), CloseHook);
    DetourAttach(reinterpret_cast<PVOID*>(&resetOriginal), ResetHook);
    DetourAttach(reinterpret_cast<PVOID*>(&drawOriginal), DrawHook);
    DetourAttach(reinterpret_cast<PVOID*>(&indexedOriginal), IndexedHook);
    DetourAttach(reinterpret_cast<PVOID*>(&dispatchOriginal), DispatchHook);
    DetourAttach(reinterpret_cast<PVOID*>(&copyOriginal), CopyHook);
    DetourAttach(reinterpret_cast<PVOID*>(&copyResourceOriginal), CopyResourceHook);
    DetourAttach(reinterpret_cast<PVOID*>(&barrierOriginal), BarrierHook);
    DetourAttach(reinterpret_cast<PVOID*>(&indirectOriginal), IndirectHook);
    const auto result = DetourTransactionCommit();
    hooksReady.store(result == NO_ERROR);
    LOG_INFO("XESS-NATIVE command boundary probe installed: {}", result == NO_ERROR);
}
// Opt-in, one-frame diagnostic. The request contains an existing absolute
// output directory; all snapshots go into a new child directory. No request
// means no additional splice hooks or readbacks in ordinary native XeSS mode.
std::mutex captureMutex;
bool captureHooksReady = false;
bool captureScheduled = false;
std::filesystem::path CaptureRequestPath() {
    return Util::DllPath().parent_path() / L"xess-native-capture.request";
}
void PrepareCapture(ID3D12Device* device) {
    std::lock_guard lock(captureMutex);
    if (captureHooksReady) return;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(CaptureRequestPath(), ec)) return;
    std::string error;
    captureHooksReady = D3D12CommandSplice::Install(device,
        [](const auto& message) { LOG_WARN("XESS-NATIVE {}", message); }, error);
    LOG_INFO("XESS-NATIVE diagnostic splice installed={} error={}", captureHooksReady, error);
}
bool TryCapture(xess_context_handle_t context, ID3D12GraphicsCommandList* list,
                const xess_d3d12_execute_params_t& params, uint64_t frame, bool report) {
    std::lock_guard lock(captureMutex);
    if (!captureHooksReady || captureScheduled) return false;
    std::error_code ec;
    const auto request = CaptureRequestPath();
    if (!std::filesystem::is_regular_file(request, ec)) return false;
    std::ifstream input(request);
    std::string rootText;
    std::getline(input, rootText);
    if (!rootText.empty() && rootText.back() == '\r') rootText.pop_back();
    if (rootText == "wait") return false; // Arm only after the scene loads.
    const std::filesystem::path root(rootText);
    if (rootText.size() > 4096 || !root.is_absolute() || !std::filesystem::is_directory(root, ec)) {
        if (report) LOG_WARN("XESS-NATIVE capture request must name an existing absolute directory");
        return false;
    }
    std::vector<Dx12InputSnapshot::Texture> sources;
    for (const auto& [name, resource] : std::array<std::pair<const char*,ID3D12Resource*>,3>{{
             {"color", params.pColorTexture}, {"motion", params.pVelocityTexture},
             {"exposure", params.pExposureScaleTexture}}}) {
        if (!resource) continue;
        const auto desc = resource->GetDesc();
        if (!(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)) {
            if (report) LOG_WARN("XESS-NATIVE diagnostic capture cannot assume COMMON for non-simultaneous {}", name);
            return false;
        }
        Dx12InputSnapshot::Texture texture; texture.name = name; texture.resource = resource;
        sources.push_back(std::move(texture));
    }
    if (!params.pColorTexture || !params.pVelocityTexture) return false;
    const auto& sdk = XeSSProxy::GameRuntime();
    float vx=1,vy=1,jx=1,jy=1,multiplier=1;
    if (sdk.xessGetVelocityScale) sdk.xessGetVelocityScale(context,&vx,&vy);
    if (sdk.xessGetJitterScale) sdk.xessGetJitterScale(context,&jx,&jy);
    if (sdk.xessGetExposureMultiplier) sdk.xessGetExposureMultiplier(context,&multiplier);
    xess_d3d12_init_params_t requested {}, effective {};
    { std::lock_guard contextLock(mutex); requested=contexts.at(context).init; }
    auto queried=sdk.xessD3D12GetInitParams ? sdk.xessD3D12GetInitParams(context,&effective) : XESS_RESULT_ERROR_UNSUPPORTED;
    std::ostringstream metadata;
    metadata.precision(9);
    metadata << "{\"frame\":" << frame << ",\"input\":[" << params.inputWidth << ',' << params.inputHeight
             << "],\"output\":[" << requested.outputResolution.x << ',' << requested.outputResolution.y
             << "],\"requested_flags\":" << requested.initFlags << ",\"effective_flags\":" << effective.initFlags
             << ",\"effective_query_result\":" << int(queried) << ",\"jitter\":[" << params.jitterOffsetX << ',' << params.jitterOffsetY
             << "],\"jitter_scale\":[" << jx << ',' << jy << "],\"velocity_scale\":[" << vx << ',' << vy
             << "],\"color_base\":[" << params.inputColorBase.x << ',' << params.inputColorBase.y
             << "],\"motion_base\":[" << params.inputMotionVectorBase.x << ',' << params.inputMotionVectorBase.y
             << "],\"exposure_scale\":" << params.exposureScale << ",\"exposure_multiplier\":" << multiplier
             << ",\"reset_history\":" << params.resetHistory << ",\"depth_present\":" << (params.pDepthTexture ? "true" : "false")
             << ",\"depth_captured\":false,\"snr\":false}\n";
    const auto directory=root/("frame-"+std::to_string(frame)+"-"+std::to_string(GetTickCount64()));
    std::string error;
    const bool split=D3D12CommandSplice::Begin(list,
        [sources=std::move(sources),directory,metadata=metadata.str()](auto* device,auto* queue) {
            try {
                std::string error;
                if (!Dx12InputSnapshot::Capture(device,queue,sources,directory,error)) {
                    LOG_ERROR("XESS-NATIVE snapshot failed: {}",error); return;
                }
                std::ofstream file(directory/"frame.json"); file<<metadata;
                if (!file) { LOG_ERROR("XESS-NATIVE snapshot metadata write failed"); return; }
                LOG_INFO("XESS-NATIVE snapshot complete: {}",directory.string());
            } catch (const std::exception& e) {
                LOG_ERROR("XESS-NATIVE snapshot exception: {}",e.what());
            }
        },error);
    if (!split) {
        if (report) LOG_INFO("XESS-NATIVE snapshot waiting for safe list boundary: {}",error);
        return false;
    }
    captureScheduled=true;
    auto consumed=request; consumed += L".consumed-"+std::to_wstring(GetTickCount64());
    std::filesystem::rename(request,consumed,ec);
    if (ec) LOG_WARN("XESS-NATIVE could not rename consumed capture request: {}",ec.message());
    LOG_INFO("XESS-NATIVE snapshot scheduled at frame={} (native XeSS, SNR still off)",frame);
    return true;
}

void LogResource(const char* name, ID3D12Resource* resource, xess_coord_t base) {
    if (!resource) { LOG_INFO("XESS-NATIVE {} absent", name); return; }
    const auto d = resource->GetDesc();
    LOG_INFO("XESS-NATIVE {} resource={:X} format={} size={}x{} base={},{} samples={} flags={}",
        name, (size_t)resource, (unsigned)d.Format, d.Width, d.Height, base.x, base.y,
        d.SampleDesc.Count, (unsigned)d.Flags);
}
}

bool Enabled() { return Config::Instance()->DlssNrNativeXeSS.value_or_default(); }
bool Owns(xess_context_handle_t context) {
    std::lock_guard lock(mutex);
    return contexts.contains(context);
}
xess_result_t Create(ID3D12Device* device, xess_context_handle_t* context) {
    const auto fn = XeSSProxy::GameRuntime().xessD3D12CreateContext;
    if (!fn) return XESS_RESULT_ERROR_UNSUPPORTED;
    const auto result = fn(device, context);
    if (result == XESS_RESULT_SUCCESS && context && *context) {
        PrepareCapture(device);
        XeSSNrNative::Create(*context, device);
        std::lock_guard lock(mutex);
        contexts.emplace(*context, Context{});
        LOG_INFO("XESS-NATIVE real game context created: {:X}; SDK parameters preserved", (size_t)*context);
    }
    return result;
}
xess_result_t Destroy(xess_context_handle_t context) {
    const auto fn = XeSSProxy::GameRuntime().xessDestroyContext;
    if (!fn) return XESS_RESULT_ERROR_UNSUPPORTED;
    const auto result = fn(context);
    if (result == XESS_RESULT_SUCCESS) {
        std::lock_guard lock(mutex);
        contexts.erase(context);
        XeSSNrNative::Destroy(context);
    }
    return result;
}
xess_result_t Init(xess_context_handle_t context, const xess_d3d12_init_params_t* params) {
    const auto fn = XeSSProxy::GameRuntime().xessD3D12Init;
    if (!fn) return XESS_RESULT_ERROR_UNSUPPORTED;
    const auto result = fn(context, params);
    if (result == XESS_RESULT_SUCCESS && params) {
        std::lock_guard lock(mutex);
        contexts[context] = Context{*params};
        XeSSNrNative::Reset(context);
        LOG_INFO("XESS-NATIVE init context={:X} output={}x{} quality={} flags=0x{:X} native HDR={} autoExposure={} exposureTexture={} highResMV={} jitteredMV={}",
            (size_t)context, params->outputResolution.x, params->outputResolution.y, (int)params->qualitySetting,
            params->initFlags, !(params->initFlags & XESS_INIT_FLAG_LDR_INPUT_COLOR),
            !!(params->initFlags & XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE),
            !!(params->initFlags & XESS_INIT_FLAG_EXPOSURE_SCALE_TEXTURE),
            !!(params->initFlags & XESS_INIT_FLAG_HIGH_RES_MV), !!(params->initFlags & XESS_INIT_FLAG_JITTERED_MV));
    }
    return result;
}
xess_result_t Execute(xess_context_handle_t context, ID3D12GraphicsCommandList* list,
                      const xess_d3d12_execute_params_t* params) {
    const auto& sdk = XeSSProxy::GameRuntime();
    if (!sdk.xessD3D12Execute) return XESS_RESULT_ERROR_UNSUPPORTED;
    if (!list || !params) return XESS_RESULT_ERROR_INVALID_ARGUMENT;
    InstallProbeHooks(list);
    uint64_t frame;
    bool report;
    {
        std::lock_guard lock(mutex);
        auto& c = contexts.at(context);
        frame = ++c.frames;
        report = frame <= 5 || frame % 300 == 0 || params->resetHistory ||
                 c.width != params->inputWidth || c.height != params->inputHeight;
        c.width = params->inputWidth; c.height = params->inputHeight;
        // Internal SDK dispatches are not part of the game's tail.
        tails.erase(list);
    }
    if (report) {
        float vx = 1, vy = 1, jx = 1, jy = 1, multiplier = 1;
        if (sdk.xessGetVelocityScale) sdk.xessGetVelocityScale(context, &vx, &vy);
        if (sdk.xessGetJitterScale) sdk.xessGetJitterScale(context, &jx, &jy);
        if (sdk.xessGetExposureMultiplier) sdk.xessGetExposureMultiplier(context, &multiplier);
        LOG_INFO("XESS-NATIVE frame={} context={:X} list={:X} input={}x{} jitter={},{} jitterScale={},{} velocityScale={},{} exposureScale={} exposureMultiplier={} reset={} (producer parameters, before SNR scheduling)",
            frame, (size_t)context, (size_t)list, params->inputWidth, params->inputHeight,
            params->jitterOffsetX, params->jitterOffsetY, jx, jy, vx, vy,
            params->exposureScale, multiplier, params->resetHistory);
        LogResource("color", params->pColorTexture, params->inputColorBase);
        LogResource("motion", params->pVelocityTexture, params->inputMotionVectorBase);
        LogResource("depth", params->pDepthTexture, params->inputDepthBase);
        LogResource("exposure", params->pExposureScaleTexture, {});
        LogResource("mask", params->pResponsivePixelMaskTexture, params->inputResponsiveMaskBase);
        LogResource("output", params->pOutputTexture, params->outputColorBase);
    }
    bool split = false;
    try { split = TryCapture(context, list, *params, frame, report); }
    catch (const std::exception& e) { LOG_ERROR("XESS-NATIVE capture preparation failed: {}", e.what()); }
    auto nativeParams = *params;
    try { split = XeSSNrNative::Record(context, list, nativeParams, frame, split) || split; }
    catch (const std::exception& e) {
        LOG_ERROR("XESS-NR preparation failed: {}", e.what());
        XeSSNrNative::Reset(context);
        nativeParams = *params;
        nativeParams.resetHistory = 1;
    }
    xess_result_t result;
    if (split) {
        D3D12CommandSplice::InternalScope scope;
        result = sdk.xessD3D12Execute(context, D3D12CommandSplice::Continuation(list), &nativeParams);
    } else {
        result = sdk.xessD3D12Execute(context, list, &nativeParams);
    }
    if (result != XESS_RESULT_SUCCESS) LOG_ERROR("XESS-NATIVE execute failed: {}", (int)result);
    if (report && hooksReady.load()) {
        Tail tail;
        tail.frame = frame;
        tail.resources = { params->pColorTexture, params->pVelocityTexture, params->pDepthTexture,
                           params->pExposureScaleTexture, params->pResponsivePixelMaskTexture, params->pOutputTexture };
        tail.finalStates.fill(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        tail.finalStates[5] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        std::lock_guard lock(mutex);
        tails[list] = tail;
    }
    return result;
}
}
