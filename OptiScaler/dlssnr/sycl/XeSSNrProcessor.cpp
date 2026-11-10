#include "SnrControls.h"
#define NOMINMAX
#include "XeSSNrProcessor.h"
#include <chrono>
#include <pch.h>
#include <stdexcept>

namespace XeSSNr
{
namespace
{
void Check(HRESULT hr, const char* what)
{
    if (FAILED(hr))
        throw std::runtime_error(std::string(what) + ": " + std::to_string(hr));
}
} // namespace
ArcNrSyclLoader::Preparation Processor::Prepare(const std::filesystem::path& directory, unsigned width, unsigned height,
                                                std::string& error)
{
    std::lock_guard lock(mutex_);
    if (!provider_.Loaded() && !provider_.Load(directory / "arc_nr_sycl.dll", error))
        return ArcNrSyclLoader::Preparation::Failed;
    const auto logical = (directory / "arc_nr_weights" / "logical").u8string();
    const auto derived = (directory / "arc_nr_weights" / "derived").u8string();
    const ArcNrSyclCreate args { sizeof(args), width, height, reinterpret_cast<const char*>(logical.c_str()),
                                 reinterpret_cast<const char*>(derived.c_str()) };
    const auto result = provider_.PrepareAsync(args, error);
    if (result == ArcNrSyclLoader::Preparation::Ready)
    {
        preparedWidth_ = width;
        preparedHeight_ = height;
    }
    else
    {
        preparedWidth_ = preparedHeight_ = 0;
        history_ = false;
    }
    return result;
}
std::string Processor::PreparationMessage()
{
    std::lock_guard lock(mutex_);
    return provider_.PreparationMessage();
}
ArcNrSyclGpuTimings Processor::Timings()
{
    std::lock_guard lock(mutex_);
    return timings_;
}
double Processor::LastFrameMs()
{
    std::lock_guard lock(mutex_);
    return lastMs_;
}
void Processor::Submit(ID3D12CommandQueue* queue)
{
    Check(list_->Close(), "native NR Close");
    ID3D12CommandList* lists[] = { list_.Get() };
    queue->ExecuteCommandLists(1, lists);
    std::string error;
    if (!bridge_.Flush(device_.Get(), queue, error))
        throw std::runtime_error(error);
    Check(allocator_->Reset(), "native NR allocator Reset");
    Check(list_->Reset(allocator_.Get(), nullptr), "native NR list Reset");
}
void Processor::RecordOriginal(const Frame& f)
{
    if (!allocator_)
    {
        Check(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_)),
              "native NR allocator");
        Check(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(), nullptr,
                                         IID_PPV_ARGS(&list_)),
              "native NR list");
    }
    // The producer submission commits this fallback before model inference.
    gpuTimer_.Start(list_.Get(), NrGpuTimer::Fallback);
    if (f.colour->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)
    {
        InputAdapter::CopyOriginal(list_.Get(), f.colour.Get(), f.colourX, f.colourY, f.width, f.height,
                                   f.output.Get());
    }
    else
    {
        if (!readableCopy_)
            readableCopy_ = std::make_unique<ReadableCopy>(device_.Get());
        readableCopy_->Record(list_.Get(), 0, f.colour.Get(), f.colourX, f.colourY, f.width, f.height, f.output.Get());
    }
    gpuTimer_.Stop(list_.Get(), NrGpuTimer::Fallback);
}
void Processor::CopyAndSubmit(ID3D12CommandQueue* queue, const Frame& f)
{
    RecordOriginal(f);
    Submit(queue);
}
void Processor::CopyOnly(ID3D12CommandQueue* queue, const Frame& f)
{
    std::lock_guard lock(mutex_);
    CopyAndSubmit(queue, f);
    history_ = false;
}
bool Processor::Run(ID3D12CommandQueue* queue, const Frame& f, std::string& error)
{
    std::lock_guard lock(mutex_);
    const auto start = std::chrono::steady_clock::now();
    gpuTimer_.BeginFrame(device_.Get(), queue);
    if (!provider_.Ready() || preparedWidth_ != f.width || preparedHeight_ != f.height)
    {
        CopyAndSubmit(queue, f);
        history_ = false;
        error = "recorded size was superseded while SNR prepared";
        return false;
    }
    if (!adapter_ || adapterWidth_ != f.width || adapterHeight_ != f.height)
    {
        adapter_ = std::make_unique<InputAdapter>(device_.Get(), f.width, f.height);
        adapterWidth_ = f.width;
        adapterHeight_ = f.height;
        history_ = false;
    }
    // Commit fallback, HDR encode, motion conversion and interop input copies
    // together. The bridge waits for this producer before evaluating the model.
    RecordOriginal(f);
    if (!options_.batchSubmissions)
        Submit(queue);
    const bool controlsChanged = !SnrControls::Equal(previousControls_, f.controls);
    const bool reset = f.reset || !history_ || controlsChanged || f.sequence != lastSequence_ + 1 ||
                       modelGeneration_ != provider_.Generation() || previousWhite_ != f.hdr.WhitePoint ||
                       previousApply_ != f.applyModel || previousExposureTexture_ != bool(f.exposure);
    const auto transform = MotionToRenderPixels(f.width, f.height, f.velocityScale, f.ndc, f.jitteredMotion, f.jitter,
                                                reset ? f.jitter : previousJitter_);
    gpuTimer_.Start(list_.Get(), NrGpuTimer::Encode);
    if (f.exposure)
    {
        if (!readableCopy_)
            readableCopy_ = std::make_unique<ReadableCopy>(device_.Get());
        if (!exposureSnapshot_)
            exposureSnapshot_ = InputAdapter::MakeExposure(device_.Get());
        // Snapshot the current frame once. Encode and resolve must use the same
        // exposure even while inference is in flight; no CPU pixel readback.
        readableCopy_->Record(list_.Get(), 1, f.exposure.Get(), 0, 0, 1, 1, exposureSnapshot_.Get());
    }
    adapter_->Encode(list_.Get(), f.output.Get(), f.motion.Get(), f.motionX, f.motionY, transform, f.hdr,
                     f.exposure ? exposureSnapshot_.Get() : nullptr);
    gpuTimer_.Stop(list_.Get(), NrGpuTimer::Encode);
    auto controls = f.controls;
    controls.struct_size = sizeof(controls);
    controls.frame_index = static_cast<uint32_t>(f.sequence);
    controls.reset_history = reset ? 1 : 0;
    // Full colour/lighting in normalized proxy space. No source-tone filter.
    controls.reserved = 0;
    bool recording = true;
    const bool success =
        bridge_.Run(device_.Get(), queue, list_.Get(), allocator_.Get(), adapter_->Proxy(),
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, adapter_->Neural(),
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, adapter_->Motion(), f.width, f.height, 0, 0, 1, 1, provider_,
                    controls, recording, error, false, &gpuTimer_, options_.batchSubmissions, options_.localSharedHeap);
    if (!recording)
    {
        history_ = false;
        throw std::runtime_error("native NR GPU boundary: " + error);
    }
    // Failure uses the shader's exact bypass, also restoring scratch states.
    gpuTimer_.Start(list_.Get(), NrGpuTimer::Resolve);
    adapter_->Resolve(list_.Get(), f.output.Get(), success && f.applyModel && f.controls.intensity > 0.0f,
                      f.exposure ? exposureSnapshot_.Get() : nullptr);
    gpuTimer_.Stop(list_.Get(), NrGpuTimer::Resolve);
    Submit(queue);
    const auto gpu = gpuTimer_.ReadCompleted();
    gpuMs_ = gpu;
    if (f.sequence % 30 == 0)
        LOG_INFO(
            "NR-GPU frame={} {}x{} fallback={:.3f} encode={:.3f} inputCopy={:.3f} outputCopy={:.3f} resolve={:.3f}ms",
            f.sequence, f.width, f.height, gpu[0], gpu[1], gpu[2], gpu[3], gpu[4]);
    history_ = success;
    previousJitter_ = f.jitter;
    previousWhite_ = f.hdr.WhitePoint;
    previousApply_ = f.applyModel;
    previousExposureTexture_ = bool(f.exposure);
    lastSequence_ = f.sequence;
    modelGeneration_ = provider_.Generation();
    previousControls_ = f.controls;
    if (success)
        timings_ = bridge_.LastProviderTimings();
    lastMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return success;
}
} // namespace XeSSNr
