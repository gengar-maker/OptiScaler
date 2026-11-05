#pragma once

#include <filesystem>
#include <future>
#include <memory>
#include <string>

#include "ArcNrSyclApi.h"

// Windows host side of the native SYCL module boundary. This class only
// loads the ABI and owns a model session; it does not imply that a D3D12
// command list has been submitted. The caller must establish the input/output
// fence ordering described in ArcNrSyclApi.h before calling Evaluate.
class ArcNrSyclLoader {
public:
    ArcNrSyclLoader() = default;
    ~ArcNrSyclLoader();
    ArcNrSyclLoader(const ArcNrSyclLoader&) = delete;
    ArcNrSyclLoader& operator=(const ArcNrSyclLoader&) = delete;

    bool Load(const std::filesystem::path& module_path, std::string& error);
    bool Create(const ArcNrSyclCreate& create, std::string& error);
    enum class Preparation { Ready, Pending, Failed };
    // Create/compile on a worker. Polling never waits for a model build. A
    // resize supersedes the pending size; only one build runs at a time.
    Preparation PrepareAsync(const ArcNrSyclCreate& create, std::string& error);
    std::string PreparationMessage() const;
    void RetryPreparation();
    // DllMain process termination: other threads may already be gone. Never
    // join a compiler worker or tear down the GPU runtime under loader lock.
    static void OnProcessShutdown() noexcept;
    ArcNrSyclStatus Evaluate(const ArcNrSyclFrame& frame, std::string& error);
    ArcNrSyclStatus EvaluateGpu(const ArcNrSyclGpuFrame& frame, std::string& error);
    void ResetSession();
    void Reset();
    bool Loaded() const { return module_ != nullptr; }
    bool Ready() const { return module_ != nullptr && session_ != nullptr; }
    uint64_t Generation() const { return generation_; }

private:
    void* module_ = nullptr;
    void* session_ = nullptr;
    ArcNrSyclApi api_ {};
    struct Prepared {
        void* session = nullptr;
        void (*destroy)(void*) = nullptr;
        uint32_t width = 0, height = 0;
        std::string error;
        ~Prepared() { if (session && destroy) destroy(session); }
    };
    std::unique_ptr<std::future<std::unique_ptr<Prepared>>> pending_;
    uint32_t active_width_ = 0, active_height_ = 0;
    uint32_t failed_width_ = 0, failed_height_ = 0;
    std::string preparation_error_;
    uint64_t generation_ = 0;
};
