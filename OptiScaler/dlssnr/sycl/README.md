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

The provider and loader are not yet a working in-game backend. In the current D3D12 path the
upscaler, NR colour encoding, NR evaluation and output resolve are recorded
into one game-owned open command list. The SYCL API must **not** be called
there: the colour encode has not executed. OptiScaler must first install one
of these explicit scheduling routes:

1. Same-frame: acquire a host-controlled boundary that can submit the colour
   producer, wait for it, run SYCL, and submit the output consumer before UI.
   This requires changing command-list recording/ownership; the existing
   `ExecuteCommandLists` hook alone runs too late to split the list.
2. Delayed: record and fence an input readback in frame N, execute SYCL after
   submission, then consume the ready output in a later frame. This can use
   the existing submission hook, but changes temporal behavior and must be an
   explicit user choice.

Until a route is implemented and tested on a Windows Intel GPU, do not expose
this as a working menu option or silently fall back to NVIDIA's model. The
Windows loader exists but is not called by the NR dispatch path yet.
