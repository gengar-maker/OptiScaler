# Native SYCL NR backend boundary

This directory contains an *OptiScaler-owned* Intel SYCL model provider. It
does not reuse the NVIDIA NGX forwarder ABI. `ArcNrSyclApi.h` is the versioned
interface to `arc_nr_sycl.dll`; `ArcNrSyclProvider.cpp` builds that module
with Intel oneAPI and the reconstructed SNR model; `ArcNrSyclLoader.cpp` is
compiled into OptiScaler and loads/checks the module ABI. ABI v3 retains
host buffers for standalone tests, adds D3D12 shared GPU heap handles, and
optionally returns stage timings for GPU frames.
The game path copies between tiled textures and row-major shared buffers on
the D3D12 queue. SYCL imports those buffers as device memory; no frame pixels
pass through host memory.

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
OptiScaler. The Windows provider build, standalone invocation, and native
D3D12 path in Black Myth: Wukong have passed on Intel Arc Pro B70.
For a reproducible Windows provider build and non-black frame test, follow
[windows/README.md](windows/README.md). That test does not verify a game's
native D3D12 command-list boundary.

The chosen scheduling contract is **same-frame**. The D3D11→D3D12 and
Vulkan→D3D12 bridges own their command allocators, lists and queue. The new
`ArcNrSyclOwnedListBridge` submits the encoded input, waits for a D3D12 fence,
evaluates SYCL over shared GPU buffers, copies the answer into the output
texture, waits for another fence, then resets the owned list so the caller
can record the resolve and copy-back. It rejects non-RGBA16F surfaces.

The D3D11-to-D3D12 upscaler bridge now calls the owned-list primitive when
`[DlssNr] Backend=1`. Put `arc_nr_sycl.dll` and the `arc_nr_weights` directory
(with `logical` and `derived` children) beside OptiScaler. The full Windows
OptiScaler solution builds with this integration. The provider has passed a
standalone GPU frame test, but the D3D11 in-game path has not been exercised.
The first integration uses independent frames; temporal history and motion
conversion are still needed before claiming equivalent NR output. Failures
are logged and do not silently fall back to NVIDIA.

Native D3D12 games supply a game-owned open list. OptiScaler cannot close or
reset that list. An opt-in `NativeAfterSubmit` path records the upscaler's
output/depth/motion resources, waits for the game to submit the producing
list, and then runs encode, SYCL inference, GPU copy and resolve on an
OptiScaler-owned list on the same direct queue. The queue hook splits a batch
at the producer list, so later game lists in that batch run after the resolve.
`NativeOutputState` must name the actual D3D12 resource state at the end of
the producer list. A game that consumes or aliases the output later in the
*same* producer list cannot use this path: a queue hook cannot insert work
among commands already recorded in one list. Wukong ran with
`NativeAfterSubmit=true` and `NativeOutputState=8`; verify the boundary and
state separately for other games.

Both owned-list paths use explicit synchronization: copy the encoded RGBA16F
texture into a shared GPU heap, submit and wait for a D3D12 fence, run
synchronous SYCL inference over imported heap memory, copy the answer from
another shared GPU heap on the same queue, and wait for a second fence.
Standalone tests verified SYCL writes visible to D3D12 and bit-identical
320x320 model output through the shared path on Intel Arc Pro B70.
On Windows, `arc_nr_sycl_provider_gpu_smoke` exercises D3D12 shared heaps,
two explicit queue fences and the provider's `EvaluateGpu` entry point.
It injects a raw test image with an upload buffer and writes a readback file
only to verify the result; the OptiScaler game path uses neither host buffer.

## Performance profile on Arc Pro B70

`NR-PROFILE` logs allocation, producer fence, provider, and output fence times
every 30 native D3D12 frames. The provider's stage breakdown includes heap
import, input copy, model execution, and output copy. At 1920x1080, the
original shared-heap output kernel took about 93 ms and model execution about
570 ms per warmed frame. A SYCL device-to-device `memcpy` for packed rows
reduced output copy to about 0.4 ms. A tiled window-attention kernel in the
SNR runtime reduced model execution to about 350 ms by sharing each 8x8
window's keys and values in GPU local memory. Offline output remained bitwise
identical at 1920x1080, 960x540, and 480x270.

The optimized Wukong log measured provider time of 350-353 ms, including
348-351 ms for the model, with 2-5 ms bridge allocation and 10-14 ms producer
work on sampled frames. The first frame can take several seconds for model
weight preparation and SYCL kernel compilation. Full-resolution inference is
still too expensive for real-time play; most remaining time is window
attention. `SNR_PROFILE_MODEL=1` prints per-stage and top-block model timings
for a standalone provider run.
