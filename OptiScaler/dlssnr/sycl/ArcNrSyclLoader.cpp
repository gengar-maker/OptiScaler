#include <pch.h>
#include "ArcNrSyclLoader.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <utility>

namespace { std::atomic<bool> process_shutdown {false}; }
void ArcNrSyclLoader::OnProcessShutdown() noexcept { process_shutdown = true; }

ArcNrSyclLoader::~ArcNrSyclLoader() { Reset(); }

bool ArcNrSyclLoader::Load(const std::filesystem::path& module_path,
                           std::string& error) {
    Reset();
    if (!std::filesystem::is_regular_file(module_path)) {
        error = "arc_nr_sycl.dll is missing";
        return false;
    }
    HMODULE module = LoadLibraryExW(module_path.c_str(), nullptr,
                                    LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module == nullptr) {
        error = "arc_nr_sycl.dll or one of its dependencies could not load (Win32 " +
                std::to_string(GetLastError()) + ")";
        return false;
    }
    auto get_api = reinterpret_cast<PFN_ArcNrSyclGetApi>(
        GetProcAddress(module, ARC_NR_SYCL_GET_API_NAME));
    ArcNrSyclApi api {};
    api.struct_size = sizeof(api);
    if (get_api == nullptr ||
        !get_api(ARC_NR_SYCL_API_VERSION, &api) ||
        api.version != ARC_NR_SYCL_API_VERSION ||
        api.create == nullptr || api.evaluate == nullptr ||
        api.destroy == nullptr || api.evaluate_gpu == nullptr) {
        FreeLibrary(module);
        error = "arc_nr_sycl.dll has an incompatible ABI";
        return false;
    }
    module_ = module;
    api_ = api;
    error.clear();
    return true;
}

bool ArcNrSyclLoader::Create(const ArcNrSyclCreate& create,
                             std::string& error) {
    if (module_ == nullptr || api_.create == nullptr) {
        error = "SYCL provider is not loaded";
        return false;
    }
    ResetSession();
    std::array<char, 512> diagnostic {};
    session_ = api_.create(&create, diagnostic.data(),
                           static_cast<uint32_t>(diagnostic.size()));
    error = diagnostic.data();
    if (session_) { active_width_ = create.width; active_height_ = create.height; ++generation_; }
    return session_ != nullptr;
}

ArcNrSyclLoader::Preparation ArcNrSyclLoader::PrepareAsync(
    const ArcNrSyclCreate& create, std::string& error) {
    error.clear();
    if (!Loaded() || !api_.create || !create.logical_weights_directory ||
        !create.derived_weights_directory) {
        error = "SYCL provider is not loaded or paths are invalid";
        return Preparation::Failed;
    }
    if (Ready() && active_width_ == create.width && active_height_ == create.height)
        return Preparation::Ready;
    if (failed_width_ == create.width && failed_height_ == create.height) {
        error = preparation_error_;
        return Preparation::Failed;
    }
    std::unique_ptr<Prepared> retired;
    if (pending_ && pending_->valid()) {
        if (pending_->wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return Preparation::Pending;
        retired = pending_->get();
        if (retired->width == create.width && retired->height == create.height) {
            if (!retired->session) {
                failed_width_ = create.width; failed_height_ = create.height;
                error = preparation_error_ = retired->error;
                return Preparation::Failed;
            }
            session_ = std::exchange(retired->session, nullptr);
            ++generation_;
            active_width_ = create.width; active_height_ = create.height;
            RetryPreparation();
            return Preparation::Ready;
        }
    }
    // Retire the old model on the worker as well. The caller must only request
    // a new size between completed, synchronous Evaluate calls.
    void* old = std::exchange(session_, nullptr);
    const auto api = api_;
    const auto width = create.width, height = create.height;
    std::string logical(create.logical_weights_directory);
    std::string derived(create.derived_weights_directory);
    pending_ = std::make_unique<std::future<std::unique_ptr<Prepared>>>(std::async(std::launch::async,
        [api, width, height, logical = std::move(logical), derived = std::move(derived),
         old, retired = std::move(retired)]() mutable {
            retired.reset();
            if (old) api.destroy(old);
            auto result = std::make_unique<Prepared>();
            result->width = width; result->height = height; result->destroy = api.destroy;
            const ArcNrSyclCreate args {sizeof(ArcNrSyclCreate), width, height,
                                       logical.c_str(), derived.c_str()};
            std::array<char, 1024> message {};
            result->session = api.create(&args, message.data(), static_cast<uint32_t>(message.size()));
            result->error = message.data();
            if (!result->session && result->error.empty())
                result->error = "SNR model preparation failed without a diagnostic";
            return result;
        }));
    return Preparation::Pending;
}

std::string ArcNrSyclLoader::PreparationMessage() const {
    using Query = void (*)(char*, uint32_t);
    const auto query = module_ ? reinterpret_cast<Query>(GetProcAddress(
        static_cast<HMODULE>(module_), "arc_nr_sycl_preparation_status")) : nullptr;
    std::array<char, 512> message {};
    if (query) query(message.data(), static_cast<uint32_t>(message.size()));
    return message[0] ? message.data() : "Preparing SNR model";
}

void ArcNrSyclLoader::RetryPreparation() {
    failed_width_ = failed_height_ = 0;
    preparation_error_.clear();
}

ArcNrSyclStatus ArcNrSyclLoader::Evaluate(const ArcNrSyclFrame& frame,
                                            std::string& error) {
    if (!Ready()) {
        error = "SYCL model session is not ready";
        return ARC_NR_SYCL_MODEL_UNAVAILABLE;
    }
    std::array<char, 512> diagnostic {};
    const ArcNrSyclStatus status = api_.evaluate(
        session_, &frame, diagnostic.data(),
        static_cast<uint32_t>(diagnostic.size()));
    error = diagnostic.data();
    return status;
}

ArcNrSyclStatus ArcNrSyclLoader::EvaluateGpu(const ArcNrSyclGpuFrame& frame,
                                               std::string& error) {
    if (!Ready()) {
        error = "SYCL model session is not ready";
        return ARC_NR_SYCL_MODEL_UNAVAILABLE;
    }
    std::array<char, 512> diagnostic {};
    const ArcNrSyclStatus status = api_.evaluate_gpu(
        session_, &frame, diagnostic.data(),
        static_cast<uint32_t>(diagnostic.size()));
    error = diagnostic.data();
    return status;
}

void ArcNrSyclLoader::ResetSession() {
    if (!process_shutdown && session_ != nullptr && api_.destroy != nullptr)
        api_.destroy(session_);
    session_ = nullptr;
    active_width_ = active_height_ = 0;
}

void ArcNrSyclLoader::Reset() {
    if (process_shutdown) {
        // Intentionally let the OS reclaim these at process termination.
        // A std::async future destructor would join a terminated worker here.
        pending_.release();
        session_ = module_ = nullptr;
        return;
    }
    // Only unload/destruction waits; the render-time resize path never does.
    if (pending_ && pending_->valid()) pending_->get().reset();
    pending_.reset();
    ResetSession();
    RetryPreparation();
    if (module_ != nullptr)
        FreeLibrary(static_cast<HMODULE>(module_));
    module_ = nullptr;
    api_ = {};
}
