#pragma once

#include <filesystem>
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
    ArcNrSyclStatus Evaluate(const ArcNrSyclFrame& frame, std::string& error);
    void Reset();
    bool Ready() const { return module_ != nullptr && session_ != nullptr; }

private:
    void* module_ = nullptr;
    void* session_ = nullptr;
    ArcNrSyclApi api_ {};
};
