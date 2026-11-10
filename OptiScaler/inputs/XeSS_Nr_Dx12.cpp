#include "pch.h"
#include <dlssnr/sycl/SnrControls.h>
#include "XeSS_Nr_Dx12.h"
#include <dlssnr/sycl/D3D12CommandSplice.h>
#include <dlssnr/sycl/XeSSNrProcessor.h>
#include <proxies/XeSS_Proxy.h>
#include <Config.h>
#include <Util.h>
#include <atomic>
#include <fstream>
#include <d3d12sdklayers.h>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace XeSSNrNative
{
namespace
{
using Microsoft::WRL::ComPtr;
enum class Diagnostic
{
    Model,
    Bypass,
    Boundary,
    Copy
};
std::atomic<Diagnostic> diagnosticPhase { Diagnostic::Model };
Diagnostic ReadDiagnostic()
{
    // Opt-in only. The file can be switched during a single game run. An
    // absent file preserves the production path; an invalid file bypasses NR.
    static std::mutex mutex;
    static ULONGLONG nextRead = 0;
    static Diagnostic cached = Diagnostic::Model;
    std::lock_guard lock(mutex);
    const auto now = GetTickCount64();
    if (now < nextRead)
        return cached;
    nextRead = now + 250;
    std::ifstream input(Util::DllPath().parent_path() / "xess-native-nr-mode.txt");
    if (!input)
        return cached = Diagnostic::Model;
    std::string mode;
    input >> mode;
    if (mode == "boundary")
        return cached = Diagnostic::Boundary;
    if (mode == "copy")
        return cached = Diagnostic::Copy;
    if (mode == "model")
        return cached = Diagnostic::Model;
    return cached = Diagnostic::Bypass;
}

void CALLBACK ValidationMessage(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id,
                                LPCSTR description, void*)
{
    if (severity > D3D12_MESSAGE_SEVERITY_WARNING)
        return;
    const auto phase = diagnosticPhase.load();
    // Keep repeated SDK diagnostics bounded while retaining their first details
    // independently in each phase. Never re-enter D3D12 from its callback.
    static std::mutex mutex;
    static std::map<std::pair<int, int>, uint64_t> counts;
    uint64_t count;
    {
        std::lock_guard lock(mutex);
        count = ++counts[{ int(phase), int(id) }];
    }
    if (count <= 16 || count % 10000 == 0)
        LOG_WARN("D3D12-VALIDATION mode={} severity={} id={} occurrence={}: {}", int(phase), int(severity), int(id),
                 count, description);
}
struct Context
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12InfoQueue> validationQueue;
    std::shared_ptr<XeSSNr::Processor> processor;
    std::atomic<bool> forceReset { true };
    bool lastApplied = false, hooks = false;
};
void PollValidation(const std::shared_ptr<Context>& c)
{
    if (!c->validationQueue)
        return;
    auto* q = c->validationQueue.Get();
    const auto count = q->GetNumStoredMessagesAllowedByRetrievalFilter();
    for (UINT64 i = 0; i < count; ++i)
    {
        SIZE_T bytes = 0;
        if (FAILED(q->GetMessage(i, nullptr, &bytes)))
            break;
        std::vector<char> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (SUCCEEDED(q->GetMessage(i, message, &bytes)))
            ValidationMessage(message->Category, message->Severity, message->ID, message->pDescription, nullptr);
    }
    // This opt-in collector drains the queue into the bounded log. It does not
    // change validation filters or enable the layer in ordinary runs.
    q->ClearStoredMessages();
}
std::mutex contextMutex, statusMutex;
std::unordered_map<xess_context_handle_t, std::shared_ptr<Context>> contexts;
Status status;
ULONGLONG preparationStart = 0, lastStatus = 0, lastCompleted = 0, visibleUntil = 0;
void Update(unsigned width, unsigned height, bool pending, bool failed, const std::string& message)
{
    std::lock_guard lock(statusMutex);
    const auto now = GetTickCount64();
    if (pending && (!status.pending || status.width != width || status.height != height))
        preparationStart = now;
    const bool changed = message != status.message || pending != status.pending || failed != status.failed;
    if (changed)
    {
        visibleUntil = now + 6000;
        if (!message.empty())
            LOG_INFO("XESS-NR {}x{}: {}", width, height, message);
    }
    if (pending)
        visibleUntil = now + 6000;
    status.active = true;
    status.width = width;
    status.height = height;
    status.pending = pending;
    status.failed = failed;
    status.message = message;
    status.seconds = preparationStart ? double(now - preparationStart) / 1000 : 0;
    lastStatus = now;
}
bool ValidTexture(ID3D12Resource* resource, UINT width, UINT height, xess_coord_t base, bool colour)
{
    if (!resource || !width || !height || width > 8192 || height > 8192)
        return false;
    const auto d = resource->GetDesc();
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.DepthOrArraySize == 1 && d.MipLevels == 1 &&
           d.SampleDesc.Count == 1 && !(d.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) &&
           uint64_t(base.x) + width <= d.Width && uint64_t(base.y) + height <= d.Height &&
           (colour ? d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT
                   : (d.Format == DXGI_FORMAT_R16G16_FLOAT || d.Format == DXGI_FORMAT_R32G32_FLOAT));
}
bool ValidExposure(ID3D12Resource* resource)
{
    if (!resource)
        return false;
    const auto d = resource->GetDesc();
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width == 1 && d.Height == 1 &&
           d.DepthOrArraySize == 1 && d.MipLevels == 1 && d.SampleDesc.Count == 1 &&
           !(d.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) &&
           (d.Format == DXGI_FORMAT_R32_FLOAT || d.Format == DXGI_FORMAT_R16_FLOAT ||
            d.Format == DXGI_FORMAT_R8_UNORM || d.Format == DXGI_FORMAT_R16_UNORM);
}
} // namespace
void Create(xess_context_handle_t handle, ID3D12Device* device)
{
    auto c = std::make_shared<Context>();
    c->device = device;
    c->processor = std::make_shared<XeSSNr::Processor>(device);
    if (std::filesystem::exists(Util::DllPath().parent_path() / "xess-native-validation.request"))
    {
        const auto hr = device->QueryInterface(IID_PPV_ARGS(&c->validationQueue));
        LOG_INFO("XESS-NR D3D12 validation polling active={} hr={:X}; mode ids: model=0 bypass=1 boundary=2 copy=3",
                 SUCCEEDED(hr), unsigned(hr));
    }
    std::string error;
    c->hooks = D3D12CommandSplice::Install(device, [](const auto& m) { LOG_WARN("XESS-NR {}", m); }, error);
    if (!c->hooks)
        LOG_ERROR("XESS-NR boundary installation: {}", error);
    std::lock_guard lock(contextMutex);
    contexts[handle] = std::move(c);
}
void Destroy(xess_context_handle_t handle)
{
    std::shared_ptr<Context> retired;
    {
        std::lock_guard lock(contextMutex);
        auto it = contexts.find(handle);
        if (it != contexts.end())
        {
            retired = std::move(it->second);
            contexts.erase(it);
        }
    }
    if (retired)
        PollValidation(retired);
    // Pending submission callbacks retain their context and GPU resources.
}
void Reset(xess_context_handle_t handle)
{
    std::lock_guard lock(contextMutex);
    auto it = contexts.find(handle);
    if (it != contexts.end())
    {
        it->second->forceReset = true;
        it->second->lastApplied = false;
    }
}
Status GetStatus()
{
    std::lock_guard lock(statusMutex);
    auto result = status;
    const auto now = GetTickCount64();
    result.active = now - lastStatus < 2000;
    result.running =
        result.active && now - lastCompleted < 2000 && Config::Instance()->DlssNrEnabled.value_or_default();
    result.visible = result.active && now < visibleUntil;
    if (result.pending)
        result.seconds = double(now - preparationStart) / 1000;
    return result;
}
bool Record(xess_context_handle_t handle, ID3D12GraphicsCommandList* list, xess_d3d12_execute_params_t& p,
            uint64_t sequence, bool capture)
{
    std::shared_ptr<Context> context;
    {
        std::lock_guard lock(contextMutex);
        auto it = contexts.find(handle);
        if (it == contexts.end())
            return false;
        context = it->second;
    }
    PollValidation(context);
    const auto& cfg = *Config::Instance();
    const auto skip = [&](const std::string& message, bool pending = false, bool failed = false)
    {
        if (context->lastApplied)
            p.resetHistory = 1;
        context->lastApplied = false;
        context->forceReset = true;
        Update(p.inputWidth, p.inputHeight, pending, failed, message);
        return false;
    };
    if (capture || !cfg.DlssNrEnabled.value_or_default() || cfg.DlssNrBackend.value_or_default() != 1)
        return skip("");
    if (!context->hooks)
        return skip("SNR unavailable: command boundary hooks failed", false, true);
    const auto diagnostic = ReadDiagnostic();
    diagnosticPhase = diagnostic;
    if (diagnostic == Diagnostic::Bypass)
        return skip("Diagnostic: original XeSS, no command split");
    if (diagnostic == Diagnostic::Boundary)
    {
        context->forceReset = true;
        if (context->lastApplied)
            p.resetHistory = 1;
        context->lastApplied = false;
        std::string error;
        const bool split = D3D12CommandSplice::Begin(
            list,
            [sequence](auto*, auto*)
            {
                if (sequence <= 5 || sequence % 60 == 0)
                    LOG_INFO("XESS-NR-DIAG frame={} mode=boundary; no copy, no model, original XeSS inputs", sequence);
            },
            error);
        if (!split)
            return skip("Diagnostic boundary unavailable: " + error, false, true);
        Update(p.inputWidth, p.inputHeight, false, false, "Diagnostic: command split only; original XeSS inputs");
        return true;
    }

    const auto& sdk = XeSSProxy::GameRuntime();
    xess_d3d12_init_params_t init {};
    float vx = 1, vy = 1, jx = 1, jy = 1, exposureMultiplier = 1;
    if (!sdk.xessD3D12GetInitParams || sdk.xessD3D12GetInitParams(handle, &init) != XESS_RESULT_SUCCESS ||
        !sdk.xessGetVelocityScale || sdk.xessGetVelocityScale(handle, &vx, &vy) != XESS_RESULT_SUCCESS ||
        !sdk.xessGetJitterScale || sdk.xessGetJitterScale(handle, &jx, &jy) != XESS_RESULT_SUCCESS ||
        !sdk.xessGetExposureMultiplier ||
        sdk.xessGetExposureMultiplier(handle, &exposureMultiplier) != XESS_RESULT_SUCCESS)
        return skip("SNR unavailable: native XeSS input contract could not be queried");
    // Other modes stay on the game's original SDK until independently tested.
    if (init.initFlags & (XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE | XESS_INIT_FLAG_HIGH_RES_MV))
        return skip("SNR requires explicit scalar/texture exposure and low-resolution engine motion vectors");
    const bool exposureTexture = (init.initFlags & XESS_INIT_FLAG_EXPOSURE_SCALE_TEXTURE) != 0;
    if (exposureTexture && !ValidExposure(p.pExposureScaleTexture))
        return skip("SNR requires a supported single-pixel exposure texture");
    if (!ValidTexture(p.pColorTexture, p.inputWidth, p.inputHeight, p.inputColorBase, true) ||
        !ValidTexture(p.pVelocityTexture, p.inputWidth, p.inputHeight, p.inputMotionVectorBase, false))
        return skip("SNR requires single-mip RGBA16F colour and RG16F/RG32F engine motion");
    std::string error;
    if (diagnostic == Diagnostic::Model)
    {
        const auto preparation =
            context->processor->Prepare(Util::DllPath().parent_path(), p.inputWidth, p.inputHeight, error);
        if (preparation != ArcNrSyclLoader::Preparation::Ready)
            return skip(preparation == ArcNrSyclLoader::Preparation::Pending ? context->processor->PreparationMessage()
                                                                             : error,
                        preparation == ArcNrSyclLoader::Preparation::Pending,
                        preparation == ArcNrSyclLoader::Preparation::Failed);
    }
    auto f = std::make_shared<XeSSNr::Frame>();
    f->width = p.inputWidth;
    f->height = p.inputHeight;
    f->sequence = sequence;
    f->colour = p.pColorTexture;
    f->motion = p.pVelocityTexture;
    if (exposureTexture)
        f->exposure = p.pExposureScaleTexture;
    f->colourX = p.inputColorBase.x;
    f->colourY = p.inputColorBase.y;
    f->motionX = p.inputMotionVectorBase.x;
    f->motionY = p.inputMotionVectorBase.y;
    f->velocityScale = { vx, vy };
    f->jitter = { p.jitterOffsetX * jx, p.jitterOffsetY * jy };
    f->ndc = (init.initFlags & XESS_INIT_FLAG_USE_NDC_VELOCITY) != 0;
    f->jitteredMotion = (init.initFlags & XESS_INIT_FLAG_JITTERED_MV) != 0;
    f->applyModel = cfg.DlssNrApplyModel.value_or_default();
    // Texture exposure REPLACES execute.exposureScale; it is not multiplied
    // by it. Both selected exposure modes still use the SDK multiplier.
    f->hdr.WhitePoint = XeSSNr::ScalarWhitePoint(exposureTexture ? 1.f : p.exposureScale, exposureMultiplier);
    f->hdr.ExposurePreMul = exposureTexture ? f->hdr.WhitePoint : 1.f;
    f->hdr.MaxRatio = 2;
    f->hdr.CompareZoom = 1;
    f->hdr.TransferStrength = cfg.DlssNrTransferStrength.value_or_default();
    f->hdr.ColourStrength = cfg.DlssNrColourStrength.value_or_default();
    SnrControls::Set(f->controls, cfg.DlssNrStyle.value_or_default(), cfg.DlssNrLocalTone.value_or_default(),
                     cfg.DlssNrLocalStructure.value_or_default(), cfg.DlssNrSkinStructure.value_or_default(),
                     cfg.DlssNrAutoMask.value_or_default(), cfg.DlssNrIntensity.value_or_default());
    f->output = XeSSNr::InputAdapter::MakeOutput(context->device.Get(), f->width, f->height);
    if (diagnostic == Diagnostic::Copy)
    {
        context->forceReset = true;
        if (context->lastApplied)
            p.resetHistory = 1;
        context->lastApplied = false;
        const bool split = D3D12CommandSplice::Begin(
            list,
            [context, f](auto*, auto* queue)
            {
                context->processor->CopyOnly(queue, *f);
                if (f->sequence <= 5 || f->sequence % 60 == 0)
                    LOG_INFO("XESS-NR-DIAG frame={} mode=copy; exact GPU colour copy, no HDR transform, no model",
                             f->sequence);
            },
            error);
        if (!split)
            return skip("Diagnostic copy boundary unavailable: " + error, false, true);
        p.pColorTexture = f->output.Get();
        p.inputColorBase = { 0, 0 };
        Update(p.inputWidth, p.inputHeight, false, false, "Diagnostic: GPU colour copy only; no model");
        return true;
    }
    const bool transition = context->lastApplied != f->applyModel;
    const bool forced = context->forceReset.exchange(false);
    f->reset = p.resetHistory || transition || forced;
    const bool split = D3D12CommandSplice::Begin(
        list,
        [context, f](auto*, auto* queue)
        {
            std::string error;
            if (!context->processor->Run(queue, *f, error))
            {
                context->forceReset = true;
                Update(f->width, f->height, false, true, "SNR bypassed: " + error);
                return;
            }
            if (f->reset)
                LOG_INFO("SNR-CONTROLS style={:.7f} tone={:.3f} structure={:.3f} skin={:.3f} automatic={:.3f} "
                         "intensity={:.3f}",
                         f->controls.normalized_style, f->controls.local_tone, f->controls.local_structure,
                         f->controls.skin_structure, f->controls.automatic_structure, f->controls.intensity);
            const auto t = context->processor->Timings();
            const auto ms = context->processor->LastFrameMs();
            {
                std::lock_guard lock(statusMutex);
                status.frameMs = ms;
                status.modelMs = t.model_us / 1000.;
                lastCompleted = GetTickCount64();
            }
            Update(f->width, f->height, false, false, "SNR ready");
            if (f->reset || f->sequence <= 5 || f->sequence % 60 == 0)
                LOG_INFO("XESS-NR frame={} {}x{} neural=true apply={} temporal={} pass={:.3f}ms model={:.3f}ms "
                         "jitter={},{} HDR-white={}",
                         f->sequence, f->width, f->height, f->applyModel, t.temporal_active, ms, t.model_us / 1000.,
                         f->jitter.x, f->jitter.y, f->hdr.WhitePoint);
        },
        error);
    if (!split)
        return skip("SNR waiting for supported command-list boundary: " + error);
    context->lastApplied = f->applyModel;
    if (transition || (forced && f->applyModel))
        p.resetHistory = 1;
    p.pColorTexture = f->output.Get();
    p.inputColorBase = { 0, 0 };
    return true;
}
} // namespace XeSSNrNative
