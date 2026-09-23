#include "ArcNrSyclLoader.h"

#include <Windows.h>

#include <array>

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
        api.destroy == nullptr) {
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
    if (session_ != nullptr) {
        api_.destroy(session_);
        session_ = nullptr;
    }
    std::array<char, 512> diagnostic {};
    session_ = api_.create(&create, diagnostic.data(),
                           static_cast<uint32_t>(diagnostic.size()));
    error = diagnostic.data();
    return session_ != nullptr;
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

void ArcNrSyclLoader::Reset() {
    if (session_ != nullptr && api_.destroy != nullptr)
        api_.destroy(session_);
    session_ = nullptr;
    if (module_ != nullptr)
        FreeLibrary(static_cast<HMODULE>(module_));
    module_ = nullptr;
    api_ = {};
}
