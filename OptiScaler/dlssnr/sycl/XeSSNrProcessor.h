#pragma once
#include "ArcNrSyclLoader.h"
#include "ArcNrSyclOwnedListBridge.h"
#include "XeSSNrInputAdapter.h"
#include <mutex>
#include <memory>

namespace XeSSNr
{
struct Frame
{
    Microsoft::WRL::ComPtr<ID3D12Resource> colour, motion, output, exposure;
    unsigned width = 0, height = 0, colourX = 0, colourY = 0, motionX = 0, motionY = 0;
    uint64_t sequence = 0;
    Vec2 velocityScale { 1, 1 }, jitter {};
    bool ndc = false, jitteredMotion = false, reset = false, applyModel = true;
    DlssNrConstants hdr {};
    ArcNrSyclFrame controls {};
};

struct ExecutionOptions
{
    bool batchSubmissions = true;
    bool localSharedHeap = true;
};

// One model/history per native XeSS context. Prepare and Run are serialized;
// a resized model cannot be used accidentally by an earlier recorded job.
// Inputs/outputs remain GPU resources. Compilation runs on the loader worker.
class Processor
{
  public:
    explicit Processor(ID3D12Device* device, ExecutionOptions options = {}) : device_(device), options_(options) {}
    ArcNrSyclLoader::Preparation Prepare(const std::filesystem::path& directory, unsigned width, unsigned height,
                                         std::string& error);
    std::string PreparationMessage();
    // Must run after the producer fence. Always writes an original-colour
    // fallback before inference. Returns false for a recoverable model failure.
    // Throws for a failed queue fence; caller must retain the whole job and
    // must not submit consumers using possibly unfinished GPU work.
    // Diagnostic GPU copy only: no HDR adapter, inference or interop.
    void CopyOnly(ID3D12CommandQueue* queue, const Frame& frame);
    bool Run(ID3D12CommandQueue* queue, const Frame& frame, std::string& error);
    ArcNrSyclGpuTimings Timings();
    double LastFrameMs();
    uint64_t ModelGeneration()
    {
        std::lock_guard lock(mutex_);
        return provider_.Generation();
    }
    std::array<double, NrGpuTimer::Count> GpuTimings()
    {
        std::lock_guard lock(mutex_);
        return gpuMs_;
    }

  private:
    void Submit(ID3D12CommandQueue* queue);
    void RecordOriginal(const Frame& frame);
    void CopyAndSubmit(ID3D12CommandQueue* queue, const Frame& frame);
    ExecutionOptions options_;
    std::mutex mutex_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list_;
    ArcNrSyclLoader provider_;
    ArcNrSyclOwnedListBridge bridge_;
    std::unique_ptr<InputAdapter> adapter_;
    std::unique_ptr<ReadableCopy> readableCopy_;
    Microsoft::WRL::ComPtr<ID3D12Resource> exposureSnapshot_;
    unsigned preparedWidth_ = 0, preparedHeight_ = 0, adapterWidth_ = 0, adapterHeight_ = 0;
    uint64_t lastSequence_ = 0, modelGeneration_ = 0;
    Vec2 previousJitter_ {};
    float previousWhite_ = 0;
    ArcNrSyclFrame previousControls_ {};
    bool history_ = false, previousApply_ = false, previousExposureTexture_ = false;
    ArcNrSyclGpuTimings timings_ {};
    NrGpuTimer gpuTimer_;
    std::array<double, NrGpuTimer::Count> gpuMs_ {};
    double lastMs_ = 0;
};
} // namespace XeSSNr
