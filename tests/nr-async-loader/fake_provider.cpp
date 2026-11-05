#include <Windows.h>
#include <atomic>
#include <cstdio>
#include "../../OptiScaler/dlssnr/sycl/ArcNrSyclApi.h"
namespace {
std::atomic<int> creates{0}, destroys{0};
HANDLE gate = CreateEventW(nullptr, FALSE, FALSE, nullptr);
void* Create(const ArcNrSyclCreate* args, char* error, uint32_t bytes) {
    ++creates;
    if (WaitForSingleObject(gate, 10000) != WAIT_OBJECT_0) return nullptr;
    if (args->width == 13) {
        snprintf(error, bytes, "unsupported test size"); return nullptr;
    }
    return new uint32_t(args->width);
}
void Destroy(void* session) { if (session) { ++destroys; delete static_cast<uint32_t*>(session); } }
ArcNrSyclStatus Evaluate(void*, const ArcNrSyclFrame*, char*, uint32_t) { return ARC_NR_SYCL_OK; }
ArcNrSyclStatus EvaluateGpu(void*, const ArcNrSyclGpuFrame*, char*, uint32_t) { return ARC_NR_SYCL_OK; }
}
extern "C" __declspec(dllexport) void release_build() { SetEvent(gate); }
extern "C" __declspec(dllexport) int create_count() { return creates; }
extern "C" __declspec(dllexport) int destroy_count() { return destroys; }
extern "C" int arc_nr_sycl_get_api(uint32_t version, ArcNrSyclApi* api) {
    if (version != ARC_NR_SYCL_API_VERSION || !api) return 0;
    *api = {sizeof(*api), version, Create, Evaluate, Destroy, EvaluateGpu}; return 1;
}
