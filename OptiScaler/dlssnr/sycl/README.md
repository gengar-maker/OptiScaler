# Native SYCL NR backend boundary

This directory contains an *OptiScaler-owned* Intel SYCL model provider. It
does not reuse the NVIDIA NGX forwarder ABI. `ArcNrSyclApi.h` is the versioned
interface to `arc_nr_sycl.dll`; `ArcNrSyclProvider.cpp` builds that module
with Intel oneAPI and the reconstructed SNR model; `ArcNrSyclLoader.cpp` is
compiled into OptiScaler and loads/checks the module ABI. The first ABI uses
explicit, tightly packed FP16 host buffers so that the tensor contract can be
validated without assuming D3D12 texture tiling or row pitch. Future
versions may import shared D3D12/Level Zero buffers.

The provider keeps the model, weights and device buffers alive across calls.
On Intel Arc Pro B70 it built and evaluated a captured 1920×1080 frame through
the ABI. Its RGBA16F output was bit-for-bit identical to the existing SNR
offline SYCL runner; the RGB MAE against the captured NVIDIA `model_output`
was 0.00942. A second evaluation through the same session with history and
zero motion also completed. This is not NVIDIA parity.

Build/test on an Intel oneAPI environment:

```text
cmake -S OptiScaler/dlssnr/sycl -B build-sycl \
  -DCMAKE_CXX_COMPILER=icpx -DSNR_RUNTIME_DIR=/path/to/SNR/runtime
cmake --build build-sycl --target arc_nr_sycl_provider_smoke -j
build-sycl/arc_nr_sycl_provider_smoke LOGICAL_DIR DERIVED_DIR \
  1920 1080 COLOR_RGBA.f16 OUTPUT_RGBA.f16 [--temporal]
```

On Windows, pass the Intel oneAPI compiler and oneDNN include/library paths
to CMake, then place `arc_nr_sycl.dll` and its runtime dependencies beside
OptiScaler. The Windows build and in-game invocation have not been tested.
For a reproducible Windows provider build and non-black frame test, follow
[windows/README.md](windows/README.md). That test does not enable native
D3D12 in-game SYCL scheduling.

The chosen scheduling contract is **same-frame**. The D3D11→D3D12 and
Vulkan→D3D12 bridges own their command allocators, lists and queue. The new
`ArcNrSyclOwnedListBridge` is a correctness-first primitive for those paths:
submit the encoded input, fence it, read back RGBA16F, evaluate SYCL, upload
RGBA16F, fence again, then reset the owned list so the caller can record the
resolve and copy-back. It rejects non-RGBA16F surfaces. This is intentionally
slow; it is a staging contract to validate before shared-memory optimization.

The primitive is included in the Visual Studio project but **not yet called
or Windows-build-tested**. `DlssNr_Dx12::Dispatch`
still combines NVIDIA-only initialization, encode, evaluation and resolve in
one function. It must be split so the owned-list bridge can replace only the
evaluation stage, with no NVIDIA forwarder or feature creation. The provider
also needs the game-resource/temporal inputs aligned with the exported model
before claiming equivalent NR output. Thus the DLLs are not a working in-game
SYCL option yet. Do not silently fall back to NVIDIA on a SYCL failure.

Native D3D12 games supply a game-owned open list. OptiScaler cannot close or
reset that list; an `ExecuteCommandLists` hook is too late to interleave the
SYCL call inside it. Same-frame native D3D12 support needs an explicit
host-controlled split point. Do not invoke the owned-list bridge on that path.
